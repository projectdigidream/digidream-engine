//
// What the renderer does per frame, as counts rather than milliseconds.
//
// WHY COUNTS. A rebuild count is deterministic and the same on every machine; a
// millisecond is whatever the laptop was doing at the time. A ms budget is
// either so loose it never fires or so tight it flakes, and a flaky performance
// gate gets deleted within a month -- so the gate here is "how many times did
// the expensive thing happen", and the timings are printed for a human to read
// rather than asserted.
//
// THE BUG THIS EXISTS FOR. `movers` is append-only for the life of a map, so
// `if (movers.length) rebuildDynamic()` re-derived surfaces, re-merged them by
// texture, allocated four fresh attribute buffers per group and re-uploaded the
// lot -- every frame, for ever, once a level's first door had been opened. It
// was invisible to the entire suite, and was found by somebody saying the
// renderer "seems quite unoptimised".
//
// Pure arithmetic: no wasm, no GPU, no THREE. followMovers takes its reader and
// its sector array as parameters precisely so a scripted door can drive it.
//
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { followMovers } from '../src/mapmovers.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')
// The repository: the engine is ROOT, and modules, docs, the studio and the
// synth are beside it.
const REPO = join(ROOT, '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// --- the scripted door -------------------------------------------------------
//
// A DOOR, NOT A LIFT, and that is the whole point of the fixture. A Doom door
// drops its ceiling to the floor and raises it again; its floor never moves.
// Any dirty check that compares only floorZ passes a lift-shaped test and
// freezes every door in the game -- so the fixture has to be the shape that
// tells those two apart.
const STILL_A = 120
const MOVING = 30
const STILL_B = 300
const FRAMES = STILL_A + MOVING + STILL_B + MOVING + STILL_A   // 600

/** Ceiling height at frame f: still, closing, still, opening, still. */
function ceilingAt (f) {
  if (f < STILL_A) return 128
  if (f < STILL_A + MOVING) return 128 - (f - STILL_A + 1) * 4      // moves 30 frames
  if (f < STILL_A + MOVING + STILL_B) return 8
  if (f < STILL_A + MOVING + STILL_B + MOVING) {
    return 8 + (f - (STILL_A + MOVING + STILL_B) + 1) * 4
  }
  return 128
}

/** Drive `follow` over the whole script and count how often it says "rebuild". */
function run (follow) {
  const sectors = [{ floorZ: 0, ceilZ: 128 }]
  const readLive = (i) => ({ floorZ: 0, ceilZ: ceilingAt(frame) })
  let frame = 0
  let rebuilds = 0
  for (; frame < FRAMES; frame++) if (follow([0], readLive, sectors)) rebuilds++
  return { rebuilds, sectors }
}

console.log('a door that moves for 60 of 600 frames')

const real = run(followMovers)
check(real.rebuilds === MOVING * 2,
      `rebuilds on the ${real.rebuilds} frames it actually moved, of ${FRAMES}`)
check(real.sectors[0].ceilZ === 128,
      `and the heights are still copied through (ceiling back to ${real.sectors[0].ceilZ})`)

// CONTROL: the bug itself. Without the comparison, every frame is a rebuild.
// Without this the positive above is satisfied by a followMovers that never
// returns true at all -- which would also "rebuild 60 times" if the count were
// wrong in the other direction.
const unguarded = (movers, readLive, sectors) => {
  for (const i of movers) {
    const live = readLive(i)
    sectors[i].floorZ = live.floorZ
    sectors[i].ceilZ = live.ceilZ
  }
  return true
}
const control = run(unguarded)
check(control.rebuilds === FRAMES,
      `CONTROL: unguarded rebuilds all ${control.rebuilds} frames (${(control.rebuilds / real.rebuilds).toFixed(0)}x)`)

// CONTROL: the likely wrong fix. Comparing only the floor is the obvious dirty
// check and it is wrong in a way that looks like "doors are broken" rather than
// like "the comparison is wrong", which is why it is worth pinning here.
const floorOnly = (movers, readLive, sectors) => {
  let moved = false
  for (const i of movers) {
    const live = readLive(i)
    if (sectors[i].floorZ !== live.floorZ) { moved = true }
    sectors[i].floorZ = live.floorZ
    sectors[i].ceilZ = live.ceilZ
  }
  return moved
}
check(run(floorOnly).rebuilds === 0,
      'CONTROL: comparing only floorZ never notices a door at all')

// CONTROL: the other likely wrong fix. Comparing against the map's BAKED
// heights rather than last frame's reports "changed" for as long as the door is
// away from where it started -- so it rebuilds through the whole 300-frame
// stretch where the door sits open and nothing is moving.
const vsBaked = (movers, readLive, sectors) => {
  let moved = false
  for (const i of movers) {
    const live = readLive(i)
    if (sectors[i].bakedCeil === undefined) sectors[i].bakedCeil = sectors[i].ceilZ
    if (sectors[i].bakedCeil !== live.ceilZ) moved = true
    sectors[i].ceilZ = live.ceilZ
  }
  return moved
}
const baked = run(vsBaked)
check(baked.rebuilds > real.rebuilds,
      `CONTROL: comparing against the baked height rebuilds ${baked.rebuilds}, ` +
      'through the whole time the door sits open')

// --- a mover that is genuinely still ----------------------------------------
//
// The steady state this is all for: a level whose doors have been opened and
// are now sitting still. `movers` is non-empty for ever; nothing should rebuild.
{
  const sectors = [{ floorZ: 0, ceilZ: 128 }, { floorZ: 16, ceilZ: 96 }]
  const readLive = (i) => ({ floorZ: sectors[i].floorZ, ceilZ: sectors[i].ceilZ })
  let rebuilds = 0
  for (let f = 0; f < 600; f++) if (followMovers([0, 1], readLive, sectors)) rebuilds++
  check(rebuilds === 0, `two settled movers over 600 frames: ${rebuilds} rebuilds`)
}

// A mover the render model has never heard of must not throw. A motion set can
// name a sector the client has not loaded geometry for during a map change.
{
  const sectors = [{ floorZ: 0, ceilZ: 128 }]
  let threw = false
  try { followMovers([0, 99], () => ({ floorZ: 0, ceilZ: 128 }), sectors) } catch { threw = true }
  check(!threw, 'a mover with no sector in the render model is skipped, not thrown on')
}

// --- structural budget on a real map ----------------------------------------
//
// Not a gate on absolutes -- printed so drift is visible in CI output. The one
// assertion is a RATIO, because that is the claim setDynamicSectors actually
// makes: deriving the movers and their neighbours must be materially cheaper
// than deriving the whole map. If the split ever stops paying, this says so,
// and a ratio survives being run on different hardware where a threshold would
// not.
const MAP = join(REPO, 'modules', 'doom2', 'maps', 'map14.dnpm')
if (!existsSync(MAP)) {
  console.log('\n  (skipping the real-map budget: no modules/doom2/maps/map14.dnpm)')
} else {
  const { Core } = await import('../src/core.js')
  const { readMap } = await import('../src/mapdata.js')
  const { buildSectorSurfaces, mergeByTexture } = await import('../src/mapgeom.js')

  const core = await Core.load()
  const { ok, map } = core.loadMap(readFileSync(MAP))
  check(ok, 'loaded modules/doom2/maps/map14.dnpm')
  const m = readMap(map)

  // The dynamic half as MapView.setDynamicSectors computes it: the movers plus
  // every neighbour, because upper and lower wall fields are drawn from the
  // neighbour's side. Which movers a session ends up with depends on which
  // doors got opened, so take a representative slice rather than inventing one.
  const movers = []
  for (let i = 0; i < m.sectors.length; i += 3) movers.push(i)
  const dynamic = new Set()
  for (const i of movers) {
    dynamic.add(i)
    const s = m.sectors[i]
    for (let k = 0; k < s.sideCount; k++) {
      const nb = m.sides[s.firstSide + k].neighbor
      if (nb >= 0) dynamic.add(nb)
    }
  }

  const derive = (indices) => {
    const t = process.hrtime.bigint()
    const surfaces = []
    for (const i of indices) surfaces.push(...buildSectorSurfaces(m, i))
    const groups = mergeByTexture(surfaces)
    return { us: Number(process.hrtime.bigint() - t) / 1000, surfaces: surfaces.length, groups }
  }

  const all = [...m.sectors.keys()]
  const whole = derive(all)
  const dyn = derive([...dynamic].sort((a, b) => a - b))

  console.log('\n  == budget: map14 ==')
  console.log(`  sectors ${m.sectors.length}   sides ${m.sides.length}   textures ${m.textures.length}`)
  console.log(`  movers ${movers.length}   dynamic set ${dynamic.size}   ` +
              `static ${m.sectors.length - dynamic.size}`)
  console.log(`  whole map   ${whole.surfaces} surfaces -> ${whole.groups.length} draws   ` +
              `${whole.us.toFixed(0)} us`)
  console.log(`  dynamic set ${dyn.surfaces} surfaces -> ${dyn.groups.length} draws   ` +
              `${dyn.us.toFixed(0)} us`)
  console.log(`  ratio ${(whole.us / dyn.us).toFixed(2)}x cheaper to derive only the movers\n`)

  check(dyn.surfaces < whole.surfaces,
        `the dynamic set is a real subset (${dyn.surfaces} of ${whole.surfaces} surfaces)`)
  check(whole.groups.length <= m.textures.length,
        `merged into ${whole.groups.length} draws for ${m.textures.length} textures`)
}

// --- parameter ids are resolved once, not seven times a frame ---------------
//
// readSwingShape reads seven named parameters every frame, and every read used
// to cross into wasm to ask what the name's id was: a TextEncoder, a _malloc, a
// heap write, a call and a _free, seven times, sixty times a second, for seven
// numbers that cannot change while the page is loaded.
{
  const { WasmWorld } = await import('../src/core.js')
  let lookups = 0
  let freed = 0
  const M = {
    HEAPU8: new Uint8Array(256),
    _malloc: () => 8,
    _free: () => { freed++ },
    _dnp_param_id_by_name: () => { lookups++; return 42 }
  }
  const w = new WasmWorld({ M }, 1)
  for (let i = 0; i < 100; i++) {
    w.paramId('reach'); w.paramId('halfArc'); w.paramId('ticks')
  }
  check(lookups === 3, `100 frames of three names cost ${lookups} wasm lookups`)
  check(freed === 3, `and ${freed} malloc/free pairs rather than 300`)
  check(w.paramId('reach') === 42, 'and the cached id is the id the engine gave')

  // CONTROL: the cost this is claiming to remove. Without the cache the same
  // driver crosses into wasm every single time -- if it did not, the assertion
  // above would be satisfied by a lookup that was never happening anyway.
  let raw = 0
  const w2 = new WasmWorld({ M: { ...M, _dnp_param_id_by_name: () => { raw++; return 42 } } }, 1)
  for (let i = 0; i < 100; i++) {
    w2.paramIds.clear()   // defeat the cache, keep everything else identical
    w2.paramId('reach'); w2.paramId('halfArc'); w2.paramId('ticks')
  }
  check(raw === 300, `CONTROL: without the cache, ${raw} lookups for the same work`)
}

// --- a sector that slides is DRAWN where it slid to -------------------------
//
// Heights were the whole story for a lift, a door and a crusher, and exactly
// half of it for a sector that travels: the engine moved its points, the
// client's own simulation moved its points, and the render model went on
// holding the ones the map was baked with. A sliding platform was solid where
// it had arrived and drawn where it started.
{
  console.log('\na sector that travels')

  const sectors = [{ floorZ: 0, ceilZ: 8, firstPoint: 0, pointCount: 4 }]
  const points = [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 10, y: 10 }, { x: 0, y: 10 }]
  let at = 0
  const readLive = () => ({
    floorZ: 0,
    ceilZ: 8,
    points: [{ x: at, y: 0 }, { x: 10 + at, y: 0 }, { x: 10 + at, y: 10 }, { x: at, y: 10 }],
    motion: { offX: at, offY: 0, pivotX: 0, pivotY: 0, turns: 0 }
  })

  // The first observation ESTABLISHES the placement -- the render model had
  // none -- so it reports a change; the second identical one must not, which is
  // the property the whole rebuild budget rests on.
  followMovers([0], readLive, sectors, points)
  check(followMovers([0], readLive, sectors, points) === false,
        'a sector that has not moved since reports no change')
  at = 3
  check(followMovers([0], readLive, sectors, points) === true,
        'and one that slid three units reports one')
  check(points[0].x === 3 && points[1].x === 13,
        `the footprint followed it (${points[0].x}, ${points[1].x})`)
  check(points[0].y === 0, 'and only along the axis it travelled')
  check(sectors[0].motion.offX === 3, 'and the render model knows where it was placed')

  // A TURN with no travel at all still counts. A sector spinning about its own
  // middle has the same offset every tick and different points every tick, so a
  // dirty check that watched the offset would freeze it on screen while it went
  // on turning in the simulation.
  at = 3
  let spun = 0
  const readSpun = () => {
    const a = spun * Math.PI / 2
    const c = Math.cos(a)
    const sn = Math.sin(a)
    const turn = (p) => ({ x: p.x * c - p.y * sn, y: p.x * sn + p.y * c })
    return {
      floorZ: 0,
      ceilZ: 8,
      points: [turn({ x: -5, y: -5 }), turn({ x: 5, y: -5 }),
               turn({ x: 5, y: 5 }), turn({ x: -5, y: 5 })],
      motion: { offX: 0, offY: 0, pivotX: 0, pivotY: 0, turns: spun / 4 }
    }
  }
  followMovers([0], readSpun, sectors, points)
  spun = 1
  check(followMovers([0], readSpun, sectors, points) === true,
        'a quarter turn with no travel is movement too')
  check(Math.abs(points[0].x - 5) < 1e-9 && Math.abs(points[0].y + 5) < 1e-9,
        `the corner turned to (${points[0].x.toFixed(2)}, ${points[0].y.toFixed(2)})`)
  check(sectors[0].motion.turns === 0.25, 'and the placement carries the angle')

  // CONTROL: the reader that only has heights -- which is every test of the
  // rebuild budget above, and was every caller before this -- must behave
  // exactly as it did. A version that required points would have made the door
  // fixture report movement on every frame of its 540 still ones.
  const onlyHeights = [{ floorZ: 0, ceilZ: 8, firstPoint: 0, pointCount: 4 }]
  check(followMovers([0], () => ({ floorZ: 0, ceilZ: 8 }), onlyHeights, points) === false,
        'CONTROL: a reader with no footprint reports no movement')
}

console.log(`\nrenderbudget: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
