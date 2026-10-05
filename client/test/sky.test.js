//
// The sky (docs/SKY.md): the SKY_SET message as the server writes it, where a
// direction lands on DOOM's sky, DOOM's sky hack in the mesh builder, and the
// Sky's own switching -- pictures loaded once, a sky with none drawn as before.
//
import * as THREE from 'three'
import { encodeControlMessage, decodeControlMessage, MSG } from '../src/proto.js'
import { Sky, SKY_KIND, DOOM_SKY, skyFromMessage, skyUv, isSkyName } from '../src/render/sky.js'
import { buildSectorSurfaces, SURFACE } from '../src/mapgeom.js'
import { NO_TEXTURE, NO_SECTOR } from '../src/mapdata.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-6) => Math.abs(a - b) <= eps

console.log('sky: the message, the mapping, the hack, the switching')

// --- the wire: encode_sky_set's body ------------------------------------------
const body = (kind, images, fields) => {
  const parts = [kind, images.length]
  for (const n of images) {
    const b = new TextEncoder().encode(n)
    parts.push(b.length & 255, b.length >> 8, ...b)
  }
  parts.push(fields.length & 255, fields.length >> 8)
  for (const [id, milli] of fields) {
    const v = milli >>> 0
    parts.push(id, v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >>> 24) & 255)
  }
  return Uint8Array.from(parts)
}
{
  const m = decodeControlMessage(encodeControlMessage(MSG.SKY_SET, body(1, ['SKY2'], [[1, 2000], [2, -250]])))
  check(m?.type === MSG.SKY_SET && m.kind === 1 && m.images.join() === 'SKY2' &&
        m.fields[0].milli === 2000 && m.fields[1].milli === -250, `a strip sky decodes (${JSON.stringify(m)})`)
  const s = skyFromMessage(m)
  check(s.kind === SKY_KIND.STRIP && s.repeat === 2 && s.yaw === -0.25 && s.height === DOOM_SKY.height,
        'its fields land by name, and what it does not say is DOOM\'s')
  const cube = decodeControlMessage(encodeControlMessage(MSG.SKY_SET, body(3, ['a', 'b', 'c', 'd', 'e', 'f'], [])))
  check(skyFromMessage(cube).kind === SKY_KIND.CUBE, 'a cube of six decodes')
  check(decodeControlMessage(encodeControlMessage(MSG.SKY_SET, body(4, [], []))) === null, 'CONTROL: an unknown kind is refused')
  check(decodeControlMessage(encodeControlMessage(MSG.SKY_SET, body(3, ['a', 'b', 'c', 'd', 'e', 'f', 'g'], []))) === null,
        'CONTROL: a seventh picture is refused')
  check(skyFromMessage({ kind: 3, images: ['a', 'b'], fields: [] }).kind === SKY_KIND.NONE &&
        skyFromMessage({ kind: 1, images: [], fields: [] }).kind === SKY_KIND.NONE,
        'a cube short of faces, or a strip with no picture, is no sky rather than a broken one')
  check(skyFromMessage(null).kind === SKY_KIND.NONE, 'and no message is no sky')
}

// --- where a direction lands ----------------------------------------------------
{
  const doom = { kind: SKY_KIND.STRIP, repeat: 4, yaw: 0, height: DOOM_SKY.height }
  const [, vh] = skyUv(doom, [0, 0, -1])
  check(near(vh, DOOM_SKY.below / (1 + DOOM_SKY.below)), `the horizon is DOOM's row 100 of 128 (v ${vh.toFixed(3)})`)
  check(skyUv(doom, [0, 1, 0])[1] === 1 && skyUv(doom, [0, 0.625, -1])[1] === 1, 'up, and the top edge\'s elevation, are the picture\'s top')
  check(skyUv(doom, [0, -1, 0])[1] === 0, 'down is held at its bottom row')
  const us = [0, 1, 2, 3].map((q) => skyUv(doom, [Math.sin(q * Math.PI / 2), 0, -Math.cos(q * Math.PI / 2)])[0])
  check(near(us[1] - us[0], 1) && near(us[2] - us[1], 1), `a quarter turn is one picture: four round the horizon (${us.map((u) => u.toFixed(2))})`)
  const turned = skyUv({ ...doom, yaw: 0.25 }, [0, 0, -1])[0]
  check(near(Math.abs(turned - us[0]), 1), `yaw turns it (a quarter turn moves it one picture: ${turned.toFixed(2)})`)
  const pano = { kind: SKY_KIND.PANORAMA, yaw: 0 }
  check(near(skyUv(pano, [0, 1, 0])[1], 1) && near(skyUv(pano, [0, 0, -1])[1], 0.5) && near(skyUv(pano, [0, -1, 0])[1], 0),
        'a panorama: up its top, the horizon its middle, down its bottom')
}

// --- DOOM's sky hack in the mesh builder ----------------------------------------
check(isSkyName('F_SKY1') && isSkyName('f_sky2') && !isSkyName('FLAT5') && !isSkyName(undefined), 'F_SKY* flats are sky')
{
  // Two rooms; sector 1's ceiling is lower, so sector 0 shows an upper wall.
  const map = (ceil0, ceil1) => ({
    points: [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 10, y: 10 }, { x: 0, y: 10 },
             { x: 10, y: 0 }, { x: 20, y: 0 }, { x: 20, y: 10 }, { x: 10, y: 10 }],
    sectors: [
      { floorZ: 0, ceilZ: 8, firstPoint: 0, pointCount: 4, firstSide: 0, sideCount: 4, tag: 0, firstTri: 0, triCount: 2, floorTex: 1, ceilTex: ceil0, light: 255 },
      { floorZ: 0, ceilZ: 5, firstPoint: 4, pointCount: 4, firstSide: 4, sideCount: 4, tag: 0, firstTri: 6, triCount: 2, floorTex: 1, ceilTex: ceil1, light: 255 }
    ],
    sides: [
      { a: 0, b: 1, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 1, b: 2, neighbor: 1, flags: 0, upper: 4, middle: NO_TEXTURE, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 2, b: 3, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 3, b: 0, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 4, b: 5, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 5, b: 6, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 6, b: 7, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 3, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 },
      { a: 7, b: 4, neighbor: 0, flags: 0, upper: 4, middle: NO_TEXTURE, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 }
    ],
    tris: Uint16Array.from([3, 0, 1, 1, 2, 3, 7, 4, 5, 5, 6, 7]),
    textures: ['', 'FLOOR', 'CEIL', 'WALL', 'UPPER', 'F_SKY1']
  })
  const uppers = (m) => buildSectorSurfaces(m, 0).filter((s) => s.kind === SURFACE.UPPER).length
  check(uppers(map(5, 5)) === 0, 'between two sky ceilings there is no upper wall')
  check(uppers(map(5, 2)) === 1 && uppers(map(2, 5)) === 1 && uppers(map(2, 2)) === 1,
        'CONTROL: with either ceiling not sky, the upper wall is drawn as before')
}

// --- the Sky: switching, and loading each picture once -----------------------------
{
  const asked = []
  const T = {
    ...THREE,
    TextureLoader: class { load (url, ok) { asked.push(url); ok(new THREE.Texture()) } },
    // As the real loader: its faces come marked sRGB.
    CubeTextureLoader: class { load (urls, ok) { asked.push(...urls); const t = new THREE.CubeTexture(); t.colorSpace = THREE.SRGBColorSpace; ok(t) } }
  }
  const sky = new Sky(T)
  check(!sky.background.visible && sky.uniforms.kind.value === SKY_KIND.NONE,
        'before any sky: no background, so the clear colour shows as it always did')
  await sky.set({ kind: SKY_KIND.STRIP, images: ['SKY1'], repeat: 4, yaw: 0, height: 0.625 })
  check(sky.background.visible && sky.uniforms.kind.value === SKY_KIND.STRIP && sky.uniforms.pic.value &&
        asked.join() === '/textures/SKY1.png', 'a strip: drawn, its picture from /textures/')
  await sky.set({ kind: SKY_KIND.STRIP, images: ['SKY2'], repeat: 4, yaw: 0, height: 0.625 })
  await sky.set({ kind: SKY_KIND.STRIP, images: ['SKY1'], repeat: 2, yaw: 0, height: 0.625 })
  check(asked.filter((u) => u.endsWith('SKY1.png')).length === 1 && sky.uniforms.repeatTimes.value === 2,
        'back to a picture seen before: not loaded again, its settings new')
  await sky.set({ kind: SKY_KIND.CUBE, images: ['a', 'b', 'c', 'd', 'e', 'f'], repeat: 4, yaw: 0, height: 0.625 })
  check(sky.uniforms.kind.value === SKY_KIND.CUBE && sky.uniforms.cube.value && asked.length === 8, 'a cube: six faces')
  check(sky.uniforms.cube.value.colorSpace === THREE.NoColorSpace,
        `and raw, not decoded from sRGB: the shader never encodes back (${sky.uniforms.cube.value.colorSpace})`)
  check(sky.uniforms.pic.value === null || sky.uniforms.pic.value.colorSpace === THREE.NoColorSpace,
        'CONTROL: as the strip\'s picture is')
  // The message's order is the game's directions; three.js's slots are what the
  // shader samples (docs/SKY.md 2.1, checked by eye in a browser).
  await sky.set({ kind: SKY_KIND.CUBE, images: ['E', 'W', 'N', 'S', 'U', 'D'], repeat: 4, yaw: 0, height: 0.625 })
  check(asked.slice(-6).map((u) => u.match(/(\w)\.png/)[1]).join('') === 'WEUDSN',
        `east west north south up down go to three's slots as west east up down south north (${asked.slice(-6).join(' ')})`)
  await sky.set(skyFromMessage(null))
  check(!sky.background.visible && sky.uniforms.kind.value === SKY_KIND.NONE, 'and none puts the old look back')

  const warned = []
  const broken = new Sky({ ...T, TextureLoader: class { load (url, ok, _, err) { err(new Error('404')) } } }, { warn: (m) => warned.push(m) })
  await broken.set({ kind: SKY_KIND.PANORAMA, images: ['gone'], repeat: 4, yaw: 0, height: 0.625 })
  await broken.set(skyFromMessage(null))
  await broken.set({ kind: SKY_KIND.PANORAMA, images: ['gone'], repeat: 4, yaw: 1, height: 0.625 })
  check(warned.length === 1 && /gone\.png/.test(warned[0]) && broken.uniforms.kind.value === SKY_KIND.NONE,
        'a picture that does not load: said once, and the sky drawn plain')

  // The two shaders compile as one function of direction: the same source.
  check(sky.surface.fragmentShader.includes('skyColour') && sky.background.material.fragmentShader.includes('skyColour'),
        'the background and the sky flats share one sky')
}

console.log(`\nsky: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
