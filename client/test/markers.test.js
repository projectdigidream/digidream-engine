//
// MARKERS (render/markers.js, docs/MARKERS.md): what the server encodes is what
// this client decodes; a marker is drawn over the point it hangs on, from its
// template, with its values; and it is hidden for every reason it should be.
//
import { Core } from '../src/core.js'
import { decodeControlMessage, MSG } from '../src/proto.js'
import { MarkerView, MARK_DEBUG, MARK_THROUGH } from '../src/render/markers.js'
import { parseLayout, DEFAULT_LAYOUT } from '../src/render/layout.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const FX = (n) => BigInt(Math.round(n * 4294967296))

// --- the wire: the core's encoder, this client's decoder ------------------------
console.log('the wire')
{
  const core = await Core.load()
  const M = core.M
  const frame = (point) => {
    const ptr = M._malloc(512)
    const n = M._dnp_test_mark_frame(ptr, 512, point)
    const bytes = M.HEAPU8.slice(ptr, ptr + n)
    M._free(ptr)
    return decodeControlMessage(bytes)
  }
  const e = frame(0)
  check(e && e.type === MSG.MARK_SET && e.id === 4242 && e.anchor === 0 && e.handle === ((5 << 24) | 77) >>> 0,
        `on an entity: id, anchor and handle (${JSON.stringify({ id: e?.id, anchor: e?.anchor, handle: e?.handle })})`)
  check(e && e.template === 'health' && e.flags === 3 && e.height === FX(0.25) && e.range === FX(30),
        'its template, flags, height and range')
  check(e && e.values.value === -12 && e.values.max === 300 && e.values.text === 'Mill',
        `and its values, ints and text (${JSON.stringify(e?.values)})`)
  const p = frame(1)
  check(p && p.anchor === 1 && p.pos.x === FX(-3) && p.pos.y === FX(2.5) && p.pos.z === FX(7),
        'at a place: the point, exact')
  // CONTROL: a cut message is refused, not half-read.
  const ptr = M._malloc(512)
  const n = M._dnp_test_mark_frame(ptr, 512, 0)
  const bytes = M.HEAPU8.slice(ptr, ptr + n)
  M._free(ptr)
  check(decodeControlMessage(bytes.slice(0, n - 2)) === null, 'CONTROL: a message cut short is refused')
}

// --- drawn: a fake document, as layout.test.js uses -------------------------------
// Text MEASURES as a browser's does: 14 px tall while it and everything above
// it is shown, 0 while anything is display:none -- which is what made a label
// drawn while hidden sit below its box.
function fakeDoc () {
  const shown = (n) => { for (let x = n; x; x = x.parent) if (x.style.display === 'none') return false; return true }
  const make = (tag) => {
    const n = {
      tag, style: {}, dataset: {}, children: [], className: '',
      get offsetWidth () { return this._t && shown(this) ? 8 * this._t.length : 0 },
      get offsetHeight () { return this._t && shown(this) ? 14 : 0 },
      appendChild (c) { this.children.push(c); c.parent = this; return c },
      remove () { if (this.parent) this.parent.children = this.parent.children.filter((x) => x !== this) },
      getBoundingClientRect () { return { width: 0, height: 0 } }
    }
    Object.defineProperty(n, 'textContent', {
      get () { return this._t ?? '' },
      set (v) { this._t = v; if (v === '') this.children = [] }
    })
    return n
  }
  return { createElement: make }
}
// Every text a box holds, depth first.
const texts = (n, out = []) => {
  if (n._t) out.push(n._t)
  for (const c of n.children) texts(c, out)
  return out
}

console.log('drawn')
{
  const doc = fakeDoc()
  const host = doc.createElement('body')
  const view = new MarkerView({ host, doc })
  view.setLayout(DEFAULT_LAYOUT)
  const W = 800
  const H = 600
  // A camera that maps three-space (x, y) straight to the screen and z < 0 as
  // behind it: enough to say where a box goes and when it does not.
  const project = (p) => (p.z < 0 ? null : { x: 400 + p.x * 10, y: 300 - p.y * 10 })
  const points = new Map()
  const anchorOf = (m) => points.get(m.id) ?? null
  let clear = true
  const frame = (over = {}) => view.update({ width: W, height: H, project, anchorOf, viewer: { x: 0, y: 0, z: 0 },
                                            debug: false, lineClear: () => clear, ...over })
  const box = (id) => view.marks.get(id)?.box

  view.set({ id: 1, anchor: 0, handle: 9, template: 'label', flags: 0, height: 0n, range: 0n, values: { text: 'Exit' } })
  points.set(1, { x: 5, y: 2, z: 1 })
  frame()
  const b = box(1)
  // label is 220 x 24 units at the default scale of 1: its bottom-centre on (450, 280).
  check(b && b.style.display === 'block' && b.style.transform === 'translate(340.0px, 256.0px)',
        `a label, its bottom-centre over the point (${b?.style.transform})`)
  check(texts(b).includes('Exit'), `drawing its value (${texts(b)})`)
  // Its text where the template puts it: anchored to the box's bottom, so its
  // top is the box's height less its own (24 - 14), not on the bottom edge --
  // measured while the box was hidden it was 0 tall and drawn under the box.
  const textNode = (n) => n._t === 'Exit' ? n : n.children.map(textNode).find(Boolean)
  check(textNode(b)?.style.top === '10px', `inside its box, measured shown (top ${textNode(b)?.style.top})`)

  // A replacement is whole: the text it no longer has is gone.
  view.set({ id: 1, anchor: 0, handle: 9, template: 'label', flags: 0, height: 0n, range: 0n, values: {} })
  frame()
  check(!texts(box(1)).includes('Exit'), 'replaced, a value it no longer carries is not drawn')

  // Hidden: behind the camera, beyond its range, behind a wall, or not drawn.
  points.set(1, { x: 5, y: 2, z: -1 })
  frame()
  check(box(1).style.display === 'none', 'behind the camera, hidden')
  points.set(1, { x: 5, y: 2, z: 1 })
  view.set({ id: 1, anchor: 0, handle: 9, template: 'label', flags: 0, height: 0n, range: FX(3), values: { text: 'x' } })
  frame()
  check(box(1).style.display === 'none', 'further than its range, hidden')
  view.set({ id: 1, anchor: 0, handle: 9, template: 'label', flags: 0, height: 0n, range: FX(30), values: { text: 'x' } })
  frame()
  check(box(1).style.display === 'block', 'CONTROL: within it, shown')
  clear = false
  frame()
  check(box(1).style.display === 'none', 'a wall between it and the camera, hidden')
  view.set({ id: 1, anchor: 0, handle: 9, template: 'label', flags: MARK_THROUGH, height: 0n, range: 0n, values: { text: 'x' } })
  frame()
  check(box(1).style.display === 'block', 'unless it draws through walls')
  clear = true
  points.delete(1)
  frame()
  check(box(1).style.display === 'none', 'hung on something this client is not drawing, hidden')

  // DEBUG: only with the overlay.
  view.set({ id: 2, anchor: 0, handle: 11, template: 'debug_health', flags: MARK_DEBUG, height: 0n, range: 0n, values: { value: 30, max: 50 } })
  points.set(2, { x: 0, y: 0, z: 1 })
  frame({ debug: false })
  check(box(2).style.display === 'none', 'a debug marker, the overlay off: hidden')
  frame({ debug: true })
  check(box(2).style.display === 'block' && texts(box(2)).includes('30/50'),
        `the overlay on: shown, "${texts(box(2))}"`)

  // A health bar's fill is the value's share of the max.
  view.set({ id: 3, anchor: 0, handle: 12, template: 'health', flags: 0, height: 0n, range: 0n, values: { value: 25, max: 100 } })
  points.set(3, { x: 1, y: 1, z: 1 })
  frame()
  const fill = (n) => n.style.width?.endsWith('%') && n.style.height === '100%' ? n : n.children.map(fill).find(Boolean)
  check(fill(box(3))?.style.width === '25.0%', `a bar a quarter full (${fill(box(3))?.style.width})`)

  // A module's own template, from its layout.
  const own = parseLayout({ markers: { boss: { w: 100, h: 10, elements: [{ id: 't', type: 'text', anchor: 'c', text: 'BOSS {value}' }] } } })
  view.setLayout(own.layout)
  view.set({ id: 4, anchor: 0, handle: 13, template: 'boss', flags: 0, height: 0n, range: 0n, values: { value: 3 } })
  points.set(4, { x: 0, y: 0, z: 1 })
  frame()
  check(texts(box(4)).includes('BOSS 3'), `a module's own template (${texts(box(4))})`)
  // CONTROL: a template nobody defined draws nothing.
  const warned = console.warn
  let said = 0
  console.warn = () => { said++ }
  view.set({ id: 5, anchor: 0, handle: 14, template: 'nobody', flags: 0, height: 0n, range: 0n, values: {} })
  console.warn = warned
  check(!box(5) && said === 1, 'CONTROL: a template nobody has is not drawn, and said once')

  // Taken down, and all of them.
  // Changed while hidden, then shown again: measured again, not left as it
  // was drawn while it could not be measured.
  view.set({ id: 6, anchor: 0, handle: 15, template: 'label', flags: 0, height: 0n, range: 0n, values: { text: 'Exit' } })
  points.set(6, { x: 0, y: 0, z: -1 })
  frame()
  view.set({ id: 6, anchor: 0, handle: 15, template: 'label', flags: 0, height: 0n, range: 0n, values: { text: 'Exit' } })
  points.set(6, { x: 0, y: 0, z: 1 })
  frame()
  check(textNode(box(6))?.style.top === '10px', `first drawn while hidden, placed right once shown (top ${textNode(box(6))?.style.top})`)

  view.clear(1)
  check(!view.marks.has(1) && !view.layer.children.includes(b), 'cleared, its box is gone')
  view.clearAll()
  check(view.count === 0 && view.layer.children.length === 0, 'and all of them, on a new map')
}

console.log(`\nmarkers: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
