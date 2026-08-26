//
// The isometric camera (design §4.1).
//
// Native and client-side. Lua configures it and triggers shakes; it never
// implements the motion, for the same reason the physics core does not let Lua
// implement movement -- except the stake here is feel rather than determinism,
// so the rule is softer and the reason is different: camera motion has to run
// at the display's frame rate, and server Lua runs at the tick rate.
//
// THE ONE RULE THAT MATTERS: the camera follows the PREDICTED position, never
// the position out of a snapshot.
//
// Snapshots arrive at half the tick rate and over a lossy channel, so a camera
// tracking them moves in visible steps and stalls whenever a datagram is
// dropped -- and it does that even when prediction is perfect and the character
// on screen is moving smoothly. The result is a camera that judders around a
// character that does not, which reads as a rendering bug and is really a
// question of which number was sampled. followsSnapshotStutter() below
// demonstrates the difference rather than asserting the rule, because the rule
// lives at the call site and no amount of code in this file can enforce it.
//

// A deterministic value noise, so a shake is reproducible in a test. Nothing
// here needs to match the server -- shake is cosmetic and client-local -- but
// "reproducible" and "random-looking" are not in tension, and choosing
// Math.random would have traded a testable component for nothing.
function hashNoise (seed, n) {
  let h = (seed ^ (n * 0x9E3779B1)) >>> 0
  h = Math.imul(h ^ (h >>> 15), 0x85EBCA6B) >>> 0
  h = Math.imul(h ^ (h >>> 13), 0xC2B2AE35) >>> 0
  h = (h ^ (h >>> 16)) >>> 0
  return (h / 0xFFFFFFFF) * 2 - 1
}

export const DEFAULTS = {
  // Half-extents of the box the subject may move inside before the camera
  // follows at all. Without one the camera answers every footstep and the whole
  // world sways under a walking character.
  deadzoneX: 2.5,
  deadzoneY: 1.5,

  // Exponential approach rate, per second. Applied as 1 - exp(-rate * dt) so
  // the result does not depend on the frame rate -- a plain `lerp(a, b, 0.1)`
  // per frame smooths twice as fast at 120 Hz as at 60, which is how camera
  // feel ends up varying by machine.
  smoothing: 8,

  // Where the camera sits relative to its target, in three-space. The
  // isometric angle: equal x and z with a greater height.
  offset: { x: 24, y: 30, z: 24 }
}

export class Camera {
  constructor (opts = {}) {
    this.cfg = { ...DEFAULTS, ...opts }
    // The point the camera is trying to look at. Separate from the subject's
    // position, which is the whole content of the deadzone idea.
    this.target = { x: 0, y: 0, z: 0 }
    this.position = { x: 0, y: 0, z: 0 }
    this.shake = { intensity: 0, remaining: 0, duration: 0, seed: 1, frame: 0 }
    this._started = false
  }

  /** Jump straight to the subject, with no smoothing. Used on spawn. */
  snapTo (p) {
    this.target = { ...p }
    this.position = {
      x: p.x + this.cfg.offset.x,
      y: p.y + this.cfg.offset.y,
      z: p.z + this.cfg.offset.z
    }
    this._started = true
  }

  /**
   * Advance one rendered frame.
   *
   * `subject` is the predicted position in three-space; `dt` is the real
   * elapsed time in seconds, NOT a fixed tick. This is the render half of the
   * client and it is explicitly allowed to be frame-rate dependent -- which is
   * exactly why the smoothing is written to be frame-rate INdependent, so the
   * one thing a player would notice is not.
   */
  update (subject, dt) {
    if (!this._started) { this.snapTo(subject); return this.eye() }

    // Deadzone, on the horizontal plane only. Vertical is separate because a
    // platformer's subject leaves the ground constantly and a camera that
    // chased every jump would never be still.
    const dx = subject.x - this.target.x
    const dz = subject.z - this.target.z
    const dy = subject.y - this.target.y

    if (dx > this.cfg.deadzoneX) this.target.x = subject.x - this.cfg.deadzoneX
    else if (dx < -this.cfg.deadzoneX) this.target.x = subject.x + this.cfg.deadzoneX
    if (dz > this.cfg.deadzoneX) this.target.z = subject.z - this.cfg.deadzoneX
    else if (dz < -this.cfg.deadzoneX) this.target.z = subject.z + this.cfg.deadzoneX
    if (dy > this.cfg.deadzoneY) this.target.y = subject.y - this.cfg.deadzoneY
    else if (dy < -this.cfg.deadzoneY) this.target.y = subject.y + this.cfg.deadzoneY

    // Frame-rate independent exponential approach.
    const k = 1 - Math.exp(-this.cfg.smoothing * dt)
    const want = {
      x: this.target.x + this.cfg.offset.x,
      y: this.target.y + this.cfg.offset.y,
      z: this.target.z + this.cfg.offset.z
    }
    this.position.x += (want.x - this.position.x) * k
    this.position.y += (want.y - this.position.y) * k
    this.position.z += (want.z - this.position.z) * k

    if (this.shake.remaining > 0) {
      this.shake.remaining = Math.max(0, this.shake.remaining - dt)
      this.shake.frame++
    }

    return this.eye()
  }

  /**
   * Start a shake (design §4.1: `screen_shake(intensity, duration)`).
   *
   * A new shake REPLACES the current one rather than adding to it. Two
   * explosions a frame apart should not shake twice as hard as one -- and more
   * to the point, an engine that summed them would let a script with a loop
   * throw the camera off the map.
   */
  screenShake (intensity, duration, seed = (this.shake.seed + 1) | 0) {
    if (duration <= 0 || intensity <= 0) return
    this.shake = { intensity, duration, remaining: duration, seed, frame: 0 }
  }

  /** Current shake displacement. Decays linearly to nothing. */
  shakeOffset () {
    const s = this.shake
    if (s.remaining <= 0) return { x: 0, y: 0, z: 0 }
    const amount = s.intensity * (s.remaining / s.duration)
    return {
      x: hashNoise(s.seed, s.frame * 3) * amount,
      y: hashNoise(s.seed, s.frame * 3 + 1) * amount,
      z: hashNoise(s.seed, s.frame * 3 + 2) * amount
    }
  }

  /** Where to put the camera this frame, shake included. */
  eye () {
    const s = this.shakeOffset()
    return {
      x: this.position.x + s.x,
      y: this.position.y + s.y,
      z: this.position.z + s.z,
      // The look-at point shakes WITH the eye, so a shake translates the view
      // instead of swinging it. Rotating the view around a fixed target turns a
      // rumble into a lurch, and at any real intensity it is nauseating.
      lookAt: {
        x: this.target.x + s.x,
        y: this.target.y + s.y,
        z: this.target.z + s.z
      }
    }
  }
}

// --- screen-relative movement ------------------------------------------------
//
// Where the camera sits, and which way the stick points, are two halves of one
// fact and they must be derived from the same yaw or they drift apart. Both
// live here so a test can assert the relationship between them rather than
// re-stating either formula.
//
// The engine is z-up (x east, y north, z up); Three.js is y-up with its z equal
// to the NEGATED engine y. Losing that negation is what makes movement rotate
// opposite to the camera -- which is a reflection, not an offset, so it does
// not look like a lag that could be tuned out.

/** Camera offset from the subject, in THREE space. */
export function cameraOffset (yaw, radius = 24, height = 30) {
  return { x: Math.cos(yaw) * radius, y: height, z: Math.sin(yaw) * radius }
}

/** The same offset expressed in ENGINE space (x, y horizontal; z up). */
export function cameraOffsetEngine (yaw, radius = 24, height = 30) {
  const o = cameraOffset(yaw, radius, height)
  return { x: o.x, y: -o.z, z: o.y }
}

/**
 * Turn a stick reading into an engine-space direction.
 *
 * `my` is "away from the camera", `mx` is "camera right". At yaw 0 the camera
 * is due east of the subject, so away-from-camera is west and camera-right is
 * north -- which is just "facing west, north is on your right".
 */
export function screenRelativeMove (mx, my, yaw) {
  const c = Math.cos(yaw)
  const s = Math.sin(yaw)
  return { x: mx * s - my * c, y: mx * c + my * s }
}
