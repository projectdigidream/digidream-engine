//
// THE CELLS NEXT DOOR, BUILT OFF THE MAIN THREAD.
//
// A neighbour is decoration: geometry drawn at an offset so a corridor ending
// at a cell boundary has the other half of itself on screen. Nothing simulates
// it, nothing collides with it, and nobody is waiting for it -- which is
// exactly what makes it the right work to do slowly and elsewhere.
//
// Done on the main thread it is not slow, it is VIOLENT: joining the nine-map
// world built nine maps in one frame, and every one of them parsed a .dnpm,
// triangulated it, walked every sector for its surfaces and merged them by
// texture before a single triangle reached the GPU. The frame that did it was
// the frame you were walking through a doorway in.
//
// WHAT CAN LEAVE THE MAIN THREAD, and why all of it can:
//
//   dnp_map_load   wasm, no DOM           -- its own MapStorage per call
//   readMap        plain JS out of wasm   -- deliberately no BigInt, no pointers
//   buildMapSurfaces / mergeByTexture     -- "pure data in, pure data out,
//                                            no Three.js, no wasm, no GPU"
//
// That comment at the top of mapgeom.js was written about testability. It is
// the same property that makes this possible, and it is why there is no second
// implementation of anything here: the worker runs the renderer's own geometry
// code and posts back the same groups #build would have made.
//
// WHAT CANNOT: making a BufferGeometry, compiling a material, uploading to the
// GPU. Those need the WebGL context and stay where it is. So the boundary is
// drawn at typed arrays, which transfer at no cost rather than being copied.
//
import { Core } from './core.js'
import { buildCell } from './seam/buildcell.js'

// ONE CORE FOR THE LIFETIME OF THE WORKER, built on the first job rather than
// at import. A session that never sees a metamap never pays for a second wasm
// instance, and one that does pays once.
let core = null

// `token` is echoed, not read: it is how the main thread tells this build from
// a later one of the same level (client/src/seam/neighbourhood.js).
self.onmessage = async (e) => {
  const { level, token, bytes } = e.data
  try {
    core ??= await Core.load()
    const r = buildCell(core, new Uint8Array(bytes))
    if (r.error) { self.postMessage({ level, token, error: r.error }); return }
    // Transferred, not copied: the buffers stop existing here the moment they
    // are posted, which is what keeps a 180 KB level from being a 180 KB
    // structured clone on top of the work that produced it.
    self.postMessage({ level, token, ...r.built }, r.transfer)
  } catch (err) {
    self.postMessage({ level, token, error: String(err?.message ?? err) })
  }
}
