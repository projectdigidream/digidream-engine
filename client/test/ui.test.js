//
// Panels, from the server's bytes to the DOM and back (docs/protocol.md §11).
//
// The client's decoder is a SECOND implementation of this wire format, in a
// different language, kept in step with the C++ one only by both having been
// written from the same spec. That is not a mechanism, so this test decodes
// real bytes produced by core/tools/ui_fixture.cpp rather than by a JS encoder
// agreeing with itself.
//
// The renderer is exercised against a minimal DOM. What is under test is which
// element a widget becomes, what it sends, and when -- not how it looks.
//
import { execFileSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { decodeControlMessage, MSG, UI_KIND } from '../src/proto.js'
import { UiView, clampValue } from '../src/render/ui.js'
import { parseLayout } from '../src/render/layout.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

console.log('ui: a panel travels from the server and becomes controls')

// --- the wire, against the C++ encoder ---------------------------------------

function fixturePath () {
  for (const a of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', a, 'ui_fixture')
    if (existsSync(p)) return p
  }
  return null
}

const gen = fixturePath()
if (!gen) {
  console.log('  skip (no ui_fixture; run scripts/build_native.sh)')
  process.exit(0)
}

const bytes = execFileSync(gen, { maxBuffer: 1 << 20 })
const msg = decodeControlMessage(bytes)
check(msg !== null, 'the C++ encoder produces something this decoder accepts')
check(msg?.type === MSG.UI_OPEN, 'and it is a UI_OPEN')
check(msg?.title === 'Debug panel', `with its title intact (${msg?.title})`)
check(msg?.widgets.length === 4, `and all four widgets (${msg?.widgets.length})`)
check(msg?.widgets[1].kind === UI_KIND.SLIDER &&
      msg?.widgets[1].lo === 1 && msg?.widgets[1].hi === 40 && msg?.widgets[1].value === 8,
      'a slider keeps its range and value across the wire')
check(msg?.widgets[3].options.join(',') === 'MAP01,MAP02,MAP03',
      'and a select keeps its options')

// Truncation is refused rather than half-decoded. A control message cut short
// in transit must not become a shorter panel with plausible contents.
{
  let refused = 0
  for (let n = 5; n < bytes.length; n++) {
    if (decodeControlMessage(bytes.subarray(0, n)) === null) refused++
  }
  check(refused === bytes.length - 5,
        `every truncation is refused (${refused} of ${bytes.length - 5})`)
}

// --- clamping ----------------------------------------------------------------
//
// The server clamps too and does not trust the client (§11.3). Doing it here as
// well keeps the number ON SCREEN honest: without it a slider can show 200
// while the server acts on 40, and the player is looking at a control that
// appears not to work.

const slider = { kind: UI_KIND.SLIDER, lo: 1, hi: 40 }
check(clampValue(slider, 999) === 40 && clampValue(slider, -5) === 1,
      'a slider value is clamped to its declared range')
check(clampValue({ kind: UI_KIND.TOGGLE }, 7) === 1, 'a toggle is 0 or 1')
check(clampValue({ kind: UI_KIND.SELECT, options: ['a', 'b'] }, 9) === 1,
      'a select index is clamped to its option list')
check(clampValue({ kind: UI_KIND.SELECT, options: [] }, 3) === 0,
      'and an empty option list yields 0 rather than an index into nothing')

// --- the DOM -----------------------------------------------------------------

function makeDom () {
  const mk = (tag) => {
    const el = {
      tag, children: [], dataset: {}, style: {}, className: '', textContent: '',
      appendChild (c) { this.children.push(c); c.parent = this; return c },
      append (...cs) { for (const c of cs) this.appendChild(c) },
      remove () {
        const p = this.parent
        if (p) p.children.splice(p.children.indexOf(this), 1)
      }
    }
    return el
  }
  const doc = {
    head: mk('head'),
    body: mk('body'),
    createElement: mk
  }
  return doc
}

const all = (el, out = []) => {
  out.push(el)
  for (const c of el.children) all(c, out)
  return out
}

{
  const doc = makeDom()
  const sent = []
  const view = new UiView({ onEvent: (...a) => sent.push(a), doc })
  view.open(msg)
  check(view.count === 1, 'opening a panel shows one panel')

  const els = all(doc.body)
  check(els.some((e) => e.tag === 'input' && e.type === 'range'), 'a slider is a range input')
  check(els.some((e) => e.tag === 'select'), 'a select is a select')
  check(els.some((e) => e.tag === 'input' && e.type === 'checkbox'), 'a toggle is a checkbox')

  // A slider reports on CHANGE, not on input: a drag emits one event per pixel,
  // and every one is a reliable control message and a script callback.
  const range = els.find((e) => e.type === 'range')
  range.value = '25'
  range.oninput()
  check(sent.length === 0, 'dragging a slider sends nothing while it moves')
  range.onchange()
  check(sent.length === 1 && sent[0][2] === 25, `and one event when released (${sent[0]?.[2]})`)

  // Out of range from the DOM is still clamped on the way out, so a tampered
  // control cannot send the script something the widget never offered.
  range.value = '9999'
  range.onchange()
  check(sent[1]?.[2] === 40, `a slider cannot send past its own maximum (${sent[1]?.[2]})`)

  const sel = els.find((e) => e.tag === 'select')
  sel.value = '2'
  sel.onchange()
  check(sent[2]?.[1] === 4 && sent[2]?.[2] === 2,
        'a select sends its index against the widget id the script chose')

  // Reopening replaces rather than stacks, so a script can refresh the values a
  // panel shows without the player watching copies pile up.
  view.open(msg)
  check(view.count === 1, 'reopening the same id replaces it')

  check(view.close(msg.uiId) === true && view.count === 0, 'closing removes it')
  check(view.close(msg.uiId) === false, 'and closing it twice is a no-op')
}

// --- templates (docs/UI_LAYOUT.md 5) ----------------------------------------------
//
// On the wire: the same panel from the C++ encoder with a template, and the one
// cut a trailing field cannot tell apart.
{
  const tb = execFileSync(gen, ['template'], { maxBuffer: 1 << 20 })
  const tm = decodeControlMessage(tb)
  check(tm?.template === 'shop' && tm?.widgets.length === 4, `a template survives the wire (${tm?.template})`)
  check(msg.template === null, 'CONTROL: and a panel without one decodes with none')
  check(tb.length === bytes.length + 2 + 4 && tb.subarray(4, bytes.length).equals(bytes.subarray(4)),
        'the template only APPENDS: the rest is the old panel byte for byte')
  const accepted = []
  for (let n = 5; n < tb.length; n++) if (decodeControlMessage(tb.subarray(0, n)) !== null) accepted.push(n)
  check(accepted.length === 1 && accepted[0] === bytes.length,
        `every cut is refused but the end of the widgets, which is the panel without it (${accepted.join(',')})`)
  const bad = Buffer.from(tb)
  bad[bad.length - 1] = 'P'.charCodeAt(0)
  check(decodeControlMessage(bad) === null, 'a name outside [a-z0-9_] is refused')
}

const TEMPLATES = parseLayout({
  scale: { reference: 390, min: 1, max: 1 },
  panels: {
    shop: {
      anchor: 'c', w: 300, h: 120, modal: true,
      slots: [
        { widget: 2, anchor: 'tl', x: 12, y: 10, w: 140, style: { bg: '#2a4' } },
        { widget: 4, anchor: 'tr', x: -12, y: 10, w: 120 },
        { widget: 99, x: 0, y: 0 }
      ],
      decor: [{ id: 'coin', type: 'text', text: 'gold', anchor: 'br', x: -8, y: -8 }]
    },
    bare: { anchor: 'tl', x: 20, y: 30, w: 200, closeable: false, title: { show: false } }
  }
})
check(TEMPLATES.ok, `a layout with two templates parses (${TEMPLATES.errors.join('; ')})`)
const shape = (el) => `${el.tag}.${el.className}[${el.textContent ?? ''}](${el.children.map(shape).join(',')})`
const rootOf = (doc) => all(doc.body).filter((e) => /(^| )dnp-ui( |$)/.test(e.className)).at(-1)

{
  // Without a template, a layout with templates in it changes NOTHING.
  const d0 = makeDom()
  new UiView({ onEvent: () => {}, doc: d0 }).open(msg)
  const d1 = makeDom()
  new UiView({ onEvent: () => {}, doc: d1, layout: TEMPLATES.layout, size: { width: 800, height: 600 } }).open(msg)
  check(shape(d0.body) === shape(d1.body), 'a panel naming no template draws exactly as before')
  const d2 = makeDom()
  const warn = console.warn
  let warned = ''
  console.warn = (s) => { warned += s }
  new UiView({ onEvent: () => {}, doc: d2, layout: TEMPLATES.layout }).open({ ...msg, template: 'nope' })
  console.warn = warn
  check(shape(d0.body) === shape(d2.body) && /nope/.test(warned), 'and one naming a template this layout lacks draws the default, said once')
}
{
  const doc = makeDom()
  let changes = 0
  const view = new UiView({ onEvent: () => {}, onChange: () => changes++, doc, layout: TEMPLATES.layout, size: { width: 800, height: 600 } })
  view.open({ ...msg, template: 'shop' })
  const root = rootOf(doc)
  check(/dnp-ui-t/.test(root.className) && root.style.width === '300px', `the template's panel, as wide as it says (${root.style.width})`)
  check(root.style.left === '250px', `centred: the c anchor (${root.style.left})`)
  const slots = all(root).filter((e) => /dnp-ui-slot/.test(e.className))
  check(slots.length === 2, `the two widgets it names are slotted; a slot for one never sent is not (${slots.length})`)
  const slider = slots.find((e) => e.children.some((c) => c.type === 'range'))
  check(slider && slider.style.left === '12px' && slider.style.top === '10px' && slider.style.width === '140px',
        `widget 2 at its slot (${slider?.style.left}, ${slider?.style.top})`)
  check(slider?.style.background === '#2a4', 'with the slot\'s style')
  const sel = slots.find((e) => e.children.some((c) => c.tag === 'select'))
  check(sel && sel.style.left === `${300 - 12 - 120}px`, `widget 4 from the right edge (${sel?.style.left})`)
  const body = all(root).find((e) => e.className === 'dnp-ui-body')
  check(body && body.children.length === 2, `the two it does not place are stacked in the body: none lost (${body?.children.length})`)
  check(all(root).some((e) => e.textContent === 'gold'), 'decor is drawn, by the HUD\'s own code')
  check(view.modal && all(doc.body).some((e) => e.className === 'dnp-ui-dim'), 'a modal template dims the world and says so')
  check(changes >= 1, 'and the change is reported')
  view.set(msg.uiId, 2, 33)
  view.setLayout(TEMPLATES.layout)
  const again = all(rootOf(doc)).find((e) => e.type === 'range')
  check(again?.value === '33', `drawn again, a value the server set since is kept (${again?.value})`)
  const x = all(rootOf(doc)).find((e) => /dnp-ui-x/.test(e.className))
  x.onclick()
  check(!view.modal && !all(doc.body).some((e) => e.className === 'dnp-ui-dim'), 'closed by its ×, the dim goes and modal is over')

  view.open({ ...msg, uiId: 2, template: 'bare' })
  const bare = rootOf(doc)
  check(!all(bare).some((e) => /dnp-ui-x|dnp-ui-title/.test(e.className)), 'no title and not closeable: neither is drawn')
  check(bare.style.left === '20px' && bare.style.top === '30px' && !view.modal, 'placed tl, and not modal')
}
{
  const bad = parseLayout({ panels: { Shop: {}, ok: { anchor: 'middle' }, s2: { slots: [{ anchor: 'tl' }] } } })
  check(!bad.ok && bad.errors.length === 3 && Object.keys(bad.layout.panels).length === 0,
        `CONTROL: a bad name, anchor or slot is an error and the template is dropped (${bad.errors.length})`)
}

// --- the × tells the server (docs/PANELS.md 4) --------------------------------
{
  const doc = makeDom()
  const sent = []
  const view = new UiView({ onEvent: (...a) => sent.push(a), doc })
  view.open(msg)
  const x = all(doc.body).find((e) => /dnp-ui-x/.test(e.className))
  x.onclick()
  x.onclick()
  check(sent.length === 1 && sent[0][0] === msg.uiId && sent[0][1] === 0 && sent[0][2] === 0,
        `the player's × sends widget 0 for its panel, once (${JSON.stringify(sent)})`)
  view.open(msg)
  view.close(msg.uiId)
  check(sent.length === 1, 'CONTROL: a panel the server closes tells nobody')
}

// --- named styles (docs/UI_STYLES.md 2.2) -------------------------------------
console.log('\n--- named styles: fonts, skins, title, buttons ---')
const PARCHMENT = {
  font: 'Bitter', size: 13, color: '#3a2a1a', accent: '#8a5a2a',
  panel: { bg: '#e9dcc0', image: 'skins/parchment.png', slice: [24, 24, 24, 24], padding: 12, radius: 0, border: 'none' },
  title: { color: '#5a2a10', size: 15, weight: 700, align: 'center', bg: 'transparent', image: 'skins/ribbon.png', slice: [8, 30, 8, 30] },
  button: { bg: '#c9a86a', color: '#2a1a0a', image: 'skins/button.png', slice: [10, 10, 10, 10], radius: 4,
            pressed: { image: 'skins/button_down.png' } },
  close: { color: '#5a2a10' }
}
const STYLED = parseLayout({
  scale: { reference: 390, min: 1, max: 1 },
  theme: { font: 'ui-monospace, monospace' },
  styles: { parchment: PARCHMENT },
  panels: {
    shop: { anchor: 'c', w: 320, style: 'parchment' },
    talk: { anchor: 'c', w: 320, style: { use: 'parchment', bg: '#f4ecd8' } },
    odd: { anchor: 'c', w: 320, style: 'velvet' },
    plain: { anchor: 'c', w: 320 },
    slotted: { anchor: 'c', w: 320, style: 'parchment',
               slots: [{ widget: 5, x: 10, y: 10, w: 100, style: { bg: '#a00', image: 'skins/red.png', slice: [4, 4, 4, 4] } }] }
  }
})
check(STYLED.ok, `a layout with a named style parses (${STYLED.errors.concat(STYLED.warnings).join('; ')})`)
const withButton = { ...msg, widgets: [...msg.widgets, { kind: UI_KIND.BUTTON, id: 5, label: 'Buy' }] }
const openStyled = (template) => {
  const doc = makeDom()
  const view = new UiView({ onEvent: () => {}, doc, layout: STYLED.layout, size: { width: 800, height: 600 } })
  view.open({ ...withButton, template })
  return rootOf(doc)
}
{
  const root = openStyled('shop')
  check(/^Bitter, ui-monospace/.test(root.style.fontFamily) && root.style.fontSize === '13px' &&
        root.style.color === '#3a2a1a' && root.style.accentColor === '#8a5a2a',
        `the style's font, size, colour and accent are the panel's (${root.style.fontFamily})`)
  check(root.style.borderImage === 'url("/ui/skins/parchment.png") 24 24 24 24 fill stretch' &&
        root.style.borderWidth === '24px 24px 24px 24px' && root.style.background === '#e9dcc0',
        `the box is a 9-slice skin with its slice, over its colour (${root.style.borderImage})`)
  const title = all(root).find((e) => e.className === 'dnp-ui-title')
  check(title.style.color === '#5a2a10' && title.style.textAlign === 'center' && title.style.fontWeight === '700' &&
        /ribbon\.png"\) 8 30 8 30/.test(title.style.borderImage), 'the title bar has the title look')
  const x = all(root).find((e) => /dnp-ui-x/.test(e.className))
  check(x.style.color === '#5a2a10', 'and the × its colour')
  const b = all(root).find((e) => e.tag === 'button')
  check(b.style.background === '#c9a86a' && b.style.color === '#2a1a0a' && /button\.png/.test(b.style.borderImage),
        'a button has the button look')
  b.onpointerdown()
  check(/button_down\.png/.test(b.style.borderImage) && b.style.background === '#c9a86a', 'held, its pressed picture')
  b.onpointerup()
  check(/\/button\.png/.test(b.style.borderImage), 'released, back to its own')
}
{
  const root = openStyled('talk')
  check(root.style.background === '#f4ecd8' && /parchment\.png/.test(root.style.borderImage) && root.style.color === '#3a2a1a',
        '{ use, bg } overrides bg alone')
}
{
  const warn = console.warn
  const said = []
  console.warn = (s) => said.push(s)
  const doc = makeDom()
  const view = new UiView({ onEvent: () => {}, doc, layout: STYLED.layout, size: { width: 800, height: 600 } })
  view.open({ ...withButton, template: 'odd' })
  view.open({ ...withButton, uiId: 9, template: 'odd' })
  console.warn = warn
  const odd = rootOf(doc)
  const plain = openStyled('plain')
  check(shape(odd) === shape(plain) && !odd.style.borderImage && !odd.style.fontFamily,
        'an unknown style name draws the default panel')
  check(said.filter((s) => /velvet/.test(s)).length === 1, `and warns once (${said.length})`)
  const pb = all(plain).find((e) => e.tag === 'button')
  check(!pb.style.background && !pb.onpointerdown, 'CONTROL: without a style a button is drawn as before')
}
{
  const root = openStyled('slotted')
  const slot = all(root).find((e) => /dnp-ui-slot/.test(e.className))
  const b = all(slot).find((e) => e.tag === 'button')
  check(slot.style.background === '#a00' && /red\.png/.test(slot.style.borderImage), 'a slot\'s style lands on its slot')
  check(b.style.background === '#a00' && /red\.png/.test(b.style.borderImage) && b.style.color === '#2a1a0a',
        'and wins over the style\'s button look for its widget, keeping what it does not say')
}

{
  // assetUrl (docs/UI_STYLES.md 6.1): where a view finds the files a layout names.
  const doc = makeDom()
  new UiView({ onEvent: () => {}, doc, layout: STYLED.layout, size: { width: 800, height: 600 }, assetUrl: (f) => 'blob:' + f })
    .open({ ...withButton, template: 'shop' })
  const root = rootOf(doc)
  check(/^url\("blob:skins\/parchment\.png"\)/.test(root.style.borderImage) &&
        /blob:skins\/button\.png/.test(all(root).find((e) => e.tag === 'button').style.borderImage),
        `given assetUrl, a style's skins come from it (${root.style.borderImage})`)
  check(/^url\("\/ui\/skins\/parchment\.png"\)/.test(openStyled('shop').style.borderImage), 'CONTROL: without it, from /ui/')
}

console.log(`\nui: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
