//
// Our own missiles, flown by this client (render/ghostmissiles.js): the flight
// the server's way, the burst at the impact, and the server's copies not drawn
// twice -- against a fake world with one wall.
//
import { GhostMissiles } from '../src/render/ghostmissiles.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; if (!ok) { fails++; console.log(`  FAIL ${label}`) } }
const FX = (n) => BigInt(Math.round(n * 4294967296))

// A wall at x = 10: a sweep along +x stops there. And a floor at z = 0,
// which a sloped sweep meets where its line does (World::sweep_entities).
const world = {
  sweep (from, dir, dist, radius, ignore, slope = null) {
    if (slope !== null && slope < 0n) {
      const along = (from.z << 32n) / -slope
      if (along <= dist) {
        return { distance: along, point: { x: from.x + ((dir.x * along) >> 32n), y: from.y, z: 0n }, entity: null }
      }
    }
    const wall = FX(10)
    if (dir.x <= 0n) return null
    const room = wall - from.x - radius
    if (room > dist) return null
    return { distance: room < 0n ? 0n : room, point: from, entity: null }
  }
}
let t = 0
const drawn = { made: 0, removed: 0, bursts: [], blasts: [], at: null }
const draw = {
  make: () => { drawn.made++; return {} },
  place: (mesh, p) => { drawn.at = p },
  burst: (mesh, s) => drawn.bursts.push(s),
  remove: () => drawn.removed++,
  blast: (v) => drawn.blasts.push(v)
}
const missile = { kind: 53, speed: FX(14.6), radius: FX(0.23), zOffset: FX(0.67), offset: FX(0.83),
                  life: 300, burstAnim: 1, burstTicks: 20, vfx: [{ kind: 11, radius: FX(2) }, { kind: 13, radius: FX(1.33) }] }
const g = new GhostMissiles({ world, agent: 0, draw, now: () => t })

g.launch({ pos: { x: FX(1), y: FX(5), z: FX(1) }, dir: { x: FX(1), y: 0n }, slope: 0n, aim: 0, missile })
check(drawn.made === 1 && Math.abs(drawn.at.x - 1) < 1e-6, 'drawn at the launch point the moment it fires')
let ticks = 0
while (drawn.bursts.length === 0 && ticks < 200) { g.tick(); ticks++ }
// 14.6 units/s, 60 ticks a second: from x=1 to the wall (10 - 0.23 - 0.1) is about 36 ticks.
check(ticks > 30 && ticks < 42, `it flies at its speed and bursts on the wall (${ticks} ticks)`)
// dnp.sweep_hit's rule: a tenth short of contact, but never backwards -- so
// within one step (14.6/60) of the contact point and not past it.
check(drawn.at.x <= 10 - 0.23 && drawn.at.x > 10 - 0.23 - 0.1 - 14.6 / 60,
      `at the wall, short of it (${drawn.at.x.toFixed(3)})`)
check(drawn.bursts[0] === 1 && drawn.blasts.length === 2 && drawn.blasts[0].kind === 11 &&
      drawn.blasts[0].param === 512, 'the burst and both impact effects, the radius in 256ths')
for (let i = 0; i < 25; i++) g.tick()
check(drawn.removed === 1, 'gone after its burst')

// DEDUP: the server's copy of the blast, near, is swallowed once; a second
// one, or one elsewhere, is not.
t = 100
check(g.swallow({ kind: 11, pos: { x: FX(9.6), y: FX(5.2), z: FX(1) } }), 'the server\'s blast at our impact is ours')
check(!g.swallow({ kind: 11, pos: { x: FX(9.6), y: FX(5.2), z: FX(1) } }), 'CONTROL: but only once')
check(!g.swallow({ kind: 13, pos: { x: FX(30), y: FX(5), z: FX(1) } }), 'CONTROL: one far away is not')
t = 5000
check(!g.swallow({ kind: 13, pos: { x: FX(9.6), y: FX(5.2), z: FX(1) } }), 'CONTROL: nor one long after')

// CLAIMS: the server's rocket near a live ghost is hidden; one elsewhere, or
// of another kind, is not.
const g2 = new GhostMissiles({ world, agent: 0, draw, now: () => t })
g2.launch({ pos: { x: FX(1), y: FX(5), z: FX(1) }, dir: { x: FX(1), y: 0n }, slope: 0n, aim: 0, missile })
for (let i = 0; i < 5; i++) g2.tick()
check(g2.hides(2101, 53, { x: 1.5, y: 5 }), 'the server\'s rocket by our ghost is hidden')
check(!g2.hides(2102, 53, { x: 1.5, y: 40 }), 'CONTROL: one elsewhere is not')
check(!g2.hides(2103, 99, { x: 1.5, y: 5 }), 'CONTROL: nor another kind')
for (let i = 0; i < 100; i++) g2.tick()
check(g2.hides(2101, 53, { x: 9.7, y: 5 }), 'after the burst, a copy where we burst stays hidden')
check(!g2.hides(2101, 53, { x: 9.7, y: 25 }), 'MISPREDICTED: one still flying elsewhere is shown')

// A RECYCLED SLOT is a new rocket: the server gives the second rocket the
// slot the first one freed, and it must be judged afresh, against the ghost
// flying now -- keyed by the entity, not by the slot.
const g3 = new GhostMissiles({ world, agent: 0, draw, now: () => t })
g3.launch({ pos: { x: FX(1), y: FX(5), z: FX(1) }, dir: { x: FX(1), y: 0n }, slope: 0n, aim: 0, missile })
for (let i = 0; i < 3; i++) g3.tick()
check(g3.hides('2101/1', 53, { x: 1.5, y: 5 }), 'the first rocket in a slot is ours')
for (let i = 0; i < 100; i++) g3.tick()
g3.launch({ pos: { x: FX(1), y: FX(-20), z: FX(1) }, dir: { x: FX(1), y: 0n }, slope: 0n, aim: 0, missile })
for (let i = 0; i < 3; i++) g3.tick()
check(g3.hides('2101/2', 53, { x: 1.5, y: -20 }), 'the next rocket in the same slot is judged afresh, and is ours')
check(!g3.hides('2101/1', 53, { x: 1.5, y: -20 }),
      'CONTROL: keyed by the slot alone it would still be tied to the first ghost, and shown')

// FIRED AT THE FLOOR: a rocket aimed down bursts on it, at its height, well
// short of the wall -- the rocket jump's half of the flight.
{
  const before = drawn.bursts.length
  const g4 = new GhostMissiles({ world, agent: 0, draw, now: () => t })
  g4.launch({ pos: { x: FX(1), y: FX(5), z: FX(1) }, dir: { x: FX(1), y: 0n }, slope: FX(-1), aim: 0, missile })
  for (let i = 0; i < 20; i++) g4.tick()
  check(drawn.bursts.length === before + 1, 'a rocket fired down bursts')
  check(drawn.at.z === 0 && drawn.at.x < 2.1, `on the floor, a unit along (${drawn.at.x.toFixed(2)}, ${drawn.at.z})`)
  // CONTROL: flat, from the same place, it flies on to the wall.
  const g5 = new GhostMissiles({ world, agent: 0, draw, now: () => t })
  g5.launch({ pos: { x: FX(1), y: FX(5), z: FX(1) }, dir: { x: FX(1), y: 0n }, slope: 0n, aim: 0, missile })
  for (let i = 0; i < 20; i++) g5.tick()
  check(drawn.at.x > 4 && drawn.at.z === 1, 'CONTROL: flat, it is still flying above the floor')
}

console.log(`ghostmissiles: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
