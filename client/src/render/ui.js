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
`

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
  constructor ({ onEvent, doc = globalThis.document }) {
    this.onEvent = onEvent
    this.doc = doc
    this.panels = new Map()          // uiId -> { root, widgets }
    this.layer = null
  }

  #layer () {
    if (this.layer) return this.layer
    const style = this.doc.createElement('style')
    style.textContent = STYLE
    this.doc.head.appendChild(style)
    this.layer = this.doc.createElement('div')
    this.layer.className = 'dnp-ui-layer'
    this.doc.body.appendChild(this.layer)
    return this.layer
  }

  /**
   * Show a panel. Opening one that is already open REPLACES it, so a script can
   * re-send a panel to update the values it shows without the player watching
   * two copies pile up.
   */
  open (msg) {
    this.close(msg.uiId)
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
    for (const wgt of msg.widgets) body.appendChild(this.#widget(msg.uiId, wgt))

    root.append(title, body)
    this.#layer().appendChild(root)
    this.panels.set(msg.uiId, { root })
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

  close (uiId) {
    const p = this.panels.get(uiId)
    if (!p) return false
    p.root.remove()
    this.panels.delete(uiId)
    return true
  }

  /** Every panel gone -- used when the session ends or the map changes. */
  clear () {
    for (const id of [...this.panels.keys()]) this.close(id)
  }

  get count () { return this.panels.size }
}
