//
// MAP_DATA chunk accumulation (docs/protocol.md §8.0).
//
// The control channel is reliable and ordered, so chunks arrive in sequence and
// none go missing. That is exactly why an out-of-place chunk is treated as
// fatal rather than tolerated: on this channel it cannot be a lost packet, so
// it is a bug or an attack, and quietly reassembling around it would produce a
// map that parses but is not the one the server sent.
//
export class MapAccumulator {
  constructor () {
    this.total = 0
    this.received = 0
    this.buf = null
    this.failed = null
  }

  get complete () { return this.buf !== null && this.received === this.total && this.total > 0 }

  /**
   * @param {{totalBytes:number, offset:number, chunk:Uint8Array}} msg
   * @returns {{ok:boolean, reason?:string}}
   */
  push ({ totalBytes, offset, chunk }) {
    if (this.failed) return { ok: false, reason: this.failed }

    if (this.buf === null) {
      // Bound before allocating: totalBytes is a number chosen by the peer.
      if (totalBytes <= 0 || totalBytes > (8 << 20)) {
        this.failed = 'implausible map size'
        return { ok: false, reason: this.failed }
      }
      this.total = totalBytes
      this.buf = new Uint8Array(totalBytes)
      this.received = 0
    } else if (totalBytes !== this.total) {
      this.failed = 'total size changed mid-transfer'
      return { ok: false, reason: this.failed }
    }

    if (offset !== this.received) {
      this.failed = `chunk out of order (expected ${this.received}, got ${offset})`
      return { ok: false, reason: this.failed }
    }
    if (offset + chunk.length > this.total) {
      this.failed = 'chunk runs past the declared size'
      return { ok: false, reason: this.failed }
    }

    this.buf.set(chunk, offset)
    this.received += chunk.length
    return { ok: true }
  }

  bytes () { return this.complete ? this.buf : null }

  reset () {
    this.total = 0
    this.received = 0
    this.buf = null
    this.failed = null
  }
}
