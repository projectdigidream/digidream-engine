//
// Mesh-building tests.
//
// Everything here is arithmetic on plain data, so it is asserted directly
// rather than looked at. The three things worth being sure of, because each
// fails in a way that looks like a different bug:
//
//   1. Faces point INTO the room. Get the winding wrong and every wall
//      vanishes under backface culling -- which reads as a missing-geometry
//      bug, not a coordinate one.
//   2. Which wall fields exist follows from the two sectors' CURRENT heights.
//      Bake that decision and a moving sector leaves holes behind it.
//   3. Pegging anchors a field's texture to the right surface. Get it wrong
//      and it only shows once something moves, which is the worst time to
//      find out.
//
import { buildSectorSurfaces, mergeByTexture, SURFACE } from '../src/mapgeom.js'
import { NO_TEXTURE, NO_SECTOR, PEG } from '../src/mapdata.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  if (!ok) { fails++; console.log(`  FAIL ${label}`) }
}
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps

// Two sectors side by side sharing the edge x=10, so the shared boundary can be
// given different heights on each side and every wall field exercised.
//
//   sector 0: x 0..10, y 0..10
//   sector 1: x 10..20, y 0..10
function fixture ({ f0 = 0, c0 = 8, f1 = 0, c1 = 8, peg = 0, mid0 = NO_TEXTURE } = {}) {
  return {
    points: [
      { x: 0, y: 0 }, { x: 10, y: 0 }, { x: 10, y: 10 }, { x: 0, y: 10 },
      { x: 10, y: 0 }, { x: 20, y: 0 }, { x: 20, y: 10 }, { x: 10, y: 10 }
    ],
    sectors: [
      { floorZ: f0, ceilZ: c0, firstPoint: 0, pointCount: 4, firstSide: 0, sideCount: 4,
        tag: 0, firstTri: 0, triCount: 2, floorTex: 1, ceilTex: 2, light: 255 },
      { floorZ: f1, ceilZ: c1, firstPoint: 4, pointCount: 4, firstSide: 4, sideCount: 4,
        tag: 0, firstTri: 6, triCount: 2, floorTex: 1, ceilTex: 2, light: 128 }
    ],
    sides: [
      { a: 0, b: 1, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      // the shared edge, from sector 0's side
      { a: 1, b: 2, neighbor: 1, flags: 0, upper: 4, middle: mid0, lower: 5, peg, xOffset: 0, yOffset: 0 },
      { a: 2, b: 3, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 3, b: 0, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 4, b: 5, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 5, b: 6, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 6, b: 7, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 7, b: 4, neighbor: 0, flags: 0, upper: 4, middle: NO_TEXTURE, lower: 5, peg: 0, xOffset: 0, yOffset: 0 }
    ],
    tris: Uint16Array.from([3, 0, 1, 1, 2, 3, 7, 4, 5, 5, 6, 7]),
    textures: ['', 'FLOOR', 'CEIL', 'WALL', 'UPPER', 'LOWER']
  }
}

const find = (surfaces, kind, sideIndex) =>
  surfaces.find((s) => s.kind === kind && (sideIndex === undefined || s.side === sideIndex))

// Geometric normal of a surface's first triangle, from its positions alone --
// NOT from the normals it declares. Comparing the declared normal against
// itself would pass with the winding inverted, which is the exact bug this is
// here to catch.
function faceNormal (s) {
  const p = (i) => [s.positions[i * 3], s.positions[i * 3 + 1], s.positions[i * 3 + 2]]
  const [i0, i1, i2] = [s.indices[0], s.indices[1], s.indices[2]]
  const a = p(i0); const b = p(i1); const c = p(i2)
  const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]]
  const v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]]
  const n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
  const len = Math.hypot(...n) || 1
  return n.map((x) => x / len)
}

console.log('\n--- flats ---')
{
  const m = fixture()
  const surf = buildSectorSurfaces(m, 0)
  const floor = find(surf, SURFACE.FLOOR)
  const ceil = find(surf, SURFACE.CEIL)
  check(!!floor && !!ceil, 'a sector produces a floor and a ceiling')

  const fn = faceNormal(floor)
  check(near(fn[1], 1), `the floor faces UP (normal y=${fn[1].toFixed(3)})`)
  const cn = faceNormal(ceil)
  check(near(cn[1], -1), `the ceiling faces DOWN (normal y=${cn[1].toFixed(3)})`)

  check(floor.indices.length === 6, 'the floor uses the baked triangles (2 tris)')
  // Every vertex sits at the sector's floor height.
  let allAtFloor = true
  for (let i = 1; i < floor.positions.length; i += 3) if (floor.positions[i] !== 0) allAtFloor = false
  check(allAtFloor, 'and every floor vertex is at the floor height')
}

console.log('\n--- solid walls ---')
{
  const m = fixture()
  const surf = buildSectorSurfaces(m, 0)
  const wall = find(surf, SURFACE.MIDDLE, 0)   // the y=0 edge, a solid wall
  check(!!wall, 'a one-sided side produces one middle field')

  // Sector 0 spans y 0..10; the y=0 wall is on its south edge, so a viewer
  // inside the room is at greater y -- which is NEGATIVE z in three-space.
  const n = faceNormal(wall)
  check(near(n[2], -1), `the wall faces into the room (normal z=${n[2].toFixed(3)})`)

  check(wall.positions.length === 12, 'four vertices')
  const ys = [1, 4, 7, 10].map((i) => wall.positions[i])
  check(Math.min(...ys) === 0 && Math.max(...ys) === 8, 'spanning floor to ceiling')
}

console.log('\n--- which fields exist follows from the heights ---')
{
  // Flush sectors: no step up, no step down, so neither field is drawn.
  const flush = buildSectorSurfaces(fixture({ f0: 0, c0: 8, f1: 0, c1: 8 }), 0)
  check(!find(flush, SURFACE.LOWER), 'flush floors produce no lower field')
  check(!find(flush, SURFACE.UPPER), 'flush ceilings produce no upper field')

  // Neighbour's floor is higher: a step up, so sector 0 sees a lower field.
  const step = buildSectorSurfaces(fixture({ f0: 0, f1: 3 }), 0)
  const lower = find(step, SURFACE.LOWER)
  check(!!lower, 'a higher neighbouring floor produces a lower field')
  if (lower) {
    const ys = [1, 4, 7, 10].map((i) => lower.positions[i])
    check(Math.min(...ys) === 0 && Math.max(...ys) === 3,
          'spanning this floor to the neighbour\'s')
  }

  // Neighbour's ceiling is lower: an upper field.
  const head = buildSectorSurfaces(fixture({ c0: 8, c1: 5 }), 0)
  const upper = find(head, SURFACE.UPPER)
  check(!!upper, 'a lower neighbouring ceiling produces an upper field')
  if (upper) {
    const ys = [1, 4, 7, 10].map((i) => upper.positions[i])
    check(Math.min(...ys) === 5 && Math.max(...ys) === 8,
          'spanning the neighbour\'s ceiling to this one')
  }

  // The point of computing it per rebuild rather than baking it: as a sector
  // moves, fields appear and disappear.
  let seen = new Set()
  for (let f1 = 0; f1 <= 4; f1++) {
    const s = buildSectorSurfaces(fixture({ f0: 0, f1 }), 0)
    seen.add(find(s, SURFACE.LOWER) ? 'lower' : 'none')
  }
  check(seen.has('lower') && seen.has('none'),
        'a moving neighbour reveals and hides the lower field as it travels')
}

console.log('\n--- pegging ---')
{
  // v is world distance measured DOWN from the field's anchor, so the anchor
  // is exactly where v == 0.
  const vOf = (s) => [s.uvs[1], s.uvs[3], s.uvs[5], s.uvs[7]]

  // Lower, default: top-anchored at the higher floor. The section spans 0..3,
  // so v runs 3 (at the bottom) to 0 (at the top).
  const pegged = find(buildSectorSurfaces(fixture({ f0: 0, f1: 3 }), 0), SURFACE.LOWER)
  check(Math.min(...vOf(pegged)) === 0 && Math.max(...vOf(pegged)) === 3,
        'lower, pegged: anchored to the top of the section')

  // Lower, UNPEGGED: anchored to the sector's CEILING (8), so the same section
  // gets v from 8 down to 5. This is the rule that keeps a lift shaft's wall
  // still while the platform rides past it.
  const unpeg = find(
    buildSectorSurfaces(fixture({ f0: 0, f1: 3, peg: PEG.LOWER_UNPEGGED }), 0), SURFACE.LOWER)
  check(Math.min(...vOf(unpeg)) === 5 && Math.max(...vOf(unpeg)) === 8,
        'lower, unpegged: anchored to the ceiling instead')

  // And the consequence, which is the whole reason pegging exists: as the
  // neighbouring floor rises, an unpegged texture keeps the same v at a fixed
  // world height, while a pegged one drags its origin along with the floor.
  const vAtWorldZ = (surf, z) => {
    // The quad's vertices are (bottom, top, top, bottom) at a and b.
    const zBot = surf.positions[1]
    const zTop = surf.positions[4]
    const vBot = surf.uvs[1]
    const vTop = surf.uvs[3]
    const t = (z - zBot) / (zTop - zBot)
    return vBot + (vTop - vBot) * t
  }
  const peggedA = find(buildSectorSurfaces(fixture({ f0: 0, f1: 3 }), 0), SURFACE.LOWER)
  const peggedB = find(buildSectorSurfaces(fixture({ f0: 0, f1: 4 }), 0), SURFACE.LOWER)
  check(vAtWorldZ(peggedA, 1) !== vAtWorldZ(peggedB, 1),
        'pegged: the texture moves with the floor')

  const unpegA = find(buildSectorSurfaces(fixture({ f0: 0, f1: 3, peg: PEG.LOWER_UNPEGGED }), 0), SURFACE.LOWER)
  const unpegB = find(buildSectorSurfaces(fixture({ f0: 0, f1: 4, peg: PEG.LOWER_UNPEGGED }), 0), SURFACE.LOWER)
  check(near(vAtWorldZ(unpegA, 1), vAtWorldZ(unpegB, 1)),
        'UNPEGGED: the texture stays put as the floor rides past it')

  // Upper, default: bottom-anchored at the lower ceiling; unpegged: top.
  const upPeg = find(buildSectorSurfaces(fixture({ c0: 8, c1: 5 }), 0), SURFACE.UPPER)
  check(Math.max(...vOf(upPeg)) === 0 && Math.min(...vOf(upPeg)) === -3,
        'upper, pegged: bottom-anchored (v runs negative upward)')
  const upUn = find(
    buildSectorSurfaces(fixture({ c0: 8, c1: 5, peg: PEG.UPPER_UNPEGGED }), 0), SURFACE.UPPER)
  check(Math.min(...upUn.uvs.filter((_, i) => i % 2 === 1)) === 0,
        'upper, unpegged: top-anchored at this ceiling')

  // A solid wall's middle: top-anchored by default, bottom-anchored unpegged.
  const solid = find(buildSectorSurfaces(fixture(), 0), SURFACE.MIDDLE, 0)
  check(Math.min(...vOf(solid)) === 0 && Math.max(...vOf(solid)) === 8,
        'solid middle, pegged: top-anchored at the ceiling')
}

console.log('\n--- u runs along the wall ---')
{
  const wall = find(buildSectorSurfaces(fixture(), 0), SURFACE.MIDDLE, 0)
  const us = [0, 2, 4, 6].map((i) => wall.uvs[i])
  check(Math.min(...us) === 0 && Math.max(...us) === 10,
        'u spans the wall\'s world length')

  const m = fixture()
  m.sides[0].xOffset = 2.5
  const shifted = find(buildSectorSurfaces(m, 0), SURFACE.MIDDLE, 0)
  const us2 = [0, 2, 4, 6].map((i) => shifted.uvs[i])
  check(Math.min(...us2) === 2.5 && Math.max(...us2) === 12.5, 'xOffset shifts it')
}

console.log('\n--- two-sided middle (a grate) ---')
{
  const none = buildSectorSurfaces(fixture({ f0: 0, f1: 3 }), 0)
  check(!find(none, SURFACE.MIDDLE, 1), 'no middle texture, no middle field')

  const grate = buildSectorSurfaces(fixture({ f0: 0, c0: 8, f1: 3, c1: 6, mid0: 3 }), 0)
  const g = find(grate, SURFACE.MIDDLE, 1)
  check(!!g, 'a two-sided middle is drawn across the opening')
  if (g) {
    const ys = [1, 4, 7, 10].map((i) => g.positions[i])
    check(Math.min(...ys) === 3 && Math.max(...ys) === 6,
          'spanning only the part both sectors share')
  }
}

console.log('\n--- uv orientation (no surface may be mirrored) ---')
{
  // Whether a texture is MIRRORED is not something eyeballing a screenshot
  // answers reliably -- a rotation and a reflection look similar on a tiling
  // pattern, and only one of them is a bug. It is decidable arithmetic, so it
  // is decided here.
  //
  // The convention, stated once: uv space is V-DOWN. u runs along the surface,
  // v increases downward from the anchor, and textures.js implements that with
  // flipY = false so uv (0,0) samples the image's top-left. A surface is
  // mirrored exactly when its uv triangle winds the opposite way from its
  // geometry, measured in a frame built on the face itself.
  const mirrored = (s) => {
    const P = (i) => [s.positions[i * 3], s.positions[i * 3 + 1], s.positions[i * 3 + 2]]
    const UV = (i) => [s.uvs[i * 2], -s.uvs[i * 2 + 1]]   // v-down -> right-handed
    const [i0, i1, i2] = [s.indices[0], s.indices[1], s.indices[2]]
    const p0 = P(i0); const p1 = P(i1); const p2 = P(i2)
    const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
    const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
    const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    const norm = (a) => { const l = Math.hypot(...a) || 1; return a.map((x) => x / l) }

    const u = sub(p1, p0)
    const w = sub(p2, p0)
    const n = norm(cross(u, w))            // the visible face's normal
    const e1 = norm(u)
    // A screen frame, meaning right x up points TOWARD the viewer -- which is
    // along n, because faces are wound to face the room they belong to. e1 and
    // (n x e1) satisfy that: e1 x (n x e1) = n.
    //
    // This was briefly "corrected" to cross(e1, n) on the strength of a
    // hand-derivation that had screen-right backwards, at which point the test
    // agreed with the code and both were wrong. The check that settles it uses
    // no cross products at all: a viewer inside a room, looking south at a wall
    // running east-west, has EAST on their left -- so screen-right is -x, the
    // low-x end of the wall appears rightmost, and u running 0 -> len from a to
    // b therefore increases LEFTWARD. Mirrored, exactly as reported.
    const e2 = cross(n, e1)

    // Signed area of the triangle in the face's own 2D frame.
    const to2 = (p) => { const d = sub(p, p0); return [dot(d, e1), dot(d, e2)] }
    const [a0, a1, a2] = [to2(p0), to2(p1), to2(p2)]
    const areaGeo = (a1[0] - a0[0]) * (a2[1] - a0[1]) - (a1[1] - a0[1]) * (a2[0] - a0[0])

    const [t0, t1, t2] = [UV(i0), UV(i1), UV(i2)]
    const areaUv = (t1[0] - t0[0]) * (t2[1] - t0[1]) - (t1[1] - t0[1]) * (t2[0] - t0[0])

    if (Math.abs(areaGeo) < 1e-12 || Math.abs(areaUv) < 1e-12) return false   // degenerate
    return Math.sign(areaGeo) !== Math.sign(areaUv)
  }

  const cases = [
    ['flush', fixture()],
    ['a step up', fixture({ f0: 0, f1: 3 })],
    ['a lowered ceiling', fixture({ c0: 8, c1: 5 })],
    ['a grate', fixture({ f0: 0, c0: 8, f1: 3, c1: 6, mid0: 3 })]
  ]
  for (const [name, m] of cases) {
    for (const si of [0, 1]) {
      const bad = buildSectorSurfaces(m, si).filter(mirrored)
      check(bad.length === 0,
            `${name}, sector ${si}: no mirrored surface` +
            (bad.length ? ` (${bad.map((s) => s.kind).join(', ')})` : ''))
    }
  }
}

console.log('\n--- merging ---')
{
  const surf = buildSectorSurfaces(fixture(), 0)
  const groups = mergeByTexture(surf)
  check(groups.length < surf.length, `merged ${surf.length} surfaces into ${groups.length} draws`)
  for (const g of groups) {
    check(g.positions.length / 3 === g.colors.length / 3, 'one colour per vertex')
    check(g.positions.length / 3 === g.uvs.length / 2, 'one uv per vertex')
    check(Math.max(...g.indices) < g.positions.length / 3,
          'every index is inside the merged buffer')
  }
  // Light level reaches the vertices as a colour.
  const s0 = mergeByTexture(buildSectorSurfaces(fixture(), 0))
  const s1 = mergeByTexture(buildSectorSurfaces(fixture(), 1))
  check(near(s0[0].colors[0], 1), 'sector 0 is fully lit')
  check(near(s1[0].colors[0], 128 / 255), 'sector 1 carries its own lower light level')
}

console.log(`\nmapgeom: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
