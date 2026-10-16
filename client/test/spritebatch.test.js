//
// SPRITE BATCHING (render/spritebatch.js): sprites from one sheet on screen
// draw as one buffer, holding exactly the corners they would have drawn
// alone; everything the batch must not take draws alone, as before.
//
import * as THREE from 'three'
import { SpriteBatcher, BATCHED_LAYER } from '../src/render/spritebatch.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

const sheet = (name) => Object.assign(new THREE.MeshBasicMaterial({ transparent: true, alphaTest: 0.5 }),
  { name, userData: { shared: true } })
const sprite = (material, x, z, yaw = 0) => {
  const m = new THREE.Mesh(new THREE.PlaneGeometry(1, 2), material)
  m.position.set(x, 1, z)
  m.rotation.y = yaw
  return m
}
const worldCorners = (mesh) => {
  mesh.updateWorldMatrix(true, false)
  const p = mesh.geometry.attributes.position
  const out = []
  for (let i = 0; i < 4; i++) out.push(new THREE.Vector3().fromBufferAttribute(p, i).applyMatrix4(mesh.matrixWorld))
  return out
}
const camera = new THREE.PerspectiveCamera(70, 16 / 9, 0.1, 200)
camera.position.set(0, 1.5, 10)
camera.lookAt(0, 1, 0)
camera.updateMatrixWorld()
// What a camera would draw: visible meshes on a layer it renders.
const drawn = (scene) => {
  let n = 0
  scene.traverseVisible((o) => { if (o.isMesh && camera.layers.test(o.layers)) n++ })
  return n
}

console.log('one sheet, several on screen')
{
  const scene = new THREE.Scene()
  const a = sheet('a')
  const meshes = [sprite(a, -2, 0, 0.3), sprite(a, 0, 0, -0.7), sprite(a, 2, 1, 1.1)]
  for (const m of meshes) scene.add(m)
  const before = drawn(scene)
  const b = new SpriteBatcher({ THREE })
  const st = b.build(meshes, scene, camera)
  check(st.batches === 1 && st.quads === 3, `three sprites of one sheet are one batch (${st.batches} batch(es), ${st.quads} quads)`)
  check(meshes.every((m) => m.layers.mask === (1 << BATCHED_LAYER) >>> 0 || m.layers.test(new THREE.Layers().set(BATCHED_LAYER))),
        'and the meshes themselves are moved off the camera\'s layer')
  check(drawn(scene) === 1 && before === 3, `the camera draws one thing where it drew three (${before} -> ${drawn(scene)})`)
  // The buffer holds exactly what each mesh would have drawn.
  const batch = scene.children.find((o) => o.name === 'sprite-batch')
  const P = batch.geometry.attributes.position
  let worst = 0
  meshes.forEach((m, q) => {
    worldCorners(m).forEach((v, k) => {
      const w = new THREE.Vector3().fromBufferAttribute(P, q * 4 + k)
      worst = Math.max(worst, w.distanceTo(v))
    })
  })
  check(worst < 1e-5, `every corner is where the sprite alone would put it (worst ${worst.toExponential(1)})`)
  const uvOk = meshes.every((m, q) => [0, 1, 2, 3].every((k) =>
    batch.geometry.attributes.uv.getX(q * 4 + k) === m.geometry.attributes.uv.getX(k) &&
    batch.geometry.attributes.uv.getY(q * 4 + k) === m.geometry.attributes.uv.getY(k)))
  check(uvOk, 'and samples the sheet where the sprite alone would')
  check(batch.geometry.drawRange.count === 18, `six indices a quad (${batch.geometry.drawRange.count})`)
  check(batch.material === a, 'drawn with the sheet\'s own material')

  // A frame later, one has gone: the buffer shrinks to what is left.
  scene.remove(meshes[2])
  const st2 = b.build(meshes.slice(0, 2), scene, camera)
  check(st2.quads === 2 && batch.geometry.drawRange.count === 12, `a sprite gone is a quad gone (${st2.quads})`)
  // And with none left on screen, the batch draws nothing.
  for (const m of meshes.slice(0, 2)) m.position.x += 500
  b.build(meshes.slice(0, 2), scene, camera)
  check(!batch.visible || batch.geometry.drawRange.count === 0, 'a sheet with nothing on screen draws nothing')
}

console.log('what draws alone, as before')
{
  const scene = new THREE.Scene()
  const a = sheet('a')
  const lone = sprite(sheet('b'), 0, 0)
  const own = sprite(new THREE.MeshBasicMaterial(), 1, 0)          // a fading body's cloned material
  const fading = sprite(a, -1, 0)
  fading.add(new THREE.Mesh(new THREE.PlaneGeometry(1, 2), a))     // its ghost
  const offscreen = sprite(a, 400, 0)
  const hidden = sprite(a, 0.5, 0)
  hidden.visible = false
  const group = new THREE.Group()
  group.visible = false
  const underHidden = sprite(a, -0.5, 0)
  group.add(underHidden)
  const pair = [sprite(a, 3, 0), sprite(a, -3, 0)]
  const all = [lone, own, fading, offscreen, hidden, underHidden, ...pair]
  for (const m of [lone, own, fading, offscreen, hidden, group, ...pair]) scene.add(m)
  const b = new SpriteBatcher({ THREE })
  const st = b.build(all, scene, camera)
  const onCamera = (m) => camera.layers.test(m.layers)
  check(onCamera(lone), 'the only sprite of its sheet on screen draws alone')
  check(onCamera(own), 'a sprite with a material of its own (a cross-fade) draws alone')
  check(onCamera(fading), 'a sprite with a ghost under it draws alone')
  check(onCamera(offscreen), 'one off screen is left to the renderer\'s culling')
  check(onCamera(hidden) && onCamera(underHidden), 'hidden ones are left alone (the renderer skips them)')
  check(!onCamera(pair[0]) && !onCamera(pair[1]) && st.quads === 2,
        `CONTROL: and the two that qualify ARE batched (${st.quads} quads)`)
}

console.log(`\nspritebatch: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
