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
import { makeTextureSet, loadRealTextures, DEFAULT_WORLD_SIZE } from './textures.js'
import { NO_TEXTURE } from '../mapdata.js'
import {
  VERTEX_SHADER, FRAGMENT_SHADER, lightUniforms, MAX_ACTIVE_LIGHTS
} from './lighting.js'
import { distanceForFraming } from '../camera.js'

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
      side: THREE.FrontSide,
      // Transparent so the occlusion window can fade, but depth is still
      // WRITTEN: everything outside the window is at alpha 1, where blending is
      // identical to opaque. Turning depth writing off -- the usual reflex for
      // a transparent material -- would make the entire map sort per object
      // instead of per fragment, and a map is a handful of merged meshes.
      transparent: true,
      depthWrite: true
    }))

    // Anything the map left untextured. Visible rather than invisible: a
    // missing texture should look like a missing texture, not like a hole.
    this.missing = new THREE.MeshBasicMaterial({
      color: 0xff00ff, vertexColors: true, side: THREE.FrontSide, wireframe: true
    })
    this.staticMeshes = []
    this.dynamicMeshes = []
    this.dynamic = new Set()
    this.rebuildStatic()
  }

  /** Replace the placeholders with real images where the client has them. */
  async loadTextures (opts) {
    return loadRealTextures(THREE, this.map.textures, this.materials, opts)
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
  /**
   * Aim the occlusion window at a point, or turn it off with a null target.
   *
   * One uniform write, shared by every material -- the window follows the
   * player at no per-frame cost beyond this.
   */
  setOcclusionTarget (target, strength = 0.82) {
    if (!target) { this.lightUniforms.occludeStrength.value = 0; return }
    this.lightUniforms.occludeTarget.value.set(target.x, target.y, target.z)
    this.lightUniforms.occludeStrength.value = strength
  }

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

  /**
   * Split the map into a static half and a dynamic one.
   *
   * Everything not listed is built ONCE. Only the listed sectors -- and the
   * ones that look at them -- are rebuilt per frame.
   *
   * That split is the whole point. A moving sector changes geometry every tick,
   * and the first version rebuilt the entire map to follow it: every sector's
   * surfaces re-derived, re-merged, and every BufferGeometry disposed and
   * recreated, sixty times a second, for a lift. On a converted Doom map that
   * was a hundred geometries a frame.
   *
   * A neighbour is included because upper and lower wall fields are drawn from
   * the neighbour's side of a shared boundary: a lift rising in sector 3
   * changes what sector 2 draws even though sector 2 did not move.
   */
  setDynamicSectors (indices) {
    this.dynamic = new Set()
    for (const i of indices) {
      this.dynamic.add(i)
      const s = this.map.sectors[i]
      if (!s) continue
      for (let k = 0; k < s.sideCount; k++) {
        const nb = this.map.sides[s.firstSide + k].neighbor
        if (nb >= 0) this.dynamic.add(nb)
      }
    }
    this.rebuildStatic()
    this.rebuildDynamic()
  }

  #build (indices, into) {
    for (const m of into) { this.root.remove(m); m.geometry.dispose() }
    into.length = 0
    const surfaces = []
    for (const i of indices) surfaces.push(...buildSectorSurfaces(this.map, i))
    for (const g of mergeByTexture(surfaces)) {
      const mesh = new THREE.Mesh(geometryFromGroup(g), this.materialFor(g.texture))
      mesh.frustumCulled = false
      this.root.add(mesh)
      into.push(mesh)
    }
  }

  rebuildStatic () {
    const idx = []
    for (let i = 0; i < this.map.sectors.length; i++) if (!this.dynamic.has(i)) idx.push(i)
    this.#build(idx, this.staticMeshes)
  }

  /** Per-frame, and only for the sectors that actually move. */
  rebuildDynamic () {
    if (this.dynamic.size === 0) return
    this.#build([...this.dynamic].sort((a, b) => a - b), this.dynamicMeshes)
  }

  get meshes () { return [...this.staticMeshes, ...this.dynamicMeshes] }
}

/**
 * The view.
 *
 * Two projections, one framing. Both are set up from the same number -- how
 * many world units are visible vertically at the subject -- so switching
 * between them changes the projection and nothing else:
 *
 *   ORTHOGRAPHIC. Parallel projection: an entity's size on screen does not
 *   depend on its distance from the camera. That is what makes a view
 *   isometric rather than merely angled, and it is what design §4 describes.
 *
 *   PERSPECTIVE. Converging projection, with the camera placed at the distance
 *   where its frustum is exactly `viewHeight` tall at the subject. So the
 *   subject is framed identically and everything else is not: near geometry
 *   grows, far geometry shrinks, and vertical walls acquire visible convergence
 *   instead of being drawn as parallelograms.
 *
 * The trade is real in both directions. Perspective reads as depth and makes
 * the slabs look like objects rather than decals; it also means an entity's
 * apparent size varies with where it stands, which in a platformer can make two
 * platforms at the same height look like different sizes.
 */
export const PROJECTIONS = ['perspective', 'orthographic']

export function makeCamera (kind, aspect, viewHeight, fovDegrees = 50) {
  if (kind === 'orthographic') {
    const halfH = viewHeight / 2
    const cam = new THREE.OrthographicCamera(-halfH * aspect, halfH * aspect, halfH, -halfH, 0.1, 4000)
    cam.up.set(0, 1, 0)
    cam.userData.kind = 'orthographic'
    cam.userData.fov = fovDegrees
    return cam
  }
  const cam = new THREE.PerspectiveCamera(fovDegrees, aspect, 0.5, 5000)
  cam.up.set(0, 1, 0)
  cam.userData.kind = 'perspective'
  return cam
}

export function resizeCamera (cam, aspect, viewHeight) {
  if (cam.userData.kind === 'orthographic') {
    const halfH = viewHeight / 2
    cam.left = -halfH * aspect
    cam.right = halfH * aspect
    cam.top = halfH
    cam.bottom = -halfH
  } else {
    cam.aspect = aspect
  }
  cam.updateProjectionMatrix()
}

/**
 * How far the camera must sit to see `viewHeight` world units at the subject.
 *
 * For a perspective camera this is what turns a zoom level into a position.
 * For an orthographic one the distance does not affect framing at all, so the
 * default is kept -- moving it would only change what the near and far planes
 * clip.
 */
export function distanceFor (cam, viewHeight, fallback) {
  if (cam.userData.kind === 'orthographic') return fallback
  return distanceForFraming(cam.fov, viewHeight)
}

/**
 * A debug arrow showing which way an entity faces, and how far it can reach.
 *
 * Its LENGTH is the engine's interact reach, read from the engine rather than
 * chosen here, so the arrow answers the question you actually have when you are
 * standing at a door: is this thing in range. An arrow that merely pointed
 * would show you aiming at a switch the engine considers out of reach and give
 * you no way to tell the difference.
 *
 * Drawn with depthTest off so it stays visible through the body and through
 * whatever wall the occlusion window has faded -- it is an instrument, not part
 * of the scene, and an instrument you cannot see when you need it is worse than
 * none.
 */
export function makeFacingArrow (reach, color = 0xffe066) {
  const g = new THREE.Group()
  const mat = new THREE.MeshBasicMaterial({ color, depthTest: false, transparent: true, opacity: 0.9 })

  const headLen = Math.min(0.3, reach * 0.3)
  const shaftLen = Math.max(0.01, reach - headLen)
  const thickness = Math.max(0.015, reach * 0.03)

  // Built pointing along +X, so aiming it is one rotation about Y and the
  // conversion from a facing angle stays in one place (camera.js).
  const shaft = new THREE.Mesh(new THREE.CylinderGeometry(thickness, thickness, shaftLen, 8), mat)
  shaft.rotation.z = -Math.PI / 2
  shaft.position.x = shaftLen / 2
  g.add(shaft)

  const head = new THREE.Mesh(new THREE.ConeGeometry(thickness * 3, headLen, 10), mat)
  head.rotation.z = -Math.PI / 2
  head.position.x = shaftLen + headLen / 2
  g.add(head)

  g.renderOrder = 999
  return g
}

/**
 * Aim an arrow built by makeFacingArrow.
 *
 * `dir` is a three-space unit direction from camera.js's facingDirection. The
 * rotation is derived from it rather than from the angle again, so there is
 * exactly one place that decides which way a facing points.
 */
export function aimFacingArrow (arrow, at, dir) {
  arrow.position.set(at.x, at.y, at.z)
  arrow.rotation.y = Math.atan2(-dir.z, dir.x)
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
