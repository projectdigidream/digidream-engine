//
// Panels a script opened (docs/protocol.md §11).
//
// A shop, a terminal, a dialogue, the debug panel: all one mechanism. The
// server declares what a panel contains and this draws it; the client never
// runs script, which is the property the whole engine rests on -- a client
// predicts with the same physics and never needs the server's gameplay code.
//
// DOM rather than WebGL. A panel is not in the world: it does not occlude, it
// is not lit, it does not move with the camera, and it wants text selection,
// focus and keyboard behaviour that a canvas would have to reimplement badly.
// (§5's world-anchored UI is a different thing and is not this.)
//
// Nothing here is predicted or hashed. What a script DOES in response -- a
// parameter change, a teleport -- travels by the mechanisms that already exist
// for those and is corrected exactly as they are.
//
import { UI_KIND } from '../proto.js'
import { place, sizeOf, uiScale, HudView, DEFAULT_LAYOUT } from './layout.js'

const STYLE = `
.dnp-ui-layer{position:fixed;inset:0;pointer-events:none;z-index:50;
  font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace}
.dnp-ui{position:absolute;top:16px;right:16px;min-width:260px;max-width:340px;
  pointer-events:auto;background:rgba(18,18,22,.94);color:#e8e8ea;
  border:1px solid #3a3a44;border-radius:6px;box-shadow:0 8px 32px rgba(0,0,0,.5)}
.dnp-ui + .dnp-ui{top:auto}
.dnp-ui-title{padding:8px 10px;border-bottom:1px solid #3a3a44;font-weight:600;
  display:flex;justify-content:space-between;align-items:center;gap:8px}
.dnp-ui-x{cursor:pointer;opacity:.6;padding:0 4px}
.dnp-ui-x:hover{opacity:1}
.dnp-ui-body{padding:8px 10px;display:flex;flex-direction:column;gap:8px}
.dnp-ui-row{display:flex;align-items:center;gap:8px;justify-content:space-between}
.dnp-ui-row label{flex:0 0 auto;opacity:.85}
.dnp-ui-row input[type=range]{flex:1 1 auto;min-width:80px}
.dnp-ui-val{flex:0 0 3.5em;text-align:right;opacity:.7;font-variant-numeric:tabular-nums}
.dnp-ui button{background:#2a2a34;color:#e8e8ea;border:1px solid #45454f;
  border-radius:4px;padding:4px 10px;cursor:pointer;font:inherit;width:100%}
.dnp-ui button:hover{background:#34343f}
.dnp-ui select{background:#2a2a34;color:#e8e8ea;border:1px solid #45454f;
  border-radius:4px;padding:3px 6px;font:inherit;flex:1 1 auto}
.dnp-ui-label{opacity:.65}
/* A TEMPLATED panel (docs/UI_LAYOUT.md 5): placed by the layout, not stacked
   top-right, and as wide as it says. */
.dnp-ui.dnp-ui-t{top:auto;right:auto;max-width:none;min-width:0}
.dnp-ui-stage{position:relative}
.dnp-ui-slot{position:absolute;z-index:1;box-sizing:border-box}
.dnp-ui-x.dnp-ui-x-free{position:absolute;top:4px;right:6px;z-index:2}
.dnp-ui-dim{position:absolute;inset:0;background:rgba(0,0,0,.45);pointer-events:auto}
`
// --- named styles (docs/UI_STYLES.md 2.2) ----------------------------------
//
// A 9-SLICE SKIN: the picture as border-image, its four insets (px of the
// picture) the slices, the middle filled and stretched; the border as wide as
// the slices, in UI units, scaled like every UI unit.
// Where a file the layout names is fetched from: the server's /ui/ route, or
// the view's `assetUrl` (docs/UI_STYLES.md 6.1; the studio's own copies).
export const UI_URL = (f) => `/ui/${f}`
function nineSlice (el, image, slice, scale, url = UI_URL) {
  const [t, r, b, l] = Array.isArray(slice) ? slice : [0, 0, 0, 0]
  el.style.borderStyle = 'solid'
  el.style.borderWidth = `${t * scale}px ${r * scale}px ${b * scale}px ${l * scale}px`
  el.style.borderImage = `url("${url(image)}") ${t} ${r} ${b} ${l} fill stretch`
}
// One part of a style (the panel box, the title, a button) on an element.
function skin (el, part, scale, url) {
  if (!part || typeof part !== 'object') return
  if (typeof part.bg === 'string') el.style.background = part.bg
  if (typeof part.color === 'string') el.style.color = part.color
  if (typeof part.border === 'string') el.style.border = part.border
  if (typeof part.radius === 'number') el.style.borderRadius = `${part.radius * scale}px`
  if (typeof part.padding === 'number') el.style.padding = `${part.padding * scale}px`
  if (typeof part.size === 'number') el.style.fontSize = `${part.size * scale}px`
  if (typeof part.weight === 'number') el.style.fontWeight = String(part.weight)
  if (typeof part.align === 'string') el.style.textAlign = part.align
  if (typeof part.font === 'string') el.style.fontFamily = part.font
  if (typeof part.image === 'string') nineSlice(el, part.image, part.slice, scale, url)
}

// The slot's own look, from the template. Colours and sizes only: a template
// is presentation, so nothing here can change what a widget does.
const SLOT_STYLE = { bg: 'background', color: 'color', border: 'border' }
function styleSlot (el, st, scale, url) {
  if (!st) return
  for (const [k, css] of Object.entries(SLOT_STYLE)) if (typeof st[k] === 'string') el.style[css] = st[k]
  if (typeof st.radius === 'number') el.style.borderRadius = `${st.radius * scale}px`
  if (typeof st.size === 'number') el.style.fontSize = `${st.size * scale}px`
  if (typeof st.weight === 'number') el.style.fontWeight = String(st.weight)
  if (typeof st.opacity === 'number') el.style.opacity = String(st.opacity)
  // A slot may also name a font and a 9-slice picture (docs/UI_STYLES.md 2.2).
  if (typeof st.font === 'string') el.style.fontFamily = st.font
  if (typeof st.image === 'string') nineSlice(el, st.image, st.slice, scale, url)
}

/**
 * Clamp a widget's value to what it declared.
 *
 * The server clamps too, and does not trust this (§11.3). Doing it here as well
 * is not redundancy for its own sake: it keeps the number ON SCREEN honest, so
 * a slider cannot show 200 while the server acts on 40 and the player is left
 * looking at a control that appears not to work.
 */
export function clampValue (widget, value) {
  const v = Number.isFinite(value) ? Math.round(value) : 0
  switch (widget.kind) {
    case UI_KIND.SLIDER: return Math.max(widget.lo, Math.min(widget.hi, v))
    case UI_KIND.TOGGLE: return v ? 1 : 0
    case UI_KIND.SELECT: {
      const n = widget.options?.length ?? 0
      return n === 0 ? 0 : Math.max(0, Math.min(n - 1, v))
    }
    case UI_KIND.BUTTON: return 1
    default: return v
  }
}

/** Draws panels and reports what the player does with them. */
export class UiView {
  /**
   * `onEvent(uiId, widgetId, value)` is called for every interaction. `doc` is
   * injectable so this can be tested without a browser -- the DOM it needs is
   * small and entirely standard.
   */
  /**
   * TEMPLATES are the layout's `panels` (docs/UI_LAYOUT.md 5): `layout` gives
   * them and the scale, `host` and `size` let the studio draw a panel inside a
   * device frame rather than the window.
   */
  constructor ({ onEvent, onChange = null, doc = globalThis.document, host = null, layout = null, size = null, insets = null, assetUrl = UI_URL }) {
    this.onEvent = onEvent
    this.assetUrl = assetUrl
    // Called after any panel opens or closes, however it closed (the script,
    // or the player's ×) -- what main.js needs to know whether one is modal.
    this.onChange = onChange
    this.doc = doc
    this.host = host
    this.size = size
    this.insets = insets
    this.layout = layout ?? DEFAULT_LAYOUT
    this.warned = new Set()
    this.panels = new Map()          // uiId -> { root, inputs, msg, dim, decor }
    this.layer = null
    // Widgets of the panel currently being built, and the control the player
    // has hold of. See set().
    this.pendingInputs = new Map()
    this.dragging = null
  }

  #layer () {
    if (this.layer) return this.layer
    const style = this.doc.createElement('style')
    style.textContent = STYLE
    this.doc.head.appendChild(style)
    this.layer = this.doc.createElement('div')
    this.layer.className = 'dnp-ui-layer'
    // In a host (the studio's device frame) the layer fills THAT, not the window.
    if (this.host) this.layer.style.position = 'absolute'
    ;(this.host ?? this.doc.body).appendChild(this.layer)
    return this.layer
  }

  /** A new layout: its templates and scale. Open panels are drawn again. */
  setLayout (layout) {
    this.layout = layout ?? DEFAULT_LAYOUT
    for (const p of [...this.panels.values()]) this.open(p.msg)
  }

  /** Is a modal panel open? Then the game's touch input is the panel's. */
  get modal () {
    for (const p of this.panels.values()) if (p.dim) return true
    return false
  }

  /** The window changed size: templated panels placed again. */
  relayout () {
    for (const p of this.panels.values()) if (p.place) p.place()
  }

  #area () {
    if (this.size) return this.size
    const r = this.host?.getBoundingClientRect?.()
    if (r && r.width) return { width: r.width, height: r.height }
    return { width: globalThis.innerWidth ?? 0, height: globalThis.innerHeight ?? 0 }
  }

  #template (name) {
    if (!name) return null
    const t = this.layout?.panels?.[name]
    // An unknown name draws the DEFAULT panel: a template is presentation, and
    // a server that names one this client does not have must still be usable.
    if (!t && !this.warned.has(name)) {
      this.warned.add(name)
      console.warn(`[ui] no panel template '${name}' in this layout; drawing the default panel`)
    }
    return t ?? null
  }

  /**
   * Show a panel. Opening one that is already open REPLACES it, so a script can
   * re-send a panel to update the values it shows without the player watching
   * two copies pile up.
   */
  open (msg) {
    this.close(msg.uiId)
    const tpl = this.#template(msg.template)
    if (tpl) { this.#openTemplated(msg, tpl); this.onChange?.(); return }
    const doc = this.doc
    const root = doc.createElement('div')
    root.className = 'dnp-ui'
    root.dataset.uiId = String(msg.uiId)

    const title = doc.createElement('div')
    title.className = 'dnp-ui-title'
    const t = doc.createElement('span')
    t.textContent = msg.title
    const x = doc.createElement('span')
    x.className = 'dnp-ui-x'
    x.textContent = '×'
    // Closing is LOCAL. The panel belongs to the script, which may well reopen
    // it; dismissing the window is not the same as telling the script the
    // player is done, and conflating them would make a stray click destroy
    // state the script is holding.
    x.onclick = () => this.close(msg.uiId)
    title.append(t, x)

    const body = doc.createElement('div')
    body.className = 'dnp-ui-body'
    // Collected as the widgets are built, so UI_SET can find one by id later.
    this.pendingInputs = new Map()
    for (const wgt of msg.widgets) body.appendChild(this.#widget(msg.uiId, wgt))

    root.append(title, body)
    this.#layer().appendChild(root)
    this.panels.set(msg.uiId, { root, inputs: this.pendingInputs, msg })
    this.pendingInputs = new Map()
    this.onChange?.()
  }

  // THE TEMPLATED PANEL. The same widgets, built by the same #widget -- so every
  // control a script sent is here and behaves as it always did -- placed into
  // the template's slots by script id. A widget the template does not place is
  // stacked in the body below the slots, as in a default panel: a template can
  // never lose a control. Slot coordinates are inside the panel's content area,
  // below the title bar.
  #openTemplated (msg, tpl) {
    const doc = this.doc
    const area = this.#area()
    const s = uiScale(this.layout, area.width, area.height)
    const root = doc.createElement('div')
    root.className = 'dnp-ui dnp-ui-t'
    root.dataset.uiId = String(msg.uiId)
    root.dataset.template = msg.template
    const pw = sizeOf(tpl.w ?? 320, s, area.width, area.height)
    root.style.width = `${pw}px`
    // THE PANEL'S STYLE: a named one (a string, or `{ use, ... }` with inline
    // values over it) and/or inline values as before. The named style's text
    // look is the panel's, inherited by every widget; its parts go on the
    // box, the title, the buttons and the ×. Inline values come after, so
    // they win; a slot's style comes after those, for its widget.
    const { named, inline } = this.#style(tpl.style)
    if (named) {
      if (typeof named.font === 'string') root.style.fontFamily = `${named.font}, ${this.layout?.theme?.font ?? 'ui-monospace, monospace'}`
      if (typeof named.size === 'number') root.style.fontSize = `${named.size * s}px`
      if (typeof named.color === 'string') root.style.color = named.color
      if (typeof named.weight === 'number') root.style.fontWeight = String(named.weight)
      if (typeof named.accent === 'string') root.style.accentColor = named.accent
      skin(root, named.panel, s, this.assetUrl)
    }
    styleSlot(root, inline, s, this.assetUrl)
    this.button = named?.button ?? null
    this.buildScale = s

    const closeable = tpl.closeable !== false
    const x = doc.createElement('span')
    x.className = 'dnp-ui-x'
    x.textContent = '×'
    x.onclick = () => this.close(msg.uiId)
    if (named?.close) {
      if (typeof named.close.color === 'string') x.style.color = named.close.color
      // A picture in place of the ×.
      if (typeof named.close.image === 'string') {
        x.textContent = ''
        Object.assign(x.style, { display: 'inline-block', width: '1.2em', height: '1.2em',
                                 background: `url("${this.assetUrl(named.close.image)}") center / contain no-repeat` })
      }
    }
    if (tpl.title?.show !== false) {
      const title = doc.createElement('div')
      title.className = 'dnp-ui-title'
      skin(title, named?.title, s, this.assetUrl)
      const t = doc.createElement('span')
      t.textContent = msg.title
      title.append(t)
      if (closeable) title.append(x)
      root.append(title)
    } else if (closeable) {
      x.className = 'dnp-ui-x dnp-ui-x-free'
      root.append(x)
    }

    this.pendingInputs = new Map()
    const byId = new Map(msg.widgets.map((w) => [w.id, w]))
    // A slot for a widget the script did not send is empty space; a second slot
    // for the same widget is ignored (one control, one place).
    const slotted = new Set()
    const slots = []
    for (const sl of tpl.slots ?? []) {
      if (!byId.has(sl.widget) || slotted.has(sl.widget)) continue
      slotted.add(sl.widget)
      slots.push(sl)
    }
    // The slot area's height: the template's, or enough for the slots placed
    // from the top (32 units a slot unless it says).
    const sh = tpl.h !== undefined
      ? sizeOf(tpl.h, s, area.width, area.height)
      : Math.max(0, ...slots.map((sl) => ((sl.y ?? 0) + (sl.h ?? 32) + 8) * s))
    const stage = doc.createElement('div')
    stage.className = 'dnp-ui-stage'
    stage.style.height = `${sh}px`
    let decor = null
    if ((tpl.decor ?? []).length) {
      // Decor is HUD elements, drawn by the HUD's own code, behind the widgets.
      decor = new HudView({ host: stage, doc, size: { width: pw, height: sh }, assetUrl: this.assetUrl })
      decor.root.style.zIndex = '0'
      decor.setLayout({ ...this.layout, hud: tpl.decor })
    }
    for (const sl of slots) {
      // A slot's style wins for its widget: over the style's button look too.
      this.slotStyle = sl.style ?? null
      const el = this.#widget(msg.uiId, byId.get(sl.widget))
      this.slotStyle = null
      el.className = `${el.className} dnp-ui-slot`
      const r = place({ anchor: 'tl', ...sl, h: sl.h ?? 32 }, this.layout, pw, sh, {}, { w: pw, h: 32 * s })
      Object.assign(el.style, { left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px` })
      styleSlot(el, sl.style, s, this.assetUrl)
      stage.appendChild(el)
    }
    if (slots.length || decor) root.append(stage)
    const rest = msg.widgets.filter((w) => !slotted.has(w.id))
    if (rest.length) {
      const body = doc.createElement('div')
      body.className = 'dnp-ui-body'
      for (const wgt of rest) body.appendChild(this.#widget(msg.uiId, wgt))
      root.append(body)
    }

    const layer = this.#layer()
    // MODAL: the world dimmed behind it, and (main.js, via `modal`) the game's
    // touch input held off while it is open.
    let dim = null
    if (tpl.modal) {
      dim = doc.createElement('div')
      dim.className = 'dnp-ui-dim'
      layer.appendChild(dim)
    }
    layer.appendChild(root)
    const placeIt = () => {
      const a = this.#area()
      const r = place({ anchor: 'c', ...tpl, w: undefined, h: undefined }, this.layout, a.width, a.height,
                      this.insets ?? {}, { w: root.offsetWidth || pw, h: root.offsetHeight || 0 })
      Object.assign(root.style, { left: `${r.left}px`, top: `${r.top}px` })
      // Now attached, text has a size: decor anchored right or bottom needs it.
      decor?.refresh()
    }
    placeIt()
    this.panels.set(msg.uiId, { root, inputs: this.pendingInputs, msg, dim, decor, place: placeIt })
    this.pendingInputs = new Map()
    this.button = null
  }

  // A template's `style`: { named, inline }. A name the layout lacks is the
  // default look, said once.
  #style (st) {
    if (st === undefined || st === null) return { named: null, inline: null }
    const name = typeof st === 'string' ? st : st.use
    let named = null
    if (name !== undefined) {
      named = this.layout?.styles?.[name] ?? null
      if (!named && !this.warned.has(`style:${name}`)) {
        this.warned.add(`style:${name}`)
        console.warn(`[ui] no style '${name}' in this layout; drawing the default look`)
      }
    }
    let inline = null
    if (typeof st === 'object') { inline = { ...st }; delete inline.use }
    return { named, inline }
  }

  #widget (uiId, wgt) {
    const doc = this.doc
    const row = doc.createElement('div')
    row.className = 'dnp-ui-row'
    const send = (v) => this.onEvent(uiId, wgt.id, clampValue(wgt, v))

    if (wgt.kind === UI_KIND.LABEL) {
      row.className = 'dnp-ui-label'
      row.textContent = wgt.label
      return row
    }
    if (wgt.kind === UI_KIND.BUTTON) {
      const b = doc.createElement('button')
      b.textContent = wgt.label
      b.onclick = () => send(1)
      // The style's button look, and its `pressed` look while held.
      const bs = this.button && (this.slotStyle ? { ...this.button, ...this.slotStyle } : this.button)
      if (bs) {
        const sc = this.buildScale ?? 1
        skin(b, bs, sc, this.assetUrl)
        if (bs.pressed) {
          const down = { ...bs, ...bs.pressed }
          b.onpointerdown = () => skin(b, down, sc, this.assetUrl)
          b.onpointerup = b.onpointerleave = () => skin(b, bs, sc, this.assetUrl)
        }
      }
      row.appendChild(b)
      return row
    }
    if (wgt.kind === UI_KIND.SLIDER) {
      const l = doc.createElement('label')
      l.textContent = wgt.label
      const i = doc.createElement('input')
      i.type = 'range'
      i.min = String(wgt.lo)
      i.max = String(wgt.hi)
      i.value = String(clampValue(wgt, wgt.value))
      const out = doc.createElement('span')
      out.className = 'dnp-ui-val'
      out.textContent = i.value
      i.dnpOut = out
      this.pendingInputs.set(wgt.id, i)
      // While a thumb is down, the server does not get to move it.
      i.onpointerdown = () => { this.dragging = i }
      i.onpointerup = () => { if (this.dragging === i) this.dragging = null }
      i.oninput = () => { out.textContent = i.value }
      // On CHANGE, not on input: a drag across a range emits one event per
      // pixel, and each one is a reliable control message and a script
      // callback. The number under the thumb still follows the drag.
      i.onchange = () => send(Number(i.value))
      row.append(l, i, out)
      return row
    }
    if (wgt.kind === UI_KIND.TOGGLE) {
      const l = doc.createElement('label')
      l.textContent = wgt.label
      const i = doc.createElement('input')
      i.type = 'checkbox'
      i.checked = wgt.value !== 0
      this.pendingInputs.set(wgt.id, i)
      i.onchange = () => send(i.checked ? 1 : 0)
      row.append(l, i)
      return row
    }
    if (wgt.kind === UI_KIND.SELECT) {
      const l = doc.createElement('label')
      l.textContent = wgt.label
      const sel = doc.createElement('select')
      ;(wgt.options ?? []).forEach((name, idx) => {
        const o = doc.createElement('option')
        o.value = String(idx)
        o.textContent = name
        sel.appendChild(o)
      })
      sel.value = String(clampValue(wgt, wgt.value))
      this.pendingInputs.set(wgt.id, sel)
      sel.onchange = () => send(Number(sel.value))
      row.append(l, sel)
      return row
    }
    // An unknown kind never reaches here -- the decoder refuses the whole
    // message rather than pass one on -- but a renderer that silently drew
    // nothing would be the wrong way to find that out.
    row.textContent = `${wgt.label} (unsupported widget ${wgt.kind})`
    return row
  }

  /**
   * Change one widget's value in a panel that is already open.
   *
   * Updates what is SHOWN and nothing else -- it does not send an event back,
   * because the server is the one that just said this. A slider the player is
   * currently dragging is left alone: having a number jump out from under a
   * thumb is worse than being briefly out of date, and the server's value will
   * arrive again.
   */
  set (uiId, widgetId, value) {
    const p = this.panels.get(uiId)
    const el = p?.inputs.get(widgetId)
    if (!el) return false
    if (el === this.dragging) return false
    if (el.type === 'checkbox') el.checked = value !== 0
    else el.value = String(value)
    if (el.dnpOut) el.dnpOut.textContent = String(value)
    // Remembered, so a panel drawn again (a new layout) shows it.
    const w = p.msg?.widgets.find((x) => x.id === widgetId)
    if (w) w.value = value
    return true
  }

  close (uiId) {
    const p = this.panels.get(uiId)
    if (!p) return false
    p.root.remove()
    p.dim?.remove()
    p.decor?.destroy()
    this.panels.delete(uiId)
    this.onChange?.()
    return true
  }

  /** Every panel gone -- used when the session ends or the map changes. */
  clear () {
    for (const id of [...this.panels.keys()]) this.close(id)
  }

  get count () { return this.panels.size }
}
