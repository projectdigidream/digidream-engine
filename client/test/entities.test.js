//
// Remote entity interpolation (docs/protocol.md §11, protocol R8).
//
// Three properties, and each fails in a way that looks like something else:
//
//   Absence means UNCHANGED. A delta snapshot omits an entity that did not
//   move, so treating absence as removal makes every stationary player blink.
//   Treating it as "no new sample" is what makes them stand still.
//
//   A recycled slot is a DIFFERENT entity. Keeping the old samples interpolates
//   the newcomer from wherever its predecessor died -- an enemy visibly flying
//   across the map at the moment it is replaced.
//
//   Never extrapolate past the newest sample. A guess about someone else's
//   movement has to be taken back when it is wrong, and a visible correction is
//   worse than a few frames of standing still.
//
import { EntityView, INTERP_DELAY_TICKS } from '../src/render/entities.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps

const ENT = { POS: 1 << 0, FACING: 1 << 1, DESTROYED: 1 << 7, ANIM: 1 << 4, SOLID: 1 << 8 }
const handle = (index, generation) => (index & 0xffffff) | ((generation & 0xff) << 24)

// A stand-in for a Three.js scene and mesh: only add/remove and a position.
const fakeScene = () => {
  const objects = new Set()
  return {
    add: (o) => objects.add(o),
    remove: (o) => objects.delete(o),
    objects
  }
}
const fakeMesh = () => ({ position: { x: 0, y: 0, z: 0, set (x, y, z) { this.x = x; this.y = y; this.z = z } }, userData: {} })

const CONV = {
  fxToFloat: (v) => Number(v),
  toThree: (x, y, z) => ({ x, y: z, z: -y })
}
const snap = (tick, entities) => ({ serverTick: tick, entities })
const posEnt = (index, generation, x, y, z) => ({
  handle: handle(index, generation), mask: ENT.POS, pos: { x, y, z }
})

console.log('\n--- interpolation ---')
{
  const scene = fakeScene()
  const view = new EntityView({ makeMesh: fakeMesh, scene })

  view.ingest(snap(100, [posEnt(3, 1, 0, 0, 0)]), CONV)
  view.ingest(snap(110, [posEnt(3, 1, 10, 0, 0)]), CONV)
  check(view.count === 1, 'one entity tracked')
  check(scene.objects.size === 1, 'and one mesh in the scene')

  const mesh = [...scene.objects][0]

  // Rendered at newest - delay, so at newestTick 110 the render time is 106 --
  // 60% of the way from the sample at 100 to the one at 110.
  view.update(110, 0)
  const t = (110 - INTERP_DELAY_TICKS - 100) / 10
  check(near(mesh.position.x, 10 * t), `interpolated between samples (x=${mesh.position.x})`)

  // Halfway through a display frame, the render time moves fractionally. Whole
  // ticks would reintroduce exactly the stutter the delay removes.
  view.update(110, 0.5)
  check(mesh.position.x > 10 * t, 'a fractional frame moves it further')

  // Past the newest sample it HOLDS rather than extrapolating.
  view.update(200, 0)
  check(near(mesh.position.x, 10), `held at the last known position (x=${mesh.position.x})`)
}

console.log('\n--- a teleport is a jump, not a slide ---')
{
  const scene = fakeScene()
  const view = new EntityView({ makeMesh: fakeMesh, scene })
  const TELEPORTED = 1 << 11
  view.ingest(snap(100, [posEnt(4, 1, 0, 0, 0)]), CONV)
  view.ingest(snap(110, [{ ...posEnt(4, 1, 30, 0, 0), mask: ENT.POS | TELEPORTED }]), CONV)
  const mesh = [...scene.objects][0]
  // The render time is 106, between the two samples: a move would be drawn
  // 60% of the way, a jump is drawn where it was until it happens.
  view.update(110, 0)
  check(near(mesh.position.x, 0), `before the jump's tick it is where it was (x=${mesh.position.x})`)
  view.update(110 + INTERP_DELAY_TICKS, 0)
  check(near(mesh.position.x, 30), `and on it, where it went (x=${mesh.position.x})`)
  check(view.entities.get(4).jumpTick === 110, 'the jump is remembered, for the present path')
  // CONTROL: the same two samples without the flag are an ordinary move.
  const plain = new EntityView({ makeMesh: fakeMesh, scene: fakeScene() })
  plain.ingest(snap(100, [posEnt(4, 1, 0, 0, 0)]), CONV)
  plain.ingest(snap(110, [posEnt(4, 1, 30, 0, 0)]), CONV)
  plain.update(110, 0)
  check(plain.entities.get(4).mesh.position.x > 10, 'CONTROL: without the flag it slides')
}

console.log('\n--- absence means unchanged ---')
{
  const scene = fakeScene()
  const view = new EntityView({ makeMesh: fakeMesh, scene })
  view.ingest(snap(100, [posEnt(3, 1, 5, 0, 0)]), CONV)
  // Two snapshots with the entity entirely absent, then one where it is present
  // but carries no POS bit. Neither may remove it.
  view.ingest(snap(102, []), CONV)
  view.ingest(snap(104, [{ handle: handle(3, 1), mask: ENT.FACING, facing: 7 }]), CONV)
  check(view.count === 1, 'an omitted entity is not removed')
  check(scene.objects.size === 1, 'and its mesh stays in the scene')

  const mesh = [...scene.objects][0]
  view.update(104, 0)
  check(near(mesh.position.x, 5), 'and it holds its position rather than blinking')
}

console.log('\n--- destruction ---')
{
  const scene = fakeScene()
  const view = new EntityView({ makeMesh: fakeMesh, scene })
  view.ingest(snap(100, [posEnt(3, 1, 5, 0, 0)]), CONV)
  view.ingest(snap(102, [{ handle: handle(3, 1), mask: ENT.DESTROYED }]), CONV)
  check(view.count === 0, 'an explicit DESTROYED removes it')
  check(scene.objects.size === 0, 'and takes its mesh out of the scene')
}

console.log('\n--- a recycled slot is a different entity (R8) ---')
{
  const scene = fakeScene()
  const view = new EntityView({ makeMesh: fakeMesh, scene })

  // Generation 1 lives at x = 100 and dies there.
  view.ingest(snap(100, [posEnt(3, 1, 0, 0, 0)]), CONV)
  view.ingest(snap(110, [posEnt(3, 1, 100, 0, 0)]), CONV)

  // The slot is reused: generation 2, spawning at the origin.
  view.ingest(snap(120, [posEnt(3, 2, 0, 0, 0)]), CONV)
  view.ingest(snap(130, [posEnt(3, 2, 1, 0, 0)]), CONV)

  const mesh = [...scene.objects][0]
  view.update(130, 0)
  // Between 0 and 1, where generation 2 actually is -- NOT somewhere on the
  // line back from 100, which is what inheriting the samples would give.
  check(mesh.position.x >= 0 && mesh.position.x <= 1,
        `the newcomer interpolates from its own samples (x=${mesh.position.x.toFixed(3)})`)
  check(view.count === 1, 'and there is still exactly one entity in the slot')

  // The control: with the generation ignored, the same sequence puts it
  // somewhere between 100 and 0 -- flying across the map. Verified by feeding
  // the SAME generation, which is what "ignoring it" amounts to.
  const scene2 = fakeScene()
  const view2 = new EntityView({ makeMesh: fakeMesh, scene: scene2 })
  view2.ingest(snap(100, [posEnt(3, 1, 0, 0, 0)]), CONV)
  view2.ingest(snap(110, [posEnt(3, 1, 100, 0, 0)]), CONV)
  view2.ingest(snap(120, [posEnt(3, 1, 0, 0, 0)]), CONV)
  view2.ingest(snap(130, [posEnt(3, 1, 1, 0, 0)]), CONV)
  const mesh2 = [...scene2.objects][0]
  view2.update(122, 0)
  check(mesh2.position.x > 10,
        `CONTROL: ignoring the generation flies it across the map (x=${mesh2.position.x.toFixed(1)})`)
}

console.log('\n--- coordinates ---')
{
  const scene = fakeScene()
  const view = new EntityView({ makeMesh: fakeMesh, scene })
  view.ingest(snap(100, [posEnt(3, 1, 7, 11, 13)]), CONV)
  const mesh = [...scene.objects][0]
  view.update(100 + INTERP_DELAY_TICKS, 0)
  // Engine (x, y, z) -> three (x, z, -y), the same rotation mapgeom uses.
  check(near(mesh.position.x, 7) && near(mesh.position.y, 13) && near(mesh.position.z, -11),
        `engine (7,11,13) becomes three (${mesh.position.x},${mesh.position.y},${mesh.position.z})`)
}

// --- a corpse stops being solid, and stops moving ----------------------------
//
// THE BUG THIS EXISTS FOR. A monster that dies stops moving, so the block that
// announces it is a corpse carries SOLID (and ANIMATION_STATE) and NO POS at
// all. Solidity used to be handled after `if (!(mask & POS)) continue`, so that
// block was skipped: the death animation played, the body lay on the floor, and
// the collider this client mirrors into its own world stayed solid for ever.
// The player then walked into an invisible corpse that the server walks
// straight through -- an invisible wall exactly where something died.
//
// It only happened SOMETIMES, which is what made it hard to see: a corpse that
// settles in the same tick puts POS back in the mask and takes the working path.
console.log('\n--- a corpse that does not move still stops being solid ---')
{
  const scene = fakeScene()
  const bodies = []
  const view = new EntityView({
    makeMesh: fakeMesh, scene,
    onBody: (index, kind, pos, solid) => bodies.push({ index, pos, solid })
  })

  // Alive and moving: solid, with a position.
  view.ingest(snap(100, [{ handle: handle(4, 1), mask: ENT.POS | ENT.SOLID,
                           pos: { x: 5, y: 0, z: 0 }, solid: 1 }]), CONV)
  check(bodies.length === 1 && bodies[0].solid === true,
        'a living body is mirrored as solid')

  // Dies. No POS -- it is not going anywhere.
  view.ingest(snap(102, [{ handle: handle(4, 1), mask: ENT.SOLID | ENT.ANIM,
                           solid: 0, animationState: 4 }]), CONV)
  const last = bodies.at(-1)
  check(bodies.length === 2, `the corpse reached the mirror (${bodies.length} calls)`)
  check(last && last.solid === false, 'and did so as NOT solid')

  // THE CONTROL. The same block WITH a position took the working path all
  // along, which is why this was intermittent rather than constant -- so a test
  // that only sent one with POS would have passed against the bug.
  const scene2 = fakeScene()
  const bodies2 = []
  const view2 = new EntityView({
    makeMesh: fakeMesh, scene: scene2,
    onBody: (index, kind, pos, solid) => bodies2.push({ solid })
  })
  view2.ingest(snap(100, [{ handle: handle(4, 1), mask: ENT.POS | ENT.SOLID,
                            pos: { x: 5, y: 0, z: 0 }, solid: 1 }]), CONV)
  view2.ingest(snap(102, [{ handle: handle(4, 1), mask: ENT.POS | ENT.SOLID | ENT.ANIM,
                            pos: { x: 5, y: 0, z: 0 }, solid: 0, animationState: 4 }]), CONV)
  check(bodies2.at(-1)?.solid === false,
        'CONTROL: with a position in the same block it always worked')
}

console.log(`\nentities: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
