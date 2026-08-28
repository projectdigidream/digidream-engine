//
// The browser's connection to a server.
//
// The node tests have carried an ad-hoc version of this since Phase 1, spelled
// out again in each of them. This is the same handshake written once, for the
// client that actually plays: the reliable control stream for WELCOME, the map,
// motion parameters and parameter updates, and unreliable datagrams for input
// and snapshots (docs/protocol.md §1).
//
// Nothing here decides anything about the simulation. It moves bytes and hands
// decoded messages to a callback; prediction lives in predictor.js and the two
// do not know about each other.
//
import {
  encodeHello, encodeInputBundle, decodeControlMessage, decodeSnapshot, MSG
} from './proto.js'

export class Connection {
  constructor ({ url, certHash = null, onControl = () => {}, onSnapshot = () => {},
                 onClose = () => {} }) {
    this.url = url
    this.certHash = certHash
    this.onControl = onControl
    this.onSnapshot = onSnapshot
    this.onClose = onClose
    this.wt = null
    this.control = null
    this.cw = null
    this.dw = null
    this.closed = false
  }

  async open () {
    if (typeof WebTransport === 'undefined') {
      throw new Error('this browser has no WebTransport')
    }
    // A development server presents a self-signed certificate, and
    // serverCertificateHashes is the supported way to accept exactly that one
    // -- not a blanket "ignore certificate errors", which does not exist for
    // WebTransport and should not.
    const opts = this.certHash
      ? { serverCertificateHashes: [{ algorithm: 'sha-256', value: this.certHash }] }
      : {}
    this.wt = new WebTransport(this.url, opts)
    await this.wt.ready

    this.wt.closed.then(() => { this.closed = true; this.onClose(null) })
                  .catch((e) => { this.closed = true; this.onClose(e) })

    this.control = await this.wt.createBidirectionalStream()
    this.cw = this.control.writable.getWriter()
    // The standard is `datagrams.writable`, a WritableStream. The node library
    // the tests run against also offers createWritable(), and prints a
    // deprecation warning for `.writable`.
    //
    // `.writable` is tried FIRST anyway, and the warning is the price. Ordering
    // it the other way would silence the warning and leave node exercising a
    // branch the browser does not have -- so the browser's path, which is the
    // one that matters, would be the untested one. Do not flip this to quiet
    // the log line.
    const dgOut = this.wt.datagrams.writable ??
                  (typeof this.wt.datagrams.createWritable === 'function'
                    ? this.wt.datagrams.createWritable()
                    : null)
    if (!dgOut) throw new Error('no datagram writer on this WebTransport implementation')
    this.dw = dgOut.getWriter()

    this.#readControl()
    this.#readDatagrams()

    await this.cw.write(encodeHello())
  }

  // The control channel is a BYTE STREAM, so messages carry their own framing
  // and arrive split across reads however the network felt like splitting them
  // (docs/protocol.md §8). Reassembled here; every node test reimplements this
  // loop, and every one of them had to get the partial-header case right.
  async #readControl () {
    const reader = this.control.readable.getReader()
    let buf = new Uint8Array(0)
    try {
      for (;;) {
        const { value, done } = await reader.read()
        if (done) break
        const next = new Uint8Array(buf.length + value.length)
        next.set(buf); next.set(value, buf.length)
        buf = next
        for (;;) {
          if (buf.length < 4) break
          const len = new DataView(buf.buffer, buf.byteOffset, 4).getUint32(0, true)
          if (buf.length < 4 + len) break
          const msg = decodeControlMessage(buf.subarray(0, 4 + len))
          buf = buf.subarray(4 + len)
          if (msg) this.onControl(msg)
        }
      }
    } catch (e) {
      if (!this.closed) this.onClose(e)
    }
  }

  async #readDatagrams () {
    const reader = this.wt.datagrams.readable.getReader()
    try {
      for (;;) {
        const { value, done } = await reader.read()
        if (done) break
        // A malformed datagram is dropped in silence. It is unreliable
        // transport: the next one is already on its way, and there is nothing
        // useful to say that the next snapshot will not say better.
        const snap = decodeSnapshot(value)
        if (snap) this.onSnapshot(snap)
      }
    } catch (e) {
      if (!this.closed) this.onClose(e)
    }
  }

  /**
   * Send an input bundle.
   *
   * `frames` is newest-first and carries the last several ticks, not just this
   * one: the redundancy window of docs/protocol.md §4.1. Input is never
   * retransmitted -- a dropped datagram is covered because the NEXT one already
   * contains the frame that went missing, which costs a few bytes and no
   * latency at all.
   */
  /**
   * Anything on the RELIABLE channel -- currently UI events.
   *
   * Separate from send(), which writes datagrams: an input frame that is lost
   * is replaced by the next one a sixtieth of a second later, and a click on a
   * shop's buy button is not.
   */
  sendControl (bytes) {
    if (!this.cw) return false
    this.cw.write(bytes).catch(() => {})
    return true
  }

  send (clientTick, ackSnapshotTick, frames) {
    if (this.closed || !this.dw) return
    // Deliberately not awaited. Awaiting a datagram write ties the input rate
    // to the writer's backpressure, and input must go out on the tick it was
    // sampled or the server buffers a burst and the client's clock steering
    // fights a queue that is its own doing.
    this.dw.write(encodeInputBundle({ clientTick, ackSnapshotTick, frames }))
      .catch(() => {})
  }

  close () {
    this.closed = true
    try { this.wt?.close() } catch {}
  }
}

export { MSG }
