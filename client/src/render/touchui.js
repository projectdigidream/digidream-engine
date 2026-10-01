//
// The controls you can see, over the game.
//
// client/src/touch.js is the state machine and owns no DOM; this owns the DOM
// and decides nothing. The split is the same one render/ui.js makes and for
// the same reason -- the part with arithmetic in it is the part a test can
// reach, and a phone is the only thing that can answer whether a button is big
// enough for a thumb.
//
// DOM RATHER THAN WEBGL, again for ui.js's reasons: these are not in the world.
// They do not occlude, they are not lit, and they want hit-testing, layout and
// safe-area insets that a canvas would have to reimplement badly. A phone's
// compositor draws a handful of translucent circles for free.
//
// WHY THE PAD FLOATS. The zone your thumb must land in is fixed -- the bottom
// left is always the stick -- but the pad is drawn wherever your thumb landed
// inside it. A fixed circle means looking down to find it, which in a game
// where something is shooting at you is the same as not having it. The idle
// hint sits where a thumb usually rests so there is still something to aim at
// the first time.
//
import { TouchState, STICK_RADIUS } from '../touch.js'
import { DEFAULT_LAYOUT, TOUCH_IDS, place } from './layout.js'

const STYLE = `
.dnp-touch{position:fixed;inset:0;z-index:40;pointer-events:none;
  touch-action:none;-webkit-user-select:none;user-select:none}
/* The zone, not a control: invisible, never hit-tested by the browser, and
   read only for its rectangle. Where it is and how big comes from the layout
   (render/layout.js), like the buttons. */
.dnp-touch-zone{position:absolute}
/* The idle hint and the live pad are the same circle in two states. */
.dnp-touch-pad{position:absolute;width:${STICK_RADIUS * 2}px;height:${STICK_RADIUS * 2}px;
  margin-left:${-STICK_RADIUS}px;margin-top:${-STICK_RADIUS}px;
  border:2px solid rgba(207,227,255,.22);border-radius:50%;
  background:rgba(11,13,18,.22);transition:opacity 120ms linear}
.dnp-touch-knob{position:absolute;width:52px;height:52px;margin-left:-26px;margin-top:-26px;
  border-radius:50%;background:rgba(207,227,255,.30);
  border:2px solid rgba(207,227,255,.5);box-shadow:0 2px 10px rgba(0,0,0,.45)}
.dnp-touch-btn{position:absolute;pointer-events:auto;
  width:74px;height:74px;border-radius:50%;
  display:flex;align-items:center;justify-content:center;
  font:600 13px/1 ui-monospace,SFMono-Regular,Menlo,monospace;
  color:#cfe3ff;text-shadow:0 1px 2px #000;
  background:rgba(27,34,48,.55);border:2px solid rgba(207,227,255,.28)}
.dnp-touch-btn[data-down="1"]{background:rgba(91,143,214,.55);
  border-color:rgba(207,227,255,.7)}
/* WHERE each button goes is the layout's (render/layout.js, a module's
   assets/ui): placed by left/top from JS, inside the safe area. The default
   reproduces the hand-made arc this used to be -- a thumb arcs, so jump is
   highest and furthest in, dash lowest and outermost. */
.dnp-touch-btn.small{font-size:11px}
/* A probe for the safe-area insets, which only CSS can read. */
.dnp-touch-insets{position:fixed;visibility:hidden;pointer-events:none;
  padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)}
`

/** The buttons the engine has; each holds down one protocol bit (main.js TOUCH_BITS). */
export const TOUCH_BUTTONS = TOUCH_IDS.map((id) => ({ id }))

/**
 * The buttons a layout's touch section places, resolved: every engine button,
 * with where it goes, how big, what it says, and whether it is shown at all.
 */
export function touchLayout (layout, scheme = 'platformer') {
  const t = (scheme === 'fps' ? layout?.touch_fps : layout?.touch) ??
            (scheme === 'fps' ? DEFAULT_LAYOUT.touch_fps : DEFAULT_LAYOUT.touch)
  return { zone: t.stick.zone, buttons: t.buttons.filter((b) => !b.hidden) }
}

/** The safe-area insets, read through a probe element (only CSS can see them). */
function readInsets (probe) {
  const cs = getComputedStyle(probe)
  return { top: parseFloat(cs.paddingTop) || 0, right: parseFloat(cs.paddingRight) || 0,
           bottom: parseFloat(cs.paddingBottom) || 0, left: parseFloat(cs.paddingLeft) || 0 }
}

/**
 * Mount the controls and start feeding a TouchState.
 *
 * Returns `{ state, destroy, layout }`. `state` is what main.js asks for the
 * push and the buttons; `layout` recomputes the regions, and is called on
 * resize and on every pointerdown -- a phone rotates, the address bar comes
 * and goes, and a region measured once is wrong for the rest of the session.
 */
export function mountTouch (host = document.body, { layout = DEFAULT_LAYOUT, scheme = 'platformer', preview = false } = {}) {
  const style = document.createElement('style')
  style.textContent = STYLE
  host.append(style)
  let current = touchLayout(layout, scheme)
  const probe = document.createElement('div')
  probe.className = 'dnp-touch-insets'
  host.append(probe)

  const layer = document.createElement('div')
  layer.className = 'dnp-touch'

  const zone = document.createElement('div')
  zone.className = 'dnp-touch-zone'
  layer.append(zone)

  const pad = document.createElement('div')
  pad.className = 'dnp-touch-pad'
  const knob = document.createElement('div')
  knob.className = 'dnp-touch-knob'
  layer.append(pad, knob)

  const buttons = new Map()
  const build = () => {
    for (const el of buttons.values()) el.remove()
    buttons.clear()
    for (const b of current.buttons) {
      const el = document.createElement('div')
      el.className = `dnp-touch-btn ${b.id}${(b.size ?? 74) < 70 ? ' small' : ''}`
      el.textContent = b.label ?? b.id.toUpperCase()
      el.dataset.down = '0'
      buttons.set(b.id, el)
      layer.append(el)
    }
  }
  build()
  host.append(layer)

  // WHERE EVERYTHING GOES, from the layout, in this window, inside the safe area.
  const placeAll = () => {
    const W = innerWidth
    const H = innerHeight
    const ins = readInsets(probe)
    const z = place(current.zone, layout, W, H, ins)
    Object.assign(zone.style, { left: `${z.left}px`, top: `${z.top}px`, width: `${z.width}px`, height: `${z.height}px` })
    for (const b of current.buttons) {
      const r = place(b, layout, W, H, ins)
      Object.assign(buttons.get(b.id).style, { left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px` })
    }
  }
  placeAll()

  const state = new TouchState()
  let regions = []

  const rectOf = (el, id) => {
    const r = el.getBoundingClientRect()
    return { id, x: r.left, y: r.top, w: r.width, h: r.height }
  }

  // BUTTONS FIRST, then the stick zone. claim() takes the first match, and a
  // button that overlapped the zone on a small screen must win -- otherwise
  // jumping walks you sideways, which is the worst kind of "sometimes".
  const measure = () => {
    placeAll()
    regions = [
      ...current.buttons.map((b) => rectOf(buttons.get(b.id), b.id)),
      rectOf(zone, 'stick')
    ]
  }
  measure()

  // The idle hint: where a thumb usually rests, so there is something to aim
  // at before you have learnt that anywhere in the corner works.
  const idle = () => {
    const r = regions.find((x) => x.id === 'stick')
    return r ? { x: r.x + r.w * 0.38, y: r.y + r.h * 0.66 } : { x: 110, y: 0 }
  }

  const paint = () => {
    const origin = state.stickOrigin ?? idle()
    const push = state.move2d()
    pad.style.left = `${origin.x}px`
    pad.style.top = `${origin.y}px`
    pad.style.opacity = state.stickOrigin ? '1' : '0.45'
    // The knob sits at the push, with y flipped back: move2d is y-up because
    // that is what the camera wants, and this is drawing in the DOM's y-down.
    knob.style.left = `${origin.x + push.x * STICK_RADIUS}px`
    knob.style.top = `${origin.y - push.y * STICK_RADIUS}px`
    knob.style.opacity = state.stickOrigin ? '1' : '0.4'
    for (const [id, el] of buttons) el.dataset.down = state.pressed.has(id) ? '1' : '0'
  }
  paint()

  // POINTER EVENTS ON THE WINDOW, not on the layer.
  //
  // The layer does not receive them: it is `pointer-events: none` apart from
  // the buttons, because a full-screen overlay that swallowed pointers would
  // take them from the canvas, from the script-opened panels in render/ui.js
  // and from anything else the page grows later. What this needs is to SEE
  // every pointer and to claim it by where it landed, which is exactly what
  // a window-level listener with a region table does.
  // BLOCKED while a modal panel is open (docs/UI_LAYOUT.md 5): a thumb on the
  // panel is the panel's, not the stick's.
  let blocked = false
  const onDown = (e) => {
    if (blocked) return
    measure()
    state.down(e.pointerId, e.clientX, e.clientY, regions)
    // Only once it is ours. A pointer landing on a script's panel is that
    // panel's, and preventing its default would make the panel unusable.
    if (state.pointers.get(e.pointerId)?.kind !== 'camera' || e.target === document.body ||
        e.target?.tagName === 'CANVAS') {
      e.preventDefault()
    }
    paint()
  }
  const onMove = (e) => { state.move(e.pointerId, e.clientX, e.clientY); paint() }
  const onUp = (e) => { state.up(e.pointerId); paint() }
  const onCancel = (e) => { state.cancel(e.pointerId); paint() }

  // EVERY WAY A POINTER CAN VANISH. On a phone these are not edge cases: a
  // call arrives, a notification is pulled down, the app is backgrounded
  // mid-jump. Each one leaves the stick pushed and the player walking into a
  // wall, which is the keyboard's `blur` bug with more ways to reach it.
  const onLost = () => { state.clearAll(); paint() }

  addEventListener('pointerdown', onDown, { passive: false })
  addEventListener('pointermove', onMove, { passive: true })
  addEventListener('pointerup', onUp, { passive: true })
  addEventListener('pointercancel', onCancel, { passive: true })
  addEventListener('blur', onLost)
  addEventListener('visibilitychange', () => { if (document.hidden) onLost() })
  addEventListener('resize', measure, { passive: true })
  // A long press over a game is a text selection or a context menu, neither of
  // which is ever wanted here and both of which interrupt whatever the thumb
  // was holding.
  const onMenu = (e) => e.preventDefault()
  addEventListener('contextmenu', onMenu)

  // A PREVIEW draws and claims nothing (the studio's device frame).
  if (preview) {
    removeEventListener('pointerdown', onDown)
    removeEventListener('pointermove', onMove)
    removeEventListener('pointerup', onUp)
    removeEventListener('pointercancel', onCancel)
  }

  return {
    state,
    layout: measure,
    paint,
    /** Hold off the game's touch input (a modal panel); what was held is let go. */
    setBlocked (on) {
      if (on === blocked) return
      blocked = on
      if (on) onLost()
      layer.style.visibility = on ? 'hidden' : ''
    },
    /** A new layout or scheme: the buttons rebuilt and placed again. */
    setLayout (next = layout, nextScheme = scheme) {
      layout = next ?? DEFAULT_LAYOUT
      scheme = nextScheme
      current = touchLayout(layout, scheme)
      build()
      measure()
      paint()
    },
    destroy () {
      removeEventListener('pointerdown', onDown)
      removeEventListener('pointermove', onMove)
      removeEventListener('pointerup', onUp)
      removeEventListener('pointercancel', onCancel)
      removeEventListener('blur', onLost)
      removeEventListener('resize', measure)
      removeEventListener('contextmenu', onMenu)
      layer.remove()
      style.remove()
      probe.remove()
    }
  }
}
