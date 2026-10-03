//
// OUR BLAST'S SHOVE OF THE BODIES AROUND IT, shown when it happens.
//
// A monster hit by our rocket moves on the server in the step the rocket
// bursts in -- and reached this screen a snapshot and an interpolation delay
// later, so a rocket into a crowd burst, and the crowd flew a beat after. Our
// ghost rocket (render/ghostmissiles.js) bursts on the predicted tick; this
// moves the bodies with it.
//
// THE SHOVE is the server's rule (sim/blast.hpp, through the wasm world): the
// same reach, falloff, direction and mass. WHAT FOLLOWS is mobs.lua's apply_hit,
// approximated: the body slides, braking at KNOCK_DECEL while the shove lasts,
// and lifts and falls at the world's gravity. A path, in world units per tick.
//
// HOW IT IS DRAWN, without moving anything twice: the remote body is drawn at
// a render tick R in the past, and the server's own shove reaches R a while
// after it happened. So the body is drawn at
//
//     where the server stream has it at R  +  path(now - S) - path(R - S)
//
// for a shove at server tick S. Before the server's shove reaches R the second
// term is zero and the body is ahead by the prediction; as R passes S the
// stream carries the shove itself and the difference closes; once both are
// past the end of the path it is zero. A shove the server did not make (a
// monster that had moved) shows for the length of the path and no longer.
//
// Not collided with walls: a monster blown into one is pulled back by the
// stream when R gets there. Display only -- nothing here touches the world.
//
export const KNOCK_DECEL = 10          // mobs.lua KNOCK_DECEL, units/s^2
export const KNOCK_MIN = 0.5           // mobs.lua KNOCK_MIN: below it, no slide
export const KNOCK_MAX_TICKS = 90      // mobs.lua KNOCK_MAX_TICKS
export const GRAVITY = 16              // sim/constants.hpp GRAVITY, units/s^2
const DT = 1 / 60

/**
 * The displacement a shove of `v` (units/s; z up) makes, tick by tick from
 * the step it lands in: an array of {x, y, z}, entry 0 after that step.
 */
export function knockPath (v) {
  const speed = Math.hypot(v.x, v.y)
  const slide = speed > KNOCK_MIN ? Math.min(KNOCK_MAX_TICKS, Math.ceil(speed / KNOCK_DECEL * 60)) : 0
  const ux = speed > 0 ? v.x / speed : 0
  const uy = speed > 0 ? v.y / speed : 0
  let s = speed
  let vz = v.z
  let x = 0
  let y = 0
  let z = 0
  const out = []
  const n = Math.max(slide, vz > 0 ? Math.ceil(2 * vz / GRAVITY * 60) + 2 : 0, 1)
  for (let t = 0; t < n; t++) {
    x += ux * s * DT
    y += uy * s * DT
    if (z > 0 || vz > 0) {
      z += vz * DT
      vz -= GRAVITY * DT
      if (z <= 0) { z = 0; vz = 0 }
    }
    // Braking on the ground while the shove lasts; in the air it carries.
    if (z === 0) s = t < slide ? Math.max(0, s - KNOCK_DECEL * DT) : 0
    out.push({ x, y, z })
  }
  return out
}

const at = (path, t) => {
  if (t < 0 || path.length === 0) return null
  const i = Math.floor(t)
  if (i >= path.length - 1) return path[path.length - 1]
  const a = path[i]
  const b = path[i + 1]
  const f = t - i
  return { x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f, z: a.z + (b.z - a.z) * f }
}

export class KnockLayer {
  constructor () {
    this.shoves = []          // { key, at, path }
  }

  /**
   * A predicted shove of entity `key` (index/generation) at server tick
   * `atTick`: a velocity `v` (units/s, z up), followed with knockPath, or a
   * path already worked out -- the core's controller on the map, which is what
   * main.js passes (WasmWorld.knockPath).
   */
  add (key, atTick, v, path = null) {
    this.shoves.push({ key, at: atTick, path: path ?? knockPath(v) })
  }

  /**
   * How long the shove slides for, as apply_hit decides it: the horizontal
   * speed over KNOCK_DECEL, capped -- and none at all below KNOCK_MIN.
   */
  static slideTicks (v) {
    const speed = Math.hypot(v.x, v.y)
    return speed > KNOCK_MIN ? Math.min(KNOCK_MAX_TICKS, Math.ceil(speed / KNOCK_DECEL * 60)) : 0
  }

  /**
   * How far from the stream to draw entity `key`: in world units, for a
   * present at server tick `now` and a body drawn at `drawn`. Null if none.
   */
  offset (key, now, drawn) {
    let o = null
    for (const s of this.shoves) {
      if (s.key !== key) continue
      const a = at(s.path, now - s.at)
      if (a === null) continue
      const b = at(s.path, drawn - s.at) ?? { x: 0, y: 0, z: 0 }
      o = o ?? { x: 0, y: 0, z: 0 }
      o.x += a.x - b.x
      o.y += a.y - b.y
      o.z += a.z - b.z
    }
    return o
  }

  /** Forget the shoves the drawn stream is past. */
  prune (drawn) {
    this.shoves = this.shoves.filter((s) => drawn - s.at <= s.path.length + 2)
  }

  clear () { this.shoves = [] }
}

const toFx = (n) => BigInt(Math.round(n * 4294967296))
const fromFx = (v) => Number(v) / 4294967296

/**
 * What our blast does to one body: the shove by the server's rule (sim/blast.hpp)
 * and where the core's controller takes it on this world's map, in one call --
 * for main.js and for the test that compares it with the server. `pos` is the
 * body's fx position, `body` its { radius, height, speed } in units. Null when
 * the blast does not reach it.
 */
export function shoveOf (world, impact, pos, blast, mass, body, ticks = 120) {
  const v = world.blastShove(impact, pos, blast, mass)
  if (!v) return null
  const vf = { x: fromFx(v.x), y: fromFx(v.y), z: fromFx(v.z) }
  const path = world.knockPath(pos, v,
    { radius: toFx(body.radius), height: toFx(body.height), speed: body.speed ? toFx(body.speed) : 0n },
    toFx(KNOCK_DECEL), KnockLayer.slideTicks(vf), ticks)
  return { v: vf, path }
}
