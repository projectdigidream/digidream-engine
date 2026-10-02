//
// WHEN THE OTHERS ARE DRAWN: a smooth clock in server ticks.
//
// Other bodies are interpolated between snapshots at a render time a fixed
// delay behind the newest one (entities.js). That render time used to be
// `newest snapshot tick + the fraction of the CLIENT'S OWN tick`, which is
// wrong as soon as snapshots arrive less often than once a tick: at 30 Hz the
// fraction ran 0 to 1 twice per snapshot, so drawn time went forward one tick,
// slid back one, and then jumped two when the next snapshot landed. Every
// remote body stuttered back and forth at 30 Hz, and arrival jitter on a busy
// server made it worse.
//
// This advances at the tick rate on the client's own clock, and is steered
// gently towards the newest snapshot tick: never backwards, never a jump,
// unless it is so far off (a new level, a stall) that easing would take
// longer than snapping.
//
export class RenderClock {
  /**
   * @param tickRate  ticks a second (60)
   * @param ease      fraction of the error removed each second (per-second
   *                  rate, applied in proportion to the frame time)
   * @param snapAt    an error, in ticks, past which the clock jumps
   */
  constructor ({ tickRate = 60, ease = 4, snapAt = 6 } = {}) {
    this.tickRate = tickRate
    this.ease = ease
    this.snapAt = snapAt
    this.tick = null      // fractional server tick; null until the first snapshot
    this.newest = 0
  }

  /** A snapshot for server tick `t` arrived. */
  heard (t) { if (t > this.newest) this.newest = t }

  /** Restart from scratch: a new level, a new clock. */
  reset (t = 0) { this.tick = null; this.newest = t }

  /**
   * Advance by `dtSeconds` of real time and return the fractional server tick
   * the newest snapshot corresponds to now.
   */
  advance (dtSeconds) {
    if (this.tick === null) {
      if (this.newest === 0) return 0
      this.tick = this.newest
      return this.tick
    }
    this.tick += dtSeconds * this.tickRate
    const err = this.newest - this.tick
    if (Math.abs(err) > this.snapAt) {
      this.tick = this.newest
    } else {
      // Ease, but never step backwards: a late snapshot slows the clock, it
      // does not reverse it, or drawn bodies would walk back a little.
      const k = Math.min(1, this.ease * dtSeconds)
      const next = this.tick + err * k
      if (next > this.tick || err > 0) this.tick = Math.max(this.tick, next)
    }
    return this.tick
  }
}
