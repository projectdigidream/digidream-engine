//
// THE SKY (docs/SKY.md).
//
// Two things draw it, and they draw the SAME picture, because they share one
// GLSL function of the view direction and one set of uniforms:
//
//   - the BACKGROUND, a full-screen triangle drawn first, behind everything,
//     wherever no geometry is -- what a platformer sees past the level's edge
//     and what an open courtyard sees above its walls;
//   - SKY SURFACES, the flats a map names F_SKY* (DOOM's convention, kept so the
//     converted maps need nothing): windows onto the sky rather than a picture
//     of one, writing depth like a wall so nothing behind them shows through.
//
// A sky is a function of direction only -- infinitely far, so it never moves
// as the camera does. That is what makes a ceiling of sky line up with the sky
// beyond the wall beside it, which two separately-placed pictures never would.
//
// The kinds: `strip` (DOOM's: one picture wrapped round the horizon `repeat`
// times, the top edge at an elevation whose tangent is `height`), `panorama`
// (an equirectangular picture of the whole sphere), `cube` (six faces). And
// `none`: sky surfaces show a quiet gradient and the background stays the
// clear colour -- which is what every level looked like before this, so a
// module with no sky looks exactly as it did.
//

export const SKY_KIND = Object.freeze({ NONE: 0, STRIP: 1, PANORAMA: 2, CUBE: 3 })
// The wire's field ids (MSG.SKY_SET, docs/SKY.md): thousandths.
export const SKY_FIELD = Object.freeze({ 1: 'repeat', 2: 'yaw', 3: 'height' })
// DOOM's sky: 1024 texels round the horizon from a 256-wide picture, and the
// picture's top 100 of 128 rows above the horizon at a focal length of 160 --
// so its top edge sits at tan(elevation) = 100/160 and its bottom at -28/160.
export const DOOM_SKY = Object.freeze({ repeat: 4, height: 0.625, below: 0.28 })

/** Is this flat a window onto the sky? DOOM's name for it, F_SKY1 (and kin). */
export function isSkyName (name) {
  return typeof name === 'string' && /^F_SKY/i.test(name)
}

/** A SKY_SET message to a plain description, the defaults filled in. */
export function skyFromMessage (m) {
  const sky = { kind: m?.kind ?? SKY_KIND.NONE, images: [...(m?.images ?? [])], repeat: DOOM_SKY.repeat, yaw: 0, height: DOOM_SKY.height }
  for (const f of m?.fields ?? []) {
    const name = SKY_FIELD[f.id]
    if (name) sky[name] = f.milli / 1000
  }
  if (sky.kind === SKY_KIND.CUBE && sky.images.length !== 6) sky.kind = SKY_KIND.NONE
  if ((sky.kind === SKY_KIND.STRIP || sky.kind === SKY_KIND.PANORAMA) && sky.images.length < 1) sky.kind = SKY_KIND.NONE
  return sky
}

/**
 * Where a direction lands on a sky picture: [u, v], v = 1 the picture's top.
 * The shader's own arithmetic, in JS, so it can be tested without a GPU.
 * `dir` is three.js's frame: +y up.
 */
export function skyUv (sky, [x, y, z]) {
  const a = (sky.yaw ?? 0) * Math.PI * 2
  const rx = x * Math.cos(a) - z * Math.sin(a)
  const rz = x * Math.sin(a) + z * Math.cos(a)
  const turn = Math.atan2(rx, -rz) / (Math.PI * 2)
  if (sky.kind === SKY_KIND.PANORAMA) {
    const len = Math.hypot(rx, y, rz) || 1
    return [turn + 0.5, Math.asin(Math.max(-1, Math.min(1, y / len))) / Math.PI + 0.5]
  }
  const t = y / Math.max(Math.hypot(rx, rz), 1e-4)
  const top = sky.height ?? DOOM_SKY.height
  const bottom = -top * DOOM_SKY.below
  return [turn * (sky.repeat ?? DOOM_SKY.repeat), Math.max(0, Math.min(1, (t - bottom) / (top - bottom)))]
}

const SKY_GLSL = /* glsl */`
  uniform int kind;
  uniform sampler2D pic;
  uniform samplerCube cube;
  uniform float repeatTimes;
  uniform float yaw;
  uniform float height;
  uniform vec3 zenith;
  uniform vec3 horizon;
  uniform vec3 capTop;
  uniform vec3 capBottom;
  const float TAU = 6.28318530718;
  vec3 skyColour (vec3 d) {
    d = normalize(d);
    float a = yaw * TAU;
    vec3 r = vec3(d.x * cos(a) - d.z * sin(a), d.y, d.x * sin(a) + d.z * cos(a));
    float turn = atan(r.x, -r.z) / TAU;
    if (kind == ${SKY_KIND.STRIP}) {
      float t = r.y / max(length(r.xz), 1e-4);
      float bottom = -height * ${DOOM_SKY.below.toFixed(4)};
      float v = clamp((t - bottom) / (height - bottom), 0.0, 1.0);
      vec4 p = texture2D(pic, vec2(turn * repeatTimes, v));
      // A pixel the picture left transparent (DOOM's skies have a few) is the
      // cap's colour, not whatever colour hides under its zero alpha.
      vec3 c = mix(t > 0.0 ? capTop : capBottom, p.rgb, step(0.5, p.a));
      // Past its edges, a strip's last row would be smeared into streaks that
      // meet overhead. Fade into the edge rows' average instead (GZDoom's cap).
      c = mix(c, capTop, smoothstep(height, height + 0.3, t));
      return mix(c, capBottom, smoothstep(bottom, bottom - 0.12, t));
    }
    if (kind == ${SKY_KIND.PANORAMA}) {
      float v = asin(clamp(r.y, -1.0, 1.0)) / 3.14159265359 + 0.5;
      // Gradients from whichever of u and u+0.5 is continuous here, so the
      // wrap from 1 back to 0 does not pick the smallest mip: a seam otherwise.
      float u = turn + 0.5;
      float w = fract(turn + 1.0);
      vec2 gx = vec2(dFdx(u), dFdx(v)); vec2 gy = vec2(dFdy(u), dFdy(v));
      vec2 hx = vec2(dFdx(w), dFdx(v)); vec2 hy = vec2(dFdy(w), dFdy(v));
      if (abs(hx.x) + abs(hy.x) < abs(gx.x) + abs(gy.x)) { gx = hx; gy = hy; }
      return textureGrad(pic, vec2(u, v), gx, gy).rgb;
    }
    if (kind == ${SKY_KIND.CUBE}) {
      // Sampled exactly as three.js samples its own cube backgrounds (x
      // flipped), and the slots filled so the picture named for a direction is
      // the one seen looking that way, upright and unmirrored (Sky.set). Up and
      // down meet SOUTH, as in three.js: a set that looks right there looks the
      // same here (docs/SKY.md 2.3).
      return textureCube(cube, vec3(-r.x, r.y, r.z)).rgb;
    }
    return mix(horizon, zenith, clamp(r.y * 1.5, 0.0, 1.0));
  }
`

// The background: a triangle covering the screen at the far plane, its
// direction from the inverse of the camera's projection and view.
const BG_VERTEX = /* glsl */`
  varying vec2 vNdc;
  void main () {
    vNdc = position.xy;
    gl_Position = vec4(position.xy, 1.0, 1.0);
  }
`
const BG_FRAGMENT = /* glsl */`
  ${SKY_GLSL}
  uniform mat4 unproject;
  uniform vec3 eye;
  varying vec2 vNdc;
  void main () {
    vec4 p = unproject * vec4(vNdc, 1.0, 1.0);
    gl_FragColor = vec4(skyColour(p.xyz / p.w - eye), 1.0);
  }
`
// A sky surface: its direction from the camera.
const SURFACE_VERTEX = /* glsl */`
  varying vec3 vWorld;
  void main () {
    vec4 w = modelMatrix * vec4(position, 1.0);
    vWorld = w.xyz;
    gl_Position = projectionMatrix * viewMatrix * w;
  }
`
const SURFACE_FRAGMENT = /* glsl */`
  ${SKY_GLSL}
  varying vec3 vWorld;
  void main () {
    gl_FragColor = vec4(skyColour(vWorld - cameraPosition), 1.0);
  }
`

/**
 * The sky, for one renderer: its uniforms, the background mesh to add to the
 * scene, and the material every MapView gives its sky surfaces.
 */
export class Sky {
  constructor (THREE, { baseUrl = '/textures', clear = 0x0b0d12, warn = (m) => console.warn(m) } = {}) {
    this.THREE = THREE
    this.baseUrl = baseUrl
    this.warn = warn
    this.sky = skyFromMessage(null)
    this.key = ''
    this.warned = new Set()
    // Every picture loaded, by kind and names: a level change back to a sky
    // seen before reuses it rather than uploading a second copy. Never freed --
    // a module has a handful of skies, and a freed one still in use elsewhere
    // would be a flicker.
    this.cache = new Map()
    const c = new THREE.Color(clear)
    this.uniforms = {
      kind: { value: SKY_KIND.NONE },
      pic: { value: null },
      cube: { value: null },
      repeatTimes: { value: DOOM_SKY.repeat },
      yaw: { value: 0 },
      height: { value: DOOM_SKY.height },
      zenith: { value: c.clone() },
      horizon: { value: c.clone().lerp(new THREE.Color(0x2a3550), 0.6) },
      capTop: { value: new THREE.Color(0, 0, 0) },
      capBottom: { value: new THREE.Color(0, 0, 0) },
      unproject: { value: new THREE.Matrix4() },
      eye: { value: new THREE.Vector3() }
    }
    this.surface = new THREE.ShaderMaterial({
      uniforms: this.uniforms, vertexShader: SURFACE_VERTEX, fragmentShader: SURFACE_FRAGMENT,
      // Front faces only, as every map surface: a sky ceiling seen from above
      // (a platformer's camera) must not cover the room beneath it.
      side: THREE.FrontSide, depthWrite: true
    })
    const geo = new THREE.BufferGeometry()
    geo.setAttribute('position', new THREE.Float32BufferAttribute([-1, -1, 0, 3, -1, 0, -1, 3, 0], 3))
    this.background = new THREE.Mesh(geo, new THREE.ShaderMaterial({
      uniforms: this.uniforms, vertexShader: BG_VERTEX, fragmentShader: BG_FRAGMENT,
      depthTest: false, depthWrite: false
    }))
    this.background.name = 'sky'
    this.background.frustumCulled = false
    // First, behind everything: it writes no depth, so the level draws over it.
    this.background.renderOrder = -1e9
    this.background.visible = false
  }

  /** Before each frame: where the camera is looking from. */
  update (camera) {
    if (!this.background.visible) return
    camera.updateMatrixWorld()
    this.uniforms.unproject.value.multiplyMatrices(camera.matrixWorld, camera.projectionMatrixInverse)
    this.uniforms.eye.value.setFromMatrixPosition(camera.matrixWorld)
  }

  /**
   * Show a sky (skyFromMessage's shape). Pictures load in the background; until
   * they do -- and for good, if one fails, said once -- the gradient is drawn.
   */
  set (sky) {
    const key = JSON.stringify(sky)
    if (key === this.key) return Promise.resolve(false)
    this.key = key
    this.sky = sky
    const u = this.uniforms
    u.repeatTimes.value = sky.repeat
    u.yaw.value = sky.yaw
    u.height.value = sky.height
    u.kind.value = SKY_KIND.NONE
    this.background.visible = sky.kind !== SKY_KIND.NONE
    if (sky.kind === SKY_KIND.NONE) return Promise.resolve(true)
    const THREE = this.THREE
    const url = (n) => `${this.baseUrl}/${n}.png`
    const fail = (n) => {
      if (this.warned.has(n)) return
      this.warned.add(n)
      this.warn(`[sky] ${url(n)} did not load -- the sky is drawn plain`)
    }
    const cacheKey = `${sky.kind}:${sky.images.join(',')}`
    const cached = this.cache.get(cacheKey)
    if (cached) {
      if (sky.kind === SKY_KIND.CUBE) u.cube.value = cached
      else u.pic.value = cached
      if (sky.kind === SKY_KIND.STRIP) this.#caps(cached.image)
      u.kind.value = sky.kind
      return Promise.resolve(true)
    }
    return new Promise((resolve) => {
      const done = (apply) => {
        if (this.key !== key) { resolve(false); return }   // another sky since
        apply()
        u.kind.value = sky.kind
        resolve(true)
      }
      if (sky.kind === SKY_KIND.CUBE) {
        // The message's order is the game's directions -- east west north
        // south up down -- and three.js's slots are +x -x +y -y +z -z of its
        // own frame, sampled with x flipped (as three's own backgrounds are):
        // so west, east, up, down, south, north.
        const [e, w, n, s, up, down] = sky.images
        new THREE.CubeTextureLoader().load([w, e, up, down, s, n].map(url),
          (t) => { this.cache.set(cacheKey, t); done(() => { u.cube.value = t }) },
          undefined, () => { sky.images.forEach(fail); resolve(false) })
        return
      }
      new THREE.TextureLoader().load(url(sky.images[0]), (t) => {
        t.wrapS = THREE.RepeatWrapping
        t.wrapT = THREE.ClampToEdgeWrapping
        // DOOM's sky is pixel art, drawn with its pixels; a photograph is not.
        const px = sky.kind === SKY_KIND.STRIP ? THREE.NearestFilter : THREE.LinearFilter
        t.magFilter = px
        t.minFilter = sky.kind === SKY_KIND.STRIP ? THREE.NearestFilter : THREE.LinearMipmapLinearFilter
        t.needsUpdate = true
        if (sky.kind === SKY_KIND.STRIP) this.#caps(t.image)
        this.cache.set(cacheKey, t)
        done(() => { u.pic.value = t })
      }, undefined, () => { fail(sky.images[0]); resolve(false) })
    })
  }

  // The average of a strip's top and bottom rows (eight each), which its
  // edges fade into. Read from the picture once; without a canvas (a test) the
  // caps stay black.
  #caps (img) {
    const u = this.uniforms
    try {
      const w = img?.width ?? 0
      const h = img?.height ?? 0
      if (!w || !h || typeof document === 'undefined') return
      const cv = document.createElement('canvas')
      cv.width = w
      cv.height = h
      const g = cv.getContext('2d', { willReadFrequently: true })
      g.drawImage(img, 0, 0)
      const avg = (y0) => {
        const d = g.getImageData(0, y0, w, Math.min(8, h)).data
        let r = 0; let gg = 0; let b = 0; let n = 0
        for (let i = 0; i < d.length; i += 4) { if (d[i + 3] < 128) continue; r += d[i]; gg += d[i + 1]; b += d[i + 2]; n++ }
        return n ? [r / n / 255, gg / n / 255, b / n / 255] : [0, 0, 0]
      }
      u.capTop.value.setRGB(...avg(0))
      u.capBottom.value.setRGB(...avg(Math.max(0, h - 8)))
    } catch { /* a picture the canvas will not read: the caps stay as they were */ }
  }
}
