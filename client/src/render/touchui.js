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

const STYLE = `
.dnp-touch{position:fixed;inset:0;z-index:40;pointer-events:none;
  touch-action:none;-webkit-user-select:none;user-select:none}
/* The zone, not a control: invisible, never hit-tested by the browser, and
   read only for its rectangle. Its size is what makes the stick reachable on
   a big phone and it is CSS so it can be changed without touching the code. */
.dnp-touch-zone{position:absolute;left:0;bottom:0;
  width:min(46vw,340px);height:min(58vh,380px)}
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
/* Bottom right, clear of the home indicator and of a curved corner. The
   right-hand cluster is laid out by hand rather than by a grid because a
   thumb arcs: jump is highest and furthest in, dash is lowest and outermost. */
.dnp-touch-btn.jump  {right:calc(22px + env(safe-area-inset-right));bottom:calc(150px + env(safe-area-inset-bottom))}
.dnp-touch-btn.attack{right:calc(104px + env(safe-area-inset-right));bottom:calc(78px + env(safe-area-inset-bottom))}
.dnp-touch-btn.use   {right:calc(22px + env(safe-area-inset-right));bottom:calc(72px + env(safe-area-inset-bottom))}
.dnp-touch-btn.dash  {right:calc(104px + env(safe-area-inset-right));bottom:calc(158px + env(safe-area-inset-bottom));
  width:60px;height:60px;font-size:11px}
`

/** The buttons, and which protocol bit each one holds down. */
export const TOUCH_BUTTONS = [
  { id: 'jump', label: 'JUMP' },
  { id: 'attack', label: 'ATK' },
  { id: 'use', label: 'USE' },
  { id: 'dash', label: 'DASH' }
]

/**
 * Mount the controls and start feeding a TouchState.
 *
 * Returns `{ state, destroy, layout }`. `state` is what main.js asks for the
 * push and the buttons; `layout` recomputes the regions, and is called on
 * resize and on every pointerdown -- a phone rotates, the address bar comes
 * and goes, and a region measured once is wrong for the rest of the session.
 */
export function mountTouch (host = document.body) {
  const style = document.createElement('style')
  style.textContent = STYLE
  host.append(style)

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
  for (const b of TOUCH_BUTTONS) {
    const el = document.createElement('div')
    el.className = `dnp-touch-btn ${b.id}`
    el.textContent = b.label
    el.dataset.down = '0'
    buttons.set(b.id, el)
    layer.append(el)
  }
  host.append(layer)

  const state = new TouchState()
  let regions = []

  const rectOf = (el, id) => {
    const r = el.getBoundingClientRect()
    return { id, x: r.left, y: r.top, w: r.width, h: r.height }
  }

  // BUTTONS FIRST, then the stick zone. claim() takes the first match, and a
  // button that overlapped the zone on a small screen must win -- otherwise
  // jumping walks you sideways, which is the worst kind of "sometimes".
  const layout = () => {
    regions = [
      ...TOUCH_BUTTONS.map((b) => rectOf(buttons.get(b.id), b.id)),
      rectOf(zone, 'stick')
    ]
  }
  layout()

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
  const onDown = (e) => {
    layout()
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
  addEventListener('resize', layout, { passive: true })
  // A long press over a game is a text selection or a context menu, neither of
  // which is ever wanted here and both of which interrupt whatever the thumb
  // was holding.
  const onMenu = (e) => e.preventDefault()
  addEventListener('contextmenu', onMenu)

  return {
    state,
    layout,
    paint,
    destroy () {
      removeEventListener('pointerdown', onDown)
      removeEventListener('pointermove', onMove)
      removeEventListener('pointerup', onUp)
      removeEventListener('pointercancel', onCancel)
      removeEventListener('blur', onLost)
      removeEventListener('resize', layout)
      removeEventListener('contextmenu', onMenu)
      layer.remove()
      style.remove()
    }
  }
}
