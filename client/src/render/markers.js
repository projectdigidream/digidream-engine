//
// MARKERS (docs/MARKERS.md): small UIs hung on things in the world -- a health
// bar over a monster, a sign over a door, a pointer over a goal -- each drawn
// from a template in the module's UI layout (`markers`) and placed, every
// frame, over the point it hangs on.
//
// THE HUD'S OWN ELEMENTS. A marker is a box of text, number, bar, image and
// group elements, built and filled by HudView itself with the marker's values
// as its bindings -- so a marker's bar is the HUD's bar, styled the same way,
// and a layout author learns one thing. (The design's "world-anchored UI, a
// separate non-interactive instance of the same template mechanism".)
//
// NOT INTERACTIVE, and DOM rather than in the scene: text stays crisp at any
// distance and costs no draw call, and nothing here is ever clicked.
//
// Hidden when the point is behind the camera, off the screen, further than the
// marker's range from the viewer, behind a wall (unless it says it draws
// through them; asked of the map, a few markers a frame), a debug marker while
// the debug overlay is off, or hung on an entity this client is not drawing.
//
import { HudView, uiScale, DEFAULT_MARKERS } from './layout.js'

export const MARK_DEBUG = 1
export const MARK_THROUGH = 2
// How many markers ask the map whether a wall hides them, each frame. A
// verdict holds until the marker's turn comes round again.
const OCCLUSION_PER_FRAME = 24

export class MarkerView {
  /**
   * @param host    the element the markers' layer goes in (document.body)
   * @param doc     the document (injected, as HudView's is)
   */
  constructor ({ host, doc = globalThis.document } = {}) {
    this.doc = doc
    this.layer = doc.createElement('div')
    this.layer.className = 'dnp-markers'
    Object.assign(this.layer.style, { position: 'absolute', inset: '0', pointerEvents: 'none', zIndex: '15', overflow: 'hidden' })
    host.appendChild(this.layer)
    this.layout = null
    this.marks = new Map()      // id -> { m, box, hud, template, hidden, occluded }
    this.cursor = 0             // round robin of the occlusion test
    this.warned = new Set()
  }

  /** The layout whose `markers` templates draw them. Rebuilds every marker. */
  setLayout (layout) {
    this.layout = layout
    for (const k of this.marks.values()) this.#build(k)
  }

  /** MARK_SET: add one, or replace it whole by id. */
  set (m) {
    let k = this.marks.get(m.id)
    if (!k) {
      k = { m, box: null, hud: null, template: null, occluded: false }
      this.marks.set(m.id, k)
    }
    const rebuild = !k.box || k.m.template !== m.template
    // A replacement is WHOLE: a value it no longer has is cleared, not kept
    // from before (HudView merges what it is given).
    const values = { ...(m.values ?? {}) }
    for (const name of Object.keys(k.m.values ?? {})) if (!(name in values)) values[name] = undefined
    k.m = m
    if (rebuild) this.#build(k)
    else if (k.hud) k.hud.update(values)
  }

  /** MARK_CLEAR. */
  clear (id) {
    const k = this.marks.get(id)
    if (!k) return
    k.box?.remove()
    this.marks.delete(id)
  }

  /** Everything: a new map, a new world. */
  clearAll () {
    for (const k of this.marks.values()) k.box?.remove()
    this.marks.clear()
  }

  get count () { return this.marks.size }

  #template (name) {
    const t = this.layout?.markers?.[name] ?? DEFAULT_MARKERS[name]
    if (!t && !this.warned.has(name)) {
      this.warned.add(name)
      console.warn(`[markers] no marker template '${name}' in the layout; its markers are not drawn`)
    }
    return t ?? null
  }

  #build (k) {
    k.box?.remove()
    k.box = null
    k.hud = null
    const t = this.#template(k.m.template)
    k.template = t
    if (!t) return
    const box = this.doc.createElement('div')
    Object.assign(box.style, { position: 'absolute', left: '0', top: '0', display: 'none' })
    this.layer.appendChild(box)
    k.box = box
    k.scale = null
    k.hud = null
  }

  // The HudView inside a box, at the screen's scale: built when the box is
  // first shown (or the scale changes), so a thousand markers off screen cost
  // nothing.
  #ensureHud (k, scale) {
    if (k.hud && k.scale === scale) return
    k.scale = scale
    const t = k.template
    const w = t.w * scale
    const h = t.h * scale
    Object.assign(k.box.style, { width: `${w}px`, height: `${h}px` })
    k.box.textContent = ''
    k.hud = new HudView({ host: k.box, doc: this.doc, size: { width: w, height: h } })
    k.hud.setLayout({ version: 1, scale: { fixed: scale }, theme: this.layout?.theme ?? {}, hud: t.elements })
    // HudView's own layer sits under the HUD's; a marker's is only its box.
    k.hud.root.style.zIndex = '0'
    k.hud.update(k.m.values ?? {})
  }

  /**
   * One frame.
   * @param width, height  the canvas, CSS px
   * @param project(p)     a three-space point -> { x, y } CSS px, or null when
   *                       it is behind the camera
   * @param anchorOf(m)    the three-space point the marker hangs from (the top
   *                       of its entity's body, or its place), or null
   * @param viewer         the three-space point range is measured from
   * @param debug          whether the debug overlay is showing
   * @param lineClear(p)   whether the map leaves the camera's line to p clear
   */
  update ({ width, height, project, anchorOf, viewer, debug = false, lineClear = null }) {
    const scale = uiScale(this.layout, width, height)
    const ids = [...this.marks.keys()]
    let asked = 0
    for (let i = 0; i < ids.length; i++) {
      const k = this.marks.get(ids[(this.cursor + i) % ids.length])
      if (!k.box) continue
      const m = k.m
      let p = null
      if (!(m.flags & MARK_DEBUG) || debug) p = anchorOf(m)
      if (p && m.range > 0n && viewer) {
        const r = Number(m.range) / 4294967296
        const dx = p.x - viewer.x; const dy = p.y - viewer.y; const dz = p.z - viewer.z
        if (dx * dx + dy * dy + dz * dz > r * r) p = null
      }
      const s = p ? project(p) : null
      if (s && (s.x < -200 || s.y < -200 || s.x > width + 200 || s.y > height + 200)) p = null
      if (p && s && lineClear && !(m.flags & MARK_THROUGH) && asked < OCCLUSION_PER_FRAME) {
        asked++
        k.occluded = !lineClear(p)
      }
      if (!p || !s || (k.occluded && !(m.flags & MARK_THROUGH))) {
        if (k.box.style.display !== 'none') k.box.style.display = 'none'
        continue
      }
      // SHOWN BEFORE IT IS PAINTED: HudView measures its text as it paints,
      // and text in a hidden box measures 0 tall -- a label anchored to the
      // bottom was then drawn under its box. And measured again on every
      // return from hidden, for what changed while it could not be measured.
      const wasHidden = k.box.style.display !== 'block'
      if (wasHidden) k.box.style.display = 'block'
      const fresh = !k.hud || k.scale !== scale
      this.#ensureHud(k, scale)
      if (wasHidden && !fresh) k.hud.refresh()
      const t = k.template
      const left = s.x - (t.w * scale) / 2
      const top = s.y - t.h * scale
      k.box.style.transform = `translate(${left.toFixed(1)}px, ${top.toFixed(1)}px)`
      // Nearer in front.
      if (viewer) {
        const d = Math.hypot(p.x - viewer.x, p.y - viewer.y, p.z - viewer.z)
        k.box.style.zIndex = String(Math.max(0, 100000 - Math.round(d * 10)))
      }
    }
    if (ids.length) this.cursor = (this.cursor + OCCLUSION_PER_FRAME) % ids.length
  }
}
