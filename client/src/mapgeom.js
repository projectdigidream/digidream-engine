//
// Turning a map into drawable surfaces (design §3.1, §4).
//
// Pure data in, pure data out: no Three.js, no wasm, no GPU. That is
// deliberate. The interesting parts of a Doom-style renderer -- which wall
// fields exist, how their UVs are anchored, which way a face points -- are
// arithmetic, and arithmetic is worth testing directly rather than by looking
// at a screenshot. The Three.js layer above this only uploads what it is given.
//
// COORDINATES. The engine is z-up (x, y horizontal, z vertical); Three.js is
// y-up. The conversion is (x, y, z) -> (x, z, -y), which is a rotation of -90
// degrees about x. A proper rotation, not a reflection: the tempting (x, z, y)
// has determinant -1 and would silently invert every winding in the map, so
// every surface would face the wrong way and the fix would look like a normals
// bug rather than a coordinate bug.
//
// TEXTURE COORDINATES ARE IN WORLD UNITS, not in texture repeats. `u` is
// distance along the wall, `v` is distance measured DOWNWARD from the field's
// anchor line. The material divides by the texture's world size. That is what
// keeps the mesh independent of the asset -- the same map serves clients whose
// textures are different sizes -- and it is also what makes pegging
// expressible without knowing the texture's height here (see below).
//

import { NO_TEXTURE, NO_SECTOR, PEG } from './mapdata.js'

// Engine (z-up) -> Three (y-up).
export const toThreeX = (x) => x
export const toThreeY = (z) => z
export const toThreeZ = (y) => -y

export const SURFACE = {
  FLOOR: 'floor',
  CEIL: 'ceil',
  UPPER: 'upper',
  MIDDLE: 'middle',
  LOWER: 'lower'
}

// --- pegging ---------------------------------------------------------------
//
// Doom's rules, and they matter here for a reason specific to this engine
// rather than for nostalgia: sectors MOVE (§3.1.2). Pegging decides whether a
// wall field's texture origin is nailed to the moving surface or to the room,
// which is the difference between a lift shaft whose wall slides with the
// platform and one that stays put while the platform travels past it.
//
// Each field resolves to ONE anchor height, and `v = anchor - z`. A field
// anchored at its top gets v in [0, height]; one anchored at its bottom gets v
// in [-height, 0], which with repeat wrapping puts the texture's bottom edge
// exactly on the section's bottom edge. Expressing it as a signed distance is
// what lets this file stay ignorant of how tall the texture is.
//
//   upper   default   bottom-anchored at the lower of the two ceilings
//           UNPEGGED  top-anchored at this sector's ceiling
//   lower   default   top-anchored at the higher of the two floors
//           UNPEGGED  anchored at this sector's CEILING -- the rule that keeps
//                     a lift shaft's wall still while the floor rides past it
//   middle  default   top-anchored at this sector's ceiling
//   (solid) UNPEGGED  bottom-anchored at this sector's floor

function upperAnchor (sector, back, peg) {
  return (peg & PEG.UPPER_UNPEGGED) ? sector.ceilZ : back.ceilZ
}

function lowerAnchor (sector, back, peg) {
  return (peg & PEG.LOWER_UNPEGGED) ? sector.ceilZ : back.floorZ
}

function solidAnchor (sector, peg) {
  return (peg & PEG.LOWER_UNPEGGED) ? sector.floorZ : sector.ceilZ
}

// --- surfaces --------------------------------------------------------------

function wallQuad ({ kind, sector, sectorIndex, side, sideIndex, texture,
                     zBot, zTop, anchorZ, a, b }) {
  const dx = b.x - a.x
  const dy = b.y - a.y
  const len = Math.hypot(dx, dy)

  // Wound to face INTO the sector that owns the side. A sector's polygon is
  // counter-clockwise, so the outward normal of edge a->b is (dy, -dx); the
  // face a viewer standing in the room can see is the other one.
  const positions = [
    toThreeX(a.x), toThreeY(zBot), toThreeZ(a.y),
    toThreeX(a.x), toThreeY(zTop), toThreeZ(a.y),
    toThreeX(b.x), toThreeY(zTop), toThreeZ(b.y),
    toThreeX(b.x), toThreeY(zBot), toThreeZ(b.y)
  ]

  const nx = len === 0 ? 0 : -dy / len
  const ny = len === 0 ? 0 : dx / len
  const normals = []
  for (let i = 0; i < 4; i++) normals.push(toThreeX(nx), toThreeY(0), toThreeZ(ny))

  // u is measured from B, not from A.
  //
  // A sector's polygon is counter-clockwise, so its interior lies to the LEFT
  // of a->b -- which means a viewer standing inside always sees a->b running
  // right to left across their view. Numbering u from a therefore makes it
  // increase LEFTWARD and every wall in the map comes out mirrored. It follows
  // from the winding, so it is every wall, always; on a tiling texture it reads
  // as "slightly off" rather than as backwards, which is why it survived a
  // first look at the render.
  const uA = side.xOffset + len
  const uB = side.xOffset
  const vAt = (z) => anchorZ - z + side.yOffset
  const uvs = [
    uA, vAt(zBot),
    uA, vAt(zTop),
    uB, vAt(zTop),
    uB, vAt(zBot)
  ]

  return {
    kind,
    sector: sectorIndex,
    side: sideIndex,
    texture,
    light: sector.light,
    positions,
    normals,
    uvs,
    indices: [0, 1, 2, 0, 2, 3]
  }
}

/**
 * Every drawable surface belonging to one sector.
 *
 * Per-sector rather than whole-map, and that is the important part of the
 * shape: sectors move, and a mover's floor, ceiling and every wall field around
 * it change height on the tick. Rebuilding one sector has to be cheap, or a
 * lift costs a full map rebuild every frame.
 */
export function buildSectorSurfaces (map, sectorIndex) {
  const s = map.sectors[sectorIndex]
  const out = []

  // --- flats, from the BAKED triangles -------------------------------------
  //
  // Not re-triangulated here. The mesh the renderer draws is the mesh the
  // collision code was validated against, so a sector cannot look like one
  // shape and collide like another -- which is the whole reason the triangles
  // are in the file at all (docs/protocol.md §8.1).
  const triStart = s.firstTri
  const triCount = s.triCount * 3

  for (const [kind, z, tex, up] of [
    [SURFACE.FLOOR, s.floorZ, s.floorTex, true],
    [SURFACE.CEIL, s.ceilZ, s.ceilTex, false]
  ]) {
    if (tex === NO_TEXTURE) continue
    const positions = []
    const normals = []
    const uvs = []
    const indices = []
    // Indices are absolute into the point array, so they are re-based here
    // onto a compact per-surface vertex list rather than uploading the whole
    // map's points for every sector.
    const remap = new Map()
    for (let i = 0; i < triCount; i++) {
      const pi = map.tris[triStart + i]
      let local = remap.get(pi)
      if (local === undefined) {
        local = remap.size
        remap.set(pi, local)
        const p = map.points[pi]
        positions.push(toThreeX(p.x), toThreeY(z), toThreeZ(p.y))
        normals.push(0, up ? 1 : -1, 0)
        // Flats are projected straight down the vertical axis, so a floor's
        // pattern lines up across sector boundaries the way a tiled floor
        // should, instead of restarting at each sector's own origin.
        //
        // v is negated because textures are sampled with v running DOWN the
        // image (see textures.js) while the world's y runs the other way.
        //
        // u is negated for CEILINGS only. A ceiling is the same triangles seen
        // from below, so its winding is reversed just after this -- and
        // reversing the geometry without reversing the uvs would leave its
        // texture mirrored relative to the floor's.
        uvs.push(up ? p.x : -p.x, -p.y)
      }
      indices.push(local)
    }
    // A ceiling is the same triangles seen from below, so its winding is
    // reversed. Flipping the normal alone would light it correctly and still
    // let backface culling remove it.
    if (!up) {
      for (let i = 0; i < indices.length; i += 3) {
        const t = indices[i + 1]; indices[i + 1] = indices[i + 2]; indices[i + 2] = t
      }
    }
    out.push({
      kind, sector: sectorIndex, side: -1, texture: tex, light: s.light,
      positions, normals, uvs, indices
    })
  }

  // --- wall fields ---------------------------------------------------------
  for (let k = 0; k < s.sideCount; k++) {
    const sideIndex = s.firstSide + k
    const side = map.sides[sideIndex]
    const a = map.points[side.a]
    const b = map.points[side.b]

    if (side.neighbor === NO_SECTOR) {
      // A solid wall has one field, floor to ceiling.
      if (side.middle === NO_TEXTURE) continue
      out.push(wallQuad({
        kind: SURFACE.MIDDLE, sector: s, sectorIndex, side, sideIndex,
        texture: side.middle, zBot: s.floorZ, zTop: s.ceilZ,
        anchorZ: solidAnchor(s, side.peg), a, b
      }))
      continue
    }

    const back = map.sectors[side.neighbor]

    // Which fields exist follows from the two sectors' heights and is computed
    // HERE, every rebuild -- never baked. A sector that moves reveals and hides
    // its neighbours' upper and lower fields as it travels, and a map that had
    // recorded which fields to draw would be wrong the moment anything moved.
    if (back.floorZ > s.floorZ && side.lower !== NO_TEXTURE) {
      out.push(wallQuad({
        kind: SURFACE.LOWER, sector: s, sectorIndex, side, sideIndex,
        texture: side.lower, zBot: s.floorZ, zTop: back.floorZ,
        anchorZ: lowerAnchor(s, back, side.peg), a, b
      }))
    }
    if (back.ceilZ < s.ceilZ && side.upper !== NO_TEXTURE) {
      out.push(wallQuad({
        kind: SURFACE.UPPER, sector: s, sectorIndex, side, sideIndex,
        texture: side.upper, zBot: back.ceilZ, zTop: s.ceilZ,
        anchorZ: upperAnchor(s, back, side.peg), a, b
      }))
    }
    if (side.middle !== NO_TEXTURE) {
      // A grate or window across the opening: the part of the boundary both
      // sectors share.
      const zBot = Math.max(s.floorZ, back.floorZ)
      const zTop = Math.min(s.ceilZ, back.ceilZ)
      if (zTop > zBot) {
        out.push(wallQuad({
          kind: SURFACE.MIDDLE, sector: s, sectorIndex, side, sideIndex,
          texture: side.middle, zBot, zTop,
          anchorZ: (side.peg & PEG.LOWER_UNPEGGED) ? zBot : zTop, a, b
        }))
      }
    }
  }

  return out
}

/** Every surface in the map. */
export function buildMapSurfaces (map) {
  const out = []
  for (let i = 0; i < map.sectors.length; i++) out.push(...buildSectorSurfaces(map, i))
  return out
}

/**
 * Merge surfaces into one buffer per texture.
 *
 * One draw call per texture rather than per surface. Kept separate from the
 * building above so that a moving sector can be rebuilt and re-merged without
 * the merge logic being tangled into the geometry rules.
 */
export function mergeByTexture (surfaces) {
  const byTexture = new Map()
  for (const s of surfaces) {
    let g = byTexture.get(s.texture)
    if (!g) {
      g = { texture: s.texture, positions: [], normals: [], uvs: [], colors: [], indices: [] }
      byTexture.set(s.texture, g)
    }
    const base = g.positions.length / 3
    for (const v of s.positions) g.positions.push(v)
    for (const v of s.normals) g.normals.push(v)
    for (const v of s.uvs) g.uvs.push(v)
    // Light level as a vertex colour (design §4). Flat per sector, which is
    // what a sector-lit engine means -- gradients across a sector would be a
    // different lighting model, not a nicer version of this one.
    const l = s.light / 255
    for (let i = 0; i < s.positions.length / 3; i++) g.colors.push(l, l, l)
    for (const i of s.indices) g.indices.push(base + i)
  }
  return [...byTexture.values()]
}
