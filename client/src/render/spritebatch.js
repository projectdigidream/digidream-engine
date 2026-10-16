//
// SPRITES FROM ONE SHEET, IN ONE DRAW.
//
// Every prop and body drawn from a ThreeSprite sheet is its own mesh -- that
// is what lets entities.js place it, main.js turn it and props.js animate it
// -- and so its own draw call: a busy room was 150 of them for 20 sheets. They
// all share their sheet's material (props.js, main.js `atlases`), so they can
// be drawn together: once a frame, after everything has placed, turned and
// animated them, each one's quad is copied in world space into one buffer per
// sheet, and that buffer is drawn instead. The meshes themselves stay exactly
// as they were for everybody who moves them; they are only moved to a layer
// the camera does not draw.
//
// ONLY WHAT IS ON SCREEN, AND ONLY WHERE IT PAYS. A sprite outside the view
// is left to the renderer's own culling, as before; a buffer drawn without it
// would draw every sprite of the sheet in the level. And a sheet with fewer
// than two sprites on screen is drawn the old way too: a level whose bodies
// are each from a sheet of their own (Cinderfall has 44 sheets) gains nothing
// from a buffer per sheet, and a batch of one is just the same draw again.
//
// NOT BATCHED: a mesh with a material of its own (a cross-fade clones one,
// props.js #fade) or with children (the fade's ghost). Those draw themselves,
// as before.
//
// DRAWN FIRST of the transparent things. The sheet material is transparent,
// so three.js would sort each sprite against the grates and effects by
// distance; one buffer cannot be sorted that way. But a sprite is a cut-out
// (alphaTest) that writes depth, so drawing every sprite before anything
// see-through gives the right picture: a grate in front of a sprite blends
// over it, one behind it is hidden by its depth.
//

/** The layer batched meshes are moved to: one no camera draws. */
export const BATCHED_LAYER = 31

/** Whether a mesh and every parent above it is visible (what the renderer checks). */
function shownInScene (mesh) {
  for (let o = mesh; o; o = o.parent) {
    if (!o.visible) return false
    if (!o.parent) return o.isScene === true
  }
  return false
}

export class SpriteBatcher {
  constructor ({ THREE }) {
    this.THREE = THREE
    this.batches = new Map()     // material -> { mesh, capacity }
    this.stats = { quads: 0, batches: 0, alone: 0 }
    this._v = new THREE.Vector3()
    this._frustum = new THREE.Frustum()
    this._m = new THREE.Matrix4()
  }

  #batchFor (material, scene) {
    let b = this.batches.get(material)
    if (!b) {
      const THREE = this.THREE
      const geometry = new THREE.BufferGeometry()
      const mesh = new THREE.Mesh(geometry, material)
      mesh.name = 'sprite-batch'
      mesh.frustumCulled = false
      mesh.matrixAutoUpdate = false
      mesh.renderOrder = -1
      b = { mesh, capacity: 0, count: 0 }
      this.batches.set(material, b)
    }
    if (b.mesh.parent !== scene) scene.add(b.mesh)
    return b
  }

  #grow (b, quads) {
    if (quads <= b.capacity) return
    const THREE = this.THREE
    let cap = Math.max(16, b.capacity)
    while (cap < quads) cap *= 2
    const g = b.mesh.geometry
    const pos = new THREE.BufferAttribute(new Float32Array(cap * 4 * 3), 3)
    const uv = new THREE.BufferAttribute(new Float32Array(cap * 4 * 2), 2)
    pos.setUsage(THREE.DynamicDrawUsage)
    uv.setUsage(THREE.DynamicDrawUsage)
    const index = new Uint32Array(cap * 6)
    g.setAttribute('position', pos)
    g.setAttribute('uv', uv)
    g.setIndex(new THREE.BufferAttribute(index, 1))
    b.capacity = cap
  }

  /**
   * Batch `meshes` (each a sprite drawn from a shared sheet material) into
   * `scene`, for the frame `camera` is about to draw. Call after every mesh has
   * been placed, turned and animated, and before rendering.
   */
  build (meshes, scene, camera) {
    if (camera) {
      camera.updateMatrixWorld()
      this._m.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse)
      this._frustum.setFromProjectionMatrix(this._m)
    }
    // Who goes where, first: a batch's buffer is sized before it is written.
    const groups = new Map()
    let alone = 0
    for (const mesh of meshes) {
      mesh.layers.set(0)
      const shared = mesh.material?.userData?.shared === true
      const plain = mesh.children.length === 0 && mesh.geometry?.attributes?.position?.count === 4
      if (!shared || !plain || !shownInScene(mesh)) {
        if (mesh.parent) alone++
        continue
      }
      // Off screen: the renderer culls it, as it always did.
      mesh.updateWorldMatrix(true, false)
      if (camera && mesh.frustumCulled !== false && !this._frustum.intersectsObject(mesh)) continue
      let list = groups.get(mesh.material)
      if (!list) { list = []; groups.set(mesh.material, list) }
      list.push(mesh)
    }
    // A sheet with one sprite on screen draws it alone, as before.
    for (const [material, list] of groups) {
      if (list.length < 2) { groups.delete(material); alone += list.length }
    }
    let quads = 0
    for (const b of this.batches.values()) b.count = 0
    for (const [material, list] of groups) {
      const b = this.#batchFor(material, scene)
      this.#grow(b, list.length)
      const g = b.mesh.geometry
      const P = g.attributes.position.array
      const U = g.attributes.uv.array
      const I = g.index.array
      let q = 0
      for (const mesh of list) {
        const src = mesh.geometry
        const sp = src.attributes.position
        const su = src.attributes.uv
        const base = q * 4
        for (let k = 0; k < 4; k++) {
          this._v.fromBufferAttribute(sp, k).applyMatrix4(mesh.matrixWorld)
          P[(base + k) * 3] = this._v.x
          P[(base + k) * 3 + 1] = this._v.y
          P[(base + k) * 3 + 2] = this._v.z
          U[(base + k) * 2] = su.getX(k)
          U[(base + k) * 2 + 1] = su.getY(k)
        }
        // The quad's own triangles, in its own order (a PlaneGeometry's), so
        // its winding -- and so which face is front -- is what it was.
        const si = src.index
        for (let k = 0; k < 6; k++) I[q * 6 + k] = base + (si ? si.getX(k) : [0, 2, 1, 2, 3, 1][k])
        mesh.layers.set(BATCHED_LAYER)
        q++
      }
      b.count = q
      quads += q
      g.attributes.position.needsUpdate = true
      g.attributes.uv.needsUpdate = true
      g.index.needsUpdate = true
      g.setDrawRange(0, q * 6)
    }
    // A sheet with nothing shown this frame draws nothing.
    for (const b of this.batches.values()) {
      if (b.count === 0) b.mesh.geometry.setDrawRange(0, 0)
      b.mesh.visible = b.count > 0
    }
    this.stats = { quads, batches: groups.size, alone }
    return this.stats
  }

  dispose () {
    for (const b of this.batches.values()) {
      b.mesh.parent?.remove(b.mesh)
      b.mesh.geometry.dispose()
    }
    this.batches.clear()
  }
}
