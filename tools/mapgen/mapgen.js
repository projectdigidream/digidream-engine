//
// A random map generator, for stress rather than for play.
//
//   node tools/mapgen/mapgen.js [--cells 24] [--seed 1] [--mode landmass]
//
// Two modes:
//
//   landmass     (default) sectors that TOUCH -- a connected sheet of irregular
//                concave polygons sharing exact edges, with real neighbour
//                links. This is the demanding one: shared edges mean the nav
//                graph has edges, walking between sectors exercises the step
//                rules and passability, and mapc's overlap check has to tell an
//                abutting sector from an overlapping one on every pair.
//
//   archipelago  disjoint islands over the void. Simpler, and what the first
//                version of this produced.
//
// SEEDED, and not with Math.random. The same seed must produce the same map, or
// a failure found in a generated map cannot be looked at twice -- and the point
// of generating them is being able to keep the one that broke something.
//
import { writeFileSync } from 'node:fs'

const args = process.argv.slice(2)
const arg = (name, fallback) => {
  const i = args.indexOf(`--${name}`)
  return i >= 0 && args[i + 1] !== undefined ? args[i + 1] : fallback
}

const MODE = arg('mode', 'landmass')
const N = Number(arg('cells', 24))
const SEED = Number(arg('seed', 1))
const OUT = arg('out', 'maps/big.map')
const CELL = Number(arg('cell', 12))
const STEP = 0.5                       // sim::STEP_HEIGHT; one of these is walkable

// xorshift32: small, seeded, reproducible across machines, which Math.random
// is explicitly not.
let state = (SEED >>> 0) || 1
const rnd = () => {
  state ^= state << 13; state >>>= 0
  state ^= state >>> 17
  state ^= state << 5; state >>>= 0
  return state / 4294967296
}
const range = (lo, hi) => lo + rnd() * (hi - lo)
const pick = (a) => a[Math.floor(rnd() * a.length) % a.length]
const round = (v) => Number(v.toFixed(3))

const FLOOR_TEX = ['PLATE1', 'PLATE2', 'PLATE3', 'ROCK1', 'ROCK2', 'GRATE1']
const WALL_TEX = ['EDGE1', 'EDGE2', 'EDGE3', 'RIM1', 'BRICK1']

const lines = []
const say = (s = '') => lines.push(s)

// --- the corner lattice ------------------------------------------------------
//
// (N+1)^2 points, each jittered. Cells share corner POINTS, not merely
// coincident coordinates, so two neighbouring sectors have exactly equal
// vertices -- which is what makes an abutting edge exact rather than
// nearly-exact, and what mapc's boundary test relies on.
//
// Jitter is capped at a quarter of a cell. Beyond about half, a quad can turn
// itself inside out and stop being a simple polygon, and the triangulator would
// rightly refuse it.
const JIT = CELL * 0.25
const corner = []
for (let j = 0; j <= N; j++) {
  corner.push([])
  for (let i = 0; i <= N; i++) {
    corner[j].push([round(i * CELL + range(-JIT, JIT)), round(j * CELL + range(-JIT, JIT))])
  }
}
const key = (i, j) => j * (N + 1) + i

// --- merge cells into regions ------------------------------------------------
//
// Growing edge-adjacent cells from a seed gives polyominoes: L, T, S and worse.
// Those are the shapes worth generating -- every reflex corner is a vertex the
// ear-clipper has to skip and come back to, and a map of rectangles has none.
const region = new Int32Array(N * N).fill(-1)
let regionCount = 0
const order = []
for (let c = 0; c < N * N; c++) order.push(c)
for (let i = order.length - 1; i > 0; i--) {
  const j = Math.floor(rnd() * (i + 1))
  ;[order[i], order[j]] = [order[j], order[i]]
}

for (const seed of order) {
  if (region[seed] !== -1) continue
  const id = regionCount++
  const want = 1 + Math.floor(rnd() * 5)      // 1..5 cells
  const queue = [seed]
  region[seed] = id
  let taken = 1
  while (queue.length && taken < want) {
    const c = queue.shift()
    const ci = c % N
    const cj = (c / N) | 0
    for (const [di, dj] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      if (taken >= want) break
      const ni = ci + di
      const nj = cj + dj
      if (ni < 0 || nj < 0 || ni >= N || nj >= N) continue
      const nc = nj * N + ni
      if (region[nc] !== -1) continue
      if (rnd() > 0.72) continue
      region[nc] = id
      queue.push(nc)
      taken++
    }
  }
}

// --- trace each region's boundary --------------------------------------------
//
// A cell's four edges, counter-clockwise. An edge is on the boundary when the
// cell across it is not in the same region; the region on the far side becomes
// that side's `neighbor`, or -1 for the edge of the world.
function traceRegion (id, cells) {
  const out = new Map()          // start corner -> { end, neighbour }
  for (const c of cells) {
    const i = c % N
    const j = (c / N) | 0
    const c00 = key(i, j)
    const c10 = key(i + 1, j)
    const c11 = key(i + 1, j + 1)
    const c01 = key(i, j + 1)
    const edges = [
      [c00, c10, i, j - 1],
      [c10, c11, i + 1, j],
      [c11, c01, i, j + 1],
      [c01, c00, i - 1, j]
    ]
    for (const [a, b, ni, nj] of edges) {
      const inside = ni >= 0 && nj >= 0 && ni < N && nj < N
      const other = inside ? region[nj * N + ni] : -1
      if (other === id) continue
      // A corner with two outgoing boundary edges means the region touches
      // itself at a point -- a pinch, which is not a simple polygon. Reported
      // rather than emitted; the caller splits it.
      if (out.has(a)) return null
      out.set(a, { end: b, neighbour: other })
    }
  }
  if (out.size < 3) return null

  const start = out.keys().next().value
  const loop = []
  let at = start
  for (let guard = 0; guard <= out.size; guard++) {
    const e = out.get(at)
    if (!e) return null
    loop.push({ corner: at, neighbour: e.neighbour })
    at = e.end
    if (at === start) break
  }
  // One closed loop that used every boundary edge. Fewer means the region has a
  // hole in it, which a no-holes sector cannot represent.
  if (at !== start || loop.length !== out.size) return null
  return loop
}

const cellsOf = new Map()
for (let c = 0; c < N * N; c++) {
  const id = region[c]
  if (!cellsOf.has(id)) cellsOf.set(id, [])
  cellsOf.get(id).push(c)
}

// Trace, splitting any region whose boundary is not a single simple loop into
// its individual cells -- a quad always traces.
let finalRegions = []
for (const [id, cells] of cellsOf) {
  const loop = traceRegion(id, cells)
  if (loop) { finalRegions.push({ cells, loop }); continue }
  for (const c of cells) {
    const solo = traceRegion(id, [c])
    if (solo) finalRegions.push({ cells: [c], loop: solo })
  }
}

// Renumber: a region's index in this array is the sector index mapc will give
// it, and that is what a `neighbor` reference has to name.
const indexOfOldId = new Map()
finalRegions.forEach((r, idx) => { for (const c of r.cells) indexOfOldId.set(c, idx) })
for (const r of finalRegions) {
  for (const v of r.loop) {
    // The neighbour was recorded as an old region id; translate through any
    // cell of it. After a split those ids no longer name one sector, so the
    // lookup goes via the CELL across the edge instead.
    v.neighbourIndex = -1
  }
}
// Re-derive neighbours per boundary edge from the cell across it, which
// survives the split.
finalRegions.forEach((r, idx) => {
  const mine = new Set(r.cells)
  const newLoop = []
  for (const v of r.loop) {
    newLoop.push(v)
  }
  r.loop = newLoop
  r.index = idx
  r.mine = mine
})

// For each region, walk its cells again and record, per boundary edge, which
// SECTOR is across it.
const acrossOf = new Map()   // `${regionIdx}:${startCorner}` -> sector index or -1
finalRegions.forEach((r, idx) => {
  for (const c of r.cells) {
    const i = c % N
    const j = (c / N) | 0
    const c00 = key(i, j); const c10 = key(i + 1, j)
    const c11 = key(i + 1, j + 1); const c01 = key(i, j + 1)
    const edges = [[c00, i, j - 1], [c10, i + 1, j], [c11, i, j + 1], [c01, i - 1, j]]
    for (const [a, ni, nj] of edges) {
      const inside = ni >= 0 && nj >= 0 && ni < N && nj < N
      const otherCell = inside ? nj * N + ni : -1
      const otherIdx = otherCell >= 0 ? indexOfOldId.get(otherCell) : -1
      if (otherIdx === idx) continue
      acrossOf.set(`${idx}:${a}`, otherIdx === undefined ? -1 : otherIdx)
    }
  }
})

// --- heights -----------------------------------------------------------------
//
// Quantised to the step threshold, from a smooth field plus noise. Most
// neighbours end up exactly one step apart, which is walkable, and some end up
// several, which is a wall -- so the map exercises the step rule in both
// directions rather than being uniformly flat or uniformly impassable.
function heightAt (cx, cy) {
  // Gentle on purpose. The field has to change slowly enough across one cell
  // that quantising to the step threshold leaves most seams at zero or one
  // step -- walkable. The first version used an amplitude eight times this and
  // produced a map where 51 of 257 sectors were reachable on foot: technically
  // a stress test, but of the reachability analysis rather than of walking.
  const h = Math.sin(cx * 0.02) * 0.9 + Math.cos(cy * 0.017) * 0.9 +
            Math.sin((cx + cy) * 0.011) * 0.6
  return round(Math.round(h / STEP) * STEP)
}

say('# Generated by tools/mapgen/mapgen.js -- do not hand-edit.')
say('#')
say(`#   node tools/mapgen/mapgen.js --mode ${MODE} --cells ${N} --seed ${SEED} --cell ${CELL}`)
say('#')
if (MODE === 'landmass') {
  say('# A connected sheet: every sector shares exact edges with its neighbours, so')
  say('# the nav graph has real edges and walking between sectors goes through the')
  say('# step rules. Heights are quantised to the step threshold, so most seams are')
  say('# walkable and some are cliffs. Only the outer boundary is open.')
} else {
  say('# Disjoint islands over the void.')
}
say()

let points = 0
let openEdges = 0
let linkedEdges = 0

for (const r of finalRegions) {
  let cx = 0
  let cy = 0
  for (const v of r.loop) {
    const i = v.corner % (N + 1)
    const j = (v.corner / (N + 1)) | 0
    cx += corner[j][i][0]
    cy += corner[j][i][1]
  }
  cx /= r.loop.length
  cy /= r.loop.length

  // A minority of regions are plateaus: lifted clear of the step threshold, so
  // they are cliffs their neighbours cannot walk onto. That is where the map
  // gets its verticality, rather than from a terrain field steep enough to make
  // every seam impassable.
  const plateau = MODE === 'landmass' && rnd() < 0.12 ? round(Math.round(range(2, 7) / STEP) * STEP) : 0
  const floor = MODE === 'landmass' ? round(heightAt(cx, cy) + plateau) : round(range(0, 14))
  // One sky over the whole landmass, with a minority of low-ceilinged pockets.
  //
  // Giving every sector its own random ceiling put an `upper` field at every
  // seam -- a tall wall between each pair of neighbours, technically correct and
  // visually unreadable. A shared ceiling means an upper field appears only
  // where one is meant to, and the low pockets still exercise the rule that a
  // body cannot enter a space shorter than it is.
  const lowCeiling = MODE === 'landmass' && rnd() < 0.09
  const ceil = MODE === 'landmass'
    // 24, not 60. A low pocket makes each of its neighbours draw an upper field
    // from the pocket's ceiling all the way to the sky, so a high sky turns
    // every pocket into a tower. Correct geometry either way; this is the height
    // at which the terrain is still readable.
    ? (lowCeiling ? round(floor + range(1.2, 1.9)) : 24)
    : round(floor + Math.round(range(8, 40) / STEP) * STEP)
  const light = 60 + Math.floor(rnd() * 70)
  const wall = pick(WALL_TEX)

  r.floor = floor
  say(`sector floor ${floor} ceil ${ceil} tag 1 light ${light} ` +
      `floortex ${pick(FLOOR_TEX)} thickness 2`)
  for (const v of r.loop) {
    const i = v.corner % (N + 1)
    const j = (v.corner / (N + 1)) | 0
    const [x, y] = corner[j][i]
    const across = MODE === 'landmass' ? (acrossOf.get(`${r.index}:${v.corner}`) ?? -1) : -1
    if (across >= 0) {
      // A two-sided edge needs LOWER and UPPER, not `tex`. `tex` is the middle
      // field, which on a shared boundary is only drawn across the opening --
      // so a step between two sectors at different heights had no texture on
      // its face at all, and the terrain came out full of black gaps where
      // every riser should have been.
      say(`  vertex ${x} ${y} neighbor ${across} lower ${wall} upper ${wall}`)
      linkedEdges++
    } else {
      say(`  vertex ${x} ${y} open tex ${wall}`)
      openEdges++
    }
    points++
  }
  say('end')
}

say()
say('# Lamps. Sector light levels are the ambient floor; these add on top.')
let lamps = 0
for (const r of finalRegions) {
  if (lamps >= 110 || rnd() > 0.16) continue
  const i = r.loop[0].corner % (N + 1)
  const j = (r.loop[0].corner / (N + 1)) | 0
  const [x, y] = corner[j][i]
  const warm = rnd() < 0.6
  say(`light ${x} ${y} ${round(range(10, 20))} ` +
      `color ${warm ? '255 214 165' : '165 195 255'} ` +
      `radius ${round(range(18, 30))} intensity ${round(range(1.2, 1.9))}`)
  lamps++
}

say()
const spawnAt = (name, r) => {
  // Just above the sector's own floor. A fixed height was used first and put
  // every spawn at z = 30, which is above the sky at 24 -- so an arriving
  // player started outside the room they were spawning into, and only fell into
  // it because a ceiling does not stop something already above it.
  //
  // Averaged over the polygon rather than taken from one corner, because a
  // corner of a concave sector is not necessarily inside it.
  let sx = 0
  let sy = 0
  for (const v of r.loop) {
    const i = v.corner % (N + 1)
    const j = (v.corner / (N + 1)) | 0
    sx += corner[j][i][0]
    sy += corner[j][i][1]
  }
  say(`spawn ${name} ${round(sx / r.loop.length)} ${round(sy / r.loop.length)} ` +
      `${round(r.floor + 2)}`)
}
spawnAt('start', finalRegions[Math.floor(finalRegions.length / 2)])
spawnAt('west', finalRegions[0])
spawnAt('east', finalRegions[finalRegions.length - 1])

writeFileSync(OUT, lines.join('\n') + '\n')
console.error(`[mapgen] ${OUT}: ${finalRegions.length} sectors, ${points} points, ` +
              `${linkedEdges} shared edges, ${openEdges} open, ${lamps} lights, seed ${SEED}`)
