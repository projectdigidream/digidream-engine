//
// THE FIRST-PERSON WEAPON: the gun and the hands holding it, over the view.
//
// Doom draws a weapon as a picture on its 320x200 screen, placed by the
// picture's own offsets, and animates it through a fixed list of frames at 35
// tics a second -- PISGB0 for six tics with the muzzle flash PISFA0 over it,
// then PISGC0, and so on. This does the same, from data
// (assets/sprites/weapons.json, a module's), over the 3D view as a DOM layer:
// pictures, not geometry, so they never clip into a wall and never take part in
// the scene's lighting or depth.
//
// WHICH WEAPON is the server's (hud.weapon, from the weapons runtime). WHEN IT
// FIRES is the client's own prediction -- the moment this client predicts the
// shot (audio/ownsounds.js), so the gun kicks on the press and not a round trip
// later.
//
// The arithmetic is pure and exported; the DOM half is a thin class over it.
//

import { BASE } from '../base.js'
export const TIC = 1 / 35

/**
 * Which frame of a list is showing `t` seconds into it, or null past the end.
 * `loop` wraps instead of ending.
 */
export function frameAt (frames, t, loop = false) {
  if (!frames || frames.length === 0) return null
  const total = frames.reduce((s, f) => s + (f[1] ?? 1), 0) * TIC
  if (total <= 0) return frames[0]
  let at = t
  if (loop) at = ((t % total) + total) % total
  else if (at >= total) return null
  for (const f of frames) {
    const d = (f[1] ?? 1) * TIC
    if (at < d) return f
    at -= d
  }
  return frames[frames.length - 1]
}

/**
 * Where a picture goes on the real screen: Doom's 320x200, scaled by height and
 * centred, the picture's top-left at (sx - left, sy - top) in Doom's pixels.
 */
export function place ({ width, height }, size, offset, sx, sy, screen = [320, 200]) {
  const s = height / screen[1]
  const x0 = (width - screen[0] * s) / 2
  return {
    left: x0 + (sx - offset[0]) * s,
    top: (sy - offset[1]) * s,
    width: size[0] * s,
    height: size[1] * s
  }
}

/**
 * A weapon's `anims` (docs/VIEWMODEL_PHASES.md): animation state number ->
 * { frames, loop }. A value is a frame list, or { frames, loop }; `loop`
 * defaults to true for state 0 and false otherwise. A bad key or a frame list
 * of the wrong shape is reported in `errors` and left out; the rest is used.
 */
export function parseAnims (anims, where, errors) {
  const out = new Map()
  if (anims === undefined) return out
  if (!anims || typeof anims !== 'object' || Array.isArray(anims)) { errors.push(`${where}: anims is not an object`); return out }
  const frameOk = (f) => Array.isArray(f) && typeof f[0] === 'string' && f[0] !== '' &&
    (f[1] === undefined || (typeof f[1] === 'number' && f[1] >= 0)) && (f[2] === undefined || typeof f[2] === 'string')
  for (const [key, v] of Object.entries(anims)) {
    if (!/^(0|[1-9][0-9]*)$/.test(key) || Number(key) > 65535) { errors.push(`${where}.anims: '${key}' is not an animation state (0..65535)`); continue }
    const state = Number(key)
    const frames = Array.isArray(v) ? v : v?.frames
    const loop = Array.isArray(v) || v?.loop === undefined ? state === 0 : v.loop
    if (!Array.isArray(frames) || frames.length === 0 || !frames.every(frameOk)) {
      errors.push(`${where}.anims["${key}"]: a list of [picture, tics(, flash)] frames, or { frames, loop }`)
      continue
    }
    if (typeof loop !== 'boolean') { errors.push(`${where}.anims["${key}"]: loop is true or false`); continue }
    out.set(state, { frames, loop })
  }
  return out
}

// Doom's numbers: the weapon at rest sits at sy 32 (WEAPONTOP); lowered, at 128.
const TOP = 32
const BOTTOM = 128
const SWITCH_SECONDS = 0.25   // down, then up: Doom lowers at 6 px a tic

export class ViewModel {
  /**
   * @param host   the element to draw into (full-window, over the canvas)
   * @param doc    for a test or the studio: a document to make elements in
   * @param sprites  URL of the sprite directory
   */
  constructor ({ host, doc = globalThis.document, sprites = `${BASE}sprites/`, log = () => {} } = {}) {
    this.log = log
    // The animation the player's weapon is in (setAnim), and how far into it.
    this.animState = null
    this.animKey = null
    this.animT = 0
    this.doc = doc
    this.sprites = sprites
    this.defs = null
    this.index = { sizes: {}, offsets: {} }
    this.layer = doc.createElement('div')
    this.layer.className = 'dnp-viewmodel'
    Object.assign(this.layer.style, {
      position: 'fixed', inset: '0', pointerEvents: 'none', overflow: 'hidden', zIndex: '2'
    })
    this.gun = this.#img()
    this.flash = this.#img()
    host.appendChild(this.layer)
    this.weapon = null      // the one drawn
    this.wanted = null      // the one held (hud.weapon)
    this.switchT = 0        // > 0 while lowering (first half) and raising
    this.fireT = -1         // seconds into the fire sequence, -1 at rest
    this.readyT = 0
    this.bobT = 0
    this.visible = false
  }

  #img () {
    const el = this.doc.createElement('img')
    Object.assign(el.style, { position: 'absolute', imageRendering: 'pixelated', display: 'none' })
    el.alt = ''
    this.layer.appendChild(el)
    return el
  }

  /** The weapon definitions and the sprite index (sizes, offsets). */
  setDefs (defs, index) {
    this.defs = defs
    this.index = { sizes: index?.sizes ?? {}, offsets: index?.offsets ?? {} }
    // Each weapon's per-state animations, read once. What is wrong is said.
    this.anims = new Map()
    this.errors = []
    for (const [name, def] of Object.entries(defs?.weapons ?? {})) {
      this.anims.set(name, parseAnims(def?.anims, `weapons.${name}`, this.errors))
    }
    for (const e of this.errors) this.log(`[viewmodel] ${e}`)
  }

  /**
   * The player's weapon is in animation `state` (its phase's `anim` or a
   * `set_anim`, predicted). `key` changes when that state is ENTERED -- a new
   * swing of the same phase as much as a different one -- and restarts it.
   */
  setAnim (state, key = state) {
    if (key !== this.animKey) { this.animKey = key; this.animT = 0 }
    this.animState = state
  }

  /** The weapon in hand changed: lower the old one, raise this. */
  setWeapon (name) {
    if (name === this.wanted) return
    this.wanted = name
    if (this.weapon === null) { this.weapon = name; return }
    this.switchT = SWITCH_SECONDS
    this.fireT = -1
    this.animKey = null
  }

  /** A shot, the moment it is predicted. */
  fire () {
    if (this.switchT > 0) return
    this.fireT = 0
  }

  /**
   * One frame.
   * @param dt       seconds since the last
   * @param speed    how fast the body moves, 0..1 of its top speed (the bob)
   * @param visible  whether there is a first-person view at all
   * @param size     the window, {width, height}
   */
  update (dt, speed, visible, size) {
    this.visible = visible
    const def = this.defs?.weapons?.[this.weapon]
    if (!visible || !def) {
      this.gun.style.display = 'none'
      this.flash.style.display = 'none'
      return
    }
    // The switch: down for the first half, the new weapon up for the second.
    let lower = 0
    if (this.switchT > 0) {
      this.switchT = Math.max(0, this.switchT - dt)
      const half = SWITCH_SECONDS / 2
      if (this.switchT > half) lower = 1 - (this.switchT - half) / half
      else {
        if (this.weapon !== this.wanted) this.weapon = this.wanted
        lower = this.switchT / half
      }
    }
    const live = this.defs?.weapons?.[this.weapon]
    if (!live) return
    let frame = null
    // THE WEAPON'S OWN ANIMATION for the state it is in, when it has one: from
    // its start, looping or holding its last frame. Otherwise ready and fire,
    // exactly as before.
    const anim = this.animState === null ? null : this.anims?.get(this.weapon)?.get(this.animState)
    if (anim) {
      this.animT += dt
      frame = frameAt(anim.frames, this.animT, anim.loop) ?? anim.frames[anim.frames.length - 1]
      this.fireT = -1
    }
    if (frame === null && this.fireT >= 0) {
      this.fireT += dt
      frame = frameAt(live.fire, this.fireT)
      if (frame === null) this.fireT = -1
    }
    if (frame === null) {
      this.readyT += dt
      // At rest the ready frames loop -- a one-frame pose for ever, or a
      // chainsaw's idle judder.
      frame = frameAt(live.ready, this.readyT, true)
    }
    if (frame === null) return
    // THE BOB, Doom's shape: a figure-of-eight scaled by speed, still at rest.
    this.bobT += dt * (2 + 6 * speed)
    const amp = 6 * Math.min(1, speed)
    const bx = 1 + amp * Math.cos(this.bobT)
    const by = TOP + amp * Math.abs(Math.sin(this.bobT)) + lower * (BOTTOM - TOP)
    this.#show(this.gun, frame[0], bx, by, size)
    if (frame[2]) this.#show(this.flash, frame[2], bx, by, size)
    else this.flash.style.display = 'none'
  }

  #show (el, name, sx, sy, size) {
    const sz = this.index.sizes[name]
    const off = this.index.offsets[name]
    if (!sz || !off) { el.style.display = 'none'; return }
    const r = place(size, sz, off, sx, sy, this.defs?.screen)
    const src = `${this.sprites}${name}.png`
    if (el.dataset.src !== src) { el.src = src; el.dataset.src = src }
    Object.assign(el.style, {
      display: 'block', left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px`
    })
  }

  destroy () { this.layer.remove() }
}
