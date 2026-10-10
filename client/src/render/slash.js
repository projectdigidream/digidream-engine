//
// THE SWING, AS A PLAYER SEES IT: a slash streak that follows the blade's real
// sweep and fades behind it (docs/SWINGS.md).
//
// Not the debug drawings in swing.js. Those show the COLLIDER -- the region
// tested each tick, the whole reach and arc -- for somebody tuning a weapon,
// and go with the debug overlay. This is the effect: a band near the tip of
// the swept region, laid down tick by tick between where the blade was and
// where it is, so successive pieces join into one streak along the arc the
// swing actually took. Its look is the module's (assets/sprites/swings.json):
// a default, one per weapon by name, one per monster by catalogue name.
//
// The geometry follows the engine's own numbers (readSwingShape,
// sweepOffset), so a streak never claims a sweep the collider did not make.
//

import { swingYaw, sweptSpan } from './swing.js'

/** What a swing looks like when nothing says otherwise. */
export const DEFAULT_SLASH = Object.freeze({
  show: true,
  color: '#fff2c8',
  opacity: 0.6,
  // Seconds a piece of the streak takes to fade. Short: a slash is a blur, not
  // a record -- the debug trail is the record.
  fade: 0.22,
  // How much of the swept region, from the tip inward, the band covers: 1 is
  // all of it (down to the swing's inner reach), 0.4 the outer 40 per cent.
  width: 0.4,
  blend: 'add',
  // A picture under /sprites/, stretched along the sweep (u) and across the
  // band (v, inner to outer); its colour is multiplied by `color`.
  texture: null
})

const FIELDS = {
  show: (v) => typeof v === 'boolean',
  color: (v) => typeof v === 'string' && /^#[0-9a-fA-F]{6}$/.test(v),
  opacity: (v) => typeof v === 'number' && v >= 0 && v <= 1,
  fade: (v) => typeof v === 'number' && v > 0 && v <= 5,
  width: (v) => typeof v === 'number' && v > 0 && v <= 1,
  blend: (v) => v === 'add' || v === 'normal',
  texture: (v) => v === null || (typeof v === 'string' && /^[A-Za-z0-9_\-./]{1,96}\.(png|webp)$/.test(v) && !v.includes('..'))
}
const RULE = {
  show: 'true or false', color: 'a #rrggbb colour', opacity: '0 to 1', fade: 'seconds, above 0, at most 5',
  width: 'above 0, at most 1', blend: "'add' or 'normal'", texture: 'a .png or .webp under sprites/, or null'
}

/**
 * Read swings.json: { version: 1, default: {...}, weapons: { name: {...} },
 * monsters: { name: {...} } }, each entry a partial style. What is wrong is
 * said and left out; the rest is used.
 */
export function parseSwingStyles (json) {
  const errors = []
  const out = { default: { ...DEFAULT_SLASH }, weapons: {}, monsters: {} }
  let data = json
  if (typeof json === 'string') {
    try { data = JSON.parse(json) } catch (e) { return { ok: false, errors: [`not JSON: ${e.message}`], styles: out } }
  }
  if (!data || typeof data !== 'object' || Array.isArray(data)) return { ok: false, errors: ['not an object'], styles: out }
  const entry = (where, e) => {
    const style = {}
    if (!e || typeof e !== 'object' || Array.isArray(e)) { errors.push(`${where}: not an object`); return style }
    for (const [k, v] of Object.entries(e)) {
      if (!FIELDS[k]) { errors.push(`${where}: no field '${k}' (${Object.keys(FIELDS).join(', ')})`); continue }
      if (!FIELDS[k](v)) { errors.push(`${where}: ${k} is ${RULE[k]}, not ${JSON.stringify(v)}`); continue }
      style[k] = v
    }
    return style
  }
  for (const k of Object.keys(data)) {
    if (!['version', 'default', 'weapons', 'monsters'].includes(k)) errors.push(`no section '${k}' (default, weapons, monsters)`)
  }
  if (data.version !== undefined && data.version !== 1) errors.push(`version ${data.version} is not one this client reads (1)`)
  if (data.default !== undefined) Object.assign(out.default, entry('default', data.default))
  for (const section of ['weapons', 'monsters']) {
    const s = data[section]
    if (s === undefined) continue
    if (!s || typeof s !== 'object' || Array.isArray(s)) { errors.push(`${section}: not an object of names`); continue }
    for (const [name, e] of Object.entries(s)) out[section][name] = entry(`${section}.${name}`, e)
  }
  return { ok: errors.length === 0, errors, styles: out }
}

/** The style for one swinger: the default, then its weapon's or its own entry. */
export function styleFor (styles, { weapon = null, monster = null } = {}) {
  const base = styles?.default ?? DEFAULT_SLASH
  const own = (weapon && styles?.weapons?.[weapon]) || (monster && styles?.monsters?.[monster]) || null
  return own ? { ...base, ...own } : base
}

/** The band a style covers: from `inner` to the tip, in world units. */
export function slashBand (shape, width) {
  const outer = shape.reach
  const inner = Math.max(shape.inner ?? 0, outer * (1 - width))
  return { inner, outer }
}

/**
 * One piece of the streak: the swept band between two offsets (turns, from
 * the facing), as positions and uvs in the swing's local frame -- +X the
 * facing, the same frame aimSwing turns. `uOf(offset)` places the texture
 * along the whole sweep.
 */
export function slashPiece ({ inner, outer, from, to, steps = 4, uOf = () => 0 }) {
  const pos = []
  const uv = []
  const idx = []
  for (let i = 0; i <= steps; i++) {
    const o = from + (to - from) * (i / steps)
    // rotateY(+t) turns +X toward -Z, so an offset of `o` is at -o here.
    const a = -o * Math.PI * 2
    const u = uOf(o)
    pos.push(inner * Math.cos(a), 0, inner * Math.sin(a), outer * Math.cos(a), 0, outer * Math.sin(a))
    uv.push(u, 0, u, 1)
    if (i < steps) {
      const v = i * 2
      idx.push(v, v + 1, v + 2, v + 1, v + 3, v + 2)
    }
  }
  return { pos, uv, idx }
}

/**
 * The streaks, for every body that swings. Fed each frame per swinger (`key`:
 * 'self', or an entity index) and faded on update. Pieces are small and short
 * lived, so each is built and freed rather than pooled.
 */
export class SlashView {
  constructor ({ THREE, scene, textureUrl = (f) => `/sprites/${f}`, loader = null }) {
    this.THREE = THREE
    this.scene = scene
    this.textureUrl = textureUrl
    this.loader = loader ?? (THREE.TextureLoader ? new THREE.TextureLoader() : null)
    this.textures = new Map()
    this.swingers = new Map()   // key -> { left, off }
    this.live = []              // { mesh, born, fade, opacity }
  }

  #texture (file) {
    if (!file || !this.loader) return null
    let t = this.textures.get(file)
    if (!t) {
      t = this.loader.load(this.textureUrl(file))
      this.textures.set(file, t)
    }
    return t
  }

  /**
   * This frame's state for one swinger. A piece is laid down when its swing
   * has moved on (its countdown changed), from where the blade was to where it
   * is now; the first piece of a swing starts at the sweep's beginning.
   */
  offer (key, { shape, left, offsetTurns, pitchTurns = 0, facingTurns, x, y, z, style, now }) {
    const s = this.swingers.get(key)
    if (!left || left <= 0 || offsetTurns === null || offsetTurns === undefined || !shape || !(shape.reach > 0)) {
      this.swingers.delete(key)
      return false
    }
    if (s && s.left === left) return false
    const fresh = !s || left > s.left
    const from = fresh ? shape.from : s.off
    this.swingers.set(key, { left, off: offsetTurns })
    if (!style || style.show === false) return false

    const THREE = this.THREE
    const { inner, outer } = slashBand(shape, style.width)
    // A thrust does not sweep: give it a sliver of its arc so it is seen.
    let a = from
    let b = offsetTurns
    if (Math.abs(b - a) < 1e-6) {
      const half = Math.max(shape.halfArc * 0.25, 0.004)
      a = offsetTurns - half
      b = offsetTurns + half
    }
    const span = sweptSpan(shape)
    const lo = span.mid - span.half
    const uOf = (o) => (span.half > 0 ? (o - lo) / (2 * span.half) : 0.5)
    const piece = slashPiece({ inner, outer, from: a, to: b, uOf })
    const geo = new THREE.BufferGeometry()
    geo.setAttribute('position', new THREE.Float32BufferAttribute(piece.pos, 3))
    geo.setAttribute('uv', new THREE.Float32BufferAttribute(piece.uv, 2))
    geo.setIndex(piece.idx)
    const mat = new THREE.MeshBasicMaterial({
      color: style.color,
      map: this.#texture(style.texture),
      transparent: true,
      opacity: style.opacity,
      side: THREE.DoubleSide,
      depthWrite: false,
      blending: style.blend === 'add' ? THREE.AdditiveBlending : THREE.NormalBlending
    })
    const mesh = new THREE.Mesh(geo, mat)
    mesh.position.set(x, y, z)
    mesh.rotation.set(0, 0, 0)
    mesh.rotateY(swingYaw(facingTurns, 0))
    if (pitchTurns) mesh.rotateZ(pitchTurns * Math.PI * 2)
    mesh.renderOrder = 990
    this.scene.add(mesh)
    this.live.push({ mesh, born: now, fade: style.fade, opacity: style.opacity })
    return true
  }

  /** Fade the streaks, and free what has gone. */
  update (now) {
    for (let i = this.live.length - 1; i >= 0; i--) {
      const p = this.live[i]
      const f = now - p.born < 0 ? 1 : 1 - (now - p.born) / p.fade
      if (f <= 0) {
        this.scene.remove(p.mesh)
        p.mesh.geometry.dispose()
        p.mesh.material.dispose()
        this.live.splice(i, 1)
        continue
      }
      p.mesh.material.opacity = p.opacity * f
    }
  }

  /** Forget a swinger that is gone (its streak fades on its own). */
  forget (key) { this.swingers.delete(key) }

  get count () { return this.live.length }
}
