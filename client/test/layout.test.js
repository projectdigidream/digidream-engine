//
// THE UI LAYOUT, the pure half (client/src/render/layout.js), and the touch
// controls' reading of it (render/touchui.js touchLayout).
//
// The claim that matters most is the last block: a module with no layout looks
// EXACTLY as the client did when the HUD and the buttons were hard-coded CSS.
// The old rules are written out here as numbers, so the default is held to them
// rather than to itself.
//
import {
  parseLayout, place, uiScale, sizeOf, isVisible, layoutFileFor, HudView, DEFAULT_LAYOUT, ANCHORS
} from '../src/render/layout.js'
import { touchLayout } from '../src/render/touchui.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 0.5) => Math.abs(a - b) <= eps

// --- parsing -----------------------------------------------------------------
{
  const d = parseLayout(JSON.stringify(DEFAULT_LAYOUT))
  check(d.ok && d.errors.length === 0 && d.warnings.length === 0, 'the default, written out, reads back clean')
  const empty = parseLayout('{}')
  check(empty.ok && empty.layout.hud === DEFAULT_LAYOUT.hud, 'an empty file is the default')
  const junk = parseLayout('{ not json')
  check(!junk.ok && junk.layout === DEFAULT_LAYOUT && /not JSON/.test(junk.errors[0]), 'a file that is not JSON is said, and the default used')
  const newer = parseLayout({ version: 2, sparkle: 1, hud: [{ id: 'x', type: 'hologram' }, { id: 'y', type: 'text', text: 'hi' }] })
  check(newer.ok, 'a NEWER file still loads: unknown keys and types are warnings')
  check(newer.warnings.some((w) => /sparkle/.test(w)) && newer.warnings.some((w) => /hologram/.test(w)) &&
        newer.warnings.some((w) => /version 2/.test(w)), `and each is named (${newer.warnings.length} warnings)`)
  check(newer.layout.hud.length === 1 && newer.layout.hud[0].id === 'y', 'the unknown element is skipped, the known one kept')
  const bad = parseLayout({ hud: [{ id: 'a', type: 'text', anchor: 'middle' }, { id: 'b', type: 'text', anchor: 'br' }] })
  check(!bad.ok && bad.errors.some((e) => /'middle'/.test(e)), 'CONTROL: an anchor nobody can draw is an ERROR naming it')
  check(bad.layout.hud.length === 1 && bad.layout.hud[0].id === 'b', 'and that element is dropped, not put in a corner')
  check(!parseLayout({ hud: [{ id: 'a', type: 'text', show: 'tv' }] }).ok, 'a bad `show` is an error')
  check(!parseLayout({ hud: [{ id: 'a', type: 'text', visible: { bind: 'hp', op: 'about' } }] }).ok, 'a bad `visible` is an error')
  check(!parseLayout({ hud: [{ type: 'text' }] }).ok, 'an element with no id is an error')
  const kids = parseLayout({ hud: [{ id: 'g', type: 'group', children: [{ id: 'k', type: 'text', anchor: 'nowhere' }] }] })
  check(!kids.ok && kids.layout.hud[0].children.length === 0, 'a group\'s children are checked too')

  const t = parseLayout({ touch: { buttons: [{ id: 'jump', x: -30 }, { id: 'teleport' }] } })
  const jump = t.layout.touch.buttons.find((b) => b.id === 'jump')
  check(jump.x === -30 && jump.y === -150 && jump.size === 74, 'a button named is MERGED onto its default')
  check(t.layout.touch.buttons.length === DEFAULT_LAYOUT.touch.buttons.length &&
        t.layout.touch.buttons.find((b) => b.id === 'attack').x === -104, 'and one not named keeps its default')
  check(t.warnings.some((w) => /teleport/.test(w)) && !t.layout.touch.buttons.some((b) => b.id === 'teleport'),
        'CONTROL: a layout cannot invent a button the engine has no bit for')
  check(DEFAULT_LAYOUT.touch.buttons.find((b) => b.id === 'jump').x === -22, 'and merging did not touch the default itself')
}

// --- placement: the nine anchors ----------------------------------------------
{
  const L = { scale: { reference: 390, min: 1, max: 1 } }
  const W = 400
  const H = 300
  const el = (anchor) => ({ anchor, x: 0, y: 0, w: 40, h: 20 })
  const want = {
    tl: [0, 0], t: [180, 0], tr: [360, 0],
    l: [0, 140], c: [180, 140], r: [360, 140],
    bl: [0, 280], b: [180, 280], br: [360, 280]
  }
  let bad = []
  for (const a of ANCHORS) {
    const r = place(el(a), L, W, H)
    if (r.left !== want[a][0] || r.top !== want[a][1] || r.width !== 40 || r.height !== 20) bad.push(`${a}=(${r.left},${r.top})`)
  }
  check(bad.length === 0, `each anchor puts the element's own corner at the screen's${bad.length ? ': ' + bad.join(' ') : ''}`)
  // Inside the safe area: the same, in the rectangle the insets leave.
  const ins = { top: 10, right: 20, bottom: 30, left: 40 }
  bad = []
  for (const a of ANCHORS) {
    const r = place(el(a), L, W, H, ins)
    const ex = a.endsWith('l') ? 40 : a.endsWith('r') ? 400 - 20 - 40 : (40 + 380) / 2 - 20
    const ey = a.startsWith('t') ? 10 : a.startsWith('b') ? 300 - 30 - 20 : (10 + 270) / 2 - 10
    if (r.left !== ex || r.top !== ey) bad.push(`${a}=(${r.left},${r.top}) not (${ex},${ey})`)
  }
  check(bad.length === 0, `and inside the safe area, with insets${bad.length ? ': ' + bad.join(' ') : ''}`)
  const off = place({ anchor: 'br', x: -10, y: -5, w: 40, h: 20 }, L, W, H)
  check(off.left === 350 && off.top === 275, '+x is right and +y is down, from any anchor')
  const nat = place({ anchor: 'br' }, L, W, H, {}, { w: 50, h: 10 })
  check(nat.left === 350 && nat.top === 290, 'an element with no size is placed by its natural one')
}

// --- scaling ---------------------------------------------------------------
{
  const L = { scale: { reference: 390, min: 0.5, max: 2 } }
  check(uiScale(L, 390, 844) === 1, 'a phone the reference size is one unit a pixel')
  check(near(uiScale(L, 1170, 2532), 2, 1e-9), 'a big screen is clamped to the max')
  check(near(uiScale(L, 600, 800), 600 / 390, 1e-9), 'between, it is the short side over the reference')
  check(uiScale(L, 100, 100) === 0.5, 'a tiny one is clamped to the min')
  check(uiScale(DEFAULT_LAYOUT, 2560, 1440) === 1, 'CONTROL: the default is pinned, whatever the window')
  const big = place({ anchor: 'br', x: -10, y: -10, size: 50 }, L, 1170, 2532)
  check(big.width === 100 && big.left === 1170 - 20 - 100, 'offsets and sizes scale together')
  check(sizeOf({ vw: 50, max: 100 }, 1, 400, 300) === 100 && sizeOf({ vw: 10, max: 100 }, 1, 400, 300) === 40,
        'a viewport size is capped by its max')
  check(sizeOf({ vh: 50 }, 1, 400, 300) === 150, 'and vh reads the height')
}

// --- visibility and bindings ---------------------------------------------------
{
  const b = new Map([['hud.ammo', 0], ['hud.weapon', 'pistol']])
  const v = (op, bind, value) => isVisible({ visible: { op, bind, value } }, b)
  check(v('set', 'hud.weapon') && !v('set', 'hud.armor'), 'set: a value has arrived')
  check(v('gt', 'hud.ammo', -1) && !v('gt', 'hud.ammo', 0), 'gt')
  check(v('lt', 'hud.ammo', 1) && !v('lt', 'hud.ammo', 0), 'lt')
  check(v('eq', 'hud.weapon', 'pistol') && !v('eq', 'hud.weapon', 'fist'), 'eq')
  check(v('ne', 'hud.weapon', 'fist') && !v('ne', 'hud.weapon', 'pistol'), 'ne')
  check(!v('gt', 'hud.armor', -1), 'CONTROL: no value yet is not greater than anything')
  check(isVisible({}, b), 'no rule: shown')
  const index = { default: 'base.json', maps: { MAP07: 'boss.json' } }
  check(layoutFileFor(index, 'MAP07') === 'boss.json', 'a level with its own layout gets it, by name')
  check(layoutFileFor(index, 'MAP01') === 'base.json' && layoutFileFor(index, null) === 'base.json', 'any other gets the default')
  check(layoutFileFor({}, 'MAP07') === null && layoutFileFor(null, 'x') === null, 'no index, no file: the built-in one')
}

// --- the HUD, drawn into a fake document ---------------------------------------
function fakeDoc () {
  const make = (tag) => {
    const n = {
      tag, style: {}, children: [], className: '', textContent: '', offsetWidth: 0, offsetHeight: 0,
      appendChild (c) { this.children.push(c); c.parent = this; return c },
      remove () { if (this.parent) this.parent.children = this.parent.children.filter((x) => x !== this) }
    }
    Object.defineProperty(n, 'textContent', {
      get () { return this._t ?? '' },
      set (v) { this._t = v; if (v === '') this.children = [] }
    })
    return n
  }
  return { createElement: make }
}
{
  const doc = fakeDoc()
  const host = doc.createElement('body')
  const hud = new HudView({ host, doc, touch: false, size: { width: 800, height: 600 } })
  check(hud.nodes.length === 1 && hud.nodes[0].el.id === 'health', 'on a desktop the desktop health box is built')
  check(hud.nodes[0].dom.style.display === 'none', 'and hidden until a health arrives (solo keeps none)')
  hud.update({ hp: 100 })
  const box = hud.nodes[0]
  const num = box.kids[0].kids[0].dom
  const fill = box.kids[1].fill
  check(box.dom.style.display === 'flex' && num.textContent === '100', `a health shows it (${num.textContent})`)
  check(fill.style.width === '100.0%' && fill.style.background === '#5fbf6a', 'the bar, drawn against the most seen, full and green')
  hud.update({ hp: 60 })
  check(fill.style.width === '60.0%' && fill.style.background === '#d6a13a', `at 60% it is amber, as the old bar was (${fill.style.background})`)
  hud.update({ hp: 26 })
  check(fill.style.background === '#d6a13a', 'still amber at 26%')
  hud.update({ hp: 25 })
  check(fill.style.background === '#d65b5b', 'red from 25% down')
  hud.update({ hp: -5 })
  check(num.textContent === '0' && fill.style.width === '0.0%', 'a negative health reads 0')
  hud.update({ 'hud.hp_max': 200, hp: 100 })
  check(fill.style.width === '50.0%', 'CONTROL: a published hp_max wins over the most seen')
  check(box.dom.style.left === '12px', `placed 12 px in from the left (${box.dom.style.left})`)
  const touchHud = new HudView({ host: doc.createElement('body'), doc, touch: true, size: { width: 800, height: 600 } })
  check(touchHud.nodes.length === 1 && touchHud.nodes[0].el.id === 'health_touch' && touchHud.nodes[0].dom.style.top === '34px',
        'CONTROL: with touch controls it is the top-left one, out of the stick\'s corner')
  hud.setLayout(parseLayout({ hud: [{ id: 'ammo', type: 'text', text: '{hud.ammo} shells', anchor: 'br' }] }).layout)
  hud.update({ 'hud.ammo': 12 })
  check(hud.nodes.length === 1 && hud.nodes[0].dom.textContent === '12 shells', 'a module\'s own element, bound to a HUD name')
  hud.setLayout(parseLayout({ hud: [{ id: 'key', type: 'text', text: 'KEY', visible: { bind: 'hud.key', op: 'set' } }] }).layout)
  hud.update({ 'hud.key': 1 })
  const shown = hud.nodes[0].dom.style.display
  hud.update({ 'hud.key': undefined })
  check(shown === 'block' && hud.nodes[0].dom.style.display === 'none', 'a value the server clears is gone from the HUD (HUD_SET kind 2)')
  hud.destroy()
  check(host.children.length === 0, 'destroy takes it all away')
}

// --- touch: the scheme's buttons ----------------------------------------------
{
  const p = touchLayout(DEFAULT_LAYOUT, 'platformer')
  check(p.buttons.map((b) => b.id).join() === 'jump,attack,use,dash', 'a platformer has the four buttons; alt is hidden')
  const f = touchLayout(DEFAULT_LAYOUT, 'fps')
  check(f.buttons.some((b) => b.id === 'alt') && !f.buttons.some((b) => b.id === 'dash'), 'first person has ALT and no DASH')
  check(f.buttons.find((b) => b.id === 'attack').size > p.buttons.find((b) => b.id === 'attack').size, 'and a bigger trigger')
  const own = parseLayout({ touch_fps: { buttons: [{ id: 'dash', hidden: false }] } }).layout
  check(touchLayout(own, 'fps').buttons.some((b) => b.id === 'dash'), 'a module can bring a hidden button back')
  check(touchLayout(null, 'fps').buttons.length === f.buttons.length, 'no layout at all is the default')
}

// --- THE DEFAULT IS WHAT WAS HARD-CODED ------------------------------------------
//
// The CSS this replaced, as numbers (index.html #health, touchui.js .dnp-touch-*):
//   button   right:R  bottom:B  74x74 (dash 60x60)
//   zone     left:0 bottom:0  min(46vw,340px) x min(58vh,380px)
//   health   left:12px  bottom:34px (touch: top:34px)  width:min(240px,40vw)
const OLD_BUTTONS = { jump: [22, 150, 74], attack: [104, 78, 74], use: [22, 72, 74], dash: [104, 158, 60] }
for (const [W, H] of [[390, 844], [844, 390]]) {
  const ins = { top: 0, right: 0, bottom: 0, left: 0 }
  const t = touchLayout(DEFAULT_LAYOUT, 'platformer')
  let worst = 0
  for (const b of t.buttons) {
    const [R, B, S] = OLD_BUTTONS[b.id]
    const r = place(b, DEFAULT_LAYOUT, W, H, ins)
    worst = Math.max(worst, Math.abs(r.left - (W - R - S)), Math.abs(r.top - (H - B - S)), Math.abs(r.width - S), Math.abs(r.height - S))
  }
  check(worst <= 0.5, `${W}x${H}: every button where the CSS put it (worst ${worst} px)`)
  const z = place(t.zone, DEFAULT_LAYOUT, W, H, ins)
  const zw = Math.min(0.46 * W, 340)
  const zh = Math.min(0.58 * H, 380)
  check(near(z.left, 0) && near(z.width, zw) && near(z.height, zh) && near(z.top, H - zh),
        `${W}x${H}: the stick's zone too (${z.width.toFixed(1)}x${z.height.toFixed(1)})`)
  const natH = 46
  const hb = place(DEFAULT_LAYOUT.hud[0], DEFAULT_LAYOUT, W, H, ins, { w: 0, h: natH })
  check(near(hb.left, 12) && near(hb.top, H - 34 - natH) && near(hb.width, Math.min(240, 0.4 * W)),
        `${W}x${H}: the health box, bottom left`)
  const ht = place(DEFAULT_LAYOUT.hud[1], DEFAULT_LAYOUT, W, H, ins, { w: 0, h: natH })
  check(near(ht.left, 12) && near(ht.top, 34), `${W}x${H}: and top left beside the touch controls`)
  // And the notch: the CSS added env(safe-area-inset-*) to every offset.
  const notch = { top: 47, right: 0, bottom: 34, left: 0 }
  const j = place(t.buttons[0], DEFAULT_LAYOUT, W, H, notch)
  check(near(j.top, H - 34 - 150 - 74), `${W}x${H}: the safe area is added, as calc(… + env()) did`)
}
const CONTROL = place({ ...touchLayout(DEFAULT_LAYOUT).buttons[0], x: -23 }, DEFAULT_LAYOUT, 390, 844)
check(Math.abs(CONTROL.left - (390 - 22 - 74)) > 0.5, 'CONTROL: a button one pixel off is caught')

console.log(`\nlayout: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
