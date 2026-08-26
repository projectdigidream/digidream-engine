//
// The Three.js layer (design §4).
//
// Deliberately thin. Everything decided here is a rendering decision; every
// geometric decision was made in mapgeom.js, where it could be tested without a
// GPU. If a question has an answer that could be asserted in a unit test, it
// does not belong in this file.
//
import * as THREE from 'three'
import { buildMapSurfaces, buildSectorSurfaces, mergeByTexture } from '../mapgeom.js'
import { makeTextureSet, DEFAULT_WORLD_SIZE } from './textures.js'
import { NO_TEXTURE } from '../mapdata.js'
import {
  VERTEX_SHADER, FRAGMENT_SHADER, lightUniforms, MAX_ACTIVE_LIGHTS
} from './lighting.js'

function geometryFromGroup (g) {
  const geo = new THREE.BufferGeometry()
  geo.setAttribute('position', new THREE.Float32BufferAttribute(g.positions, 3))
  geo.setAttribute('normal', new THREE.Float32BufferAttribute(g.normals, 3))
  geo.setAttribute('uv', new THREE.Float32BufferAttribute(g.uvs, 2))
  geo.setAttribute('color', new THREE.Float32BufferAttribute(g.colors, 3))
  geo.setIndex(g.indices)
  return geo
}

export class MapView {
  constructor (map) {
    this.map = map
    this.root = new THREE.Group()
    this.textures = makeTextureSet(THREE, map.textures)

    // Lights become uniforms shared by every material, so moving one costs a
    // uniform write rather than a shader recompile.
    this.lights = (map.lights ?? []).slice(0, MAX_ACTIVE_LIGHTS).map((L) => ({
      px: L.x, py: L.z, pz: -L.y,       // engine z-up -> three y-up
      r: L.r, g: L.g, b: L.b,
      radius: L.radius, intensity: L.intensity
    }))
    if ((map.lights?.length ?? 0) > MAX_ACTIVE_LIGHTS) {
      // Never silently capped. A map that quietly lost half its lighting looks
      // like a renderer bug, and the author has no way to tell which half.
      console.warn(`[render] map has ${map.lights.length} lights; ` +
                   `only the first ${MAX_ACTIVE_LIGHTS} are drawn`)
    }
    this.lightUniforms = lightUniforms(THREE, this.lights)

    // A custom material rather than one of Three's lit ones, because the model
    // is not the conventional one: sector light is an ADDITIVE FLOOR, not a
    // multiplier. See lighting.js -- multiplying, which is what vertexColors on
    // a standard material does, makes a torch in a dark room stay dark.
    this.materials = this.textures.map((tex) => new THREE.ShaderMaterial({
      uniforms: {
        map: { value: tex },
        repeat: { value: new THREE.Vector2(1 / DEFAULT_WORLD_SIZE, 1 / DEFAULT_WORLD_SIZE) },
        ...this.lightUniforms
      },
      vertexShader: VERTEX_SHADER,
      fragmentShader: FRAGMENT_SHADER,
      side: THREE.FrontSide
    }))

    // Anything the map left untextured. Visible rather than invisible: a
    // missing texture should look like a missing texture, not like a hole.
    this.missing = new THREE.MeshBasicMaterial({
      color: 0xff00ff, vertexColors: true, side: THREE.FrontSide, wireframe: true
    })
    this.meshes = []
    this.rebuildAll()
  }

  materialFor (texture) {
    return texture === NO_TEXTURE ? this.missing : (this.materials[texture] ?? this.missing)
  }

  /**
   * Move or retint the lights.
   *
   * Uniforms are shared across every material, so this is one write per light
   * and no recompilation -- which is what makes a flickering lamp or a light
   * riding a lift cost nothing per frame.
   */
  setLights (lights) {
    const n = Math.min(lights.length, MAX_ACTIVE_LIGHTS)
    for (let i = 0; i < MAX_ACTIVE_LIGHTS; i++) {
      const L = lights[i]
      this.lightUniforms.lightPos.value[i].set(L ? L.px : 0, L ? L.py : 0, L ? L.pz : 0)
      this.lightUniforms.lightColor.value[i].setRGB(L ? L.r : 0, L ? L.g : 0, L ? L.b : 0)
      this.lightUniforms.lightRadius.value[i] = L ? L.radius : 0
      this.lightUniforms.lightIntensity.value[i] = L ? L.intensity : 0
    }
    this.lightUniforms.numLights.value = n
  }

  disposeMeshes () {
    for (const m of this.meshes) {
      this.root.remove(m)
      m.geometry.dispose()
    }
    this.meshes = []
  }

  rebuildAll () {
    this.disposeMeshes()
    for (const g of mergeByTexture(buildMapSurfaces(this.map))) {
      const mesh = new THREE.Mesh(geometryFromGroup(g), this.materialFor(g.texture))
      mesh.frustumCulled = false
      this.root.add(mesh)
      this.meshes.push(mesh)
    }
  }

  /**
   * Rebuild only the sectors that moved, plus the ones that look at them.
   *
   * A neighbour's upper and lower fields are drawn from ITS side of the shared
   * boundary, so a lift rising in sector 3 changes what sector 2's wall shows
   * even though sector 2 did not move. Rebuilding only the mover leaves the
   * hole it climbed out of on screen.
   */
  rebuildSectors (indices) {
    const dirty = new Set(indices)
    for (const i of indices) {
      const s = this.map.sectors[i]
      for (let k = 0; k < s.sideCount; k++) {
        const nb = this.map.sides[s.firstSide + k].neighbor
        if (nb >= 0) dirty.add(nb)
      }
    }
    // Rebuilt as one batch. The alternative -- a mesh per sector, updated in
    // place -- trades this rebuild for one draw call per sector, and a map is
    // far more sectors than textures.
    const surfaces = []
    for (let i = 0; i < this.map.sectors.length; i++) {
      surfaces.push(...buildSectorSurfaces(this.map, i))
    }
    this.disposeMeshes()
    for (const g of mergeByTexture(surfaces)) {
      const mesh = new THREE.Mesh(geometryFromGroup(g), this.materialFor(g.texture))
      mesh.frustumCulled = false
      this.root.add(mesh)
      this.meshes.push(mesh)
    }
    return dirty.size
  }
}

/**
 * An isometric view.
 *
 * Orthographic, which is what makes it isometric rather than merely angled: a
 * perspective camera at the same angle changes an entity's apparent size with
 * its distance from the camera, and in an isometric platformer that reads as
 * the character shrinking when they walk away from nothing in particular.
 */
export function makeIsoCamera (aspect, viewHeight = 26) {
  const halfH = viewHeight / 2
  const halfW = halfH * aspect
  const cam = new THREE.OrthographicCamera(-halfW, halfW, halfH, -halfH, 0.1, 4000)
  cam.up.set(0, 1, 0)
  return cam
}

export function resizeIsoCamera (cam, aspect, viewHeight) {
  const halfH = viewHeight / 2
  const halfW = halfH * aspect
  cam.left = -halfW; cam.right = halfW; cam.top = halfH; cam.bottom = -halfH
  cam.updateProjectionMatrix()
}

/** A stand-in body. Real entity rendering (sprites, meshes) is slice 2c. */
export function makeAgentMesh (color = 0x66ccff, radius = 0.5, height = 2) {
  const geo = new THREE.CapsuleGeometry(radius, Math.max(0.01, height - radius * 2), 4, 12)
  const mat = new THREE.MeshBasicMaterial({ color })
  const mesh = new THREE.Mesh(geo, mat)
  // The simulation's position is the entity's FEET; a capsule's origin is its
  // centre. Offsetting here rather than in the simulation keeps "position" one
  // thing everywhere else.
  mesh.userData.yOffset = height / 2
  return mesh
}
