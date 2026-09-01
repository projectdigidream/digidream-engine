//
// The desktop client's connection to a server running on the same machine.
//
// SAME INTERFACE AS net.js's Connection -- open/send/sendControl/close plus
// onControl/onSnapshot/onClose -- because that is the whole seam between the
// transport and the game. net.js says it plainly: "Nothing here decides
// anything about the simulation." Neither does this. main.js picks one of the
// two and nothing downstream of it can tell which it got, which is the property
// that makes a desktop build the same client rather than a second one.
//
// WHY NOT JUST RUN WEBTRANSPORT LOCALLY. The packaged app has the server and
// the client in the same process tree, on one machine, talking over loopback.
// Terminating HTTP/3 in between would cost:
//
//   * a native quiche addon, which has to be rebuilt for Electron's ABI on
//     every platform -- and is the single hardest thing to get working on
//     Windows;
//   * a TLS certificate. WebTransport's serverCertificateHashes escape hatch
//     requires ECDSA P-256 and AT MOST 14 DAYS of validity, so a shipped game
//     cannot carry one; it would have to mint a fresh certificate at every
//     launch, which means depending on openssl being installed on a player's
//     machine;
//   * a listening TCP port, which on Windows means a firewall prompt the first
//     time somebody opens the game.
//
// None of that buys anything over loopback. So the desktop build relays the
// SAME link frames the edge relays (server/edge/src/framing.js) over Electron
// IPC instead, and the WebTransport path stays exactly as it is for playing
// against a real server over a real network.
//
// WHAT IS DIFFERENT, AND IT IS NOT NOTHING. Datagrams here travel over a
// reliable, ordered pipe. That is a SUPERSET of what the protocol asks for, so
// nothing breaks -- but it also means the desktop build never exercises the
// loss and reordering path that tools/netsim covers. A bug that only appears
// when a snapshot goes missing cannot be found by playing the packaged game.
// That is why this is an addition and not a replacement: the WebTransport path
// remains the tested one, and scripts/test.sh still runs the matrix against it.
//
import {
  encodeHello, encodeInputBundle, decodeControlMessage, decodeSnapshot, MSG
} from './proto.js'

/** True when running inside the desktop shell, which exposes the bridge. */
export const hasLocalTransport = () =>
  typeof window !== 'undefined' && !!window.dnp

export class LocalConnection {
  constructor ({ onControl = () => {}, onSnapshot = () => {},
                 onClose = () => {} } = {}) {
    this.onControl = onControl
    this.onSnapshot = onSnapshot
    this.onClose = onClose
    this.closed = false
    this.bridge = typeof window !== 'undefined' ? window.dnp : null
  }

  async open () {
    if (!this.bridge) throw new Error('no local transport bridge (not the desktop shell)')

    this.bridge.onMessage((msg) => {
      if (this.closed) return
      if (msg.kind === 'control') {
        // ONE WHOLE MESSAGE PER EVENT, so unlike net.js there is no
        // reassembly here and there must not be. The relay forwards the
        // frames its length-prefixed reader has already completed -- the same
        // frames the edge forwards upstream -- so a partial message never
        // reaches this point. Reassembling again would be a second framer
        // looking for headers in a stream that no longer has any.
        const m = decodeControlMessage(msg.data)
        if (m) this.onControl(m)
      } else if (msg.kind === 'datagram') {
        // Dropped in silence when malformed, exactly as the WebTransport path
        // does: the next snapshot is already on its way and says it better.
        const s = decodeSnapshot(msg.data)
        if (s) this.onSnapshot(s)
      } else if (msg.kind === 'close') {
        this.closed = true
        // `reason` rather than an Error, matching what a WebTransport close
        // carries -- main.js reads e.reason first, and the reason is the whole
        // point: it is how the shell says "the game server is not running".
        this.onClose({ reason: msg.reason || 'connection closed' })
      }
    })

    await this.bridge.ready()
    this.bridge.send('control', encodeHello())
  }

  sendControl (bytes) {
    if (this.closed || !this.bridge) return false
    this.bridge.send('control', bytes)
    return true
  }

  send (clientTick, ackSnapshotTick, frames) {
    if (this.closed || !this.bridge) return
    // Not awaited, for the same reason net.js does not await a datagram write:
    // input must leave on the tick it was sampled, or the client's clock
    // steering ends up fighting a queue of its own making.
    this.bridge.send('datagram', encodeInputBundle({ clientTick, ackSnapshotTick, frames }))
  }

  close () {
    this.closed = true
    try { this.bridge?.close() } catch {}
  }
}

export { MSG }
