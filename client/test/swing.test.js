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
import { sweepOffset, swingYaw } from '../src/render/swing.js'
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

console.log(`\nswing: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
