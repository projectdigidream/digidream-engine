//
// Touch input, which is arithmetic and a state machine.
//
// The half of a virtual thumbstick that a test can reach is the half that is
// numbers: where the origin is, how far a push counts for, what happens when
// you push into a corner, and who owns which finger. The other half -- whether
// the buttons are big enough for a real thumb -- no test was ever going to
// answer, and pretending otherwise would be the kind of check that passes
// while the thing it names does not work.
//
// So the claims here are the ones with a wrong answer you could ship:
//
//   pushing your thumb UP walks you up          (the DOM's y grows downward)
//   a thumb resting on the glass does not walk  (the dead zone)
//   a corner is not faster than a straight push (clamped to the circle)
//   a cancelled pointer STOPS you               (the phone version of blur)
//   two fingers zoom and do not spin the world
//
import {
  TouchState, stickVector, claim, wantsTouch,
  STICK_RADIUS, STICK_DEADZONE
} from '../src/touch.js'
import { screenRelativeMove } from '../src/camera.js'
import { touchLayout } from '../src/render/touchui.js'
import { DEFAULT_LAYOUT, parseLayout } from '../src/render/layout.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-9) => Math.abs(a - b) < eps

// The regions the UI lays out, in the order TouchState is given them.
const REGIONS = [
  { id: 'stick', x: 0, y: 400, w: 300, h: 300 },
  { id: 'jump', x: 700, y: 480, w: 90, h: 90 },
  { id: 'attack', x: 620, y: 570, w: 90, h: 90 },
  { id: 'use', x: 710, y: 570, w: 90, h: 90 },
  { id: 'dash', x: 700, y: 660, w: 90, h: 90 }
]
const BITS = { jump: 0x0001, dash: 0x0002, use: 0x0040, attack: 0x0080 }

console.log('--- the push, in screen space with y up ---')
{
  const R = STICK_RADIUS
  // The browser's y grows DOWNWARD, so a thumb pushed up is a smaller y. A
  // client that forwarded that sign walks away from your thumb.
  const up = stickVector(100, 100, 100, 100 - R)
  check(near(up.x, 0) && up.y > 0.9, `thumb up is +y (${up.y.toFixed(3)})`)
  const down = stickVector(100, 100, 100, 100 + R)
  check(near(down.x, 0) && down.y < -0.9, `CONTROL: thumb down is -y (${down.y.toFixed(3)})`)
  const right = stickVector(100, 100, 100 + R, 100)
  check(right.x > 0.9 && near(right.y, 0), `and thumb right is +x (${right.x.toFixed(3)})`)
}

console.log('\n--- a thumb resting on the glass does not walk ---')
{
  // The touch version of a key that never came up, and the reason it needs a
  // dead zone at all: a finger held still on a capacitive screen wanders by a
  // pixel or two a frame, which is a slow walk into a wall.
  const still = stickVector(100, 100, 100 + STICK_RADIUS * 0.1, 100)
  check(still.x === 0 && still.y === 0, `inside the dead zone is exactly zero (${still.x})`)

  // AND IT RAMPS. Gating alone would jump from nothing to 18% the instant it
  // engages, which reads as a twitch -- so just past the boundary must be
  // small, not suddenly a fifth of full speed.
  const just = stickVector(100, 100, 100 + STICK_RADIUS * (STICK_DEADZONE + 0.02), 100)
  check(just.x > 0 && just.x < 0.05,
        `just past it is small, not a step to ${STICK_DEADZONE} (${just.x.toFixed(4)})`)
  // CONTROL: the ramp still reaches full push at the rim, so it is a rescale
  // and not a permanent slow-down.
  const full = stickVector(100, 100, 100 + STICK_RADIUS, 100)
  check(near(full.x, 1, 1e-6), `and the rim is still full push (${full.x.toFixed(6)})`)
}

console.log('\n--- a corner is not faster than a straight push ---')
{
  const R = STICK_RADIUS
  const corner = stickVector(100, 100, 100 + R, 100 - R)
  const len = Math.hypot(corner.x, corner.y)
  check(near(len, 1, 1e-6), `pushed into a corner, the length is 1 (${len.toFixed(6)})`)
  // CONTROL: the number this is protecting against. Without the clamp the
  // diagonal would be 1.414 and diagonals would be 41% faster, which is the
  // oldest bug in top-down movement.
  check(Math.abs(Math.hypot(1, 1) - len) > 0.4,
        `CONTROL: unclamped it would be ${Math.hypot(1, 1).toFixed(3)}`)
}

console.log('\n--- which finger owns what ---')
{
  check(claim(REGIONS, 60, 600) === 'stick', 'the bottom left is the stick')
  check(claim(REGIONS, 740, 520) === 'jump', 'a button is that button')
  check(claim(REGIONS, 400, 100) === 'camera', 'and everything else turns the camera')
  // The edges, because a region is half-open and getting that wrong makes one
  // row of pixels between two buttons do nothing.
  check(claim(REGIONS, 700, 480) === 'jump', 'the top-left pixel of a button is inside it')
  check(claim(REGIONS, 790, 480) === 'camera', 'and the pixel past its right edge is not')
}

console.log('\n--- a cancelled pointer stops you ---')
{
  // THE FAILURE THIS FILE IS MOSTLY FOR. A phone loses pointers without a
  // `pointerup` all the time: a call arrives, a notification is pulled down,
  // the app is backgrounded, a thumb slides off the glass. The keyboard has
  // the same bug and the same fix -- main.js clears `held` on blur.
  const t = new TouchState()
  t.down(1, 60, 600, REGIONS)
  t.move(1, 60, 600 - STICK_RADIUS)
  check(t.move2d().y > 0.9, 'walking')
  t.cancel(1)
  check(t.move2d().y === 0 && t.move2d().x === 0, 'cancelled, and stopped')

  // CONTROL: without the cancel it keeps walking, so the check above is the
  // handler working rather than the stick decaying on its own.
  const u = new TouchState()
  u.down(1, 60, 600, REGIONS)
  u.move(1, 60, 600 - STICK_RADIUS)
  check(u.move2d().y > 0.9, 'CONTROL: an uncancelled pointer is still pushing')

  // And the whole-hand version, for blur and visibilitychange.
  const v = new TouchState()
  v.down(1, 60, 600, REGIONS)
  v.down(2, 740, 520, REGIONS)
  check(v.buttons(BITS) === BITS.jump, 'jump held')
  v.clearAll()
  check(v.buttons(BITS) === 0 && v.move2d().len === 0 && !v.active,
        'and everything let go at once')
}

console.log('\n--- a button you slid off is still held ---')
{
  // Releasing on leave would end a jump because a thumb drifted two pixels,
  // which on a phone is every jump. The pointer decides what it is when it
  // lands, and only `up` releases it.
  const t = new TouchState()
  t.down(1, 740, 520, REGIONS)
  check(t.buttons(BITS) === BITS.jump, 'pressed')
  t.move(1, 500, 300)
  check(t.buttons(BITS) === BITS.jump, 'slid right off it, and still pressed')
  t.up(1)
  check(t.buttons(BITS) === 0, 'and let go when the thumb comes up')

  // CONTROL: sliding off must not have turned it into a camera drag either,
  // or every button press would spin the world on the way out.
  check(t.takeYaw() === 0, 'CONTROL: and it never turned the camera on the way')
}

console.log('\n--- several buttons at once ---')
{
  const t = new TouchState()
  t.down(1, 740, 520, REGIONS)   // jump
  t.down(2, 660, 610, REGIONS)   // attack
  check(t.buttons(BITS) === (BITS.jump | BITS.attack),
        `both bits are set (0x${t.buttons(BITS).toString(16)})`)
  t.up(1)
  check(t.buttons(BITS) === BITS.attack, 'and letting one go leaves the other')
}

console.log('\n--- a second thumb does not steal the stick ---')
{
  // Two fingers in one corner is a fumble, not an instruction. Letting the
  // newer one win makes the direction jump to wherever the fumble landed.
  const t = new TouchState()
  t.down(1, 60, 600, REGIONS)
  t.move(1, 60, 600 - STICK_RADIUS)
  const first = t.move2d()
  t.down(2, 200, 500, REGIONS)
  const after = t.move2d()
  check(near(after.x, first.x) && near(after.y, first.y),
        'the first thumb keeps it')
  t.move(2, 200, 500 + STICK_RADIUS)
  check(near(t.move2d().y, first.y), 'and moving the second one changes nothing')
  t.up(1)
  check(t.move2d().len === 0, 'lifting the first releases the stick')
}

console.log('\n--- turning the camera, and zooming ---')
{
  const t = new TouchState()
  t.down(1, 400, 200, REGIONS)
  t.move(1, 500, 200)
  const yaw = t.takeYaw()
  check(yaw > 0, `a drag right turns the camera (${yaw.toFixed(4)} rad)`)
  check(t.takeYaw() === 0, 'and TAKING it zeroes it -- the next frame must not turn again')

  // CONTROL: two fingers are a pinch and must NOT yaw. Spreading them moves
  // them in opposite directions, so treating each as a drag would turn by the
  // difference and the world would lurch on every zoom.
  const p = new TouchState()
  p.down(1, 300, 300, REGIONS)
  p.down(2, 500, 300, REGIONS)
  p.move(1, 290, 300)
  p.move(2, 510, 300)
  check(p.takeYaw() === 0, 'CONTROL: pinching does not turn the camera')
  const z = p.takeZoom()
  check(z < 0, `and spreading zooms one way (${z})`)

  const q = new TouchState()
  q.down(1, 300, 300, REGIONS)
  q.down(2, 500, 300, REGIONS)
  q.move(1, 350, 300)
  q.move(2, 450, 300)
  check(q.takeZoom() > 0, 'and pinching in zooms the other')

  // Lifting one finger must not leave the gap behind as a zoom: the next
  // single-finger drag would zoom by the distance between them.
  q.up(2)
  q.move(1, 380, 300)
  check(q.takeZoom() === 0, 'lifting a finger ends the pinch cleanly')
  check(q.takeYaw() !== 0, 'and the one left behind is a drag again')
}

console.log('\n--- the stick hands the same thing to the camera the keyboard does ---')
{
  // ONE IMPLEMENTATION of "a push on screen is a direction in the world".
  // Both inputs produce a screen vector and both hand it to the SAME
  // screenRelativeMove -- a touch path that rotated for itself would be the
  // second implementation this repository keeps paying for, and it would
  // invert on one of them the first time the camera's sign changed.
  const yaw = 45 * Math.PI / 180
  const t = new TouchState()
  t.down(1, 60, 600, REGIONS)
  t.move(1, 60, 600 - STICK_RADIUS)          // full push "up"
  const touchPush = t.move2d()

  // What the keyboard produces for W, from main.js's own arithmetic.
  const keyPush = { x: 0, y: 1 }

  const a = screenRelativeMove(touchPush.x, touchPush.y, yaw)
  const b = screenRelativeMove(keyPush.x, keyPush.y, yaw)
  check(near(a.x, b.x, 1e-6) && near(a.y, b.y, 1e-6),
        `a full push up is W (${a.x.toFixed(4)}, ${a.y.toFixed(4)})`)

  // CONTROL: the comparison is capable of failing. A push in a different
  // direction must not land on the same world vector.
  const t2 = new TouchState()
  t2.down(1, 60, 600, REGIONS)
  t2.move(1, 60 + STICK_RADIUS, 600)
  const c = screenRelativeMove(t2.move2d().x, t2.move2d().y, yaw)
  check(!near(c.x, b.x, 1e-6) || !near(c.y, b.y, 1e-6),
        'CONTROL: a push right is NOT the same world direction as W')
}

console.log('\n--- whether to show the controls at all ---')
{
  const coarse = (q) => ({ matches: q === '(pointer: coarse)' })
  const fine = () => ({ matches: false })

  check(wantsTouch(new URLSearchParams(''), coarse) === true,
        'a coarse pointer gets them')
  check(wantsTouch(new URLSearchParams(''), fine) === false,
        'and a fine one does not')

  // Both overrides, because the default is a guess either way: a touchscreen
  // laptop with a keyboard says coarse and does not want a thumbstick over the
  // game, and a desktop is where these get looked at while being built.
  check(wantsTouch(new URLSearchParams('touch=1'), fine) === true,
        '?touch=1 forces them on, which is how they are looked at on a desktop')
  check(wantsTouch(new URLSearchParams('touch=0'), coarse) === false,
        '?touch=0 forces them off')

  // A browser with no matchMedia is not a phone. Answering "yes" would put a
  // thumbstick over a game that has a keyboard.
  check(wantsTouch(new URLSearchParams(''), undefined) === false,
        'and no matchMedia at all is not a phone')
}

// THE MODULE'S OWN KEYS on the touch screen (docs/KEYS.md): those with a
// label get a button; placed by the layout when it says, otherwise in a column.
{
  const keys = [{ name: 'reload', code: 'KeyR', label: 'RELOAD' }, { name: 'map', code: 'KeyM', label: '' },
                { name: 'inv', code: 'KeyI', label: 'BAG' }]
  const plain = touchLayout(DEFAULT_LAYOUT, 'platformer')
  const withKeys = touchLayout(DEFAULT_LAYOUT, 'platformer', new Set(), keys)
  const extra = withKeys.buttons.filter((b) => b.id.startsWith('key:'))
  check(extra.length === 2 && extra.map((b) => b.label).join() === 'RELOAD,BAG',
        `a button for each key with a label, none for one without (${extra.map((b) => b.id).join(' ')})`)
  check(withKeys.buttons.length === plain.buttons.length + 2, 'and the engine\'s buttons are all still there')
  check(extra[0].x !== extra[1].x || extra[0].y !== extra[1].y, 'placed apart, not on top of each other')
  const placed = parseLayout({ touch: { stick: DEFAULT_LAYOUT.touch.stick,
                                        buttons: [...DEFAULT_LAYOUT.touch.buttons, { id: 'key:reload', anchor: 'tr', x: -30, y: 30, size: 50 }] } })
  check(placed.warnings.length === 0, `a layout may place key:<name> (${placed.warnings.join('; ')})`)
  const at = touchLayout(placed.layout, 'platformer', new Set(), keys).buttons.find((b) => b.id === 'key:reload')
  check(at?.anchor === 'tr' && at.x === -30 && at.label === 'RELOAD', 'and that is where it goes, with the key\'s label')
  check(!touchLayout(placed.layout, 'platformer').buttons.some((b) => b.id === 'key:reload'),
        'CONTROL: a placed key the server never named is not drawn')
}

console.log(`\ntouch: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
