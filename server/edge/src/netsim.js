//
// Network impairment: latency, jitter, loss and reordering.
//
// Injected in the EDGE rather than in the client, so the code under test is
// completely unmodified -- a client that behaved differently when being measured
// would not be evidence of anything.
//
// The PRNG is seeded and deterministic on purpose. An impairment driven by
// Math.random() produces a failure you cannot reproduce, which is the worst
// possible property for a tool whose entire job is investigating rare,
// timing-dependent bugs. Same seed, same drop pattern, every run.
//
// Loss and reordering apply only to DATAGRAMS. The control channel is reliable
// and ordered by definition (docs/protocol.md 1); dropping bytes out of a byte
// stream would not simulate a lossy network, it would simulate a corrupt one,
// and the framing would desynchronise in a way no real transport permits.
// Latency does apply to both, because latency applies to everything.
//

/** mulberry32 -- small, fast, and adequate for shaping impairment. */
function makeRng (seed) {
  let a = seed >>> 0
  return () => {
    a = (a + 0x6d2b79f5) >>> 0
    let t = Math.imul(a ^ (a >>> 15), 1 | a)
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

export class Impairment {
  /**
   * @param {object} o
   * @param {number} o.latencyMs   one-way delay
   * @param {number} o.jitterMs    +/- uniform variation on that delay
   * @param {number} o.lossPct     percentage of datagrams dropped outright
   * @param {number} o.reorderPct  percentage delayed enough to arrive late
   * @param {number} o.seed
   */
  constructor ({ latencyMs = 0, jitterMs = 0, lossPct = 0, reorderPct = 0, seed = 1 } = {}) {
    this.latencyMs = latencyMs
    this.jitterMs = jitterMs
    this.lossPct = lossPct
    this.reorderPct = reorderPct
    this.rng = makeRng(seed)
    this.stats = { sent: 0, dropped: 0, reordered: 0, delivered: 0 }
    this.timers = new Set()
    this.stopped = false
  }

  get enabled () {
    return this.latencyMs > 0 || this.jitterMs > 0 || this.lossPct > 0 || this.reorderPct > 0
  }

  /** Schedule `deliver` according to the configured impairment. */
  datagram (deliver) {
    this.stats.sent++
    if (this.stopped) return

    if (this.lossPct > 0 && this.rng() * 100 < this.lossPct) {
      this.stats.dropped++
      return
    }

    let delay = this.latencyMs
    if (this.jitterMs > 0) delay += (this.rng() * 2 - 1) * this.jitterMs

    if (this.reorderPct > 0 && this.rng() * 100 < this.reorderPct) {
      // Enough extra delay to land behind at least one subsequent packet.
      delay += 20 + this.rng() * 40
      this.stats.reordered++
    }

    this._schedule(deliver, Math.max(0, delay))
  }

  /** Latency only -- never dropped, never reordered. */
  reliable (deliver) {
    if (this.stopped) return
    let delay = this.latencyMs
    if (this.jitterMs > 0) delay += (this.rng() * 2 - 1) * this.jitterMs
    // IN ORDER, as a reliable stream is: never before the message ahead of
    // it. Jitter drawn per message used to let a later chunk overtake an
    // earlier one -- reordering bytes of an ordered stream, which no real
    // transport does, and which corrupted a map download under any jitter.
    const now = performance.now()
    const at = Math.max(now + Math.max(0, delay), this.reliableAt ?? 0)
    this.reliableAt = at
    this._schedule(deliver, at - now)
  }

  _schedule (deliver, delay) {
    if (delay === 0) {
      this.stats.delivered++
      deliver()
      return
    }
    const t = setTimeout(() => {
      this.timers.delete(t)
      if (this.stopped) return
      this.stats.delivered++
      deliver()
    }, delay)
    this.timers.add(t)
  }

  stop () {
    this.stopped = true
    for (const t of this.timers) clearTimeout(t)
    this.timers.clear()
  }

  describe () {
    if (!this.enabled) return 'clean'
    return `rtt=${this.latencyMs * 2}ms jitter=${this.jitterMs}ms loss=${this.lossPct}% reorder=${this.reorderPct}%`
  }

  report () {
    const { sent, dropped, reordered } = this.stats
    const pct = sent > 0 ? ((dropped / sent) * 100).toFixed(1) : '0.0'
    return `${sent} datagrams, ${dropped} dropped (${pct}%), ${reordered} reordered`
  }
}
