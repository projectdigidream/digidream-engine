//
// Reading a loaded map out of wasm, for the renderer.
//
// THE RENDER BOUNDARY. Everywhere else in this client, fx values stay exact:
// positions and velocities are raw Q32.32 BigInts, because exact-match
// reconciliation (docs/protocol.md R3) breaks the moment one of them becomes a
// double. Here they become doubles on purpose. Nothing read through this module
// ever goes back into the simulation -- it goes to the GPU, where a wrong last
// bit is a sub-pixel difference nobody can see.
//
// That asymmetry is the reason this is its own file rather than a few methods
// on WasmMap: the conversion should be somewhere a reader can see all of, and
// the rule about which direction it may flow should be written down once.
//

// Q32.32 -> double. Exact for any value a level uses: the fractional part is
// 32 bits and a double carries 52, so nothing is lost until coordinates pass
// 2^20 units, which is 16 times the largest map the format can express.
export const fxToFloat = (raw) => Number(raw) / 4294967296

export const NO_TEXTURE = 0xFFFF
export const NO_SECTOR = -1

export const PEG = {
  UPPER_UNPEGGED: 1 << 0,
  LOWER_UNPEGGED: 1 << 1
}

export const SIDE_FLAG = {
  CLIMBABLE: 1 << 0,
  GRAPPLE: 1 << 1,
  BLOCKING: 1 << 2,
  LEDGE: 1 << 3
}

/**
 * Read a WasmMap into plain JS.
 *
 * Returns a structure with no BigInt and no wasm pointers in it, so everything
 * downstream -- mesh building, tests, the editor later -- is pure data and can
 * run without a wasm module at all. That is what lets the geometry tests be
 * ordinary node tests instead of an integration harness.
 */
export function readMap (map) {
  const core = map.core
  const M = core.M
  const p = map.ptr

  const pointCount = M._dnp_map_point_count(p)
  const sectorCount = M._dnp_map_sector_count(p)
  const sideCount = M._dnp_map_side_count(p)
  const triCount = M._dnp_map_tri_index_count(p)
  const textureCount = M._dnp_map_texture_count(p)

  const b64 = core.buf64 / 8
  const b32 = core.buf32 / 4

  const points = new Array(pointCount)
  for (let i = 0; i < pointCount; i++) {
    M._dnp_map_point(p, i, core.buf64)
    const { i64 } = core.views
    points[i] = { x: fxToFloat(i64[b64]), y: fxToFloat(i64[b64 + 1]) }
  }

  const sectors = new Array(sectorCount)
  for (let i = 0; i < sectorCount; i++) {
    M._dnp_map_sector(p, i, core.buf64, core.buf32)
    const { i64, u32 } = core.views
    sectors[i] = {
      floorZ: fxToFloat(i64[b64]),
      ceilZ: fxToFloat(i64[b64 + 1]),
      firstPoint: u32[b32], pointCount: u32[b32 + 1],
      firstSide: u32[b32 + 2], sideCount: u32[b32 + 3],
      tag: u32[b32 + 4],
      firstTri: u32[b32 + 5], triCount: u32[b32 + 6],
      floorTex: u32[b32 + 7], ceilTex: u32[b32 + 8],
      light: u32[b32 + 9],
      thickness: fxToFloat(i64[b64 + 2])
    }
  }

  const sides = new Array(sideCount)
  for (let i = 0; i < sideCount; i++) {
    M._dnp_map_side(p, i, core.buf64, core.buf32)
    const { i64, u32 } = core.views
    sides[i] = {
      a: u32[b32], b: u32[b32 + 1],
      // Written as unsigned by the binding to preserve the bits; NO_SECTOR is
      // -1 and would otherwise arrive as 4294967295 and index nothing.
      neighbor: u32[b32 + 2] | 0,
      flags: u32[b32 + 3],
      upper: u32[b32 + 4], middle: u32[b32 + 5], lower: u32[b32 + 6],
      peg: u32[b32 + 7],
      xOffset: fxToFloat(i64[b64]),
      yOffset: fxToFloat(i64[b64 + 1])
    }
  }

  const triPtr = M._malloc(Math.max(2, triCount * 2))
  const got = M._dnp_map_tris(p, triPtr, triCount)
  const tris = new Uint16Array(M.HEAPU8.buffer, triPtr, got).slice()
  M._free(triPtr)

  const lightCount = M._dnp_map_light_count(p)
  const lights = new Array(lightCount)
  for (let i = 0; i < lightCount; i++) {
    M._dnp_map_light(p, i, core.buf64, core.buf32)
    const { i64, u32 } = core.views
    lights[i] = {
      x: fxToFloat(i64[b64]), y: fxToFloat(i64[b64 + 1]), z: fxToFloat(i64[b64 + 2]),
      radius: fxToFloat(i64[b64 + 3]),
      intensity: fxToFloat(i64[b64 + 4]),
      // 0..1 for the shader; the map stores bytes because a light's colour is
      // authored, not computed.
      r: u32[b32] / 255, g: u32[b32 + 1] / 255, b: u32[b32 + 2] / 255
    }
  }

  const NAME_CAP = 24
  const namePtr = M._malloc(NAME_CAP)
  const textures = new Array(textureCount)
  for (let i = 0; i < textureCount; i++) {
    const n = M._dnp_map_texture_name(p, i, namePtr, NAME_CAP)
    textures[i] = new TextDecoder().decode(M.HEAPU8.subarray(namePtr, namePtr + n))
  }
  M._free(namePtr)

  return { points, sectors, sides, tris, textures, lights }
}

// --- camera paths (design §4.1) ---------------------------------------------
//
// Read apart from readMap, because paths have a lifetime readMap's results do
// not: a path bolted to a moving sector has NEW POINT POSITIONS EVERY TICK, and
// the structure around them never changes. So the structure is read once and
// the positions are refreshed in place.

/**
 * Every camera path's structure, and its points as of now.
 *
 * Engine space, not THREE space. buildSpline works in whatever space it is
 * given and toThreeX/Y/Z is a signed permutation -- linear, an isometry -- so
 * the curve and its arc length commute with it, and converting once at the end
 * is identical to converting first. client/test/spline.test.js asserts exactly
 * that, with a nonlinear map as the control.
 */
export function readPaths (map) {
  const out = []
  const n = map.pathCount
  for (let i = 0; i < n; i++) {
    const info = map.pathInfo(i)
    if (!info) continue
    out.push({
      index: i,
      tag: info.tag,
      flags: info.flags,
      loop: info.loop,
      count: info.count,
      curveLength: info.curveLength,
      pos: map.pathPoints(i),
      anchors: map.pathAnchors(i),
      // The other half of the double buffer in refreshPathPositions. Allocated
      // on its first call, then reused for the life of the map.
      _spare: undefined
    })
  }
  return out
}

/**
 * Re-read one path's live point positions, in place. Returns whether any moved.
 *
 * The return value is the point of the function. A static path is the common
 * case by a wide margin, and rebuilding its arc-length table sixty times a
 * second for a set of numbers that did not change is exactly the waste
 * client/src/mapmovers.js exists to record having made once already.
 *
 * Reading is cheap; what this guards is the SPLINE REBUILD downstream.
 */
export function refreshPathPositions (map, path) {
  // DOUBLE-BUFFERED, because pathPoints rewrites its target in place: reading
  // into the array we already hold would destroy the only copy of the previous
  // positions, and there would be nothing left to compare against. So the two
  // arrays swap roles each call and neither is allocated after the first.
  const prev = path.pos
  const next = map.pathPoints(path.index, path._spare)
  path._spare = prev
  path.pos = next
  if (prev === undefined || prev.length !== next.length) return true
  for (let i = 0; i < next.length; i++) if (next[i] !== prev[i]) return true
  return false
}

/** The first path with this tag, or -1. */
export function pathIndexForTag (paths, tag) {
  // FIRST match. Tags are not unique and nothing enforces it -- mapc will
  // happily bake two paths tagged 5 -- so the rule is written down here, tested
  // against a map that actually contains the ambiguity, and kept the same as
  // every other tag lookup in the engine.
  for (let i = 0; i < paths.length; i++) if (paths[i].tag === tag) return i
  return -1
}
