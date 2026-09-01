//
// The desktop shell's relay: the browser window on one side, the C++ server's
// link socket on the other.
//
// THIS IS THE EDGE, MINUS WEBTRANSPORT. server/edge/src/edge.js maps each
// WebTransport session to a link session id, forwards control messages and
// datagrams upstream, and forwards link frames back down the matching channel.
// This does exactly that, with Electron IPC where the WebTransport session was,
// and it keeps the edge's defining property: it does not parse the game
// protocol, does not buffer state and makes no gameplay decisions. If it ever
// starts to, the wire format has two implementations that have to agree, which
// is the hazard design section 2 exists to avoid.
//
// ONE SESSION. The packaged game is one player on one machine, so the session
// id is a constant. The link protocol carries a session id per frame anyway --
// it is what lets a real edge multiplex -- and passing 1 forever costs nothing
// and keeps the frames identical to the ones a real edge sends. The server
// cannot tell the difference, which is the point.
//
// framing.js and upstream.js are COPIED here at build time by
// scripts/build_app.sh, from server/edge/src/. Copied rather than
// reimplemented, and copied rather than imported across a package boundary: a
// second implementation of the link framing is exactly the "two programs, one
// number" failure this repository keeps paying for, and a build-time copy is
// how mapc already solves the same problem for a map's scripts.
//
import { Upstream } from './upstream.js'
import { LINK } from './framing.js'

export class Relay {
  constructor ({ port, host = '127.0.0.1', log = () => {} }) {
    this.log = log
    this.sessionId = 1
    this.open = false
    this.onDown = () => {}
    this.upstream = new Upstream({ host, port })
  }

  /** @param onDown  called as ({ kind, data }) with kind control|datagram|close */
  start (onDown) {
    this.onDown = onDown

    this.upstream.on('connected', () => {
      this.log('upstream connected')
      this.open = true
      // The session is announced the moment the link is up, exactly as the
      // edge does on a new WebTransport session. Without it the server has
      // never heard of us and every frame we send is addressed to a session
      // that does not exist.
      this.upstream.sessionOpen(this.sessionId, { path: '/dnp' })
    })

    this.upstream.on('disconnected', () => {
      // The authoritative server is the only thing that knows the state of a
      // session, so if the link drops the session is orphaned. Saying so beats
      // letting the window keep sending input into a void while looking fine.
      this.log('upstream lost')
      this.open = false
      this.onDown({ kind: 'close', reason: 'the game server stopped' })
    })

    this.upstream.on('link-error', () => {})   // routine while the server starts

    this.upstream.on('frame', ({ type, sessionId, payload }) => {
      if (sessionId !== this.sessionId) return   // stale: a no-op, never an error
      // Copied out of the reader's buffer before it crosses the IPC boundary.
      // `payload` is a view into the link reader's buffer and structured clone
      // would copy it anyway -- but the copy is explicit here because the edge
      // learned this the hard way with netsim holding a view across a timer.
      if (type === LINK.DATAGRAM) {
        this.onDown({ kind: 'datagram', data: new Uint8Array(payload) })
      } else if (type === LINK.CONTROL) {
        this.onDown({ kind: 'control', data: new Uint8Array(payload) })
      } else if (type === LINK.SESSION_CLOSE) {
        this.onDown({ kind: 'close', reason: 'the server closed the session' })
      }
    })

    this.upstream.connect()
  }

  /** From the window: one whole control message, or one datagram. */
  send (kind, bytes) {
    if (!this.open) return false
    const buf = Buffer.from(bytes)
    // Each complete control message goes up individually, so the C++ server
    // never has to reassemble a stream -- the same contract the edge honours.
    return this.upstream.send(kind === 'control' ? LINK.CONTROL : LINK.DATAGRAM,
                              this.sessionId, buf)
  }

  stop () { this.upstream.stop() }
}
