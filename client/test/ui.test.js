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

console.log(`\nui: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
