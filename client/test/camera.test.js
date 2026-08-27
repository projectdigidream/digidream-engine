//
// Camera tests (design §4.1).
//
import {
  Camera, DEFAULTS, cameraOffset, cameraOffsetEngine, screenRelativeMove, facingDirection, aimFor, billboardYaw,
  ISO_ELEVATION, DEFAULT_DISTANCE, distanceForFraming
} from '../src/camera.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-6) => Math.abs(a - b) <= eps

console.log('\n--- deadzone ---')
{
  const c = new Camera()
  c.snapTo({ x: 0, y: 0, z: 0 })
  // A small move inside the box moves nothing.
  for (let i = 0; i < 20; i++) c.update({ x: 1, y: 0, z: 0 }, 1 / 60)
  check(near(c.target.x, 0), `a subject inside the deadzone does not move the camera (target x=${c.target.x})`)

  // Leaving the box drags the target with it, but only by the excess.
  c.update({ x: 5, y: 0, z: 0 }, 1 / 60)
  check(near(c.target.x, 5 - DEFAULTS.deadzoneX),
        `leaving it drags the target by the excess only (${c.target.x})`)

  // And it is symmetric.
  const d = new Camera()
  d.snapTo({ x: 0, y: 0, z: 0 })
  d.update({ x: -5, y: 0, z: 0 }, 1 / 60)
  check(near(d.target.x, -5 + DEFAULTS.deadzoneX), 'symmetric in the other direction')

  // Vertical has its own, larger box: a platformer's subject leaves the ground
  // constantly and a camera that chased every jump would never be still.
  const e = new Camera()
  e.snapTo({ x: 0, y: 0, z: 0 })
  e.update({ x: 0, y: DEFAULTS.deadzoneY * 0.9, z: 0 }, 1 / 60)
  check(near(e.target.y, 0), 'a short hop does not move the camera vertically')
}

console.log('\n--- smoothing is frame-rate independent ---')
{
  // The same second of wall-clock time, at three frame rates. A naive
  // per-frame lerp would give three different answers, and camera feel would
  // depend on the machine.
  const after = (fps) => {
    const c = new Camera()
    c.snapTo({ x: 0, y: 0, z: 0 })
    const dt = 1 / fps
    for (let i = 0; i < fps; i++) c.update({ x: 40, y: 0, z: 0 }, dt)
    return c.position.x
  }
  const a30 = after(30)
  const a60 = after(60)
  const a144 = after(144)
  check(near(a30, a60, 0.05) && near(a60, a144, 0.05),
        `one second of catch-up agrees across 30/60/144 Hz (${a30.toFixed(3)}, ${a60.toFixed(3)}, ${a144.toFixed(3)})`)

  // And it converges rather than overshooting.
  const c = new Camera()
  c.snapTo({ x: 0, y: 0, z: 0 })
  let overshot = false
  const want = 40 - DEFAULTS.deadzoneX + DEFAULTS.offset.x
  for (let i = 0; i < 600; i++) {
    c.update({ x: 40, y: 0, z: 0 }, 1 / 60)
    if (c.position.x > want + 1e-9) overshot = true
  }
  check(!overshot, 'the approach never overshoots')
  check(near(c.position.x, want, 1e-6), `and settles exactly on the target (${c.position.x.toFixed(4)})`)
}

console.log('\n--- why it must follow the PREDICTED position ---')
{
  // The same motion sampled two ways. Prediction runs every tick; snapshots
  // arrive at half the rate and drop. Feeding the camera the snapshot stream
  // is the mistake, and this measures what it costs rather than asserting it.
  const run = (positions) => {
    const c = new Camera({ smoothing: 40, deadzoneX: 0, deadzoneY: 0 })
    c.snapTo({ x: positions[0], y: 0, z: 0 })
    const out = []
    for (const x of positions) out.push(c.update({ x, y: 0, z: 0 }, 1 / 60).x)
    return out
  }
  // Second differences: how much the camera's velocity changes frame to frame.
  const jerk = (xs) => {
    let total = 0
    for (let i = 2; i < xs.length; i++) total += Math.abs(xs[i] - 2 * xs[i - 1] + xs[i - 2])
    return total
  }

  const N = 120
  const predicted = []
  for (let i = 0; i < N; i++) predicted.push(i * 0.2)

  // Snapshots at half rate, with one dropped: the value is held until the next
  // arrives, which is exactly what a camera reading `snapshot.self.pos` sees.
  const fromSnapshots = []
  let held = 0
  for (let i = 0; i < N; i++) {
    if (i % 2 === 0 && i !== 40 && i !== 42) held = predicted[i]
    fromSnapshots.push(held)
  }

  const jPredicted = jerk(run(predicted))
  const jSnapshot = jerk(run(fromSnapshots))
  check(jSnapshot > jPredicted * 2,
        `tracking snapshots is ${(jSnapshot / jPredicted).toFixed(1)}x jerkier than tracking prediction`)
  check(jPredicted < 0.5, `following prediction is smooth (jerk ${jPredicted.toFixed(4)})`)
}

console.log('\n--- screen shake ---')
{
  const c = new Camera()
  c.snapTo({ x: 0, y: 0, z: 0 })
  const still = c.eye()
  check(still.x === c.position.x, 'no shake, no displacement')

  c.screenShake(2, 0.5, 7)
  let maxDisp = 0
  let frames = 0
  for (let i = 0; i < 60; i++) {
    const e = c.update({ x: 0, y: 0, z: 0 }, 1 / 60)
    maxDisp = Math.max(maxDisp, Math.abs(e.x - c.position.x))
    if (Math.abs(e.x - c.position.x) > 0) frames++
  }
  check(maxDisp > 0 && maxDisp <= 2, `shake displaces within its intensity (max ${maxDisp.toFixed(3)})`)
  check(frames > 0 && frames < 60, `and stops (${frames} of 60 frames shook)`)
  check(c.shake.remaining === 0, 'the shake is spent')

  // The eye and the look-at point move together, so a shake translates the
  // view instead of swinging it around a fixed point.
  const d = new Camera()
  d.snapTo({ x: 0, y: 0, z: 0 })
  d.screenShake(3, 1, 11)
  d.update({ x: 0, y: 0, z: 0 }, 1 / 60)
  const e = d.eye()
  check(near(e.x - d.position.x, e.lookAt.x - d.target.x),
        'the eye and the look-at point shake by the same amount')

  // Two shakes do not compound. A script in a loop must not be able to throw
  // the camera off the map.
  const f = new Camera()
  f.snapTo({ x: 0, y: 0, z: 0 })
  f.screenShake(1, 0.5, 3)
  f.screenShake(1, 0.5, 3)
  f.update({ x: 0, y: 0, z: 0 }, 1 / 60)
  const disp = Math.abs(f.eye().x - f.position.x)
  check(disp <= 1, `a second shake replaces the first rather than adding (${disp.toFixed(3)} <= 1)`)

  // Deterministic, so this test means something at all.
  const seedRun = (seed) => {
    const g = new Camera()
    g.snapTo({ x: 0, y: 0, z: 0 })
    g.screenShake(1, 0.5, seed)
    const xs = []
    for (let i = 0; i < 10; i++) { g.update({ x: 0, y: 0, z: 0 }, 1 / 60); xs.push(g.eye().x) }
    return xs.join(',')
  }
  check(seedRun(5) === seedRun(5), 'the same seed shakes the same way')
  check(seedRun(5) !== seedRun(6), 'and a different seed does not')
}

console.log('\n--- movement follows the camera ---')
{
  // Asserted as a RELATIONSHIP between the two, not as a formula restated in
  // the test. Restating it would pass for any pair of formulas that happened to
  // match, including the pair that was wrong: the original had the forward and
  // right axes swapped, which is a reflection, so the controls rotated opposite
  // to the camera.
  for (const deg of [0, 30, 45, 90, 137, 180, 250, 315]) {
    const yaw = deg * Math.PI / 180
    const off = cameraOffsetEngine(yaw)

    // Away from the camera: the horizontal direction from camera to subject.
    const len = Math.hypot(off.x, off.y)
    const away = { x: -off.x / len, y: -off.y / len }

    const w = screenRelativeMove(0, 1, yaw)
    const wl = Math.hypot(w.x, w.y)
    const dotAway = (w.x / wl) * away.x + (w.y / wl) * away.y
    check(near(dotAway, 1, 1e-9), `yaw ${deg}: W moves away from the camera (dot ${dotAway.toFixed(6)})`)

    // Camera-right, from the away vector alone: looking along `away` with z up,
    // right is `away` rotated -90 degrees, which is (y, -x).
    const right = { x: away.y, y: -away.x }
    const d = screenRelativeMove(1, 0, yaw)
    const dl = Math.hypot(d.x, d.y)
    const dotRight = (d.x / dl) * right.x + (d.y / dl) * right.y
    check(near(dotRight, 1, 1e-9), `yaw ${deg}: D moves to the camera's right (dot ${dotRight.toFixed(6)})`)
  }

  // And the axes stay perpendicular and unit, so diagonal input is not faster.
  const w = screenRelativeMove(0, 1, 1.1)
  const d = screenRelativeMove(1, 0, 1.1)
  check(near(Math.hypot(w.x, w.y), 1) && near(Math.hypot(d.x, d.y), 1), 'both axes are unit length')
  check(near(w.x * d.x + w.y * d.y, 0), 'and perpendicular')
}

console.log('\n--- projection ---')
{
  // Zooming changes the DISTANCE, never the angle. If the angle moved with the
  // zoom, which faces are visible -- and which way "away from the camera"
  // points, which the controls are derived from -- would change as you
  // scrolled.
  const elevation = (d) => {
    const o = cameraOffset(1.1, d)
    return Math.atan2(o.y, Math.hypot(o.x, o.z))
  }
  check(near(elevation(10), elevation(200), 1e-12), 'the view angle is the same at every distance')
  check(near(elevation(38), ISO_ELEVATION, 1e-12), 'and is the elevation the constant names')

  // The default distance reproduces the fixed offset this replaced, exactly, so
  // switching projection is the only thing that changed.
  const o = cameraOffset(0, DEFAULT_DISTANCE)
  check(near(o.x, 24) && near(o.y, 30) && near(o.z, 0),
        `the default is still radius 24 at height 30 (${o.x.toFixed(3)}, ${o.y.toFixed(3)})`)

  // A perspective camera is placed at the distance where its frustum is exactly
  // `viewHeight` tall at the subject. That is what makes the two projections
  // frame the subject identically, so a comparison between them shows the
  // projection and not a different shot.
  const FOV = 50
  for (const vh of [8, 26, 44, 140]) {
    const d = distanceForFraming(FOV, vh)
    const framed = 2 * d * Math.tan((FOV * Math.PI / 180) / 2)
    check(near(framed, vh, 1e-9), `fov ${FOV} at viewHeight ${vh} frames exactly ${framed.toFixed(3)} units`)
  }

  // A narrower lens has to stand further back for the same framing.
  const wide = distanceForFraming(80, 40)
  const narrow = distanceForFraming(20, 40)
  check(narrow > wide, `a narrower lens stands further back (${wide.toFixed(1)} -> ${narrow.toFixed(1)})`)
}

console.log('\n--- facing ---')
{
  // Asserted against the input encoding, not against a restated formula.
  //
  // main.js turns a world-space move direction into an aim brad with
  // atan2(wy, wx); facingDirection has to invert exactly that, through the same
  // engine->three mapping the geometry uses. Comparing the two round-trips is
  // what makes a sign error impossible to pass -- restating `-sin` here would
  // agree with a reflected implementation, which is precisely the bug that
  // shipped once already.
  const toBrad = (wx, wy) =>
    Math.round((Math.atan2(wy, wx) / (Math.PI * 2)) * 4294967296) >>> 0

  for (const deg of [0, 30, 45, 90, 137, 180, 250, 315]) {
    const th = deg * Math.PI / 180
    // An engine-space direction, and its three-space form by the mapping in
    // mapgeom.js: (x, y, z) -> (x, z, -y), so a horizontal (x, y) -> (x, -y).
    const ex = Math.cos(th)
    const ey = Math.sin(th)
    const d = facingDirection(toBrad(ex, ey))
    check(near(d.x, ex, 1e-6) && near(d.z, -ey, 1e-6),
          `facing ${deg}deg -> three (${d.x.toFixed(3)}, ${d.z.toFixed(3)}), ` +
          `wanted (${ex.toFixed(3)}, ${(-ey).toFixed(3)})`)
  }

  // Unit length at every angle, or the arrow would change size as it turns.
  for (const raw of [0, 0x20000000, 0x55555555, 0x9e3779b9, 0xffffffff]) {
    const d = facingDirection(raw)
    check(near(Math.hypot(d.x, d.z), 1, 1e-9), `raw ${raw >>> 0} is a unit direction`)
  }

  // CONTROL: the reflected version -- +sin instead of -sin -- must DISAGREE
  // with the round-trip above everywhere except where sin is zero. Without
  // this, the check above passes for both handednesses at 0 and 180 degrees.
  let disagreements = 0
  for (const deg of [30, 45, 90, 137, 250, 315]) {
    const th = deg * Math.PI / 180
    const d = facingDirection(toBrad(Math.cos(th), Math.sin(th)))
    if (!near(d.z, Math.sin(th), 1e-6)) disagreements++
  }
  check(disagreements === 6,
        `CONTROL: a reflected conversion differs at all 6 off-axis angles (${disagreements})`)
}

console.log('\n--- aim is held across a stop ---')
{
  // A centred stick means "keep facing where I was", not "face east".
  //
  // Asserted as a sequence rather than a single call, because the property is
  // about STATE: walking north, stopping, and stopping again must all report
  // north. The version this replaces returned 0 when centred, so releasing the
  // keys snapped the agent due east -- and brad 0 being a legitimate direction
  // is exactly why the engine cannot fix this by ignoring zero.
  const NORTH = aimFor(0, 1, 0)
  check(NORTH !== 0, `walking north aims somewhere other than east (${NORTH})`)

  let aim = 0
  aim = aimFor(0, 1, aim)      // walk north
  check(aim === NORTH, 'walking north aims north')
  aim = aimFor(0, 0, aim)      // let go
  check(aim === NORTH, 'letting go holds north')
  aim = aimFor(0, 0, aim)      // and stays let go
  check(aim === NORTH, 'and keeps holding it')
  aim = aimFor(-1, 0, aim)     // walk west
  check(aim !== NORTH, `walking west turns (${aim})`)
  const WEST = aim
  aim = aimFor(0, 0, aim)
  check(aim === WEST, 'and holds west after stopping')

  // Due east is a real direction, not a sentinel: walking east must report it,
  // and holding it must keep reporting it. An "ignore zero" implementation
  // would make this direction unreachable.
  let e = aimFor(0, 1, 0)
  e = aimFor(1, 0, e)
  check(e === 0, `walking east aims at brad 0 (${e})`)
  check(aimFor(0, 0, e) === 0, 'and stopping holds due east rather than reverting')

  // The held aim round-trips through the direction conversion, so the arrow
  // points where the agent last walked and not merely somewhere stable.
  const d = facingDirection(aimFor(0, 0, NORTH))
  check(near(d.x, 0, 1e-6) && near(d.z, -1, 1e-6),
        `a held north aim still points north (${d.x.toFixed(3)}, ${d.z.toFixed(3)})`)
}

console.log('\n--- billboarding ---')
{
  // Asserted as a RELATIONSHIP to cameraOffset, not as the formula restated.
  // Restating pi/2 - yaw here would agree with any implementation that happened
  // to match it, including the one with the sign of yaw backwards -- which is
  // what shipped, and what a player noticed before a test did.
  for (const deg of [0, 30, 45, 90, 137, 180, 250, 315]) {
    const yaw = deg * Math.PI / 180
    const t = billboardYaw(yaw)

    // A plane's normal after rotation.y = t.
    const n = { x: Math.sin(t), z: Math.cos(t) }
    // The horizontal direction from the sprite toward the camera.
    const off = cameraOffset(yaw)
    const len = Math.hypot(off.x, off.z)
    const toCam = { x: off.x / len, z: off.z / len }

    const dot = n.x * toCam.x + n.z * toCam.z
    check(near(dot, 1, 1e-9), `yaw ${deg}: the plane faces the camera (dot ${dot.toFixed(6)})`)
  }

  // CONTROL: the sign error. `yaw + pi/2` must point the plane AWAY from the
  // camera everywhere except where the two happen to coincide -- and if it did
  // not, the assertions above would be satisfied by either version.
  let wrong = 0
  for (const deg of [30, 45, 137, 250, 315]) {
    const yaw = deg * Math.PI / 180
    const t = yaw + Math.PI / 2
    const n = { x: Math.sin(t), z: Math.cos(t) }
    const off = cameraOffset(yaw)
    const len = Math.hypot(off.x, off.z)
    const dot = n.x * (off.x / len) + n.z * (off.z / len)
    if (!near(dot, 1, 1e-6)) wrong++
  }
  check(wrong === 5, `CONTROL: the wrong sign fails at all 5 off-axis angles (${wrong})`)
}

console.log(`\ncamera: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
