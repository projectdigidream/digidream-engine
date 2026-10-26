//
// THE MAP'S LIGHT ON THE THINGS IN IT: props, monsters and players lit as the
// map's surfaces are (lighting.js) -- their sector's light level as the floor,
// and every point light near them added on top, by the same falloff. Without
// the angle term: a sprite always faces the camera and has no side to turn
// away from a lamp. A kind the catalogue calls `bright` (a fireball, a lamp, a
// lost soul) is not lit: it is its own light, drawn at full brightness.
//
// Carried as the colour of the quad's vertices, which its material multiplies
// the picture by (vertexColors). Per body, although every body from one sheet
// shares one material, and copied into the sheet's batch with the rest of the
// quad (spritebatch.js). White, the picture as drawn, until something lights it.
//
import { attenuation } from './lighting.js'

const NONE = { x: 0, y: 0, z: 0 }

/**
 * The light at `p` (three-space) in a sector of light level `level` (0..255),
 * from `lights` (MapView's: three-space, relative to `origin`, the map's root).
 */
export function propLight (level, p, lights, origin = NONE) {
  const l = level / 255
  let r = l
  let g = l
  let b = l
  for (const L of lights) {
    const d = Math.hypot(L.px + origin.x - p.x, L.py + origin.y - p.y, L.pz + origin.z - p.z)
    const a = attenuation(d, L.radius) * L.intensity
    if (a > 0) { r += L.r * a; g += L.g * a; b += L.b * a }
  }
  return { r, g, b }
}

/** A sprite quad's colour attribute, white: what the picture looks like unlit. */
export function lightable (THREE, geometry) {
  const n = geometry.attributes?.position?.count
  if (!n || !THREE.BufferAttribute) return geometry   // a test's stand-in for three.js
  geometry.setAttribute('color', new THREE.BufferAttribute(new Float32Array(n * 3).fill(1), 3))
  return geometry
}

/** Light a mesh (and what it wears: a look's layers, a fade's ghost). Written only when it changed. */
export function lightMesh (mesh, c) {
  const col = mesh.geometry?.getAttribute?.('color')
  if (col) {
    const r = Math.fround(c.r)
    const g = Math.fround(c.g)
    const b = Math.fround(c.b)
    const a = col.array
    if (a[0] !== r || a[1] !== g || a[2] !== b) {
      for (let i = 0; i < col.count; i++) { a[i * 3] = r; a[i * 3 + 1] = g; a[i * 3 + 2] = b }
      col.needsUpdate = true
    }
  }
  for (const child of mesh.children) lightMesh(child, c)
}
