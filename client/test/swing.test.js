//
// The drawn swing collider agrees with the simulated one.
//
// It is reconstructed rather than replicated -- the client already has the
// swing's shape, because it predicts with it, and how far through the swing it
// is, because it predicts that too. That costs nothing on the wire and cannot
// disagree with the simulation about anything EXCEPT by getting the arithmetic
// wrong, which is the one thing this file is for.
//
// So the test is not "sweepOffset returns plausible numbers". It runs the real
// engine through a real swing, reads the real countdown out of the wasm core,
// and checks the drawn angle against the engine's own formula at every tick.
//
import { Core } from '../src/core.js'
import { sweepOffset, swingYaw, sameSwingShape, makeSwingMesh, SwingTrail, fadeTrail }
  from '../src/render/swing.js'
import { facingDirection } from '../src/camera.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps

const core = await Core.load()
const world = core.createWorld({})
const agent = world.spawn({ x: 0n, y: 0n, z: 0n })

const par = (n) => Number(world.getParamRaw(agent, world.paramId(n))) / 4294967296
const shape = {
  from: par('attack_sweep_from'),
  to: par('attack_sweep_to'),
  ticks: Number(world.getParamRaw(agent, world.paramId('attack_ticks')))
}

console.log('--- an idle agent has no collider ---')
{
  world.step()
  const a = world.readAgent(agent)
  check(a.attackLeft === 0, 'not swinging')
  check(sweepOffset({ ...shape, left: a.attackLeft }) === null, 'so nothing is drawn')
}

console.log('\n--- a swing runs for its whole duration ---')
{
  const ATTACK_PRIMARY = 0x0080
  const seen = []
  // Pressed once. A swing is STARTED by a press and then runs on its own, so a
  // held button would be testing something else.
  world.setInput(agent, { buttons: ATTACK_PRIMARY, moveX: 0, moveY: 0, aim: 0 })
  world.step()
  seen.push(world.readAgent(agent).attackLeft)
  // Derived from the parameter, not written out: the loop count was 14 and the
  // duration was 10, and raising the duration to 16 turned three assertions
  // into statements about how long the loop happened to be.
  for (let i = 0; i < shape.ticks + 4; i++) {
    world.setInput(agent, { buttons: 0, moveX: 0, moveY: 0, aim: 0 })
    world.step()
    seen.push(world.readAgent(agent).attackLeft)
  }

  const live = seen.filter((n) => n > 0).length
  console.log(`  countdown: ${seen.join(',')}`)
  // ticks - 1 observable values, not ticks: the engine resolves the collider
  // and THEN decrements, and a client reads state after the tick. The first
  // live sweep has already been counted down past by the time anyone can look
  // at it. See sweepOffset -- this is the observation lag, stated rather than
  // rounded off.
  check(live === shape.ticks - 1,
        `the collider is observable for ${shape.ticks - 1} of its ${shape.ticks} ticks (${live})`)
  // CONTROL: it ends. A swing that never expired would satisfy "is live" on
  // every tick and would be a weapon that is always swinging.
  check(seen.at(-1) === 0, 'and then stops')
  // Strictly decreasing while live, so the phase is monotonic and the collider
  // sweeps one way rather than jittering.
  const liveOnly = seen.filter((n) => n > 0)
  check(liveOnly.every((n, i) => i === 0 || n < liveOnly[i - 1]), 'counting down, never back up')
}

console.log('\n--- the drawn angle is the engine\'s angle ---')
{
  // Let any swing left over from the block above expire, or this one starts
  // mid-sweep and measures the tail of a different swing.
  for (let i = 0; i < shape.ticks + 2; i++) {
    world.setInput(agent, { buttons: 0, moveX: 0, moveY: 0, aim: 0 })
    world.step()
  }
  // The engine's formula, restated here ONCE and deliberately: this is the
  // comparison, so writing it out is the test. What it is compared against is
  // sweepOffset reading the countdown the engine actually produced.
  const engineOffset = (left) => {
    const total = shape.ticks === 0 ? 1 : shape.ticks
    const elapsed = total - left
    return shape.from + (shape.to - shape.from) * (elapsed / total)
  }

  let agreed = 0
  let moved = new Set()
  const ATTACK_PRIMARY = 0x0080
  world.setInput(agent, { buttons: ATTACK_PRIMARY, moveX: 0, moveY: 0, aim: 0 })
  world.step()
  for (let i = 0; i < shape.ticks + 2; i++) {
    const left = world.readAgent(agent).attackLeft
    if (left > 0) {
      const drawn = sweepOffset({ ...shape, left })
      if (near(drawn, engineOffset(left))) agreed++
      moved.add(drawn.toFixed(6))
    }
    world.setInput(agent, { buttons: 0, moveX: 0, moveY: 0, aim: 0 })
    world.step()
  }
  check(agreed === shape.ticks - 1,
        `the drawn angle matches on every observable tick (${agreed})`)
  // CONTROL: the angle actually MOVES. Every assertion above is satisfied by a
  // function returning a constant, which is precisely the bug this is drawn to
  // make visible -- a collider that does not sweep looks like one that does.
  check(moved.size > 1, `and it sweeps rather than holding still (${moved.size} distinct angles)`)
  // It starts at `from` and ends nearer `to`, in that order.
  const first = sweepOffset({ ...shape, left: shape.ticks })
  const last = sweepOffset({ ...shape, left: 1 })
  check(near(first, shape.from), `starting at attack_sweep_from (${first})`)
  check(Math.abs(last - shape.to) < Math.abs(first - shape.to), 'and ending nearer attack_sweep_to')
}

console.log('\n--- the drawn wedge points where the engine is aiming ---')
{
  // The wedge is authored in XY and laid flat, so its local +X lands at three
  // (cos phi, 0, -sin phi) after a Y rotation of phi. That has to equal the
  // engine's own facing direction in three space, which facingDirection
  // computes INDEPENDENTLY from the brad. Comparing the two is the test;
  // restating swingYaw's formula here would agree with any implementation,
  // including the negated one that shipped and drew the collider mirrored.
  let agreed = 0
  for (const deg of [0, 30, 45, 90, 137, 180, 250, 315]) {
    const turns = deg / 360
    const raw = Math.round(turns * 4294967296) >>> 0
    const phi = swingYaw(turns, 0)
    const drawn = { x: Math.cos(phi), z: -Math.sin(phi) }
    const engine = facingDirection(raw)
    if (near(drawn.x, engine.x, 1e-6) && near(drawn.z, engine.z, 1e-6)) agreed++
  }
  check(agreed === 8, `the wedge aims along the engine's facing at every angle (${agreed}/8)`)

  // CONTROL: the negated version -- what shipped -- must DISAGREE everywhere
  // the answer is not its own mirror. Without this, the check above passes for
  // both signs at 0 and 180 and says nothing about the four in between.
  let wrong = 0
  for (const deg of [30, 45, 137, 250, 315]) {
    const turns = deg / 360
    const raw = Math.round(turns * 4294967296) >>> 0
    const phi = -swingYaw(turns, 0)
    const drawn = { x: Math.cos(phi), z: -Math.sin(phi) }
    const engine = facingDirection(raw)
    if (!(near(drawn.x, engine.x, 1e-6) && near(drawn.z, engine.z, 1e-6))) wrong++
  }
  check(wrong === 5, `CONTROL: the mirrored version fails at all 5 off-axis angles (${wrong})`)

  // The sweep offset turns the wedge, and turns it the SAME way the offset
  // grows -- so the bright collider crosses the faint span in one direction
  // rather than jumping back and forth.
  const a0 = swingYaw(0, -0.125)
  const a1 = swingYaw(0, 0)
  const a2 = swingYaw(0, 0.125)
  check(a0 < a1 && a1 < a2, 'the offset rotates the wedge monotonically')
}



// Enough of three.js to inspect a Shape. What is under test is the geometry the
// builder produces, which needs no GPU and no library.
function fakeThree () {
  return {
    Shape: class {
      constructor () { this.points = [] }
      moveTo (x, y) { this.points.push({ x, y }) }
      lineTo (x, y) { this.points.push({ x, y }) }
    },
    ShapeGeometry: class {
      constructor (shape) { this.points = shape.points }
      rotateX () { return this }
      dispose () {}
    },
    MeshBasicMaterial: class { constructor (o) { Object.assign(this, o) } },
    Mesh: class {
      constructor (g, m) {
        this.geometry = g
        this.material = m
        this.visible = false
        this.rotation = { x: 0, y: 0, z: 0 }
        this.position = { x: 0, y: 0, z: 0, set (x, y, z) { this.x = x; this.y = y; this.z = z } }
      }
    }
  }
}

// --- the shape follows the parameters ----------------------------------------
//
// These were read once at startup and baked into geometry, so every attack
// parameter looked inert: the wedge on screen was the weapon the session began
// with, whatever the sliders said. Reported as "the attack graphics seemingly
// do not change".

{
  const a = { reach: 2, inner: 0, halfArc: 0.125, from: -0.125, to: 0.125 }
  check(sameSwingShape(a, { ...a }), 'an identical shape needs no rebuild')
  for (const k of ['reach', 'inner', 'halfArc', 'from', 'to']) {
    check(!sameSwingShape(a, { ...a, [k]: a[k] + 1 }),
          `a change to ${k} is a rebuild`)
  }
  check(sameSwingShape(a, { ...a, ticks: 99 }),
        'but duration is not: it changes WHEN, not what shape')
}

// A minimum reach makes a ring segment, not a wedge -- and the difference has
// to be visible, because it is the parameter that changes what the weapon can
// hit rather than how far.
{
  const THREE = fakeThree()
  const wedge = makeSwingMesh(THREE, { reachUnits: 4, halfArcTurns: 0.125 })
  const ring = makeSwingMesh(THREE, { reachUnits: 4, halfArcTurns: 0.125, innerUnits: 2 })

  // A wedge touches the origin -- it is drawn from the body outward. A ring
  // does not: its near edge is the minimum reach, which is exactly the region
  // the collider cannot hit.
  const near = (pts) => Math.min(...pts.map((p) => Math.hypot(p.x, p.y)))
  check(near(wedge.geometry.points) < 0.01,
        `a wedge reaches the body (${near(wedge.geometry.points).toFixed(2)})`)
  check(Math.abs(near(ring.geometry.points) - 2) < 0.01,
        `a ring starts at its minimum reach (${near(ring.geometry.points).toFixed(2)})`)
  // And both reach the same distance, so the inner edge has not been drawn by
  // shrinking the whole thing.
  const far = (pts) => Math.max(...pts.map((p) => Math.hypot(p.x, p.y)))
  check(Math.abs(far(wedge.geometry.points) - 4) < 0.01 &&
        Math.abs(far(ring.geometry.points) - 4) < 0.01,
        'and both still reach as far as the weapon does')
}


// --- the trail ---------------------------------------------------------------
//
// The moving collider crosses and is gone inside a sixth of a second, which is
// "appears for a brief moment and then disappears" -- and makes the sweep
// impossible to read, because you cannot see where it has been. Each trail
// segment is the collider at ONE TICK, so the trail is the set of regions that
// were actually tested rather than a decoration approximating them.

{
  const THREE = fakeThree()
  const scene = { children: [], add (o) { this.children.push(o) }, remove (o) {} }
  const trail = new SwingTrail({ THREE, scene, life: 0.5, poolSize: 8 })
  const shape = { reach: 3, inner: 0, halfArc: 0.1, from: -0.2, to: 0.2, ticks: 4 }
  const at = { shape, facingTurns: 0, x: 0, y: 0, z: 0 }

  // ONE SEGMENT PER TICK, however many frames each tick is on screen for. The
  // collider moves once per tick and the display runs at whatever rate it runs
  // at; pushing per frame would draw the same tick several times at 144 Hz and
  // skip ticks at 30, and the segment count would stop meaning "how long".
  let added = 0
  for (const left of [4, 4, 4, 3, 3, 2, 1, 1, 1]) {
    if (trail.offer({ ...at, left, offsetTurns: left * 0.05, now: 100 })) added++
  }
  check(added === 4, `four ticks leave four segments, not nine frames (${added})`)
  check(trail.count === 4, `and all four are live (${trail.count})`)

  // A segment sits where the collider was, not where the body is now.
  const angles = scene.children.filter((m) => m.visible).map((m) => m.rotation?.y)
  check(new Set(angles).size === 4, `each at its own angle (${new Set(angles).size} distinct)`)

  // No swing means no trail, and it must not carry over into the next one: a
  // countdown that starts again at 4 has to read as a new tick.
  trail.offer({ ...at, left: 0, offsetTurns: null, now: 100 })
  check(trail.offer({ ...at, left: 4, offsetTurns: 0, now: 100 }) === true,
        'a new swing starts a new trail rather than being swallowed as a repeat')

  // It fades and is recycled, or a minute of fighting leaves the screen solid.
  trail.update(100 + 0.25)
  check(trail.count === 5, 'mid-life the segments are still there')
  trail.update(100 + 0.6)
  check(trail.count === 0, 'past their life they are gone')
  check(trail.pool.length === 8, `and back in the pool (${trail.pool.length})`)
}

// Not-yet-born is fully visible, not expired. The same guard the tracers needed
// -- a segment pushed later in a frame than the timestamp used to fade it has a
// negative age, and treating that as expired destroys it on creation.
check(fadeTrail(-0.01, 0.5) === 1, 'a segment not yet born is visible')
check(fadeTrail(0, 0.5) === 1, 'a new one is fully visible')
check(fadeTrail(0.25, 0.5) === 0.5, 'half way through it is half faded')
check(fadeTrail(0.5, 0.5) === 0, 'and at its life it is gone')

console.log(`\nswing: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
