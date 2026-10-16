//
// THE BODIES NEAR US, DRAWN AT THE PRESENT.
//
// Every remote body is drawn interpolated, INTERP_DELAY_TICKS behind the
// newest snapshot (render/entities.js) -- smooth, and never wrong, but a
// monster at arm's length is shown where it was a beat ago plus the half round
// trip the snapshot took to arrive. Far away that is invisible. Up close, in a
// fight, it is the delay you feel.
//
// So a body near us is drawn where it is NOW: its newest sample carried
// forward, at the velocity its two newest samples imply, to the server tick our
// own prediction is on -- or, if our blast has shoved it, carried by that shove
// instead (render/knock.js), which already knows where it goes. A body far away
// keeps the interpolated picture, and between the two the weight fades with
// distance, so nothing pops as it walks in or out of range.
//
// A guess about where another thing is going is sometimes wrong -- a monster
// turns, stops, is blocked. When the next sample says so, the jump between the
// old guess and the new one is not shown: it is kept as an error that decays
// over a few frames, so the body slides onto the truth instead of snapping.
// And the guess never reaches further ahead than MAX_AHEAD ticks.
//
// Display only. What this client collides with and shoots at stays the
// snapshot's (the mirrored bodies in its world); a shot at a body drawn at the
// present asks the server to judge it as now -- see main.js drawnTick.
//
export const NEAR = 10          // units: drawn fully at the present inside this
export const FAR = 20           // units: fully interpolated beyond this
export const MAX_AHEAD = 12     // ticks: the furthest a guess reaches
export const ERROR_RATE = 12    // 1/s: how fast a corrected guess settles
const MAX_SPEED = 60            // units/s: faster than this between samples is a teleport

/** How much of the present a body at `distance` is drawn with. */
export function presentWeight (distance) {
  return Math.max(0, Math.min(1, (FAR - distance) / (FAR - NEAR)))
}

export class NearPresent {
  constructor () {
    this.rec = new Map()        // key -> { tick, x, y, z, vx, vy, vz, at, ex, ey, ez }
  }

  /**
   * Where the present puts a body, in three-space: `samples` are its
   * snapshot samples ({ tick, x, y, z }, three-space), `now` the server tick
   * our prediction is on, `dt` the frame's seconds. `shoved(fromTick)`, when
   * given, is the knock layer's offset from a sample at `fromTick` to now, in
   * three-space -- used instead of the velocity guess while it lasts.
   * Null with fewer than one sample.
   */
  present (key, samples, now, dt, shoved = null) {
    const n = samples.at(-1)
    if (!n) return null
    let m = null
    for (let i = samples.length - 2; i >= 0; i--) {
      if (samples[i].tick < n.tick) { m = samples[i]; break }
    }
    let vx = 0
    let vy = 0
    let vz = 0
    if (m) {
      const span = n.tick - m.tick
      vx = (n.x - m.x) / span
      vy = (n.y - m.y) / span
      vz = (n.z - m.z) / span
      if (n.jump || Math.hypot(vx, vy, vz) * 60 > MAX_SPEED) { vx = 0; vy = 0; vz = 0 }
    }
    const ahead = Math.max(0, Math.min(MAX_AHEAD, now - n.tick))
    const k = shoved ? shoved(n.tick) : null
    // Shoved: the knock layer carries it, and the velocity the samples show is
    // that same shove -- counted once, not twice.
    const target = k
      ? { x: n.x + k.x, y: n.y + k.y, z: n.z + k.z }
      : { x: n.x + vx * ahead, y: n.y + vy * ahead, z: n.z + vz * ahead }
    if (k) { vx = 0; vy = 0; vz = 0 }

    let r = this.rec.get(key)
    if (!r) {
      r = { tick: n.tick, x: target.x, y: target.y, z: target.z, vx, vy, vz, at: now, ex: 0, ey: 0, ez: 0 }
      this.rec.set(key, r)
    } else if (r.tick !== n.tick) {
      if (n.jump) {
        // A TELEPORT (ent_field::TELEPORTED): there, at once. Settling the
        // difference as an error would slide it across the room in a quarter
        // of a second -- the one thing a teleport must not look like.
        r.ex = 0; r.ey = 0; r.ez = 0
      } else {
        // A NEW SAMPLE: what the old guess says for now, against the new one.
        // The difference is kept as an error that settles, not shown as a jump.
        const step = now - r.at
        const was = { x: r.x + r.vx * step, y: r.y + r.vy * step, z: r.z + r.vz * step }
        r.ex += was.x - target.x
        r.ey += was.y - target.y
        r.ez += was.z - target.z
      }
      r.tick = n.tick
    }
    r.x = target.x; r.y = target.y; r.z = target.z
    r.vx = vx; r.vy = vy; r.vz = vz
    r.at = now
    const decay = Math.exp(-ERROR_RATE * dt)
    r.ex *= decay; r.ey *= decay; r.ez *= decay
    return { x: target.x + r.ex, y: target.y + r.ey, z: target.z + r.ez }
  }

  /** Forget what is no longer drawn. */
  keep (keys) {
    for (const k of this.rec.keys()) if (!keys.has(k)) this.rec.delete(k)
  }

  clear () { this.rec.clear() }
}
