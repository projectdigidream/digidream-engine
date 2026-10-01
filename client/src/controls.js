//
// CONTROL SCHEMES: how input becomes an input frame.
//
// The module decides which (module.txt `controls fps` or `controls platformer`,
// told to the client in WELCOME); the player does not switch. Two schemes:
//
//   platformer  what the client always did: a SCREEN-space push that
//               camera.js turns into a world one, and an aim the body turns
//               towards at its own rate. Not implemented here -- main.js keeps
//               it exactly as it was.
//   fps         the view IS the aim. Mouse (under pointer lock) or a touch drag
//               turns a yaw and a pitch the client owns; the stick or WASD
//               pushes forward and sideways RELATIVE TO THAT YAW, and the
//               simulation turns the push by the body's facing
//               (MoveParams::move_relative), which the module makes equal to the
//               aim by giving the body an instant turn. So what the client sends
//               is exactly what it predicts with, and nothing about the view has
//               to be reconciled.
//
// Pure: it takes deltas and keys and says what to send. The DOM half (pointer
// lock, listeners) is in main.js, so this can be tested without a browser.
//

export const SCHEME = { PLATFORMER: 0, FPS: 1 }

/** Radians of view per pixel of mouse movement. Doom's default is about this. */
export const MOUSE_SENSITIVITY = 0.0025
/** Radians per pixel of a touch drag: a thumb travels less than a mouse. */
export const TOUCH_SENSITIVITY = 0.006
/** How far up or down the view may look: the simulation's own clamp (80 deg). */
export const MAX_PITCH_RAD = (14563 / 65536) * 2 * Math.PI

const TURN = 2 * Math.PI

export class FpsLook {
  constructor ({ yaw = 0, pitch = 0 } = {}) {
    this.yaw = yaw       // radians, counter-clockwise from +x, the simulation's convention
    this.pitch = pitch   // radians, up is positive
  }

  /** A mouse or touch movement, in pixels (+x right, +y down, as the DOM gives). */
  move (dx, dy, perPixel = MOUSE_SENSITIVITY) {
    // Right turns clockwise, which is a NEGATIVE angle counter-clockwise.
    this.yaw = ((this.yaw - dx * perPixel) % TURN + TURN) % TURN
    this.pitch = Math.max(-MAX_PITCH_RAD, Math.min(MAX_PITCH_RAD, this.pitch - dy * perPixel))
  }

  /** Face where the body faces -- on arrival, so the view starts where the spawn points. */
  face (facingBrad) {
    this.yaw = ((facingBrad >>> 0) / 4294967296) * TURN
  }

  /** The aim, as the wire carries it: a brad. */
  aimBrad () { return Math.floor((this.yaw / TURN) * 4294967296) >>> 0 }

  /** The pitch, as the wire carries it: 1/65536 of a turn, clamped. */
  pitchWire () {
    const v = Math.round((this.pitch / TURN) * 65536)
    return Math.max(-14563, Math.min(14563, v))
  }

  /** The direction the view looks, in the simulation's space (x, y ground, z up). */
  forward () {
    const c = Math.cos(this.pitch)
    return { x: Math.cos(this.yaw) * c, y: Math.sin(this.yaw) * c, z: Math.sin(this.pitch) }
  }
}

/**
 * The push for an FPS scheme: forward and strafe, in -1..1, from what is held.
 *
 * Relative to the view -- y is forward, x is to the right -- and the
 * simulation turns it by the body's facing. A diagonal is normalised so running
 * at a corner is not faster than running straight, which is the bug every
 * first-person game ships once.
 */
export function fpsPush ({ forward = false, back = false, left = false, right = false, stick = null } = {}) {
  let x = 0
  let y = 0
  if (stick && (stick.x !== 0 || stick.y !== 0)) {
    x = stick.x
    y = stick.y
  } else {
    if (forward) y += 1
    if (back) y -= 1
    if (right) x += 1
    if (left) x -= 1
  }
  const len = Math.hypot(x, y)
  if (len > 1) { x /= len; y /= len }
  return { x, y }
}
