//
// ONE CELL, FROM ITS BYTES TO THE GROUPS A MapView UPLOADS.
//
// Shared by the worker (client/src/seamworker.js) and by the main thread when
// no worker could be started, so there is one implementation of what a
// neighbour is built into and not two believed to match.
//
// Pure data out: typed arrays, names and numbers. No Three.js, no DOM.
//
import { readMap } from '../mapdata.js'
import { buildMapSurfaces, mergeByTexture } from '../mapgeom.js'

/**
 * @returns {{ built: object, transfer: ArrayBuffer[] } | { error: string }}
 */
export function buildCell (core, bytes) {
  const r = core.loadMap(bytes)
  if (!r.ok) return { error: `map would not load (status ${r.status})` }
  const map = readMap(r.map)
  // The map's own texture scale travels with it: a level converted at 1/48
  // wants 48, and the observer's own map may have been drawn at something else.
  const textureScale = r.map.textureScale
  const hash = r.map.hash
  r.map.destroy()

  // The renderer's own pipeline, unchanged. Static only -- a neighbour has no
  // moving sectors worth the machinery, and a door opening one cell away that
  // does not visibly open is a better failure than a frame budget spent on it.
  const groups = mergeByTexture(buildMapSurfaces(map)).map((g) => ({
    // The NAME, not the index. The main thread resolves it against its own
    // texture set, and the two maps have no reason to agree on an order.
    texture: g.texture >= 0 ? map.textures[g.texture] : null,
    alpha: g.alpha,
    positions: new Float32Array(g.positions),
    normals: new Float32Array(g.normals),
    uvs: new Float32Array(g.uvs),
    colors: new Float32Array(g.colors),
    indices: new Uint32Array(g.indices)
  }))
  const transfer = []
  for (const g of groups) {
    transfer.push(g.positions.buffer, g.normals.buffer, g.uvs.buffer,
                  g.colors.buffer, g.indices.buffer)
  }
  return { built: { textures: map.textures, lights: map.lights, textureScale, hash, groups }, transfer }
}
