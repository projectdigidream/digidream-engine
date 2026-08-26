//
// The real pipeline: a baked .dnpm -> wasm -> plain JS -> drawable surfaces.
//
// mapgeom.test.js proves the geometry RULES on hand-built fixtures. This proves
// the rules are being applied to the map the server actually ships, through the
// same wasm module the client predicts with. The two are different failures: a
// correct builder fed a map read out wrong draws a plausible-looking wrong
// room, and no amount of fixture testing would see it.
//
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Core } from '../src/core.js'
import { readMap, NO_TEXTURE, NO_SECTOR } from '../src/mapdata.js'
import { buildMapSurfaces, mergeByTexture, SURFACE } from '../src/mapgeom.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

const candidates = ['arm64', 'x86_64', 'native'].map((a) => join(ROOT, 'build', a, 'test.dnpm'))
const MAP_PATH = candidates.find((p) => existsSync(p))
if (!MAP_PATH) { console.error('no baked map; run mapc first'); process.exit(2) }

const core = await Core.load()
const { ok, map } = core.loadMap(readFileSync(MAP_PATH))
check(ok, `loaded ${MAP_PATH.replace(ROOT + '/', '')}`)

const m = readMap(map)
check(m.sectors.length === 3, `read ${m.sectors.length} sectors`)
check(m.sides.length === 12, `read ${m.sides.length} sides`)
check(m.points.length === 12, `read ${m.points.length} points`)
check(m.tris.length === 18, `read ${m.tris.length} triangle indices`)

// maps/test.map names these; if the table came back empty the render section
// was written but never read, which round-tripping in C++ would not notice.
check(m.textures.length > 0, `read ${m.textures.length} texture names: ${m.textures.join(' ')}`)
check(m.textures.includes('WALL1'), 'including WALL1, which the map authors')
// Asserted as PROPERTIES, not as literals. The first version pinned the exact
// values the map happened to have, and broke the moment the map was relit --
// reporting a lighting change as a map-loading failure. What matters is that
// the authored levels arrived at all and are per-sector.
const lights = m.sectors.map((s) => s.light)
check(lights.every((l) => l >= 0 && l <= 255), `light levels in range (${lights.join(', ')})`)
check(lights.some((l) => l !== 255),
      'at least one sector is not at the default, so the render section was read')
check(new Set(lights).size > 1, 'and the levels differ between sectors')
check(m.lights.length > 0, `read ${m.lights.length} point lights`)
check(m.lights.every((L) => L.radius > 0 && L.intensity > 0),
      'each with a usable radius and intensity')

// The neighbour links survived the unsigned round trip. -1 arriving as
// 4294967295 would index nothing and every solid wall would be treated as a
// portal to sector 4 billion.
const walls = m.sides.filter((s) => s.neighbor === NO_SECTOR).length
const portals = m.sides.filter((s) => s.neighbor !== NO_SECTOR).length
check(walls === 8 && portals === 4, `${walls} solid walls, ${portals} two-sided`)
check(m.sides.every((s) => s.neighbor === NO_SECTOR || (s.neighbor >= 0 && s.neighbor < 3)),
      'every neighbour index is in range')

const surfaces = buildMapSurfaces(m)
check(surfaces.length > 0, `built ${surfaces.length} surfaces`)
const kinds = {}
for (const s of surfaces) kinds[s.kind] = (kinds[s.kind] ?? 0) + 1
console.log(`       ${Object.entries(kinds).map(([k, v]) => `${k}:${v}`).join('  ')}`)
check(kinds[SURFACE.FLOOR] === 3, 'one floor per sector')
check(kinds[SURFACE.CEIL] === 3, 'one ceiling per sector')
check((kinds[SURFACE.MIDDLE] ?? 0) === 8, 'one middle field per solid wall')

// maps/test.map's sector 2 floor is at 4 while its neighbour sector 1 is at 0,
// so sector 1 sees a lower field where it meets sector 2 -- and sector 2 does
// not, because the step goes the other way.
const lowerSides = surfaces.filter((s) => s.kind === SURFACE.LOWER).map((s) => s.sector)
check(lowerSides.includes(1) && !lowerSides.includes(2),
      'the step up to sector 2 is drawn from sector 1\'s side only')

// Nothing degenerate reaches the GPU.
let finite = true
let inRange = true
let uvFinite = true
for (const s of surfaces) {
  for (const v of s.positions) if (!Number.isFinite(v)) finite = false
  for (const v of s.uvs) if (!Number.isFinite(v)) uvFinite = false
  if (s.texture !== NO_TEXTURE && s.texture >= m.textures.length) inRange = false
  for (const i of s.indices) if (i >= s.positions.length / 3) inRange = false
}
check(finite, 'every position is finite')
check(uvFinite, 'every uv is finite')
check(inRange, 'every texture id and index is in range')

// The mesh occupies the space the map describes: x 0..80, y 0..40 (which is
// three-space z -40..0), z 0..40.
let minX = Infinity; let maxX = -Infinity
let minY = Infinity; let maxY = -Infinity
let minZ = Infinity; let maxZ = -Infinity
for (const s of surfaces) {
  for (let i = 0; i < s.positions.length; i += 3) {
    minX = Math.min(minX, s.positions[i]); maxX = Math.max(maxX, s.positions[i])
    minY = Math.min(minY, s.positions[i + 1]); maxY = Math.max(maxY, s.positions[i + 1])
    minZ = Math.min(minZ, s.positions[i + 2]); maxZ = Math.max(maxZ, s.positions[i + 2])
  }
}
check(minX === 0 && maxX === 80, `x spans ${minX}..${maxX} (map is 0..80)`)
check(minY === 0 && maxY === 40, `height spans ${minY}..${maxY} (floors 0..4, ceilings 40)`)
check(minZ === -40 && maxZ === 0, `depth spans ${minZ}..${maxZ} (map y 0..40, negated)`)

const groups = mergeByTexture(surfaces)
check(groups.length <= m.textures.length,
      `merged into ${groups.length} draw calls for ${m.textures.length} textures`)

// A moving sector must be rebuildable without touching the rest of the map,
// and its surfaces must actually follow it. This is what the per-sector
// factoring exists for.
import { buildSectorSurfaces } from '../src/mapgeom.js'
const before = buildSectorSurfaces(m, 1).find((s) => s.kind === SURFACE.FLOOR)
m.sectors[1].floorZ += 1
m.sectors[1].ceilZ += 1
const after = buildSectorSurfaces(m, 1).find((s) => s.kind === SURFACE.FLOOR)
check(after.positions[1] === before.positions[1] + 1,
      'rebuilding one sector follows its motion')
// And the neighbour's wall fields appear and disappear as it travels, which is
// the reason that decision is made per rebuild instead of baked. Heights are
// set absolutely rather than nudged: the first version of this nudged upward
// from a floor an earlier assertion had already raised, so the field was
// revealed before the test that claimed to reveal it and the count could not
// move either way.
const lowersFromSector0 = (floorZ) => {
  m.sectors[1].floorZ = floorZ
  return buildSectorSurfaces(m, 0).filter((s) => s.kind === SURFACE.LOWER).length
}
check(lowersFromSector0(0) === 0, 'a flush neighbour shows sector 0 no lower field')
check(lowersFromSector0(3) === 1, 'and raising it reveals one')
check(lowersFromSector0(0) === 0, 'and lowering it hides that field again')

console.log(`\nmapmesh: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
