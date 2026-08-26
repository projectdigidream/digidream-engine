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
      skirt: fxToFloat(i64[b64 + 2])
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

  const NAME_CAP = 24
  const namePtr = M._malloc(NAME_CAP)
  const textures = new Array(textureCount)
  for (let i = 0; i < textureCount; i++) {
    const n = M._dnp_map_texture_name(p, i, namePtr, NAME_CAP)
    textures[i] = new TextDecoder().decode(M.HEAPU8.subarray(namePtr, namePtr + n))
  }
  M._free(namePtr)

  return { points, sectors, sides, tris, textures }
}
