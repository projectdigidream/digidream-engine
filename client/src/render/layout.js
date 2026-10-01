//
// THE UI LAYOUT: a module's HUD and touch controls, as data (docs/UI_LAYOUT.md).
//
// Everything on screen that is not a script's panel used to be fixed in the
// client -- the health bar in index.html, the touch buttons in touchui.js's CSS.
// A module now DECLARES them (assets/ui/index.json and the files it names), the
// client draws from that, and the studio will edit it, previewing with this very
// code. A module with no layout gets DEFAULT_LAYOUT, which reproduces what was
// hard-coded to the pixel (client/test/layout.test.js holds it there).
//
// Pure and DOM-free except HudView, whose document is injected -- the studio
// draws it inside a device frame, not full-window.
//
// UNITS. Positions and sizes are UI units: min(width, height) / scale.reference
// CSS px, clamped to [scale.min, scale.max]. The default pins the scale to 1,
// because what it reproduces was written in pixels. A size may also be
// { vw | vh: percent, max: units } -- the stick's zone is min(46vw, 340px).
//
// SAFE AREA. Offsets are from the safe-area inset, so a notch is handled here
// once and by no author.
//

export const ANCHORS = ['tl', 't', 'tr', 'l', 'c', 'r', 'bl', 'b', 'br']
const TYPES = ['text', 'number', 'bar', 'image', 'group']
const SHOWS = ['always', 'touch', 'desktop']
const OPS = ['gt', 'lt', 'eq', 'ne', 'set']

/** The touch buttons the engine has, each one a fixed input bit. A layout can
 *  move, resize, relabel and hide them; it cannot invent one. */
export const TOUCH_IDS = ['jump', 'attack', 'use', 'dash', 'alt']

/** What the client knows itself. */
export const CLIENT_BINDINGS = [
  { name: 'hp', about: 'the player\'s health, from the snapshot' },
  { name: 'map', about: 'the level\'s name' },
  { name: 'ping', about: 'the round trip, in milliseconds, when one is measured' }
]

/** What the weapons runtime publishes for every module (HUD_SET; docs/WEAPONS.md). */
export const STANDARD_HUD_NAMES = [
  { name: 'hud.hp_max', kind: 'int', about: 'the health ceiling' },
  { name: 'hud.weapon', kind: 'str', about: 'the weapon in hand' },
  { name: 'hud.weapon_icon', kind: 'str', about: 'a picture for it' },
  { name: 'hud.ammo_type', kind: 'str', about: 'the pool it draws from' },
  { name: 'hud.ammo', kind: 'int', about: 'the amount in that pool' },
  { name: 'hud.ammo_max', kind: 'int', about: 'that pool\'s max' },
  { name: 'hud.ammo_<pool>', kind: 'int', about: 'every pool by name' }
]

// --- the built-in default -------------------------------------------------
//
// EXACTLY what index.html and touchui.js drew before layouts existed: the
// health box bottom-left at (12, 34) -- moved to the top when the touch
// controls take the corner -- the stick's zone, and the four buttons in the
// thumb's arc. Pixels, so the scale is pinned.

const HEALTH = (anchor, y, show) => ({
  id: show === 'touch' ? 'health_touch' : 'health', type: 'group', anchor, x: 12, y, show,
  // Hidden until there is a health to show: solo runs no server and keeps none.
  visible: { bind: 'hp', op: 'set' },
  w: { vw: 40, max: 240 }, stack: 'column', gap: 4,
  children: [
    { id: show === 'touch' ? 'health_row_t' : 'health_row', type: 'group', stack: 'row', gap: 8,
      children: [
        { id: show === 'touch' ? 'health_num_t' : 'health_num', type: 'number', bind: 'hp', format: '{value}',
          style: { size: 20, weight: 600, align: 'right' } },
        { id: show === 'touch' ? 'health_label_t' : 'health_label', type: 'text', text: 'health' }
      ] },
    { id: show === 'touch' ? 'health_bar_t' : 'health_bar', type: 'bar', bind: 'hp', max: 'hud.hp_max', h: 8,
      fill: '#5fbf6a', low: [{ below: 0.6, fill: '#d6a13a' }, { below: 0.25, fill: '#d65b5b' }] }
  ]
})

const BUTTONS = [
  { id: 'jump', anchor: 'br', x: -22, y: -150, size: 74, label: 'JUMP' },
  { id: 'attack', anchor: 'br', x: -104, y: -78, size: 74, label: 'ATK' },
  { id: 'use', anchor: 'br', x: -22, y: -72, size: 74, label: 'USE' },
  { id: 'dash', anchor: 'br', x: -104, y: -158, size: 60, label: 'DASH' },
  { id: 'alt', anchor: 'br', x: -186, y: -78, size: 64, label: 'ALT', hidden: true }
]

export const DEFAULT_LAYOUT = Object.freeze({
  version: 1,
  scale: { reference: 390, min: 1, max: 1 },
  theme: { font: 'ui-monospace, SFMono-Regular, Menlo, monospace', text: '#cfe3ff',
           panel: 'rgba(18,18,22,.94)', accent: '#5b8fd6' },
  hud: [HEALTH('bl', -34, 'desktop'), HEALTH('tl', 34, 'touch')],
  touch: {
    stick: { zone: { anchor: 'bl', x: 0, y: 0, w: { vw: 46, max: 340 }, h: { vh: 58, max: 380 } } },
    buttons: BUTTONS
  },
  // FIRST PERSON on a touch screen: the stick walks, a drag anywhere else
  // looks, and the trigger is a big button where the right thumb rests, with
  // the second trigger beside it. Jump and use move up out of the way.
  touch_fps: {
    stick: { zone: { anchor: 'bl', x: 0, y: 0, w: { vw: 40, max: 300 }, h: { vh: 58, max: 380 } } },
    buttons: [
      { id: 'attack', anchor: 'br', x: -40, y: -90, size: 96, label: 'FIRE' },
      { id: 'alt', anchor: 'br', x: -150, y: -60, size: 64, label: 'ALT' },
      { id: 'jump', anchor: 'br', x: -40, y: -200, size: 64, label: 'JUMP' },
      { id: 'use', anchor: 'br', x: -150, y: -150, size: 60, label: 'USE' },
      { id: 'dash', anchor: 'br', x: -110, y: -230, size: 52, label: 'DASH', hidden: true }
    ]
  },
  panels: {}
})

// --- parsing ---------------------------------------------------------------

/**
 * A layout file, checked. Unknown keys and element types are WARNINGS, never
 * failures, so an older client loads a newer file; a malformed value that would
 * otherwise be placed somewhere arbitrary (an anchor nobody can draw) is an
 * ERROR naming it, and the element is dropped rather than put at the corner.
 *
 * -> { ok, layout, errors, warnings }. `layout` is always usable: what failed is
 *    replaced by the default.
 */
export function parseLayout (json) {
  const errors = []
  const warnings = []
  let src = json
  if (typeof json === 'string') {
    try { src = JSON.parse(json) } catch (e) { return { ok: false, layout: DEFAULT_LAYOUT, errors: [`not JSON: ${e.message}`], warnings } }
  }
  if (!src || typeof src !== 'object') return { ok: false, layout: DEFAULT_LAYOUT, errors: ['not an object'], warnings }
  const known = ['version', 'scale', 'theme', 'hud', 'touch', 'touch_fps', 'panels', '_about']
  for (const k of Object.keys(src)) if (!known.includes(k)) warnings.push(`unknown key '${k}', ignored`)
  if (src.version !== undefined && src.version !== 1) warnings.push(`version ${src.version}: read as version 1`)

  const layout = {
    version: 1,
    scale: { ...DEFAULT_LAYOUT.scale, ...(src.scale ?? {}) },
    theme: { ...DEFAULT_LAYOUT.theme, ...(src.theme ?? {}) },
    hud: src.hud === undefined ? DEFAULT_LAYOUT.hud : [],
    touch: src.touch === undefined ? DEFAULT_LAYOUT.touch : touchOf(src.touch, 'touch', DEFAULT_LAYOUT.touch, errors, warnings),
    touch_fps: src.touch_fps === undefined ? DEFAULT_LAYOUT.touch_fps : touchOf(src.touch_fps, 'touch_fps', DEFAULT_LAYOUT.touch_fps, errors, warnings),
    panels: {}
  }
  if (src.panels !== undefined) {
    if (!src.panels || typeof src.panels !== 'object' || Array.isArray(src.panels)) errors.push('`panels` must be an object of templates')
    else {
      for (const [name, t] of Object.entries(src.panels)) {
        const p = panelOf(name, t, errors, warnings)
        if (p) layout.panels[name] = p
      }
    }
  }
  if (src.hud !== undefined) {
    if (!Array.isArray(src.hud)) errors.push('`hud` must be a list of elements')
    else {
      const ids = new Set()
      for (const el of src.hud) {
        const e = elementOf(el, 'hud', errors, warnings, ids)
        if (e) layout.hud.push(e)
      }
    }
  }
  return { ok: errors.length === 0, layout, errors, warnings }
}

function elementOf (el, where, errors, warnings, ids) {
  if (!el || typeof el !== 'object') { errors.push(`${where}: an element must be an object`); return null }
  const at = `${where} '${el.id ?? '?'}'`
  if (typeof el.id !== 'string' || el.id.length === 0 || el.id.length > 24) {
    errors.push(`${at}: an element needs an id of 1..24 characters`); return null
  }
  if (ids.has(el.id)) warnings.push(`${at}: id used twice`)
  ids.add(el.id)
  if (!TYPES.includes(el.type)) { warnings.push(`${at}: unknown type '${el.type}', skipped`); return null }
  if (el.anchor !== undefined && !ANCHORS.includes(el.anchor)) {
    errors.push(`${at}: anchor '${el.anchor}' is not one of ${ANCHORS.join(' ')}`); return null
  }
  if (el.show !== undefined && !SHOWS.includes(el.show)) {
    errors.push(`${at}: show '${el.show}' is not one of ${SHOWS.join(' ')}`); return null
  }
  if (el.visible !== undefined && (!el.visible || !OPS.includes(el.visible.op) || typeof el.visible.bind !== 'string')) {
    errors.push(`${at}: visible must be { bind, op: ${OPS.join('|')}, value }`); return null
  }
  const out = { ...el }
  if (el.type === 'group') {
    out.children = []
    for (const c of el.children ?? []) {
      const ce = elementOf(c, `${at} child`, errors, warnings, ids)
      if (ce) out.children.push(ce)
    }
  }
  return out
}

// A PANEL TEMPLATE (docs/UI_LAYOUT.md 5). Its name is what UI_OPEN carries, so
// it has the wire's rule; anything that would put the panel or a widget
// somewhere nobody can draw is an error, and the template is dropped -- the
// panel then draws as the default one, never misplaced.
const PANEL_KEYS = ['anchor', 'x', 'y', 'w', 'h', 'modal', 'closeable', 'title', 'slots', 'decor', 'style', '_about']
const SLOT_KEYS = ['widget', 'anchor', 'x', 'y', 'w', 'h', 'style']
function panelOf (name, t, errors, warnings) {
  const at = `panel '${name}'`
  if (!/^[a-z0-9_]{1,24}$/.test(name)) { errors.push(`${at}: a template name is 1..24 of [a-z0-9_]`); return null }
  if (!t || typeof t !== 'object') { errors.push(`${at}: must be an object`); return null }
  for (const k of Object.keys(t)) if (!PANEL_KEYS.includes(k)) warnings.push(`${at}: unknown key '${k}', ignored`)
  if (t.anchor !== undefined && !ANCHORS.includes(t.anchor)) { errors.push(`${at}: anchor '${t.anchor}' is not one of ${ANCHORS.join(' ')}`); return null }
  const out = { ...t, slots: [], decor: [] }
  if (t.slots !== undefined && !Array.isArray(t.slots)) { errors.push(`${at}: slots must be a list`); return null }
  const seen = new Set()
  for (const sl of t.slots ?? []) {
    if (!sl || !Number.isInteger(sl.widget) || sl.widget < 0 || sl.widget > 0xffff) {
      errors.push(`${at}: a slot needs \`widget\`, the script's widget id (0..65535)`); return null
    }
    if (sl.anchor !== undefined && !ANCHORS.includes(sl.anchor)) { errors.push(`${at} slot ${sl.widget}: anchor '${sl.anchor}'`); return null }
    for (const k of Object.keys(sl)) if (!SLOT_KEYS.includes(k)) warnings.push(`${at} slot ${sl.widget}: unknown key '${k}', ignored`)
    if (seen.has(sl.widget)) warnings.push(`${at}: widget ${sl.widget} has two slots; the first is used`)
    seen.add(sl.widget)
    out.slots.push({ ...sl })
  }
  const ids = new Set()
  for (const el of t.decor ?? []) {
    const e = elementOf(el, `${at} decor`, errors, warnings, ids)
    if (e) out.decor.push(e)
  }
  return out
}

function touchOf (t, where, fallback, errors, warnings) {
  if (!t || typeof t !== 'object') { errors.push(`${where} must be an object`); return fallback }
  const byId = new Map(fallback.buttons.map((b) => [b.id, { ...b }]))
  for (const b of t.buttons ?? []) {
    if (!TOUCH_IDS.includes(b.id)) { warnings.push(`${where}: no button '${b.id}' -- a layout places the engine's buttons, it cannot invent one`); continue }
    if (b.anchor !== undefined && !ANCHORS.includes(b.anchor)) { errors.push(`${where} button '${b.id}': anchor '${b.anchor}'`); continue }
    // A button absent from the list keeps its default; one named is merged.
    byId.set(b.id, { ...(byId.get(b.id) ?? { anchor: 'br', x: 0, y: 0, size: 74, label: b.id.toUpperCase() }), ...b })
  }
  let zone = fallback.stick.zone
  if (t.stick?.zone) {
    if (t.stick.zone.anchor !== undefined && !ANCHORS.includes(t.stick.zone.anchor)) errors.push(`${where} stick zone: anchor '${t.stick.zone.anchor}'`)
    else zone = { ...fallback.stick.zone, ...t.stick.zone }
  }
  return { stick: { zone }, buttons: [...byId.values()] }
}

// --- arithmetic --------------------------------------------------------------

/** CSS px per UI unit for a window this size. */
export function uiScale (layout, width, height) {
  const s = layout?.scale ?? DEFAULT_LAYOUT.scale
  const raw = Math.min(width, height) / (s.reference || 390)
  return Math.max(s.min ?? 0, Math.min(s.max ?? Infinity, raw))
}

/** A size in px: a number of units, or { vw | vh: percent, max: units }. */
export function sizeOf (v, scale, width, height) {
  if (v === undefined || v === null) return undefined
  if (typeof v === 'number') return v * scale
  const cap = v.max === undefined ? Infinity : v.max * scale
  if (v.vw !== undefined) return Math.min(width * v.vw / 100, cap)
  if (v.vh !== undefined) return Math.min(height * v.vh / 100, cap)
  return undefined
}

/**
 * Where an element goes: { left, top, width, height } in CSS px.
 *
 * The element's own matching corner or edge sits at the anchor point, moved by
 * (x, y) units, +x right and +y down, inside the safe area. `natural` is the
 * size to use when the element declares none (text sized to its content).
 */
export function place (el, layout, width, height, insets = {}, natural = { w: 0, h: 0 }) {
  const s = uiScale(layout, width, height)
  const ins = { top: 0, right: 0, bottom: 0, left: 0, ...insets }
  const w = sizeOf(el.w ?? el.size, s, width, height) ?? natural.w
  const h = sizeOf(el.h ?? el.size, s, width, height) ?? natural.h
  const a = el.anchor ?? 'tl'
  const x = (el.x ?? 0) * s
  const y = (el.y ?? 0) * s
  const L = ins.left
  const R = width - ins.right
  const T = ins.top
  const B = height - ins.bottom
  const col = a.endsWith('l') ? 'l' : a.endsWith('r') ? 'r' : 'c'
  const row = a.startsWith('t') ? 't' : a.startsWith('b') ? 'b' : 'c'
  const left = col === 'l' ? L + x : col === 'r' ? R + x - w : (L + R) / 2 + x - w / 2
  const top = row === 't' ? T + y : row === 'b' ? B + y - h : (T + B) / 2 + y - h / 2
  return { left, top, width: w, height: h }
}

/** A binding's value, or undefined when it has none yet (never an error). */
export function resolveBinding (bindings, name) {
  if (!bindings || name === undefined) return undefined
  return bindings instanceof Map ? bindings.get(name) : bindings[name]
}

/** Does an element's `visible` rule hold? No rule: yes. */
export function isVisible (el, bindings) {
  const r = el.visible
  if (!r) return true
  const v = resolveBinding(bindings, r.bind)
  switch (r.op) {
    case 'set': return v !== undefined
    case 'gt': return v !== undefined && v > r.value
    case 'lt': return v !== undefined && v < r.value
    case 'eq': return v === r.value
    case 'ne': return v !== r.value
  }
  return true
}

/** Which layout file a level uses: its own (by NAME, like the music), else the default. */
export function layoutFileFor (index, levelName) {
  if (!index) return null
  if (levelName && index.maps?.[levelName]) return index.maps[levelName]
  return index.default ?? null
}

// --- the HUD, drawn ----------------------------------------------------------

/**
 * The HUD, into any host element, with live or sample data. DOM, because a HUD
 * is not in the world (render/ui.js makes the same choice).
 */
export class HudView {
  constructor ({ host, doc = globalThis.document, touch = false, insets = null, size = null } = {}) {
    this.host = host
    this.doc = doc
    this.touch = touch
    this.insets = insets
    this.size = size
    this.bindings = new Map()
    this.maxSeen = new Map()
    this.layout = DEFAULT_LAYOUT
    this.root = doc.createElement('div')
    this.root.className = 'dnp-hud'
    Object.assign(this.root.style, { position: 'absolute', inset: '0', pointerEvents: 'none', zIndex: '20' })
    host.appendChild(this.root)
    this.nodes = []
    this.#build()
  }

  setLayout (layout) { this.layout = layout ?? DEFAULT_LAYOUT; this.#build() }

  /** Partial updates merge: { hp: 80, 'hud.ammo': 12 }. */
  update (bindings) {
    let changed = false
    for (const [k, v] of (bindings instanceof Map ? bindings : Object.entries(bindings ?? {}))) {
      if (this.bindings.get(k) === v && this.bindings.has(k)) continue
      changed = true
      this.bindings.set(k, v)
      if (typeof v === 'number') this.maxSeen.set(k, Math.max(this.maxSeen.get(k) ?? 0, v))
    }
    // Called every frame; a paint measures, and a measure is a layout.
    const area = this.#area()
    if (area.width !== this.lastArea?.width || area.height !== this.lastArea?.height) changed = true
    this.lastArea = area
    if (changed) this.#paint()
  }

  /** Measure and place again -- after the host is attached, when an element's
   *  natural size can first be read (a panel's decor is built before it is). */
  refresh () { this.#paint() }

  destroy () { this.root.remove() }

  #area () {
    if (this.size) return this.size
    const r = this.host.getBoundingClientRect?.()
    return r && r.width ? { width: r.width, height: r.height } : { width: globalThis.innerWidth ?? 0, height: globalThis.innerHeight ?? 0 }
  }

  #build () {
    this.root.textContent = ''
    this.nodes = []
    const theme = this.layout.theme ?? DEFAULT_LAYOUT.theme
    for (const el of this.layout.hud ?? []) {
      if (el.show === 'touch' && !this.touch) continue
      if (el.show === 'desktop' && this.touch) continue
      const node = this.#make(el, theme)
      node.dom.style.position = 'absolute'
      this.root.appendChild(node.dom)
      this.nodes.push(node)
    }
    this.#paint()
  }

  #make (el, theme) {
    const d = this.doc.createElement('div')
    const st = el.style ?? {}
    Object.assign(d.style, {
      font: `${st.weight ?? 400} ${st.size ?? 12}px/1.5 ${theme.font}`, color: st.color ?? theme.text,
      textShadow: '0 1px 2px #000', whiteSpace: 'pre', textAlign: st.align ?? 'left'
    })
    if (st.opacity !== undefined) d.style.opacity = String(st.opacity)
    const node = { el, dom: d, kids: [] }
    if (el.type === 'group') {
      if (el.stack === 'row') Object.assign(d.style, { display: 'flex', alignItems: 'baseline', gap: `${el.gap ?? 0}px` })
      else if (el.stack === 'column') Object.assign(d.style, { display: 'flex', flexDirection: 'column', gap: `${el.gap ?? 0}px` })
      for (const c of el.children ?? []) {
        const k = this.#make(c, theme)
        if (!el.stack) k.dom.style.position = 'absolute'
        d.appendChild(k.dom)
        node.kids.push(k)
      }
    } else if (el.type === 'bar') {
      Object.assign(d.style, { height: `${el.h ?? 8}px`, background: st.bg ?? '#1b2230', border: st.border ?? '1px solid #2c3648',
                               borderRadius: `${st.radius ?? 3}px`, overflow: 'hidden' })
      const fill = this.doc.createElement('div')
      Object.assign(fill.style, { height: '100%', width: '0%', background: el.fill ?? '#5fbf6a' })
      d.appendChild(fill)
      node.fill = fill
    } else if (el.type === 'number' && st.size) {
      d.style.minWidth = '3ch'
    } else if (el.type === 'image') {
      const img = this.doc.createElement('img')
      img.style.imageRendering = 'pixelated'
      d.appendChild(img)
      node.img = img
    }
    return node
  }

  #paint () {
    const area = this.#area()
    for (const n of this.nodes) {
      this.#fill(n)
      const r = place(n.el, this.layout, area.width, area.height, this.insets ?? {}, { w: n.dom.offsetWidth || 0, h: n.dom.offsetHeight || 0 })
      Object.assign(n.dom.style, { left: `${r.left}px`, top: `${r.top}px` })
      if (r.width) n.dom.style.width = `${r.width}px`
    }
  }

  #fill (n) {
    const el = n.el
    const show = isVisible(el, this.bindings)
    n.dom.style.display = show ? (el.type === 'group' && el.stack ? 'flex' : 'block') : 'none'
    if (!show) return
    const b = this.bindings
    if (el.type === 'text') {
      n.dom.textContent = String(el.text ?? '').replace(/\{([a-z0-9_.]+)\}/g, (_, k) => b.get(k) ?? '')
    } else if (el.type === 'number') {
      const v = b.get(el.bind)
      n.dom.textContent = v === undefined ? '--' : String(el.format ?? '{value}').replace('{value}', String(Math.max(0, v)))
        .replace('{max}', String(this.#max(el) ?? ''))
    } else if (el.type === 'bar') {
      const v = b.get(el.bind)
      const max = this.#max(el)
      const frac = v === undefined || !max ? 0 : Math.max(0, Math.min(1, v / max))
      n.fill.style.width = `${(frac * 100).toFixed(1)}%`
      let fill = el.fill ?? '#5fbf6a'
      for (const lo of [].concat(el.low ?? [])) if (frac <= lo.below) fill = lo.fill
      n.fill.style.background = fill
    } else if (el.type === 'image') {
      const frame = el.bind ? b.get(el.bind) : el.frame
      if (frame && n.img) n.img.src = `/sprites/${frame}.png`
    }
    for (const k of n.kids) this.#fill(k)
  }

  // A bar's max: a number, or a binding -- and while that binding has no value,
  // the most the bound value has been, which is what the hard-coded bar did.
  #max (el) {
    if (typeof el.max === 'number') return el.max
    const m = el.max !== undefined ? this.bindings.get(el.max) : undefined
    return m ?? this.maxSeen.get(el.bind)
  }
}
