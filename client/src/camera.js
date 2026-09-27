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
import { easeToward } from './render/animstate.js'

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

  // Where the camera sits relative to its target, in three-space. Replaced
  // every frame by the caller as yaw and zoom change; this is only where it
  // starts. See cameraOffset() for how the direction is fixed and only the
  // distance varies.
  offset: { x: 24, y: 30, z: 24 },

  // How far IN FRONT of the subject the camera aims, in world units.
  //
  // Zero keeps the old behaviour -- the camera centres on the body. Non-zero
  // aims at a point ahead of where the agent is facing, which is what a player
  // is actually looking at: the ground they are about to walk into rather than
  // the ground they are standing on. It is a distance rather than a fraction of
  // velocity so that turning on the spot moves the view, which is how you look
  // around without moving.
  lead: 0,

  // How fast the aim point catches up with a change of facing, per second.
  // Slower than the camera's own follow, deliberately: snapping the lead point
  // the instant somebody turns makes the whole view lurch on every keypress.
  leadSmoothing: 4,

  // Maximum change in the camera's velocity, in units per second squared.
  //
  // Zero means unlimited, which is the exponential approach on its own. A
  // finite value gives the camera weight: it takes time to get moving and time
  // to stop, so a subject that teleports or is knocked back does not drag the
  // view with it instantly. This is the "acceleration" half of a camera that
  // has a speed.
  maxAccel: 0,

  // --- camera paths (design §4.1) -------------------------------------------

  // Which of CAM_MODE the camera is in. FOLLOW is everything above and nothing
  // below; the rest need a path handed to update().
  mode: 0,

  // World units per second along the CURVE, for FLYTHROUGH. Signed: a negative
  // speed rides backwards and looks backwards.
  pathSpeed: 0,

  // How near the subject must come for AUTO to grab the path, in world units.
  // Zero disables attaching, which makes AUTO behave as FOLLOW.
  pathAttach: 0,

  // Rate per second of the crossfade between the free camera and the path.
  // Applied as 1 - exp(-rate * dt), like `smoothing`.
  pathBlend: 3,

  // Ceiling on how fast the camera may SLIDE ALONG a path it is tracking, in
  // units of arc per second. Without one, a subject that teleports -- or simply
  // rounds a corner into view of a far part of the path -- moves the camera the
  // length of the path in a single frame.
  pathMaxRate: 40
}

// What the camera is doing with a path.
//
// FOLLOW is the camera this file has always been: a smoothed orbit around the
// subject. The other three all keep that camera running underneath -- see
// update() -- and differ only in what they do with a path on top.
export const CAM_MODE = {
  FOLLOW: 0,      // no path
  TRACK: 1,       // position from the path, aim at the subject
  FLYTHROUGH: 2,  // position AND aim from the path, at pathSpeed
  AUTO: 3         // TRACK while the subject is near the path, FOLLOW otherwise
}

// AUTO detaches at this multiple of the radius it attaches at.
//
// Hysteresis is not a refinement here. A subject walking along the attach
// boundary crosses it many times a second, and a camera that attached and
// detached on the same number would flicker between two quite different places.
const DETACH_RATIO = 1.5

// How far ahead of itself a FLYTHROUGH looks when `lead` is not set. Only a
// default: what matters is that it looks along the tangent rather than at the
// subject, which is the whole difference between a flythrough and a track.
const FLY_LOOKAHEAD = 8

// The angle the world is viewed from, measured up from the horizontal.
//
// atan2(30, 24): the elevation the original fixed offset described. Kept as
// that exact angle rather than rounded to a nicer one, so that switching
// between an orthographic and a perspective projection changes the PROJECTION
// and nothing else -- a camera that also moved would make the two impossible
// to compare.
export const ISO_ELEVATION = Math.atan2(30, 24)

// hypot(24, 30): the old offset was a horizontal radius of 24 at a height of
// 30 whatever the yaw -- three components, only two of them independent.
export const DEFAULT_DISTANCE = Math.hypot(24, 30)

export class Camera {
  constructor (opts = {}) {
    this.cfg = { ...DEFAULTS, ...opts }
    // The point the camera is trying to look at. Separate from the subject's
    // position, which is the whole content of the deadzone idea.
    this.target = { x: 0, y: 0, z: 0 }
    this.position = { x: 0, y: 0, z: 0 }
    this.shake = { intensity: 0, remaining: 0, duration: 0, seed: 1, frame: 0 }
    // The camera's own velocity, for acceleration limiting, and the aim point's
    // current lead offset, which eases toward where the subject faces.
    this.velocity = { x: 0, y: 0, z: 0 }
    this.lead = { x: 0, z: 0 }
    this._started = false

    // --- path state ---
    //
    // `pathMix` is the whole anti-pop mechanism. The free solution above keeps
    // running every frame even while the camera is on a path, and eye() blends
    // between the two -- so a detach has somewhere continuous to go, and the
    // spring is never perturbed by the path having been there.
    this.pathAlong = 0
    this.pathMix = 0
    this.pathAttached = false
    this.pathClamped = false
    this._pathEye = null
    this._pathLook = null
    this._lastMode = CAM_MODE.FOLLOW
    this._lastPathId = null
  }

  /** Jump straight to the subject, with no smoothing. Used on spawn. */
  snapTo (p) {
    this.target = { ...p }
    this.velocity = { x: 0, y: 0, z: 0 }
    this.lead = { x: 0, z: 0 }
    this.position = {
      x: p.x + this.cfg.offset.x,
      y: p.y + this.cfg.offset.y,
      z: p.z + this.cfg.offset.z
    }
    // A snap is for a subject that TELEPORTED -- spawn, a map change, a void
    // respawn -- so any path the camera was riding is about the old place.
    // Note what this is NOT for: a detach. There the subject has not moved and
    // both candidate eye positions are valid, so cutting is a visible bug and
    // the crossfade is the answer. See the demonstration in camera.test.js.
    this.pathMix = 0
    this.pathAlong = 0
    this.pathAttached = false
    this.pathClamped = false
    this._pathEye = null
    this._pathLook = null
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
  update (subject, dt, facing = null, path = null) {
    if (!this._started) { this.snapTo(subject); return this.eye() }

    // THE POINT THE CAMERA AIMS AT is ahead of the subject, not on it.
    //
    // `facing` is a unit vector in three-space, or null for the old behaviour.
    // The offset eases toward its target rather than snapping, because the
    // subject's facing changes the instant a key is pressed and a view that
    // followed it exactly would lurch on every turn.
    if (facing && this.cfg.lead > 0) {
      const k = 1 - Math.exp(-this.cfg.leadSmoothing * dt)
      this.lead.x += (facing.x * this.cfg.lead - this.lead.x) * k
      this.lead.z += (facing.z * this.cfg.lead - this.lead.z) * k
    } else if (this.cfg.lead <= 0) {
      const k = 1 - Math.exp(-this.cfg.leadSmoothing * dt)
      this.lead.x += (0 - this.lead.x) * k
      this.lead.z += (0 - this.lead.z) * k
    }
    const aim = {
      x: subject.x + this.lead.x,
      y: subject.y,
      z: subject.z + this.lead.z
    }

    // Deadzone, on the horizontal plane only. Vertical is separate because a
    // platformer's subject leaves the ground constantly and a camera that
    // chased every jump would never be still.
    const dx = aim.x - this.target.x
    const dz = aim.z - this.target.z
    const dy = aim.y - this.target.y

    if (dx > this.cfg.deadzoneX) this.target.x = aim.x - this.cfg.deadzoneX
    else if (dx < -this.cfg.deadzoneX) this.target.x = aim.x + this.cfg.deadzoneX
    if (dz > this.cfg.deadzoneX) this.target.z = aim.z - this.cfg.deadzoneX
    else if (dz < -this.cfg.deadzoneX) this.target.z = aim.z + this.cfg.deadzoneX
    if (dy > this.cfg.deadzoneY) this.target.y = aim.y - this.cfg.deadzoneY
    else if (dy < -this.cfg.deadzoneY) this.target.y = aim.y + this.cfg.deadzoneY

    // Frame-rate independent exponential approach.
    const k = 1 - Math.exp(-this.cfg.smoothing * dt)
    const want = {
      x: this.target.x + this.cfg.offset.x,
      y: this.target.y + this.cfg.offset.y,
      z: this.target.z + this.cfg.offset.z
    }
    if (this.cfg.maxAccel > 0 && dt > 0) {
      // A CRITICALLY DAMPED SPRING, with the acceleration clamped.
      //
      // The first version accelerated toward the velocity the exponential
      // approach implied and clamped the CHANGE. That has no deceleration term:
      // the clamp limits slowing down exactly as much as speeding up, so the
      // camera sails past its target and comes back -- 211 units out on a
      // 100-unit move. A spring decelerates because the restoring force falls
      // as it closes and the damping term opposes the velocity it has.
      //
      // `smoothing` is the spring's frequency here rather than an exponential
      // rate. The two agree closely for small displacements, which is why the
      // same number means roughly the same feel either side of the switch.
      const w = this.cfg.smoothing
      const damp = 2 * w
      let ax = (want.x - this.position.x) * w * w - this.velocity.x * damp
      let ay = (want.y - this.position.y) * w * w - this.velocity.y * damp
      let az = (want.z - this.position.z) * w * w - this.velocity.z * damp
      const mag = Math.hypot(ax, ay, az)
      if (mag > this.cfg.maxAccel && mag > 0) {
        const s = this.cfg.maxAccel / mag
        ax *= s; ay *= s; az *= s
      }
      this.velocity.x += ax * dt
      this.velocity.y += ay * dt
      this.velocity.z += az * dt

      // NEVER GOING FASTER THAN IT CAN STOP FROM.
      //
      // A clamped acceleration limits slowing down exactly as much as speeding
      // up, so a long move builds a speed it then needs the same distance again
      // to shed -- 209 units out on a 100-unit move, and back, and out again. A
      // spring does not save it, because the restoring force it would need is
      // the thing being clamped.
      //
      // v = sqrt(2 * a * d) is the speed from which `a` still stops you in `d`,
      // which is the whole content of the problem. Applied as a ceiling on the
      // speed rather than as a separate braking mode, so there is one rule
      // rather than two and no moment where they disagree.
      const rx = want.x - this.position.x
      const ry = want.y - this.position.y
      const rz = want.z - this.position.z
      const dist = Math.hypot(rx, ry, rz)
      const speed = Math.hypot(this.velocity.x, this.velocity.y, this.velocity.z)
      const ceiling = Math.sqrt(2 * this.cfg.maxAccel * dist)
      if (speed > ceiling && speed > 0) {
        const s = ceiling / speed
        this.velocity.x *= s
        this.velocity.y *= s
        this.velocity.z *= s
      }

      this.position.x += this.velocity.x * dt
      this.position.y += this.velocity.y * dt
      this.position.z += this.velocity.z * dt
    } else {
      this.position.x += (want.x - this.position.x) * k
      this.position.y += (want.y - this.position.y) * k
      this.position.z += (want.z - this.position.z) * k
      // Kept current even when unused, so switching the limit on mid-session
      // does not start from a stale velocity.
      this.velocity.x = dt > 0 ? (want.x - this.position.x) * k / dt : 0
      this.velocity.y = dt > 0 ? (want.y - this.position.y) * k / dt : 0
      this.velocity.z = dt > 0 ? (want.z - this.position.z) * k / dt : 0
    }

    // The path solution, computed AFTER the free one and never instead of it.
    this._updatePath(subject, dt, path)

    if (this.shake.remaining > 0) {
      this.shake.remaining = Math.max(0, this.shake.remaining - dt)
      this.shake.frame++
    }

    return this.eye()
  }

  /**
   * Where the path would put the camera, and how much of that to use.
   *
   * Writes `_pathEye`/`_pathLook`/`pathMix` and nothing else. In particular it
   * never touches `this.position` or `this.velocity`: those belong to the free
   * camera, which keeps tracking the subject the whole time the path is in
   * charge. That is what makes a detach continuous rather than a settle-in.
   */
  _updatePath (subject, dt, path) {
    const mode = this.cfg.mode | 0
    const usable = path && path.length > 0 && mode !== CAM_MODE.FOLLOW

    // A ride restarts when the MODE or the PATH changes, and not merely because
    // a CAMERA_SET arrived: re-sending `cam_path_speed` mid-flythrough must not
    // send the camera back to the start.
    //
    // By id, not by object. A path bolted to a moving sector is REBUILT every
    // time its points move, so comparing objects would restart the ride on
    // every frame of the lift's travel. Callers set `id` to the path's tag.
    const id = path ? (path.id !== undefined ? path.id : path) : null
    if (mode !== this._lastMode || id !== this._lastPathId) {
      this.pathAlong = 0
      this.pathClamped = false
      this.pathAttached = false
      this._lastMode = mode
      this._lastPathId = id
    }

    if (!usable) {
      // Any path mode with no path must be FOLLOW exactly -- the safety net for
      // a tag that does not resolve. The mix eases out rather than dropping, so
      // a path disappearing mid-shot is a fade rather than a cut.
      this.pathAttached = false
      this._blend(0, dt)
      return
    }

    let active = false
    if (mode === CAM_MODE.FLYTHROUGH) {
      this.pathAlong += this.cfg.pathSpeed * dt
      const p = path.pointAt(this.pathAlong)
      this.pathClamped = p.clamped
      // Normalise, so a clamped ride does not keep accumulating `along` past
      // the end and then take just as long to come back.
      this.pathAlong = p.clamped
        ? (this.cfg.pathSpeed >= 0 ? path.length : 0)
        : this.pathAlong
      const t = path.tangentAt(this.pathAlong)
      const s = this.cfg.pathSpeed < 0 ? -1 : 1
      const k = this.cfg.lead > 0 ? this.cfg.lead : FLY_LOOKAHEAD
      this._pathEye = { x: p.x, y: p.y, z: p.z }
      this._pathLook = { x: p.x + t.x * k * s, y: p.y + t.y * k * s, z: p.z + t.z * k * s }
      active = true
    } else {
      // TRACK and AUTO. Nearest against the RAW SUBJECT, not the lead point:
      // aiming ahead is about where the camera looks, and feeding it in here
      // would make the camera slide along the path when you turn on the spot.
      const opts = this.pathAttached
        ? { from: this.pathAlong, window: this.cfg.pathMaxRate * dt + 1 }
        : {}
      const n = path.nearest(subject, opts)

      if (mode === CAM_MODE.AUTO && this.cfg.pathAttach > 0) {
        const d = Math.sqrt(n.distSq)
        const r = this.cfg.pathAttach
        active = this.pathAttached ? d <= r * DETACH_RATIO : d <= r
      } else {
        active = mode === CAM_MODE.TRACK
      }

      if (active) {
        if (!this.pathAttached) {
          // First frame on the path: go straight to the nearest point. The mix
          // is still near zero, so the eye has not arrived yet either way, and
          // rate-limiting from a stale `along` would crawl there instead.
          this.pathAlong = n.along
        } else {
          // RATE LIMIT. A subject that teleports, or that rounds a corner into
          // view of a far part of a doubling-back path, otherwise moves the
          // camera the length of the path in one frame.
          let d = n.along - this.pathAlong
          if (path.loop) {
            const L = path.length
            if (d > L / 2) d -= L
            else if (d < -L / 2) d += L
          }
          const lim = this.cfg.pathMaxRate * dt
          if (d > lim) d = lim
          else if (d < -lim) d = -lim
          this.pathAlong += d
        }
        const p = path.pointAt(this.pathAlong)
        this.pathAlong = p.clamped ? this.pathAlong : this.pathAlong
        this._pathEye = { x: p.x, y: p.y, z: p.z }
        // The path gives the POSITION; the subject still gives the aim, through
        // the free solution's deadzoned, lead-offset target. So `lead` and the
        // deadzone keep working on a tracked camera -- they simply stop
        // affecting where it stands, which is the point of putting it on rails.
        this._pathLook = { x: this.target.x, y: this.target.y, z: this.target.z }
      }
    }

    this.pathAttached = active
    this._blend(active ? 1 : 0, dt)
  }

  /**
   * Ease pathMix toward `want`, frame-rate independently.
   *
   * Delegates to easeToward so there is ONE exponential ease in the client
   * rather than two. Deliberately not what the sprite cross-fade uses: that
   * needs a duration, because an exponential never arrives and a cross-fade must
   * terminate so its cloned material can be disposed.
   */
  _blend (want, dt) {
    this.pathMix = easeToward(this.pathMix, want, this.cfg.pathBlend, dt)
    if (this.pathMix < 1e-6) this.pathMix = 0
    if (this.pathMix > 1 - 1e-6) this.pathMix = 1
  }

  /** What the camera is doing with a path, for the HUD and for tests. */
  get pathState () {
    return {
      mode: this.cfg.mode | 0,
      along: this.pathAlong,
      mix: this.pathMix,
      attached: this.pathAttached,
      clamped: this.pathClamped
    }
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

  /** Where to put the camera this frame: free, path, or between the two. */
  eye () {
    const s = this.shakeOffset()
    let ex = this.position.x, ey = this.position.y, ez = this.position.z
    let lx = this.target.x, ly = this.target.y, lz = this.target.z

    // THE ONLY PLACE THE PATH AND THE FREE CAMERA MEET.
    //
    // One crossfade covers all five transitions -- entering a path mode,
    // leaving it, auto-attaching, auto-detaching, and the path going away
    // because a tag stopped resolving. There is no snap anywhere in it, which
    // is deliberate: on every one of those the subject is exactly where it was
    // and both candidate positions are valid, so a cut would be a bug rather
    // than a continuity fix.
    const m = this.pathMix
    if (m > 0 && this._pathEye && this._pathLook) {
      ex += (this._pathEye.x - ex) * m
      ey += (this._pathEye.y - ey) * m
      ez += (this._pathEye.z - ez) * m
      lx += (this._pathLook.x - lx) * m
      ly += (this._pathLook.y - ly) * m
      lz += (this._pathLook.z - lz) * m
    }

    // Shake is added LAST, on top of whatever the blend produced, so a rumble
    // during a flythrough shakes the flythrough rather than dragging it back
    // toward the free camera.
    return {
      x: ex + s.x,
      y: ey + s.y,
      z: ez + s.z,
      // The look-at point shakes WITH the eye, so a shake translates the view
      // instead of swinging it. Rotating the view around a fixed target turns a
      // rumble into a lurch, and at any real intensity it is nauseating.
      lookAt: { x: lx + s.x, y: ly + s.y, z: lz + s.z }
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

/**
 * Camera offset from the subject, in THREE space.
 *
 * Fixed elevation and yaw, scaled by DISTANCE. Only the distance changes when
 * you zoom, so the view angle -- and with it which faces are visible and which
 * way "away from the camera" points -- is identical at every zoom level and
 * under either projection. An orthographic camera ignores the distance for
 * framing and a perspective one does not, which is exactly the difference
 * between them and the only difference there should be.
 */
export function cameraOffset (yaw, distance = DEFAULT_DISTANCE, elevation = ISO_ELEVATION) {
  const horizontal = Math.cos(elevation) * distance
  return {
    x: Math.cos(yaw) * horizontal,
    y: Math.sin(elevation) * distance,
    z: Math.sin(yaw) * horizontal
  }
}

/**
 * How far a perspective camera must sit to frame `viewHeight` world units.
 *
 * Pure trigonometry, and here rather than beside the camera it configures so
 * that it can be tested without pulling in a renderer -- the property it
 * encodes (both projections frame the subject identically) is exactly the sort
 * that should be asserted rather than eyeballed.
 */
export function distanceForFraming (fovDegrees, viewHeight) {
  return (viewHeight / 2) / Math.tan((fovDegrees * Math.PI / 180) / 2)
}

/** The same offset expressed in ENGINE space (x, y horizontal; z up). */
export function cameraOffsetEngine (yaw, distance = DEFAULT_DISTANCE) {
  const o = cameraOffset(yaw, distance)
  return { x: o.x, y: -o.z, z: o.y }
}

/**
 * Turn a stick reading into an engine-space direction.
 *
 * `my` is "away from the camera", `mx` is "camera right". At yaw 0 the camera
 * is due east of the subject, so away-from-camera is west and camera-right is
 * north -- which is just "facing west, north is on your right".
 */
/**
 * The aim to send this tick, given where the stick is and where it last was.
 *
 * `aim` in an input frame is an ABSOLUTE direction, and the engine assigns it
 * to facing every tick -- brad 0 is a real direction (due east), not "no
 * direction". So a client that sends 0 when the stick is centred is not saying
 * "unchanged", it is saying "face east", and the agent snaps east the instant
 * you let go of the keys.
 *
 * Holding the last aim is therefore the CLIENT's job, and it has to be done in
 * the input frame rather than in the engine: the frame is what the server
 * replays and what reconciliation replays, so an engine-side "ignore zero"
 * would make brad 0 unreachable and still leave the two sides agreeing about
 * the wrong thing.
 */
export function aimFor (wx, wy, lastAim) {
  if (wx === 0 && wy === 0) return lastAim >>> 0
  return Math.round((Math.atan2(wy, wx) / (Math.PI * 2)) * 4294967296) >>> 0
}

/**
 * The Y rotation that turns an upright plane to face the camera.
 *
 * A PlaneGeometry lies in the XY plane with its normal along +Z, so after
 * `rotation.y = t` that normal points along `(sin t, 0, cos t)`. The camera sits
 * at `cameraOffset(yaw)`, whose horizontal direction is `(cos yaw, _, sin yaw)`.
 * Equating the two gives `t = pi/2 - yaw`.
 *
 * YAW ONLY, which is what separates this from THREE.Sprite: a Sprite faces the
 * camera in all three axes, and under this camera's elevation that lays a key
 * flat on the floor as you rotate. A Doom sprite stays upright in the world and
 * turns about the vertical.
 *
 * A function with a test rather than a line in the render loop, because the
 * first version had the sign of `yaw` backwards -- which looks entirely
 * plausible until something turns, and is the same failure the engine-to-three
 * direction conversion shipped with once already.
 */
export function billboardYaw (yaw) {
  return Math.PI / 2 - yaw
}

/**
 * An agent's facing, as a unit direction in THREE space.
 *
 * `raw` is a brad: a full turn in 2^32, which is how the engine stores an angle
 * so that it wraps exactly and identically on every target. The engine's own
 * convention is `(cos, sin)` in its x/y plane -- the same one main.js encodes
 * with when it builds an input frame -- and engine (x, y) becomes three
 * (x, -z), so `sin` comes back NEGATED.
 *
 * A pure function with a test rather than three lines inlined into the render
 * loop, because the last time a direction was converted by hand between these
 * two spaces the sign was wrong and the symptom was controls that rotated the
 * opposite way from the camera. A reflection looks entirely plausible until
 * something turns.
 */
export function facingDirection (raw) {
  const theta = (Number(raw >>> 0) / 4294967296) * Math.PI * 2
  return { x: Math.cos(theta), z: -Math.sin(theta) }
}

export function screenRelativeMove (mx, my, yaw) {
  const c = Math.cos(yaw)
  const s = Math.sin(yaw)
  return { x: mx * s - my * c, y: mx * c + my * s }
}
