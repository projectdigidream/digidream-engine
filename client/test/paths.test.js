//
// Camera paths through the real pipeline: maps/campath.map -> mapc -> wasm ->
// plain JS -> a spline the camera can ride.
//
// spline.test.js proves the CURVE on hand-built points. This proves the points
// reaching it are the ones somebody typed into a .map file, through the same
// wasm module the client predicts with -- and that the arc length mapc baked
// into the file is the arc length this client computes. Those are different
// failures. A correct curve fed the wrong points draws a plausible wrong shot,
// and a correct curve whose length disagrees with the server's runs a cutscene
// that ends at the wrong moment; neither is visible in the other's test.
//
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Core } from '../src/core.js'
import { readPaths, refreshPathPositions, pathIndexForTag } from '../src/mapdata.js'
import { buildSpline } from '../src/spline.js'
import { Camera, CAM_MODE } from '../src/camera.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-6) => Math.abs(a - b) <= eps

const candidates = ['arm64', 'x86_64', 'native'].map((a) => join(ROOT, 'build', a, 'campath.dnpm'))
const MAP_PATH = candidates.find((p) => existsSync(p))
if (!MAP_PATH) { console.error('no baked campath.dnpm; run mapc first'); process.exit(2) }

const core = await Core.load()
const { ok, map } = core.loadMap(readFileSync(MAP_PATH))
check(ok, `loaded ${MAP_PATH.replace(ROOT + '/', '')}`)

const paths = readPaths(map)
const pts = (p) => {
  const out = []
  for (let i = 0; i < p.pos.length; i += 3) out.push({ x: p.pos[i], y: p.pos[i + 1], z: p.pos[i + 2] })
  return out
}
const byTag = (t) => paths[pathIndexForTag(paths, t)]

console.log('\n--- the file says what the .map said ---')
{
  check(paths.length === 7, `read ${paths.length} paths`)

  const one = byTag(1)
  check(one !== undefined && one.count === 3, 'tag 1 has 3 points')
  const p1 = pts(one)
  check(near(p1[0].x, 2) && near(p1[0].y, 2) && near(p1[0].z, 1), 'tag 1 starts at 2 2 1')
  check(near(p1[2].x, 18) && near(p1[2].y, 8), 'tag 1 ends at 18 8')
  check(one.loop === false, 'tag 1 does not loop')

  const three = byTag(3)
  check(three.loop === true && three.count === 4, 'tag 3 is a 4-point loop')

  // CONTROL. Without it a pathIndexForTag that returned 0 for everything --
  // or that found nothing and returned 0 anyway -- passes every check above.
  check(pathIndexForTag(paths, 999) === -1, 'CONTROL: a tag that is not in the map resolves to -1')
}

console.log('\n--- two paths share a tag, and first match wins ---')
{
  // Tags are not unique and nothing enforces it. campath.map contains the
  // ambiguity on purpose, because without a map that has it, a lookup
  // returning the LAST match passes every other test in this file.
  const withTag5 = paths.filter((p) => p.tag === 5)
  check(withTag5.length === 2, 'the map really does contain two paths tagged 5')
  const i = pathIndexForTag(paths, 5)
  check(i === withTag5[0].index, `first match wins (index ${i})`)
  check(i < withTag5[1].index, 'CONTROL: and it is not the later one')
}

console.log('\n--- mapc and spline.js agree about how long a path is ---')
{
  // THE TEST THAT PROTECTS EVERY CUTSCENE'S TIMING.
  //
  // The engine has no completion event, so a script times a flythrough by
  // dividing its speed into the path's length -- a number the SERVER reads
  // from the file and the CLIENT rides at. They are computed by two
  // implementations in two languages, and if they drift the cutscene ends at
  // the wrong moment with nothing to say why.
  let worst = 0
  for (const p of paths) {
    const s = buildSpline(pts(p), { loop: p.loop })
    worst = Math.max(worst, Math.abs(s.length - p.curveLength))
  }
  check(worst < 1e-6, `every path's baked length matches the client's, worst ${worst.toExponential(2)}`)

  // CONTROL: it is the CURVE's length, not the polyline's. A bake that summed
  // the chords would pass the check above only if the client did too, and then
  // both would be wrong together -- so pin that the number is bigger than the
  // polyline on a path that bends.
  const one = byTag(1)
  const q = pts(one)
  let poly = 0
  for (let i = 0; i + 1 < q.length; i++) {
    poly += Math.hypot(q[i + 1].x - q[i].x, q[i + 1].y - q[i].y, q[i + 1].z - q[i].z)
  }
  check(one.curveLength > poly + 1e-3,
    `CONTROL: the baked length is the curve's (${one.curveLength.toFixed(4)}), not the polyline's (${poly.toFixed(4)})`)

  // And on a straight path they are equal, which is the other half: a bake
  // that inflated every length would pass the check above too.
  const four = byTag(4)
  check(near(four.curveLength, 16, 1e-6),
    `CONTROL: a straight 2-point path bakes to exactly its length (${four.curveLength})`)
}

console.log('\n--- a path rides a moving sector, and a static one does not ---')
{
  // The property that earns camera paths a place in the map file rather than
  // in a script. Tag 2 is anchored to the parametric lift; tag 1 is on the
  // static yard. One of them proves nothing without the other.
  const world = core.createWorld({ map })
  const FX = (n) => BigInt(n) * 4294967296n
  world.applyMotionSet({
    sector: 1,
    motionType: 1,                                   // parametric
    from: { x: 0n, y: 0n, z: 0n },
    to: { x: 0n, y: 0n, z: FX(6) },
    startTick: 0,
    durationTicks: 120,
    loopMode: 2,                                     // PingPong
    motionTarget: 0                                  // floor
  })

  const riding = byTag(2)
  const fixed = byTag(1)
  const ridingStart = Array.from(riding.pos)
  const fixedStart = Array.from(fixed.pos)

  let ridingChanged = 0
  let fixedChanged = 0
  let maxRise = 0
  for (let t = 0; t < 120; t++) {
    world.step()
    if (refreshPathPositions(map, riding)) ridingChanged++
    if (refreshPathPositions(map, fixed)) fixedChanged++
    maxRise = Math.max(maxRise, riding.pos[2] - ridingStart[2])
  }

  // One more step to reach the top of the travel. The geometry a step writes is
  // for the tick it is ENTERING, so after 120 steps the world reads tick 120
  // while the points still show 119 -- one frame short, at 5.95 of 6 units.
  // core/tests/test_rail.cpp documents the same off-by-one for waypoints.
  world.step()
  refreshPathPositions(map, riding)
  maxRise = Math.max(maxRise, riding.pos[2] - ridingStart[2])

  check(ridingChanged > 0, `the lift-anchored path reported ${ridingChanged} changes`)
  check(fixedChanged === 0, 'CONTROL: the static path reported none')
  check(near(maxRise, 6, 1e-6), `and it rose ${maxRise.toFixed(4)} units`)
  check(fixed.pos.every((v, i) => v === fixedStart[i]), 'the static path never moved at all')

  // The refresh is what makes the change visible. Reading once at load and
  // never again is the bug this guards, so pin that the points really are
  // different from the authored ones right now.
  check(riding.pos[2] !== ridingStart[2], 'the riding path is somewhere new')

  world.destroy()
}

console.log('\n--- the binding truncates rather than overruns ---')
{
  const M = core.M
  core.ensurePathBuffers()
  const one = byTag(1)
  // cap is in TRIPLES.
  const n1 = M._dnp_map_path_points(map.ptr, one.index, core.pathbuf64, 1)
  check(n1 === 1, 'cap of 1 triple returns 1')
  const n0 = M._dnp_map_path_points(map.ptr, one.index, core.pathbuf64, 0)
  check(n0 === 0, 'CONTROL: a cap of 0 returns 0 and writes nothing')
  const nBad = M._dnp_map_path_points(map.ptr, 999, core.pathbuf64, 8)
  check(nBad === 0, 'an out-of-range path returns 0')
  check(map.pathInfo(999) === null, 'and its header is null rather than garbage')
}

console.log('\n--- every path in the map builds a usable spline ---')
{
  let allGood = true
  let detail = ''
  for (const p of paths) {
    const s = buildSpline(pts(p), { loop: p.loop })
    if (!Number.isFinite(s.length) || s.length <= 0) { allGood = false; detail = `tag ${p.tag} length ${s.length}` }
    for (let i = 0; i <= 50; i++) {
      const q = s.pointAt((i / 50) * s.length)
      if (!Number.isFinite(q.x) || !Number.isFinite(q.y) || !Number.isFinite(q.z)) {
        allGood = false; detail = `tag ${p.tag} NaN at ${i}`
      }
    }
  }
  check(allGood, `all ${paths.length} paths evaluate finitely${detail ? ` (${detail})` : ''}`)

  // Including the degenerate ones the fixture carries on purpose: the two-point
  // minimum, and the loop whose first and last points are adjacent.
  check(byTag(4).count === 2, 'the two-point path is in the fixture')
  const loopSpline = buildSpline(pts(byTag(3)), { loop: true })
  check(loopSpline.segmentCount === 4, 'the loop has one segment per point')
}

console.log('\n--- the overlay can tell which paths ride an unpredictable sector ---')
{
  // Sector 2 is the scripted-discrete mover, whose offset is streamed every
  // snapshot and has no closed form. A camera path anchored there judders at
  // the snapshot rate -- the failure docs/MAP_AUTHOR.md warns about and the
  // overlay colours differently, so the anchor has to be readable.
  const seven = byTag(7)
  check(seven.anchors.length === 3, 'tag 7 exposes an anchor per point')
  check(Array.from(seven.anchors).every((a) => a === 2), 'and every one is the discrete mover, sector 2')

  const one = byTag(1)
  check(Array.from(one.anchors).every((a) => a === 0), 'CONTROL: tag 1 is anchored to the static yard')
}

console.log('\n--- the glue: a camera ends up on the path an author typed ---')
{
  // What main.js does between readPaths and camera.update, written out here
  // because it is the one step with no home of its own -- and because it is
  // where the SPACES meet. The map is z-up engine space; the camera works in
  // y-up THREE space; toThreeX/Y/Z is (x, y, z) -> (x, z, -y). Getting that
  // wrong puts the camera on a path at right angles to the authored one, which
  // no test of either half would notice.
  const toThree = (p) => ({ x: p.x, y: p.z, z: -p.y })
  const toEngine = (p) => ({ x: p.x, y: -p.z, z: p.y })

  const authored = byTag(1)
  const enginePts = pts(authored)
  const spline = buildSpline(enginePts.map(toThree), { loop: authored.loop })
  spline.id = authored.tag

  // The conversion is an isometry, so the curve the camera rides is the same
  // length as the one mapc baked. If it were not, every cutscene on this path
  // would be timed against a different curve from the one it flies.
  check(near(spline.length, authored.curveLength, 1e-6),
    `the converted spline is the same length as the baked one (${spline.length.toFixed(4)})`)

  const c = new Camera({ mode: CAM_MODE.TRACK, pathBlend: 1000 })
  const engineSubject = { x: 10, y: 6, z: 1 }
  c.snapTo(toThree(engineSubject))
  let eye = null
  for (let i = 0; i < 200; i++) eye = c.update(toThree(engineSubject), 1 / 60, null, spline)

  // Convert the eye back and ask the ENGINE-space curve where it is. Round
  // trip, so a conversion that was wrong in a self-consistent way still fails.
  const engineSpline = buildSpline(enginePts, { loop: authored.loop })
  const back = toEngine(eye)
  const d = Math.sqrt(engineSpline.nearest(back).distSq)
  check(d < 1e-6, `the eye lands on the authored path in engine space (off by ${d.toExponential(2)})`)

  // z is the axis that would survive a wrong permutation unnoticed on this
  // path, since it is the only one the three points agree on.
  check(near(back.z, 1, 1e-6), `and at the authored height z=1 (got ${back.z.toFixed(6)})`)

  // CONTROL: skipping the conversion -- building the spline from engine points
  // and handing it a THREE-space subject -- does NOT land on the path. Without
  // this, the check above passes for any code that happens to put the camera
  // somewhere on a curve.
  const wrong = new Camera({ mode: CAM_MODE.TRACK, pathBlend: 1000 })
  wrong.snapTo(toThree(engineSubject))
  let we = null
  for (let i = 0; i < 200; i++) we = wrong.update(toThree(engineSubject), 1 / 60, null, engineSpline)
  const wd = Math.sqrt(engineSpline.nearest(toEngine(we)).distSq)
  check(wd > 1, `CONTROL: skipping the conversion misses by ${wd.toFixed(2)} units`)
}

console.log(`\npaths: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
