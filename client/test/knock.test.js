//
// Our blast's shove of the bodies around it, drawn when it happens
// (render/knock.js): the path the server's apply_hit makes, and an offset that
// puts a body ahead of the stream by exactly the part of the shove the stream
// has not drawn yet -- so it never moves twice.
//
import { knockPath, KnockLayer, KNOCK_DECEL, GRAVITY } from '../src/render/knock.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; if (!ok) { fails++; console.log(`  FAIL ${label}`) } }
const near = (a, b, e = 1e-6) => Math.abs(a - b) <= e

// A zombieman at the centre of a rocket: 11.67 units/s out, 11.67 up.
const path = knockPath({ x: 11.67, y: 0, z: 11.67 })
const end = path.at(-1)
// Up and down: a peak of v^2/2g, and back on the floor.
const peak = Math.max(...path.map((p) => p.z))
check(near(peak, 11.67 ** 2 / (2 * GRAVITY), 0.15), `it rises v^2/2g (${peak.toFixed(2)})`)
check(end.z === 0, 'and lands')
// It slides on after landing, braking at KNOCK_DECEL, and stops.
check(end.x > 11.67 * (2 * 11.67 / GRAVITY), `it travels out, further than its flight (${end.x.toFixed(2)})`)
check(near(path.at(-1).x, path.at(-2).x, 1e-9), 'and comes to rest')
check(end.y === 0, 'straight out, not sideways')
// CONTROL: a light tap does not slide.
const tap = knockPath({ x: 0.3, y: 0, z: 0 })
check(tap.at(-1).x < 0.3 / 60 * 2, `CONTROL: below KNOCK_MIN it barely moves (${tap.at(-1).x.toFixed(4)})`)
// CONTROL: a heavier body (a quarter of the shove) goes a sixteenth as far on the ground.
const heavy = knockPath({ x: 11.67 / 4, y: 0, z: 0 })
const light = knockPath({ x: 11.67, y: 0, z: 0 })
check(near(light.at(-1).x / heavy.at(-1).x, 16, 1.5),
      `the slide goes as v^2: ${(light.at(-1).x / heavy.at(-1).x).toFixed(1)}x for 4x the push (KNOCK_DECEL ${KNOCK_DECEL})`)

// THE OFFSET: a shove at server tick 100, the present at 106, the stream drawn
// at 96 -- the whole predicted displacement so far is ahead of the stream.
const k = new KnockLayer()
k.add('7/1', 100, { x: 11.67, y: 0, z: 0 })
const p = knockPath({ x: 11.67, y: 0, z: 0 })
const o1 = k.offset('7/1', 106, 96)
check(near(o1.x, p[6].x), `before the stream gets there, ahead by path(now - S) (${o1.x.toFixed(3)})`)
check(k.offset('8/1', 106, 96) === null, 'CONTROL: another body is not moved')
check(k.offset('7/2', 106, 96) === null, 'CONTROL: nor a new body in the same slot')
// As the stream reaches the shove, the offset closes...
const o2 = k.offset('7/1', 120, 110)
check(near(o2.x, p[20].x - p[10].x), 'while both are in it, ahead by the difference')
// ...and once both are past it, nothing is left: never moved twice.
const o3 = k.offset('7/1', 100 + p.length + 30, 100 + p.length + 20)
check(near(o3.x, 0) && near(o3.z, 0), 'once the stream is past it, no offset at all')
k.prune(100 + p.length + 20)
check(k.shoves.length === 0, 'and it is forgotten')

console.log(`knock: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
