//
// The map's light on the things in it (client/src/render/proplight.js): the
// sector's level plus the lamps, by the map's own rule (lighting.js lightAt,
// with the light falling square on a sprite, which always faces the camera),
// carried per body in a quad's vertex colour and kept per body in a batch.
//
import * as THREE from 'three'
import { propLight, lightable, lightMesh } from '../src/render/proplight.js'
import { lightAt } from '../src/render/lighting.js'
import { SpriteBatcher } from '../src/render/spritebatch.js'

let checks = 0
let fails = 0
const check = (ok, what) => { checks++; if (!ok) fails++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}`) }
const near = (a, b) => Math.abs(a - b) < 1e-6

// A lamp at three-space (2, 1, 0), radius 4, warm.
const lamp = { px: 2, py: 1, pz: 0, r: 1, g: 0.6, b: 0.2, radius: 4, intensity: 1.5 }

// The sector alone.
const dark = propLight(64, { x: -10, y: 1, z: 0 }, [lamp])
check(near(dark.r, 64 / 255) && near(dark.g, 64 / 255), `out of the lamp's reach, the sector's level: ${dark.r.toFixed(3)} (64/255)`)

// And with the lamp: the map's own rule, the light square on the surface.
const at = { x: 0, y: 1, z: 0 }
const lit = propLight(64, at, [lamp])
const map = lightAt(at, { x: 1, y: 0, z: 0 }, [{ x: 2, y: 1, z: 0, r: 1, g: 0.6, b: 0.2, radius: 4, intensity: 1.5 }])
check(near(lit.r, 64 / 255 + map.r) && near(lit.b, 64 / 255 + map.b),
      `in reach, the sector's level plus what the map's surfaces get: ${lit.r.toFixed(3)}, ${lit.g.toFixed(3)}, ${lit.b.toFixed(3)}`)
check(lit.r > lit.g && lit.g > lit.b, 'and in the lamp\'s colour')
// Relative to the map's root: a lamp of a level drawn elsewhere is where its level is.
const moved = propLight(64, { x: 100, y: 1, z: 0 }, [lamp], { x: 100, y: 0, z: 0 })
check(near(moved.r, lit.r), 'a lamp is where its level is drawn (origin)')

// Carried in the quad: written once, and not again while it does not change.
const geo = lightable(THREE, new THREE.PlaneGeometry(1, 1))
const mesh = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({ vertexColors: true }))
const layer = new THREE.Mesh(lightable(THREE, new THREE.PlaneGeometry(1, 1)), mesh.material)
mesh.add(layer)
check([...geo.attributes.color.array].every((v) => v === 1), 'unlit, a quad is white: the picture as drawn')
lightMesh(mesh, lit)
const v1 = geo.attributes.color.version
lightMesh(mesh, lit)
check(near(geo.attributes.color.getX(3), lit.r) && near(layer.geometry.attributes.color.getZ(0), lit.b),
      'lit, every vertex carries it, and what the body wears too')
check(geo.attributes.color.version === v1, 'the same light again uploads nothing')

// Kept per body in a sheet's batch.
const sheet = Object.assign(new THREE.MeshBasicMaterial({ vertexColors: true, transparent: true }), { userData: { shared: true } })
const scene = new THREE.Scene()
const a = new THREE.Mesh(lightable(THREE, new THREE.PlaneGeometry(1, 1)), sheet)
const b = new THREE.Mesh(lightable(THREE, new THREE.PlaneGeometry(1, 1)), sheet)
a.position.set(0, 0, 0); b.position.set(3, 0, 0)
scene.add(a, b)
lightMesh(a, { r: 0.25, g: 0.25, b: 0.25 })
lightMesh(b, { r: 1, g: 0.5, b: 0 })
const batcher = new SpriteBatcher({ THREE })
batcher.build([a, b], scene, null)
const batch = scene.children.find((c) => c.name === 'sprite-batch')
const C = batch?.geometry.attributes.color.array ?? []
check(batch && near(C[0], 0.25) && near(C[4 * 3], 1) && near(C[4 * 3 + 1], 0.5),
      'batched, each body keeps its own light')

console.log(`\nproplight: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
