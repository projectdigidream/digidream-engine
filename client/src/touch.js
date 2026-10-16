//
// Touch input: a thumbstick, some buttons, and a drag that turns the camera.
//
// WHAT THIS IS AND IS NOT. It is a pure state machine over pointer events. It
// owns no DOM, reads no globals and does no trigonometry the camera already
// does -- client/src/render/touchui.js draws the controls and feeds this, and
// main.js asks it the same two questions it asks the keyboard: which way are
// you pushing, and which buttons are down.
//
// That split is not tidiness. A virtual stick is arithmetic -- an origin, a
// radius, a dead zone, a clamp -- and arithmetic is the half that can be
// checked without a phone in your hand. The half that needs a phone is whether
// the buttons are big enough, and no test was ever going to answer that.
//
// THE CONTRACT WITH THE KEYBOARD. Both produce a SCREEN-SPACE direction in
// -1..1 which main.js hands to camera.js's screenRelativeMove(). Neither knows
// about world axes, and there is exactly one function that turns a screen push
// into a world one. A touch path that did its own rotation would be the second
// implementation this repository keeps paying for -- and it would invert on
// one of them the first time the camera's sign changed.
//
// THE FAILURE THIS IS MOSTLY DESIGNED AROUND. A pointer that goes away without
// a `pointerup` -- the browser cancels it, a phone call arrives, the finger
// slides off the edge of the glass, the tab is hidden -- leaves the stick
// pushed and the player walks into a wall for ever. The keyboard has exactly
// this bug and exactly this fix: `addEventListener('blur', () => held.clear())`
// in main.js. So `cancel()` and `clearAll()` are first-class here, every path
// that can lose a pointer calls one of them, and the test asserts a cancelled
// stick stops the player rather than trusting that it will.
//

/** The half-width, in pixels, of the pad the stick may be pushed to. */
export const STICK_RADIUS = 60

/**
 * How far the thumb must move before it counts.
 *
 * A fraction of the radius, not pixels: the pad scales with the screen. Big
 * enough that a thumb resting on the glass does not walk -- which is the
 * touch version of a key that never came up -- and small enough that a
 * deliberate nudge is not swallowed.
 */
export const STICK_DEADZONE = 0.18

/**
 * Radians of camera yaw per pixel of horizontal drag.
 *
 * Tuned against the keyboard's Q/R, which turn at 1.5 rad/s: a drag across a
 * 400-pixel phone turns about 100 degrees, which is a comfortable flick and
 * about what a second on the keyboard gives you.
 */
export const YAW_PER_PIXEL = 0.0044

/**
 * The push, from where the thumb went down to where it is now.
 *
 * Returns a SCREEN vector in -1..1 with y UP, which is what
 * camera.js's screenRelativeMove wants and the opposite of what the DOM
 * gives you -- a browser's y grows downwards, so pushing your thumb up is a
 * NEGATIVE dy and must come back positive. Getting that backwards is a game
 * that walks away from your thumb, which is the one bug in here that is
 * instantly obvious and was still worth a test.
 *
 * Clamped to the unit circle rather than the unit square: a stick pushed into
 * a corner must not be faster than one pushed straight, and normalising only
 * when the length exceeds 1 is the same rule the keyboard's diagonal uses.
 */
export function stickVector (ox, oy, px, py, radius = STICK_RADIUS, dead = STICK_DEADZONE) {
  const dx = (px - ox) / radius
  const dy = (oy - py) / radius
  const len = Math.hypot(dx, dy)
  if (len <= dead) return { x: 0, y: 0, len: 0 }

  // RESCALED FROM THE EDGE OF THE DEAD ZONE, not just gated by it. Gating
  // alone makes the stick jump from nothing to 18% the moment it engages,
  // which reads as a twitch; this starts from zero at the boundary and
  // reaches full push at the rim either way.
  const scale = Math.min(1, (len - dead) / (1 - dead)) / len
  return { x: dx * scale, y: dy * scale, len: Math.min(1, len) }
}

/**
 * Which control a pointer landing here belongs to.
 *
 * `regions` is a list of `{ id, x, y, w, h }` in page coordinates, nearest the
 * thumb first; anything landing outside all of them is a camera drag. Decided
 * ONCE, when the pointer goes down, and never revisited -- see `move()`.
 */
export function claim (regions, x, y) {
  for (const r of regions) {
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return r.id
  }
  return 'camera'
}

/**
 * Every finger on the glass, and what each of them is doing.
 *
 * One instance per session. Pointer ids are the browser's and are unique while
 * a pointer is down, which is all this needs.
 */
export class TouchState {
  constructor ({ radius = STICK_RADIUS, deadzone = STICK_DEADZONE,
                 yawPerPixel = YAW_PER_PIXEL } = {}) {
    this.radius = radius
    this.deadzone = deadzone
    this.yawPerPixel = yawPerPixel

    /** pointerId -> { kind, ox, oy, x, y } */
    this.pointers = new Map()
    /** The stick's pointer id, or null. */
    this.stick = null
    /** Buttons currently held, by region id. */
    this.pressed = new Set()
    // A TAP IS SEEN AT LEAST ONCE. The game reads the held buttons when it
    // samples input, once a tick at best and once a frame on a slow phone; a
    // tap pressed and let go between two samples was never seen, and FIRE
    // "did nothing" one tap in two. A press no sample has read yet is kept,
    // let go, until one has.
    /** Held buttons a sample has read since they went down. */
    this.read = new Set()
    /** Buttons let go before any sample read them: read once more, then gone. */
    this.unread = new Set()

    // Accumulated since the last time the frame loop took them. Deltas rather
    // than absolutes because the camera owns its own yaw and zoom and this only
    // ever nudges them -- the same shape as a wheel event.
    this.yawDelta = 0
    this.zoomDelta = 0
    // The same drag as raw PIXELS, both axes: what a first-person scheme turns
    // into yaw and pitch at its own sensitivity (client/src/controls.js).
    this.dragDx = 0
    this.dragDy = 0
    /** Distance between the two camera pointers on the previous move, or null. */
    this.pinch = null
  }

  /** Is anybody touching anything? Used to decide whether to draw the pad. */
  get active () { return this.pointers.size > 0 }

  /** Where the stick's pad should be drawn, or null when it is not held. */
  get stickOrigin () {
    const p = this.stick === null ? null : this.pointers.get(this.stick)
    return p ? { x: p.ox, y: p.oy } : null
  }

  down (id, x, y, regions) {
    const kind = claim(regions, x, y)

    // THE ORIGIN IS WHERE YOUR THUMB LANDED, not the middle of a painted
    // circle. A fixed pad means looking down to find it; a floating one means
    // the stick is wherever you happened to put your thumb, and the drawn pad
    // moves to meet it. The zone it has to land in is still fixed, so the
    // bottom-left of the screen is always the stick and nothing else.
    this.pointers.set(id, { kind, ox: x, oy: y, x, y })

    if (kind === 'stick') {
      // A second thumb in the stick zone does not steal it. Two fingers in one
      // corner is a fumble, not an instruction, and letting the newer one win
      // makes the player's direction jump to wherever the fumble landed.
      if (this.stick === null) this.stick = id
      else this.pointers.get(id).kind = 'ignored'
    } else if (kind !== 'camera') {
      this.pressed.add(kind)
      this.read.delete(kind)
    }
  }

  move (id, x, y) {
    const p = this.pointers.get(id)
    if (!p) return
    const wasX = p.x
    const wasY = p.y
    p.x = x
    p.y = y

    // A BUTTON STAYS PRESSED WHILE THE THUMB IS DOWN, even if it slides off.
    // The alternative -- releasing on leave -- makes a jump you were holding
    // stop because your thumb drifted two pixels, which on a phone is every
    // jump. The pointer decides what it is when it lands and nothing changes
    // it afterwards; `up` is the only thing that releases.

    if (p.kind === 'camera') {
      const cams = [...this.pointers.values()].filter((q) => q.kind === 'camera')
      if (cams.length >= 2) {
        // TWO FINGERS ARE A PINCH, and while pinching neither of them turns
        // the camera. Spreading your fingers apart moves them in opposite
        // directions, so treating each as a drag would yaw by the difference
        // and the world would lurch every time you zoomed.
        const d = Math.hypot(cams[0].x - cams[1].x, cams[0].y - cams[1].y)
        if (this.pinch !== null) this.zoomDelta += this.pinch - d
        this.pinch = d
        return
      }
      this.pinch = null
      this.yawDelta += (x - wasX) * this.yawPerPixel
      this.dragDx += x - wasX
      this.dragDy += y - wasY
    }
  }

  up (id) {
    const p = this.pointers.get(id)
    if (!p) return
    this.pointers.delete(id)
    if (this.stick === id) this.stick = null
    if (p.kind !== 'camera' && p.kind !== 'stick' && p.kind !== 'ignored') {
      this.pressed.delete(p.kind)
      if (!this.read.has(p.kind)) this.unread.add(p.kind)
    }
    // The pinch is over the moment either finger leaves. Keeping the distance
    // would make the next single-finger drag zoom by the gap between them.
    if ([...this.pointers.values()].filter((q) => q.kind === 'camera').length < 2) {
      this.pinch = null
    }
  }

  /** A pointer the browser took away. Identical to a release, and must be. */
  cancel (id) { this.up(id) }

  /**
   * Everything up, now.
   *
   * For blur, visibilitychange, and losing the connection. The keyboard has
   * exactly this and for exactly this reason: an input that was never released
   * is held for ever, and on a phone the ways to lose a pointer without a
   * `pointerup` are many -- a call, a notification pulled down, the app
   * backgrounded mid-jump.
   */
  clearAll () {
    this.pointers.clear()
    this.pressed.clear()
    this.unread.clear()
    this.stick = null
    this.pinch = null
  }

  /** The stick's push, as a screen vector with y up. Zero when not held. */
  move2d () {
    const p = this.stick === null ? null : this.pointers.get(this.stick)
    if (!p) return { x: 0, y: 0, len: 0 }
    return stickVector(p.ox, p.oy, p.x, p.y, this.radius, this.deadzone)
  }

  /** The OR of every held button's bit, given a region id -> bit table. */
  buttons (bits) {
    let out = 0
    for (const id of this.pressed) if (bits[id] !== undefined) { out |= bits[id]; this.read.add(id) }
    // A tap let go before this: seen now, by the reader its bit belongs to.
    for (const id of this.unread) if (bits[id] !== undefined) { out |= bits[id]; this.unread.delete(id) }
    return out
  }

  /**
   * Yaw accumulated since the last call, and zero it.
   *
   * TAKEN, not read. The frame loop applies a delta once and the next frame
   * must not apply it again -- a peek-and-clear split would be two calls with
   * a chance to forget the second, and forgetting it spins the camera.
   */
  takeYaw () { const y = this.yawDelta; this.yawDelta = 0; return y }

  /** The camera drag since the last call, in pixels ({dx, dy}, DOM axes), and zero it. */
  takeDrag () {
    const d = { dx: this.dragDx, dy: this.dragDy }
    this.dragDx = 0
    this.dragDy = 0
    return d
  }

  /** Pinch accumulated since the last call, in pixels, and zero it. */
  takeZoom () { const z = this.zoomDelta; this.zoomDelta = 0; return z }
}

/**
 * Does this device want touch controls at all?
 *
 * ASKED OF THE INPUT, not of the user agent string. A tablet with a keyboard,
 * a laptop with a touchscreen and a phone all answer this differently from how
 * their names suggest, and a sniffed name is wrong for whatever shipped last
 * month. `pointer: coarse` is the browser's own answer to "is the primary
 * pointer a finger", which is the actual question.
 *
 * Overridable both ways, because the honest default is still a guess: `?touch=1`
 * forces the controls on -- which is how they are looked at on a desktop -- and
 * `?touch=0` forces them off for a touchscreen laptop where they would be in
 * the way.
 */
export function wantsTouch (params, matchMedia = globalThis.matchMedia) {
  const forced = params?.get?.('touch')
  if (forced === '1') return true
  if (forced === '0') return false
  try {
    return !!matchMedia?.('(pointer: coarse)')?.matches
  } catch {
    // A browser without matchMedia is not a phone. Guessing "yes" here would
    // put a thumbstick over a desktop game that has a keyboard.
    return false
  }
}
