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

import { BASE } from '../base.js'
export const ANCHORS = ['tl', 't', 'tr', 'l', 'c', 'r', 'bl', 'b', 'br']
const TYPES = ['text', 'number', 'bar', 'image', 'group']
const SHOWS = ['always', 'touch', 'desktop']
const OPS = ['gt', 'lt', 'eq', 'ne', 'set']

/** The touch buttons the engine has, each one a fixed input bit. A layout can
 *  move, resize, relabel and hide them; it cannot invent one. */
export const TOUCH_IDS = ['jump', 'attack', 'use', 'dash', 'alt', 'grab']

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
  { name: 'hud.ammo_<pool>', kind: 'int', about: 'every pool by name' },
  { name: 'hud.subtitle', kind: 'str', about: 'a subtitle line; cleared, none is shown' },
  { name: 'hud.subtitle_speaker', kind: 'str', about: 'who is speaking it' }
]

// SUBTITLES: a line of text low in the middle of the screen, and who says it,
// filled by the server (dnp.hud, `hud.subtitle` / `hud.subtitle_speaker`; a
// graph's `subtitle` node). Shown only while there is a line. Every layout
// gets it -- the built-in one and a module's -- unless the layout places its
// own element with the id `subtitle`, which is how a module restyles them.
export const SUBTITLES = Object.freeze({
  id: 'subtitle', type: 'group', anchor: 'b', x: 0, y: -110, w: { vw: 84, max: 760 },
  stack: 'column', gap: 2,
  visible: { bind: 'hud.subtitle', op: 'set' },
  children: [
    { id: 'subtitle_speaker', type: 'text', text: '{hud.subtitle_speaker}',
      visible: { bind: 'hud.subtitle_speaker', op: 'set' },
      style: { size: 14, weight: 700, align: 'center', color: '#ffd27a' } },
    { id: 'subtitle_text', type: 'text', text: '{hud.subtitle}',
      style: { size: 18, align: 'center', wrap: true } }
  ]
})
function hasElement (els, id) {
  for (const el of els ?? []) {
    if (el.id === id || hasElement(el.children, id)) return true
  }
  return false
}
// ON A TOUCH SCREEN the subtitle keeps out of the buttons' corner (bottom
// right, about this many units in from the edge in either touch layout): it
// narrows to the room between the two corners when the screen is wide -- a
// phone held sideways -- and otherwise rises above the buttons.
const TOUCH_CORNER = 220
const TOUCH_RISE = 250
export function clearOfButtons (el, layout, width, height) {
  if (!(el.anchor ?? 'tl').startsWith('b')) return el      // a line placed up top is clear already
  const s = uiScale(layout, width, height)
  const room = width - 2 * TOUCH_CORNER * s
  if (room >= 300 * s) {
    const full = sizeOf(el.w ?? el.size, s, width, height) ?? width
    return full <= room ? el : { ...el, w: room / s }
  }
  return { ...el, y: Math.min(el.y ?? 0, -TOUCH_RISE) }
}

/** A layout with the subtitle line in it, unless it places its own. */
export function withSubtitles (layout) {
  if (hasElement(layout.hud, 'subtitle')) return layout
  return { ...layout, hud: [...(layout.hud ?? []), SUBTITLES] }
}

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
  { id: 'alt', anchor: 'br', x: -186, y: -78, size: 64, label: 'ALT', hidden: true },
  // Held to stay on a climbable wall. Shown by the game while its rules say
  // walls are grabbed by holding (climb_auto off); pushing into one needs none.
  { id: 'grab', anchor: 'br', x: -186, y: -158, size: 60, label: 'GRAB', hidden: true }
]

// MARKERS (docs/MARKERS.md): small UIs a script hangs on something in the
// world -- a bar over a monster, a sign over a door. Each is a box (w x h UI
// units) of the HUD's own elements, its bottom-centre on the point it hangs
// on; their bindings read the marker's values. These are always there; a
// layout's `markers` add to them and replace them by name.
export const DEFAULT_MARKERS = Object.freeze({
  // A bar: `value` of `max` -- a monster's health, red when low.
  health: { w: 36, h: 6, elements: [
    { id: 'bar', type: 'bar', anchor: 'c', w: 36, h: 5, bind: 'value', max: 'max',
      fill: '#5fbf6a', low: [{ below: 0.5, fill: '#d6a13a' }, { below: 0.25, fill: '#d65b5b' }] }
  ] },
  // A line of text: `text`.
  label: { w: 220, h: 24, elements: [
    { id: 'text', type: 'text', anchor: 'b', text: '{text}',
      style: { size: 12, align: 'center', bg: 'rgba(10,12,18,.72)', radius: 3, padding: 3 } }
  ] },
  // A pointer, with an optional line of `text` over it.
  marker: { w: 160, h: 40, elements: [
    { id: 'text', type: 'text', anchor: 'b', y: -18, text: '{text}', visible: { bind: 'text', op: 'set' },
      style: { size: 12, align: 'center', color: '#ffd27a' } },
    { id: 'arrow', type: 'text', anchor: 'b', text: '\u25BC', style: { size: 16, align: 'center', color: '#ffd27a' } }
  ] },
  // The debug overlay's health counter (mobs.lua hangs it on every monster
  // when the module has the overlay on): `value`/`max`.
  debug_health: { w: 80, h: 16, elements: [
    { id: 'n', type: 'text', anchor: 'b', text: '{value}/{max}',
      style: { size: 11, align: 'center', color: '#ffe08a', bg: 'rgba(0,0,0,.6)', radius: 2, padding: 1 } }
  ] }
})

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
      { id: 'dash', anchor: 'br', x: -110, y: -230, size: 52, label: 'DASH', hidden: true },
      { id: 'grab', anchor: 'br', x: -220, y: -150, size: 56, label: 'GRAB', hidden: true }
    ]
  },
  panels: {},
  markers: DEFAULT_MARKERS
})

// --- fonts and skins, loaded (docs/UI_STYLES.md 2.1, 3) --------------------
//
// What the studio checks to say honestly what the game draws yet.
export const UI_STYLES = Object.freeze({ version: 1, fonts: true, skins: true, hudFont: true, hudBox: true, assetUrl: true })

// Every picture a layout's styles name: panel, title, button (and pressed),
// close, in named styles, inline template styles and slot styles.
export function layoutImages (layout) {
  const out = new Set()
  const walk = (o) => {
    if (!o || typeof o !== 'object') return
    if (typeof o.image === 'string') out.add(o.image)
    for (const v of Object.values(o)) if (v && typeof v === 'object') walk(v)
  }
  walk(layout.styles)
  for (const p of Object.values(layout.panels ?? {})) {
    if (p.style && typeof p.style === 'object') walk(p.style)
    for (const sl of p.slots ?? []) walk(sl.style)
  }
  return [...out]
}

/**
 * The fonts and pictures a layout names, loaded once each. A font is FETCHED
 * and its bytes handed to FontFace -- so a shell that serves it without a font
 * MIME type still works -- and added to the document's fonts; a picture is
 * preloaded so a missing one is said. Either failing is said ONCE, naming the
 * file, and the text or the style's colours stay as they were: nothing becomes
 * invisible. `load` resolves when everything asked for has settled, which is
 * when the views draw again (main.js).
 */
export class UiAssets {
  // The browser's fetch must be called unbound (as itself): stored and called
  // as this.fetch(...) it throws "Illegal invocation", which no stub shows.
  constructor ({ fetch = (...a) => globalThis.fetch(...a), FontFace = globalThis.FontFace, fonts = globalThis.document?.fonts,
                 Image = globalThis.Image, base = `${BASE}ui/`, warn = (m) => console.warn(m) } = {}) {
    Object.assign(this, { fetch, FontFace, fonts, Image, base, warn })
    this.asked = new Map()       // key -> Promise<boolean>
  }

  #font (f) {
    const key = `font|${f.family}|${f.file}|${f.weight ?? ''}|${f.style ?? ''}`
    if (!this.asked.has(key)) {
      this.asked.set(key, (async () => {
        try {
          if (!this.fetch || !this.FontFace) throw new Error('this client cannot load fonts')
          const r = await this.fetch(this.base + f.file)
          if (!r.ok) throw new Error(`HTTP ${r.status}`)
          const face = new this.FontFace(f.family, await r.arrayBuffer(),
            { weight: f.weight ? String(f.weight) : 'normal', style: f.style ?? 'normal' })
          await face.load()
          this.fonts?.add(face)
          return true
        } catch (e) {
          this.warn(`[ui] font ${f.file} (${f.family}) did not load: ${e.message} -- its text uses the next font in the list`)
          return false
        }
      })())
    }
    return this.asked.get(key)
  }

  #image (file) {
    const key = `image|${file}`
    if (!this.asked.has(key)) {
      this.asked.set(key, new Promise((resolve) => {
        if (!this.Image) { resolve(false); return }
        const img = new this.Image()
        img.onload = () => resolve(true)
        img.onerror = () => {
          this.warn(`[ui] skin ${file} did not load -- the style's colours are drawn instead`)
          resolve(false)
        }
        img.src = this.base + file
      }))
    }
    return this.asked.get(key)
  }

  load (layout) {
    return Promise.all([...(layout.fonts ?? []).map((f) => this.#font(f)),
                        ...layoutImages(layout).map((i) => this.#image(i))])
  }
}

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
  const known = ['version', 'scale', 'theme', 'hud', 'touch', 'touch_fps', 'panels', 'markers', 'fonts', 'styles', '_about']
  for (const k of Object.keys(src)) if (!known.includes(k)) warnings.push(`unknown key '${k}', ignored`)
  if (src.version !== undefined && src.version !== 1) warnings.push(`version ${src.version}: read as version 1`)

  const layout = {
    version: 1,
    scale: { ...DEFAULT_LAYOUT.scale, ...(src.scale ?? {}) },
    theme: { ...DEFAULT_LAYOUT.theme, ...(src.theme ?? {}) },
    hud: src.hud === undefined ? DEFAULT_LAYOUT.hud : [],
    touch: src.touch === undefined ? DEFAULT_LAYOUT.touch : touchOf(src.touch, 'touch', DEFAULT_LAYOUT.touch, errors, warnings),
    touch_fps: src.touch_fps === undefined ? DEFAULT_LAYOUT.touch_fps : touchOf(src.touch_fps, 'touch_fps', DEFAULT_LAYOUT.touch_fps, errors, warnings),
    panels: {},
    markers: { ...DEFAULT_MARKERS },
    fonts: [],
    styles: {}
  }
  if (src.fonts !== undefined) {
    if (!Array.isArray(src.fonts)) errors.push('`fonts` must be a list')
    else src.fonts.forEach((f, i) => { const v = fontOf(f, i, errors); if (v) layout.fonts.push(v) })
  }
  if (src.styles !== undefined) {
    if (!src.styles || typeof src.styles !== 'object' || Array.isArray(src.styles)) errors.push('`styles` must be an object of named styles')
    else {
      for (const [name, st] of Object.entries(src.styles)) {
        const v = styleOf(name, st, errors, warnings)
        if (v) layout.styles[name] = v
      }
    }
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
  // A panel naming a style the layout lacks draws the default look (ui.js);
  // said here once, by panel, where the author can see it.
  for (const [name, p] of Object.entries(layout.panels)) {
    const use = typeof p.style === 'string' ? p.style : p.style?.use
    if (use !== undefined && layout.styles[use] === undefined) warnings.push(`panel '${name}': no style '${use}'; the default look is used`)
  }
  if (src.markers !== undefined) {
    if (!src.markers || typeof src.markers !== 'object' || Array.isArray(src.markers)) errors.push('`markers` must be an object of templates')
    else {
      for (const [name, t] of Object.entries(src.markers)) {
        const m = markerOf(name, t, errors, warnings)
        if (m) layout.markers[name] = m
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

// A marker template: a name a script can send ([a-z0-9_], as a panel's), a box
// in UI units, and the elements in it -- the HUD's, checked as the HUD's are.
function markerOf (name, t, errors, warnings) {
  const at = `marker '${name}'`
  if (!/^[a-z0-9_]{1,24}$/.test(name)) { errors.push(`${at}: a name is 1..24 of [a-z0-9_]`); return null }
  if (!t || typeof t !== 'object') { errors.push(`${at}: must be an object`); return null }
  for (const k of Object.keys(t)) if (!['w', 'h', 'elements', '_about'].includes(k)) warnings.push(`${at}: unknown key '${k}', ignored`)
  const size = (v, d) => (typeof v === 'number' && v > 0 ? v : d)
  if (!Array.isArray(t.elements)) { errors.push(`${at}: needs \`elements\`, a list`); return null }
  const ids = new Set()
  const elements = []
  for (const el of t.elements) {
    const e = elementOf(el, at, errors, warnings, ids)
    if (e) elements.push(e)
  }
  return { w: size(t.w, 40), h: size(t.h, 16), elements }
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
// --- fonts and styles (docs/UI_STYLES.md) ---------------------------------
//
// A path under the layout's own directory (assets/ui/, served at /ui/): not
// absolute, no `..`, no backslash -- a layout file cannot reach elsewhere.
function assetPath (p) {
  return typeof p === 'string' && p.length > 0 && p.length <= 200 && !p.startsWith('/') &&
         !p.includes('\\') && !p.split('/').includes('..') && /^[A-Za-z0-9_./-]+$/.test(p)
}
function fontOf (f, i, errors) {
  const at = `font ${i}`
  if (!f || typeof f !== 'object') { errors.push(`${at}: must be an object`); return null }
  if (typeof f.family !== 'string' || !/^[\x20-\x7e]{1,48}$/.test(f.family) || /[,;"'{}]/.test(f.family)) {
    errors.push(`${at}: a family is 1..48 printable characters`); return null
  }
  if (!assetPath(f.file)) { errors.push(`${at} '${f.family}': file must be a path under the layout's directory`); return null }
  if (f.weight !== undefined && !(Number.isInteger(f.weight) && f.weight >= 1 && f.weight <= 1000)) {
    errors.push(`${at} '${f.family}': weight is 1..1000`); return null
  }
  if (f.style !== undefined && f.style !== 'normal' && f.style !== 'italic') {
    errors.push(`${at} '${f.family}': style is normal or italic`); return null
  }
  return { family: f.family, file: f.file, weight: f.weight, style: f.style }
}
// What each part of a style may say. Anything else is warned of and ignored.
const STYLE_PARTS = {
  '': ['font', 'size', 'color', 'weight', 'accent', 'panel', 'title', 'button', 'close'],
  panel: ['bg', 'border', 'radius', 'padding', 'image', 'slice'],
  title: ['bg', 'color', 'size', 'weight', 'align', 'image', 'slice'],
  button: ['bg', 'color', 'border', 'radius', 'image', 'slice', 'pressed'],
  pressed: ['bg', 'color', 'image'],
  close: ['color', 'image']
}
function stylePart (part, obj, at, errors, warnings) {
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) { errors.push(`${at}: must be an object`); return false }
  for (const [k, v] of Object.entries(obj)) {
    if (!STYLE_PARTS[part].includes(k)) { warnings.push(`${at}: unknown key '${k}', ignored`); continue }
    if (k === 'slice' && !(Array.isArray(v) && v.length === 4 && v.every((n) => typeof n === 'number' && n >= 0))) {
      errors.push(`${at}: slice is four numbers >= 0 (top, right, bottom, left)`); return false
    }
    if (k === 'image' && !assetPath(v)) { errors.push(`${at}: image must be a path under the layout's directory`); return false }
    if (STYLE_PARTS[k] && !stylePart(k, v, `${at}.${k}`, errors, warnings)) return false
  }
  return true
}
function styleOf (name, st, errors, warnings) {
  const at = `style '${name}'`
  if (!/^[a-z0-9_]{1,24}$/.test(name)) { errors.push(`${at}: a style name is 1..24 of [a-z0-9_]`); return null }
  return stylePart('', st, at, errors, warnings) ? st : null
}

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
    // `key:<name>` places one of the module's own keys (docs/KEYS.md): hidden
    // until the server's KEYS names it, so a layout may place keys a module
    // declares without knowing which a game has.
    const own = typeof b.id === 'string' && /^key:[a-z0-9_]{1,16}$/.test(b.id)
    if (!TOUCH_IDS.includes(b.id) && !own) { warnings.push(`${where}: no button '${b.id}' -- a layout places the engine's buttons and the module's keys (key:<name>), it cannot invent one`); continue }
    if (b.anchor !== undefined && !ANCHORS.includes(b.anchor)) { errors.push(`${where} button '${b.id}': anchor '${b.anchor}'`); continue }
    // A button absent from the list keeps its default; one named is merged.
    // A key's label is the module's (KEYS) unless the layout gives one.
    byId.set(b.id, { ...(byId.get(b.id) ?? { anchor: 'br', x: 0, y: 0, size: 74, label: own ? undefined : b.id.toUpperCase() }), ...b })
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
  // FIXED: a marker's box is sized by the SCREEN's scale, not its own size.
  if (typeof s.fixed === 'number') return s.fixed
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
  constructor ({ host, doc = globalThis.document, touch = false, insets = null, size = null, assetUrl = (f) => `${BASE}ui/${f}`, subtitlesOver = false } = {}) {
    this.host = host
    // Where a file the layout names is fetched from (docs/UI_STYLES.md 6.1).
    // No HUD element draws a /ui/ file yet; kept for when one does.
    this.assetUrl = assetUrl
    this.doc = doc
    this.touch = touch
    this.insets = insets
    this.size = size
    this.bindings = new Map()
    this.maxSeen = new Map()
    this.layout = withSubtitles(DEFAULT_LAYOUT)
    this.root = doc.createElement('div')
    this.root.className = 'dnp-hud'
    Object.assign(this.root.style, { position: 'absolute', inset: '0', pointerEvents: 'none', zIndex: '20' })
    host.appendChild(this.root)
    // SUBTITLES ARE OVER THE CURTAIN (z-index 40) and the letterbox (41), in
    // the game's own HUD (`subtitlesOver`): a line spoken in the dark is read,
    // as the studio's scene preview shows it. Their own layer, the same size
    // as the HUD's, so they place alike.
    this.over = this.root
    if (subtitlesOver) {
      this.over = doc.createElement('div')
      this.over.className = 'dnp-hud-over'
      Object.assign(this.over.style, { position: 'absolute', inset: '0', pointerEvents: 'none', zIndex: '42' })
      host.appendChild(this.over)
    }
    this.nodes = []
    this.#build()
  }

  // Every layout shows subtitles (withSubtitles), unless it places its own.
  setLayout (layout) { this.layout = withSubtitles(layout ?? DEFAULT_LAYOUT); this.#build() }

  /**
   * Elements hidden by id, whatever their `visible` rule says: a panel's decor
   * a script hid (UiView.showDecor). A group hidden hides what is in it.
   */
  setHidden (ids) {
    this.hidden = new Set(ids ?? [])
    this.#paint()
  }

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

  destroy () { this.root.remove(); this.over.remove() }

  #area () {
    if (this.size) return this.size
    const r = this.host.getBoundingClientRect?.()
    return r && r.width ? { width: r.width, height: r.height } : { width: globalThis.innerWidth ?? 0, height: globalThis.innerHeight ?? 0 }
  }

  #build () {
    this.root.textContent = ''
    this.over.textContent = ''
    this.nodes = []
    const theme = this.layout.theme ?? DEFAULT_LAYOUT.theme
    for (const el of this.layout.hud ?? []) {
      if (el.show === 'touch' && !this.touch) continue
      if (el.show === 'desktop' && this.touch) continue
      const node = this.#make(el, theme)
      node.dom.style.position = 'absolute'
      ;(el.id === 'subtitle' ? this.over : this.root).appendChild(node.dom)
      this.nodes.push(node)
    }
    this.#paint()
  }

  #make (el, theme) {
    const d = this.doc.createElement('div')
    const st = el.style ?? {}
    // `style.font` is a family (or a list) over the theme's; `style.shadow`
    // the text-shadow ("none" for none); and on text, numbers and groups,
    // `bg`, `radius` and `padding` draw a box behind it -- a subtitle's
    // backdrop (docs/UI_STYLES.md 2.3). A bar keeps its own.
    Object.assign(d.style, {
      font: `${st.weight ?? 400} ${st.size ?? 12}px/1.5 ${st.font ? `${st.font}, ${theme.font}` : theme.font}`,
      color: st.color ?? theme.text,
      textShadow: st.shadow ?? '0 1px 2px #000', whiteSpace: st.wrap ? 'normal' : 'pre', textAlign: st.align ?? 'left'
    })
    if (st.opacity !== undefined) d.style.opacity = String(st.opacity)
    if (el.type === 'text' || el.type === 'number' || el.type === 'group') {
      if (typeof st.bg === 'string') d.style.background = st.bg
      if (typeof st.radius === 'number') d.style.borderRadius = `${st.radius}px`
      if (typeof st.padding === 'number') d.style.padding = `${st.padding}px`
    }
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
      // The picture fills the element's box (its w x h), keeping its shape; at
      // its natural size it spilled over whatever stood beside it.
      Object.assign(img.style, { imageRendering: 'pixelated', width: '100%', height: '100%', objectFit: 'contain', display: 'block' })
      d.appendChild(img)
      node.img = img
    }
    return node
  }

  #paint () {
    const area = this.#area()
    for (const n of this.nodes) {
      this.#fill(n)
      const el = this.touch && n.el.id === 'subtitle' ? clearOfButtons(n.el, this.layout, area.width, area.height) : n.el
      // The width FIRST, then the height is measured: a line that wraps is
      // as tall as it is at the width it will have, not the one it had.
      const w = place(el, this.layout, area.width, area.height, this.insets ?? {}).width
      if (w) n.dom.style.width = `${w}px`
      const r = place(el, this.layout, area.width, area.height, this.insets ?? {}, { w: n.dom.offsetWidth || 0, h: n.dom.offsetHeight || 0 })
      Object.assign(n.dom.style, { left: `${r.left}px`, top: `${r.top}px` })
    }
  }

  #fill (n) {
    const el = n.el
    const show = isVisible(el, this.bindings) && !(this.hidden?.has(el.id))
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
      // A sprite picture of that name, else a texture of it (§3.3: "a sprite
      // frame key ... or a texture name") -- a texture painted in qPainter is
      // baked to /textures/, never to /sprites/.
      if (frame && n.img && n.img.dataset.frame !== frame) {
        n.img.dataset.frame = frame
        n.img.onerror = () => { if (n.img.dataset.frame === frame && !n.img.src.includes(`${BASE}textures/`)) n.img.src = `${BASE}textures/${frame}.png` }
        n.img.src = `${BASE}sprites/${frame}.png`
      }
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
