//
// Camera tests (design §4.1).
//
import {
  Camera, DEFAULTS, CAM_MODE, cameraOffset, cameraOffsetEngine, screenRelativeMove, facingDirection, aimFor, billboardYaw,
  ISO_ELEVATION, DEFAULT_DISTANCE, distanceForFraming, viewYaw, between, autoTurnYaw, clearFraction, clearBall, byHeight
} from '../src/camera.js'
import { buildSpline } from '../src/spline.js'

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


// --- looking ahead -----------------------------------------------------------
//
// A camera centred on the body shows you the ground you are standing on. What
// you want to see is the ground you are about to walk into, so the aim point
// sits AHEAD of the subject along its facing. A distance rather than a fraction
// of velocity, so that turning on the spot moves the view -- which is how you
// look around without moving.

{
  const east = { x: 1, z: 0 }
  const north = { x: 0, z: -1 }

  // With no lead the camera centres on the subject, whatever it faces. This is
  // the default, and it has to stay the default or every existing map changes
  // how it feels.
  const plain = new Camera({ lead: 0, deadzoneX: 0, deadzoneY: 0, smoothing: 1000 })
  plain.snapTo({ x: 0, y: 0, z: 0 })
  for (let i = 0; i < 200; i++) plain.update({ x: 0, y: 0, z: 0 }, 1 / 60, east)
  check(Math.abs(plain.target.x) < 0.01,
        `lead 0 aims at the subject (${plain.target.x.toFixed(3)})`)

  // With a lead the aim point settles that far ahead, along the facing.
  const ahead = new Camera({ lead: 4, deadzoneX: 0, deadzoneY: 0, smoothing: 1000,
                             leadSmoothing: 20 })
  ahead.snapTo({ x: 0, y: 0, z: 0 })
  for (let i = 0; i < 300; i++) ahead.update({ x: 0, y: 0, z: 0 }, 1 / 60, east)
  check(Math.abs(ahead.target.x - 4) < 0.05,
        `facing east it aims 4 units east (${ahead.target.x.toFixed(2)})`)
  check(Math.abs(ahead.target.z) < 0.05, 'and nowhere north or south')

  // TURNING ON THE SPOT moves the view. The subject does not move at all here,
  // which is the whole point: the camera answers where you are looking.
  for (let i = 0; i < 300; i++) ahead.update({ x: 0, y: 0, z: 0 }, 1 / 60, north)
  check(Math.abs(ahead.target.z + 4) < 0.05,
        `turning north swings the aim north (${ahead.target.z.toFixed(2)})`)
  check(Math.abs(ahead.target.x) < 0.05, 'and back off the east axis')

  // And it EASES rather than snapping, or the view lurches on every keypress.
  const easing = new Camera({ lead: 4, deadzoneX: 0, deadzoneY: 0, smoothing: 1000,
                              leadSmoothing: 3 })
  easing.snapTo({ x: 0, y: 0, z: 0 })
  easing.update({ x: 0, y: 0, z: 0 }, 1 / 60, east)
  check(easing.lead.x > 0 && easing.lead.x < 4,
        `one frame of a turn moves the aim partway (${easing.lead.x.toFixed(3)})`)
}

// --- weight ------------------------------------------------------------------
//
// maxAccel is the difference between a camera that has weight and one that does
// not. Without it the approach is instant in velocity terms: a subject that
// teleports drags the view across the map at whatever speed the smoothing
// implies, starting on the very first frame.

{
  const free = new Camera({ maxAccel: 0, deadzoneX: 0, deadzoneY: 0, smoothing: 8 })
  const heavy = new Camera({ maxAccel: 5, deadzoneX: 0, deadzoneY: 0, smoothing: 8 })
  free.snapTo({ x: 0, y: 0, z: 0 })
  heavy.snapTo({ x: 0, y: 0, z: 0 })

  // A subject 100 units away, one frame.
  const far = { x: 100, y: 0, z: 0 }
  free.update(far, 1 / 60)
  heavy.update(far, 1 / 60)
  check(free.position.x > heavy.position.x,
        `a limited camera starts slower (${heavy.position.x.toFixed(3)} vs ` +
        `${free.position.x.toFixed(3)})`)
  // The limit is on ACCELERATION, so speed builds rather than being capped:
  // the second second of a long move covers more ground than the first.
  const a0 = heavy.position.x
  for (let i = 0; i < 60; i++) heavy.update(far, 1 / 60)
  const a1 = heavy.position.x
  for (let i = 0; i < 60; i++) heavy.update(far, 1 / 60)
  const a2 = heavy.position.x
  check((a2 - a1) > (a1 - a0),
        `speed builds (${(a1 - a0).toFixed(2)} then ${(a2 - a1).toFixed(2)} units)`)

  // It ARRIVES, and does not sail past. The first version clamped the change in
  // velocity, which limits slowing down as much as speeding up -- so it
  // overshot a 100-unit move by 111 units and came back. A spring decelerates
  // because the restoring force falls as it closes.
  let worst = 0
  for (let i = 0; i < 1200; i++) {
    heavy.update(far, 1 / 60)
    worst = Math.max(worst, heavy.position.x)
  }
  const home = 100 + heavy.cfg.offset.x
  check(Math.abs(heavy.position.x - home) < 1,
        `and settles at its target (${heavy.position.x.toFixed(1)} vs ${home})`)
  check(worst < home + 5,
        `without sailing past it (peak ${worst.toFixed(1)})`)
}

// The pitch is settable, and it is the ELEVATION rather than the yaw: at a
// higher angle the camera is further above and less far out.
{
  const low = cameraOffset(0, 40, 0.2)
  const high = cameraOffset(0, 40, 1.2)
  check(high.y > low.y, `a steeper pitch sits higher (${high.y.toFixed(1)} vs ${low.y.toFixed(1)})`)
  check(Math.hypot(high.x, high.z) < Math.hypot(low.x, low.z),
        'and closer in plan')
  // The DISTANCE is unchanged by the angle: pitch decides where on the sphere,
  // distance decides which sphere.
  check(Math.abs(Math.hypot(high.x, high.y, high.z) - 40) < 0.01 &&
        Math.abs(Math.hypot(low.x, low.y, low.z) - 40) < 0.01,
        'and both are the distance asked for')
}

// ---------------------------------------------------------------- camera paths
//
// The camera keeps its FREE solution running the whole time it is on a path.
// Most of what follows is about that: the transitions, and the fact that none
// of them is a cut.

const PT = (x, y, z) => ({ x, y, z })
const BENT = [PT(2, 2, 1), PT(18, 2, 1), PT(18, 8, 1)]
const mkPath = (pts = BENT, opts = {}) => {
  const s = buildSpline(pts, opts)
  s.id = opts.id ?? 1
  return s
}
const dist3 = (a, b) => Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z)

console.log('\n--- TRACK puts the camera ON the path ---')
{
  const path = mkPath()
  const c = new Camera({ mode: CAM_MODE.TRACK, pathBlend: 1000 })
  c.snapTo(PT(10, 0, 5))
  let worst = 0
  for (let i = 0; i < 200; i++) {
    const subj = PT(2 + i * 0.08, 0, 5)
    const e = c.update(subj, 1 / 60, null, path)
    if (i > 30) worst = Math.max(worst, Math.sqrt(path.nearest(e).distSq))
  }
  check(worst < 1e-6, `every eye is on the curve (worst ${worst.toExponential(2)})`)

  // CONTROL: the free camera is never on it, so the check above is measuring
  // the path and not measuring that any camera happens to pass nearby.
  const f = new Camera({ mode: CAM_MODE.FOLLOW })
  f.snapTo(PT(10, 0, 5))
  let best = Infinity
  for (let i = 0; i < 200; i++) {
    const subj = PT(2 + i * 0.08, 0, 5)
    const e = f.update(subj, 1 / 60, null, path)
    if (i > 30) best = Math.min(best, Math.sqrt(path.nearest(e).distSq))
  }
  check(best > 1, `CONTROL: FOLLOW never gets on it (closest ${best.toFixed(2)})`)
}

console.log('\n--- FLYTHROUGH rides at the speed it was given ---')
{
  const path = mkPath()
  const c = new Camera({ mode: CAM_MODE.FLYTHROUGH, pathSpeed: 6, pathBlend: 1000 })
  c.snapTo(PT(0, 0, 0))
  for (let i = 0; i < 120; i++) c.update(PT(0, 0, 0), 1 / 60, null, path)
  check(near(c.pathAlong, 12, 1e-9), `2s at 6 units/s is ${c.pathAlong.toFixed(4)} units of arc`)

  // CONTROL: zero speed does not move. Without it, a ride that ignored
  // pathSpeed and simply advanced would pass the check above at one speed.
  const z = new Camera({ mode: CAM_MODE.FLYTHROUGH, pathSpeed: 0, pathBlend: 1000 })
  z.snapTo(PT(0, 0, 0))
  for (let i = 0; i < 120; i++) z.update(PT(0, 0, 0), 1 / 60, null, path)
  check(z.pathAlong === 0, 'CONTROL: speed 0 does not move')

  // A non-looping ride HOLDS at the end and says so, so a script can tell
  // "finished" from "somewhere in the middle".
  const e = new Camera({ mode: CAM_MODE.FLYTHROUGH, pathSpeed: 100, pathBlend: 1000 })
  e.snapTo(PT(0, 0, 0))
  for (let i = 0; i < 120; i++) e.update(PT(0, 0, 0), 1 / 60, null, path)
  check(e.pathState.clamped === true, 'running off the end reports clamped')
  check(near(e.pathAlong, path.length, 1e-9), 'and holds there rather than running on')

  const ring = mkPath([PT(0, 0, 0), PT(10, 0, 0), PT(10, 10, 0), PT(0, 10, 0)], { loop: true, id: 2 })
  const l = new Camera({ mode: CAM_MODE.FLYTHROUGH, pathSpeed: 100, pathBlend: 1000 })
  l.snapTo(PT(0, 0, 0))
  for (let i = 0; i < 120; i++) l.update(PT(0, 0, 0), 1 / 60, null, ring)
  check(l.pathState.clamped === false, 'CONTROL: a looping ride never clamps')
}

console.log('\n--- FLYTHROUGH looks along the path; TRACK looks at you ---')
{
  const path = mkPath()
  const subj = PT(10, 0, 30)

  const f = new Camera({ mode: CAM_MODE.FLYTHROUGH, pathSpeed: 6, pathBlend: 1000 })
  f.snapTo(subj)
  let fe = null
  for (let i = 0; i < 120; i++) fe = f.update(subj, 1 / 60, null, path)
  const tan = path.tangentAt(f.pathAlong)
  const look = { x: fe.lookAt.x - fe.x, y: fe.lookAt.y - fe.y, z: fe.lookAt.z - fe.z }
  const lm = Math.hypot(look.x, look.y, look.z)
  const dot = (look.x * tan.x + look.y * tan.y + look.z * tan.z) / lm
  check(near(dot, 1, 1e-6), `the view points along the tangent (cos ${dot.toFixed(9)})`)

  // Negative speed rides backwards AND looks backwards -- one sign, not two.
  const b = new Camera({ mode: CAM_MODE.FLYTHROUGH, pathSpeed: -6, pathBlend: 1000 })
  b.snapTo(subj)
  let be = null
  for (let i = 0; i < 30; i++) be = b.update(subj, 1 / 60, null, path)
  const btan = path.tangentAt(b.pathAlong)
  const blook = { x: be.lookAt.x - be.x, y: be.lookAt.y - be.y, z: be.lookAt.z - be.z }
  const bm = Math.hypot(blook.x, blook.y, blook.z)
  const bdot = (blook.x * btan.x + blook.y * btan.y + blook.z * btan.z) / bm
  check(near(bdot, -1, 1e-6), `a negative speed looks the other way (cos ${bdot.toFixed(9)})`)

  // CONTROL: TRACK on the same path aims at the subject instead, and the two
  // answers are nowhere near each other.
  const t = new Camera({ mode: CAM_MODE.TRACK, pathBlend: 1000 })
  t.snapTo(subj)
  let te = null
  for (let i = 0; i < 120; i++) te = t.update(subj, 1 / 60, null, path)
  const tlook = { x: te.lookAt.x - te.x, y: te.lookAt.y - te.y, z: te.lookAt.z - te.z }
  const tm = Math.hypot(tlook.x, tlook.y, tlook.z)
  const tdot = (tlook.x * tan.x + tlook.y * tan.y + tlook.z * tan.z) / tm
  check(Math.abs(tdot) < Math.cos(1 * Math.PI / 180),
    `CONTROL: TRACK aims somewhere else entirely (cos ${tdot.toFixed(4)})`)
  check(near(dist3(te.lookAt, subj), 0, 1e-6), 'and what it aims at is the subject')
}

console.log('\n--- AUTO attaches and detaches, with hysteresis ---')
{
  const path = mkPath()

  // The sweep has to actually STRADDLE the boundary, so the boundary is
  // measured rather than guessed. The curve bulges off the chord -- that is the
  // whole point of it being a curve -- so the distance from (10, y, 1) to the
  // path is not y minus anything, and picking a round number for the radius
  // gives a sweep that is entirely inside or entirely outside it.
  const subjAt = (i) => PT(10, 6 + Math.sin(i * 0.31) * 0.6, 1)
  const ATTACH = Math.sqrt(path.nearest(PT(10, 6, 1)).distSq)

  let crossings = 0
  let prevIn = null
  for (let i = 0; i < 400; i++) {
    const inside = Math.sqrt(path.nearest(subjAt(i)).distSq) <= ATTACH
    if (prevIn !== null && inside !== prevIn) crossings++
    prevIn = inside
  }
  check(crossings > 10, `the sweep really does straddle the radius (${crossings} crossings)`)

  const c = new Camera({ mode: CAM_MODE.AUTO, pathAttach: ATTACH, pathBlend: 1000 })
  c.snapTo(subjAt(0))
  let flips = 0
  let was = false
  for (let i = 0; i < 400; i++) {
    c.update(subjAt(i), 1 / 60, null, path)
    if (i > 5 && c.pathAttached !== was) flips++
    was = c.pathAttached
  }
  check(flips <= 2, `hysteresis keeps the camera to ${flips} flips over the same 400 frames`)

  // CONTROL: the bare radius -- what the camera would do with DETACH_RATIO at
  // 1 -- flips on every crossing. Computed from the geometry rather than
  // through a test hook on the camera, so the control is a property of the
  // sweep and cannot be satisfied by a flag that does nothing.
  check(crossings > flips * 5,
    `CONTROL: without hysteresis the same sweep flips ${crossings} times, ${(crossings / Math.max(flips, 1)).toFixed(0)}x more`)
}

console.log('\n--- a detach is a crossfade, never a cut ---')
{
  const path = mkPath()
  const run = (snapOnDetach) => {
    const c = new Camera({ mode: CAM_MODE.AUTO, pathAttach: 4, pathBlend: 3 })
    c.snapTo(PT(10, 1, 1))
    let prev = null
    let worst = 0
    for (let i = 0; i < 400; i++) {
      // Walk in close, sit on the path, then walk away.
      const y = i < 150 ? 1 : 1 + (i - 150) * 0.08
      const wasAttached = c.pathAttached
      const e = c.update(PT(10, y, 1), 1 / 60, null, path)
      if (snapOnDetach && wasAttached && !c.pathAttached) {
        // THE VARIANT THIS PLAN REJECTED: treating a detach like a spawn.
        c.snapTo(PT(10, y, 1))
      }
      // PER FRAME, in units, because that is what a cut is: one frame in which
      // the picture is somewhere else. Averaged over a second it would be
      // indistinguishable from a fast pan, which is the thing it must be
      // distinguished from.
      if (prev) worst = Math.max(worst, dist3(e, prev))
      prev = e
    }
    return worst
  }
  const blended = run(false)
  const snapped = run(true)
  check(blended < 5, `the blend never moves the eye more than ${blended.toFixed(2)} units in a frame`)
  check(snapped > 20,
    `CONTROL: snapping on detach moves it ${snapped.toFixed(1)} units in one frame`)
  check(snapped > blended * 5,
    `which is ${(snapped / blended).toFixed(0)}x worse`)
}

console.log('\n--- a path mode with no path is FOLLOW, exactly ---')
{
  // The safety net for a tag that does not resolve. Not "close to FOLLOW":
  // identical, frame by frame, or a mistyped tag is a camera that behaves
  // subtly differently and nothing says so.
  const a = new Camera({ mode: CAM_MODE.TRACK, pathAttach: 4 })
  const b = new Camera({ mode: CAM_MODE.FOLLOW })
  a.snapTo(PT(0, 0, 0)); b.snapTo(PT(0, 0, 0))
  let identical = true
  for (let i = 0; i < 200; i++) {
    const s = PT(i * 0.1, Math.sin(i * 0.05) * 3, 0)
    const ea = a.update(s, 1 / 60, null, null)
    const eb = b.update(s, 1 / 60, null)
    if (dist3(ea, eb) !== 0 || dist3(ea.lookAt, eb.lookAt) !== 0) identical = false
  }
  check(identical, 'bit-for-bit identical to FOLLOW with no path')

  // CONTROL: hand it a path and they diverge, so the check above is not merely
  // observing that two default cameras agree.
  const path = mkPath()
  const c = new Camera({ mode: CAM_MODE.TRACK, pathBlend: 1000 })
  const d = new Camera({ mode: CAM_MODE.FOLLOW })
  c.snapTo(PT(0, 0, 0)); d.snapTo(PT(0, 0, 0))
  let diverged = false
  for (let i = 0; i < 200; i++) {
    const s = PT(i * 0.1, Math.sin(i * 0.05) * 3, 0)
    const ec = c.update(s, 1 / 60, null, path)
    const ed = d.update(s, 1 / 60, null)
    if (dist3(ec, ed) > 1) diverged = true
  }
  check(diverged, 'CONTROL: with a path they differ')
}

console.log('\n--- the free camera stays warm underneath ---')
{
  // Why a detach has somewhere continuous to go. The free solution keeps
  // tracking the subject for the whole time the path is in charge, so leaving
  // the path is a fade to a camera that is already in the right place -- not
  // one that starts catching up from wherever it was abandoned.
  const path = mkPath()
  const c = new Camera({ mode: CAM_MODE.TRACK, pathBlend: 1000 })
  c.snapTo(PT(0, 0, 0))
  const subj = PT(40, 0, 40)
  for (let i = 0; i < 300; i++) c.update(subj, 1 / 60, null, path)

  const ref = new Camera({ mode: CAM_MODE.FOLLOW })
  ref.snapTo(PT(0, 0, 0))
  for (let i = 0; i < 300; i++) ref.update(subj, 1 / 60, null)

  check(dist3(c.position, ref.position) < 1e-9,
    'the free solution ran identically the whole time it was overridden')
  check(dist3(c.eye(), c.position) > 1, 'even though the eye was somewhere else entirely')
}

console.log('\n--- shake replaces rather than sums ---')
{
  // Already true, never wired to anything. The property worth pinning before
  // it gets a wire field: two shakes a frame apart must not shake twice as
  // hard, or a script with a loop throws the camera off the map.
  const c = new Camera()
  c.snapTo(PT(0, 0, 0))
  c.screenShake(2, 0.5)
  let peak = 0
  for (let i = 0; i < 6; i++) {
    c.screenShake(2, 0.5)
    const e = c.update(PT(0, 0, 0), 1 / 60, null)
    peak = Math.max(peak, dist3(e, c.position))
  }
  check(peak <= 2 * Math.sqrt(3) + 1e-9, `six overlapping shakes peak at ${peak.toFixed(3)}, not 12`)

  // And it decays to nothing rather than ringing on.
  for (let i = 0; i < 120; i++) c.update(PT(0, 0, 0), 1 / 60, null)
  check(dist3(c.eye(), c.position) === 0, 'and it decays to exactly zero')
}

console.log('\n--- the blend does not depend on the frame rate ---')
{
  const path = mkPath()
  const at = (hz) => {
    const c = new Camera({ mode: CAM_MODE.TRACK, pathBlend: 4 })
    c.snapTo(PT(10, 2, 1))
    for (let i = 0; i < hz; i++) c.update(PT(10, 2, 1), 1 / hz, null, path)
    return c.pathMix
  }
  const a = at(30), b = at(60), d = at(144)
  check(Math.abs(a - b) < 0.01 && Math.abs(b - d) < 0.01,
    `one second of blend is the same at 30/60/144 Hz (${a.toFixed(4)}, ${b.toFixed(4)}, ${d.toFixed(4)})`)
}

console.log('\n--- the yaw the camera is actually looking along ---')
{
  // Reported as two symptoms and it is one fact: on a camera path the sprites
  // faced the wrong way and the stick pushed the wrong way. Both were reading
  // the ORBIT yaw, which on a path is neither where the camera is nor where it
  // is pointing -- it is whatever the stick last said.

  // With no path in play this must be the orbit yaw EXACTLY. Not nearly: every
  // sprite in every existing map is turned by this number, and the derived
  // angle lags it while the camera eases, so anything other than an identity
  // here is a wobble nobody asked for.
  const anywhere = { x: 3, y: 9, z: -4, lookAt: { x: 0, y: 0, z: 0 } }
  for (const y of [0, 0.7, -2.2, Math.PI, 5.9]) {
    check(viewYaw(anywhere, y, 0) === y, `mix 0 is the orbit yaw exactly (${y})`)
  }
  check(viewYaw(anywhere, 1.25, undefined) === 1.25, 'and so is no mix at all')

  // THE CONVENTION. A free camera's eye IS the orbit offset from its look-at,
  // so reading the direction back must give the orbit yaw again -- which is
  // what lets one number serve the billboards and the stick.
  for (const y of [0, 0.7, -2.2, 2.9]) {
    const off = cameraOffset(y, 12)
    const eye = { x: off.x, y: off.y, z: off.z, lookAt: { x: 0, y: 0, z: 0 } }
    check(near(viewYaw(eye, y, 1), y, 1e-9),
          `a free camera's own eye reads back as its orbit yaw (${y})`)
  }

  // ON A PATH, fully attached: the answer is where the shot is pointing and has
  // nothing to do with the orbit angle. Here the eye is due east of its
  // look-at, which is yaw 0, while the stick last said half a turn.
  const east = { x: 10, y: 4, z: 0, lookAt: { x: 0, y: 0, z: 0 } }
  check(near(viewYaw(east, Math.PI, 1), 0, 1e-9) ||
        near(Math.abs(viewYaw(east, Math.PI, 1)), Math.PI * 2, 1e-9),
        `a path camera reads its own direction (${viewYaw(east, Math.PI, 1).toFixed(4)})`)
  // CONTROL: and it is NOT the orbit yaw, or the assertion above is satisfied
  // by a function that ignores the eye entirely -- which is the bug.
  check(!near(viewYaw(east, Math.PI, 1), Math.PI, 1e-6),
        'CONTROL: and it is not the orbit angle it was handed')

  // THE SHORT ARC. An orbit yaw just under half a turn and a path direction
  // just over it are three degrees apart, not three hundred and fifty-seven --
  // and a crossfade the long way round spins every sprite in the level.
  const west = { x: -10, y: 4, z: -0.3, lookAt: { x: 0, y: 0, z: 0 } }
  const midway = viewYaw(west, Math.PI - 0.02, 0.5)
  check(Math.abs(midway) > 3, `half way across the wrap stays near a half turn (${midway.toFixed(4)})`)

  // Straight down: the look-at is directly under the eye, so the direction has
  // no yaw in it at all. The orbit angle is the only answer that is not made up.
  const overhead = { x: 5, y: 40, z: 5, lookAt: { x: 5, y: 0, z: 5 } }
  check(viewYaw(overhead, 1.1, 1) === 1.1, 'a camera looking straight down keeps the orbit yaw')
}

// --- rebase: a change of frame is not a movement ---------------------------
//
// A seam moves the body a whole cell in LOCAL coordinates and leaves it exactly
// where it was in the world. The smoothing cannot tell that from a subject that
// bolted, so it is told: everything the camera remembers moves by the same
// delta, and the view does not change at all.
{
  const PITCH = 40
  const a = new Camera()
  a.snapTo({ x: 39, y: 2, z: 5 })
  // Two frames of ordinary following, so the camera is mid-stride rather than
  // exactly on its target -- which is the state a crossing actually interrupts.
  a.update({ x: 39.2, y: 2, z: 5 }, 1 / 60)
  a.update({ x: 39.4, y: 2, z: 5 }, 1 / 60)
  const before = a.eye()

  // The crossing: local x drops by a pitch, and so does everything the camera
  // holds. The subject is the same world point said in the next cell's words.
  a.rebase({ x: -PITCH, y: 0, z: 0 })
  const after = a.eye()
  check(near(after.x, before.x - PITCH, 1e-9) &&
        near(after.y, before.y, 1e-9) && near(after.z, before.z, 1e-9),
        `rebase moves the eye by exactly the delta (${(before.x - after.x).toFixed(3)})`)

  // THE CLAIM, and it is the one that matters: the next frame after a crossing
  // looks like the next frame without one. Two cameras, one crossing a seam and
  // one walking on in a world with no boundary in it, must agree.
  const b = new Camera()
  b.snapTo({ x: 39, y: 2, z: 5 })
  b.update({ x: 39.2, y: 2, z: 5 }, 1 / 60)
  b.update({ x: 39.4, y: 2, z: 5 }, 1 / 60)
  const crossed = a.update({ x: 39.6 - PITCH, y: 2, z: 5 }, 1 / 60)
  const walked = b.update({ x: 39.6, y: 2, z: 5 }, 1 / 60)
  check(near(crossed.x + PITCH, walked.x, 1e-9) &&
        near(crossed.y, walked.y, 1e-9) && near(crossed.z, walked.z, 1e-9),
        'and the frame after a crossing is the frame that would have happened anyway')

  // CONTROL: without the rebase the same camera is a pitch behind and CHASING,
  // which is the glide this removes. Without this, "they agree" is satisfied by
  // a camera that snaps to its subject every frame and smooths nothing.
  const c = new Camera()
  c.snapTo({ x: 39, y: 2, z: 5 })
  c.update({ x: 39.2, y: 2, z: 5 }, 1 / 60)
  c.update({ x: 39.4, y: 2, z: 5 }, 1 / 60)
  const lurched = c.update({ x: 39.6 - PITCH, y: 2, z: 5 }, 1 / 60)
  check(Math.abs(lurched.x - crossed.x) > 1,
        `CONTROL: un-rebased it is ${Math.abs(lurched.x - crossed.x).toFixed(1)} units adrift`)

  // And a rate is a rate in either frame. What rebase promises is that it does
  // not TOUCH the velocity: zeroing it would stop the camera dead at the
  // boundary, which is a twitch of its own shape.
  const d = new Camera()
  d.snapTo({ x: 0, y: 2, z: 0 })
  for (let i = 1; i <= 6; i++) d.update({ x: i * 2, y: 2, z: 0 }, 1 / 60)
  const v = { ...d.velocity }
  d.rebase({ x: -PITCH, y: 0, z: 0 })
  check(d.velocity.x === v.x && d.velocity.y === v.y && d.velocity.z === v.z,
        `rebase leaves the velocity alone (${v.x.toFixed(3)})`)
  // CONTROL: there was a velocity to leave alone. A camera that had settled
  // would satisfy the check above with two zeroes.
  check(Math.abs(v.x) > 1e-6, `CONTROL: and it was moving (${v.x.toFixed(3)})`)
}

// --- a long frame is not a different simulation ----------------------------
//
// The spring is critically damped and integrated with semi-implicit Euler,
// which is stable only while w*dt stays under about two. Past that one step
// overshoots by more than the error it was correcting and the next overshoots
// further -- it rings instead of settling. Fine at 60 Hz; a quarter-second
// frame is not fine, and a seam crossing is exactly that frame because the map
// is loaded, read and spawned into inside it.
//
// Measured before the fix: a crossing placed the camera correctly and the
// spring threw it 120 units back in the same frame, then took thirty frames to
// crawl home.
{
  const far = { x: 200, y: 2, z: 0 }
  // maxAccel > 0 SELECTS THE SPRING. Left at its default of 0 every camera here
  // takes the exponential path, which is unconditionally stable and has nothing
  // to prove -- these checks passed against the broken integrator until this
  // argument was added, which is the only reason it is called out.
  const SPRING = { maxAccel: 400 }
  // One quarter-second frame, which is the clamp client/src/main.js applies.
  const slow = new Camera(SPRING)
  slow.snapTo({ x: 0, y: 2, z: 0 })
  slow.update(far, 0.25)

  // The same quarter second at 60 Hz.
  const fast = new Camera(SPRING)
  fast.snapTo({ x: 0, y: 2, z: 0 })
  for (let i = 0; i < 15; i++) fast.update(far, 1 / 60)

  const gap = Math.hypot(slow.position.x - fast.position.x,
                         slow.position.y - fast.position.y,
                         slow.position.z - fast.position.z)
  check(gap < 1, `a 0.25s frame lands where 15 frames of 1/60 land (${gap.toFixed(3)} apart)`)

  // CONTROL: it really did move. Two cameras that both stayed put would agree
  // perfectly and prove nothing.
  check(slow.position.x > 5, `CONTROL: and it travelled (${slow.position.x.toFixed(1)})`)

  // AND IT NEVER GOES BACKWARDS. The failure was not imprecision -- it was the
  // camera moving AWAY from a target it was already approaching, which no
  // amount of smoothing should ever do.
  const back = new Camera(SPRING)
  back.snapTo({ x: 0, y: 2, z: 0 })
  let worst = 0
  for (let i = 0; i < 40; i++) {
    const before = back.position.x
    back.update(far, 0.25)
    worst = Math.min(worst, back.position.x - before)
  }
  // A quarter of a unit on a two-hundred-unit move is the acceleration clamp
  // and the speed ceiling trading places, not the failure this catches: the
  // single-step integrator goes backwards by seventeen.
  check(worst >= -1, `and never lurches away from its target (worst step ${worst.toFixed(3)})`)
  // And it has SETTLED, rather than still oscillating around it. A ringing
  // integrator eventually passes near the right answer too; what it never does
  // is stop.
  const settled = back.position.x
  back.update(far, 0.25)
  back.update(far, 0.25)
  check(Math.abs(back.position.x - settled) < 0.01,
        `and comes to rest (moved ${Math.abs(back.position.x - settled).toFixed(4)} in two more frames)`)
}

// --- moving fast: the view must not jiggle ----------------------------------
console.log('camera: moving fast, the view translates rather than swings')
{
  // A subject running at 12 units a second in 60 Hz TICKS, drawn at 120 Hz: it
  // moves on every other frame, as the raw simulation position does. The view's
  // DIRECTION (eye towards what it looks at) is what reads as a jiggle.
  const run = (pick) => {
    const cam = new Camera({ deadzoneX: 0, deadzoneY: 0, smoothing: 8 })
    cam.snapTo({ x: 0, y: 0, z: 0 })
    const dirs = []
    for (let f = 0; f < 480; f++) {
      const tick = Math.floor(f / 2)
      const e = cam.update({ x: tick * 12 / 60, y: 0, z: 0 }, 1 / 120, null)
      const look = pick(cam, e)
      if (f > 360) dirs.push(Math.atan2(e.x - look.x, e.y - look.y))
    }
    // How much the angle moves frame to frame, at a steady run.
    let worst = 0
    for (let i = 1; i < dirs.length; i++) worst = Math.max(worst, Math.abs(dirs[i] - dirs[i - 1]))
    return worst
  }
  const now = run((cam, e) => e.lookAt)
  const old = run((cam) => cam.target)      // what it used to look at
  check(now < 1e-4, `looking at its smoothed focus, the view angle holds still at a steady run (${now.toExponential(2)} rad/frame)`)
  check(old > 20 * now && old > 1e-3, `CONTROL: looking at the raw target, it wobbles every tick (${old.toExponential(2)} rad/frame)`)
}
{
  // Drawn BETWEEN ticks the subject moves the same amount every frame.
  const pts = []
  for (let f = 0; f < 8; f++) {
    const tick = Math.floor(f / 2)
    const from = { x: (tick - 1) * 0.2, y: 0, z: 0 }
    const to = { x: tick * 0.2, y: 0, z: 0 }
    pts.push(between(from, to, (f % 2) / 2).x)
  }
  const steps = pts.slice(1).map((x, i) => +(x - pts[i]).toFixed(9))
  check(steps.every((d) => Math.abs(d - 0.1) < 1e-9), `between ticks, an even step every frame (${steps.join(' ')})`)
  check(between({ x: 0, y: 0, z: 0 }, { x: 10, y: 0, z: 0 }, 0.5).x === 10, 'a teleport (over the snap distance) is drawn where it ended')
  check(between(null, { x: 3, y: 1, z: 2 }, 0.5).x === 3, 'and with no tick before it, the body is where it is')
  check(between({ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 }, 7).x === 1, 'a fraction past 1 does not overshoot')
}

// --- leadVelocity: aiming further ahead the faster it runs --------------------
console.log('camera: a lead that grows with forward speed')
{
  const settle = (cfg, velocity) => {
    const cam = new Camera({ deadzoneX: 0, deadzoneY: 0, smoothing: 8, leadSmoothing: 8, ...cfg })
    cam.snapTo({ x: 0, y: 0, z: 0 })
    let e
    for (let f = 0; f < 600; f++) e = cam.update({ x: 0, y: 0, z: 0 }, 1 / 60, { x: 1, z: 0 }, null, velocity)
    return e.lookAt.x
  }
  const fast = settle({ lead: 1, leadVelocity: 0.5 }, { x: 10, z: 0 })
  check(Math.abs(fast - 6) < 0.05, `lead 1 + 0.5 x forward speed 10: it aims 6 ahead (${fast.toFixed(3)})`)
  const fixed = settle({ lead: 1, leadVelocity: 0 }, { x: 10, z: 0 })
  check(Math.abs(fixed - 1) < 0.05, `CONTROL: leadVelocity 0, the fixed lead only (${fixed.toFixed(3)})`)
  const back = settle({ lead: 1, leadVelocity: 0.5 }, { x: -10, z: 0 })
  check(Math.abs(back - 1) < 0.05, `running backwards adds nothing (${back.toFixed(3)})`)
  const side = settle({ lead: 0, leadVelocity: 0.5 }, { x: 0, z: 10 })
  check(Math.abs(side) < 0.05, `and moving sideways adds nothing: it is FORWARD speed (${side.toFixed(3)})`)
  const still = settle({ lead: 0, leadVelocity: 0.5 }, null)
  check(Math.abs(still) < 1e-6, `no velocity given, no lead (${still})`)
}

// THE AUTOMATIC CAMERA: it swings behind the body's facing, the short way
// round, and settles there; walls are found by bisecting the line to it.
{
  // Behind a body facing east (0 turns) the camera is due WEST of it.
  let yaw = 0
  for (let i = 0; i < 600; i++) yaw = autoTurnYaw(yaw, 0, 1 / 60, 2)
  const o = cameraOffsetEngine(yaw, 10)
  check(o.x < -6.2 && Math.abs(o.y) < 0.01, `behind a body facing east, the camera stands west of it (${o.x.toFixed(2)}, ${o.y.toFixed(2)})`)
  // Facing north (a quarter turn), it goes south of it -- and the short way.
  const start = yaw
  const next = autoTurnYaw(start, 0.25, 1 / 60, 2)
  let settled = start
  for (let i = 0; i < 600; i++) settled = autoTurnYaw(settled, 0.25, 1 / 60, 2)
  const s = cameraOffsetEngine(settled, 10)
  check(s.y < -6.2 && Math.abs(s.x) < 0.01, `facing north, it settles south (${s.x.toFixed(2)}, ${s.y.toFixed(2)})`)
  check(Math.abs(next - start) < 0.1 && Math.abs(settled - start) < Math.PI / 2 + 0.01, 'a smooth turn, a quarter turn the short way, not three quarters the long')
  // CONTROL: a rate of 0 does not turn at all.
  check(autoTurnYaw(1, 0.25, 1 / 60, 0) === 1, 'CONTROL: no rate, no turn')
  check(clearFraction(() => true) === 1, 'a clear line is all clear')
  const t = clearFraction((f) => f <= 0.4)
  check(t <= 0.4 && t > 0.39, `a wall 40% of the way along is found there (${t.toFixed(3)})`)
}

// BY HEIGHT: the camera 3 above the feet and 4 behind, looking at a point 1
// above them, is 2 up and 4 back from that point -- and the offset puts it
// exactly there.
{
  const h = byHeight(4, 3, 1)
  const o = cameraOffset(0, h.distance, h.elevation)
  check(near(o.y, 2, 1e-9) && near(Math.hypot(o.x, o.z), 4, 1e-9), `3 up and 4 behind, aimed 1 up: ${o.y.toFixed(3)} above the aim, ${Math.hypot(o.x, o.z).toFixed(3)} behind`)
  const below = byHeight(4, 0.5, 1)
  check(below.elevation < 0, 'CONTROL: a camera below the aim point looks up at it')
}

// THE CAMERA AS A BALL: a wall at x = 2 (a line reaching it is blocked).
{
  const wall = (a, b) => Math.max(a.x, b.x) < 2
  const head = { x: 0, y: 0, z: 1 }
  // Along the wall, 1.8 from it: the line is clear, a ball of 0.35 is not.
  const along = { x: 1.8, y: -6, z: 1 }
  const point = clearBall(wall, head, along, 0)
  const ball = clearBall(wall, head, along, 0.35)
  check(point === 1, `CONTROL: a point camera beside the wall is clear (${point})`)
  check(ball < 1 && ball > 0, `a ball there is held back from the wall (${ball.toFixed(3)} of the way)`)
  // Straight at it from 0 to 4: the point stops at the wall, the ball 0.35 short of it.
  const at = { x: 4, y: 0, z: 1 }
  const p = clearBall(wall, head, at, 0) * 4
  const b = clearBall(wall, head, at, 0.35) * 4
  check(Math.abs(p - 2) < 0.03 && Math.abs(b - 1.65) < 0.03,
        `toward the wall a point reaches ${p.toFixed(2)}, the ball ${b.toFixed(2)} (2 and 1.65)`)
  // And a clear way is clear whatever the size, straight up included.
  check(clearBall(wall, head, { x: -4, y: 3, z: 2 }, 0.35) === 1 && clearBall(wall, head, { x: 0, y: 0, z: 5 }, 0.35) === 1,
        'away from the wall, and straight up, all clear')
}

console.log(`\ncamera: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
