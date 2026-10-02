//
// SECTOR EFFECTS: rain, snow, dust and embers, and volumetric fog
// (docs/SECTOR_FX.md).
//
// Authored in the map file, one `fx` line per effect inside a sector block, and
// read here from the loaded map (mapdata.js readMap -> `fx`). Rendering only:
// nothing in the simulation reads an effect.
//
// PARTICLES are instanced quads animated wholly on the GPU -- a hash of the
// instance picks where it starts, `time` moves it, and it wraps inside its
// sector's band -- so a downpour costs no JavaScript per frame.
//
// FOG is a volume per sector, visible from outside it. The sector's prism is
// split into one convex prism per baked triangle; each prism intersects its own
// view ray analytically, clips it to the scene's depth, and integrates the fog
// along it in closed form. Exactly ONE of its faces shades a given pixel -- the
// one where the ray leaves it -- whatever the triangle's winding, and whether or
// not the camera is inside. One prism's alpha is 1 - exp(-tau), and prisms of one fog
// composite to the exact total in any order: (1 - a1)(1 - a2) = exp(-(t1 + t2)).
//
// The math is exported and pure, and client/test/sectorfx.test.js holds the
// shader's own arithmetic to it.
//
import { toThreeX, toThreeY, toThreeZ } from '../mapgeom.js'

export const KIND = { RAIN: 1, SNOW: 2, DUST: 3, EMBERS: 4, FOG: 5 }
export const PARTICLE_KINDS = [KIND.RAIN, KIND.SNOW, KIND.DUST, KIND.EMBERS]

/** Particles per map, before an even scale-down across its effects. */
export const BUDGET = { desktop: 40000, touch: 12000 }
// How far wind may carry a particle from where it started before it wraps:
// a slow flake in a strong wind would otherwise drift through the next wall.
export const MAX_DRIFT = 1.5

// --- pure: geometry -------------------------------------------------------------

/** Area of a 2D triangle, unsigned. */
export function triArea (a, b, c) {
  return Math.abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / 2
}

/**
 * A sector's baked triangles, as corner triples and a cumulative area table:
 * what sampling picks from, one triangle per particle, by area.
 */
export function sectorTriangles (map, sectorIndex) {
  const s = map.sectors[sectorIndex]
  const out = []
  let total = 0
  const cum = []
  // `firstTri` is an offset into the INDEX list, `triCount` a count of
  // triangles -- mapgeom.js reads them the same way.
  for (let t = 0; t < s.triCount; t++) {
    const k = s.firstTri + t * 3
    const a = map.points[map.tris[k]]
    const b = map.points[map.tris[k + 1]]
    const c = map.points[map.tris[k + 2]]
    if (!a || !b || !c) continue
    total += triArea(a, b, c)
    out.push([a, b, c])
    cum.push(total)
  }
  return { tris: out, cum, area: total }
}

/**
 * A point in the sector, from three numbers in [0, 1): the first picks a
 * triangle by area, the other two a point in it (the square-root fold, which
 * is uniform over the triangle).
 */
export function samplePoint (st, u, v, w) {
  if (st.tris.length === 0) return null
  const target = u * st.area
  let lo = 0
  let hi = st.cum.length - 1
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (st.cum[mid] < target) lo = mid + 1
    else hi = mid
  }
  const [a, b, c] = st.tris[lo]
  const r = Math.sqrt(v)
  const ka = 1 - r
  const kb = r * (1 - w)
  const kc = r * w
  return { x: a.x * ka + b.x * kb + c.x * kc, y: a.y * ka + b.y * kb + c.y * kc }
}

/** Point in triangle, inclusive of its edges. */
export function inTriangle (p, a, b, c) {
  const d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y)
  const d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y)
  const d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y)
  const neg = d1 < 0 || d2 < 0 || d3 < 0
  const pos = d1 > 0 || d2 > 0 || d3 > 0
  return !(neg && pos)
}

/** The band an effect lives in, absolute heights. `top` < 0 is "the ceiling". */
export function bandOf (fx, sector) {
  const lo = sector.floorZ + Math.max(0, fx.params.bottom ?? 0)
  const topRel = fx.params.top
  const hi = topRel === undefined || topRel < 0 ? sector.ceilZ : Math.min(sector.ceilZ, sector.floorZ + topRel)
  return { lo, hi: Math.max(lo, hi) }
}

/**
 * How many particles each particle effect gets: density x area x band, scaled
 * down EVENLY when the map asks for more than the budget -- one heavy storm
 * must not starve a light snow elsewhere into nothing.
 */
export function particleCounts (wants, budget) {
  const total = wants.reduce((s, n) => s + n, 0)
  const k = total > budget ? budget / total : 1
  return wants.map((n) => Math.floor(n * k))
}

// --- pure: the fog integral (the shader's own arithmetic, in JS) ------------------

/**
 * The stretch of a ray inside one triangle's prism: [t0, t1], or null.
 * `o` and `d` in simulation space (x, y, z up); `d` need not be unit length,
 * t is in its units. The three edges are half-planes on the triangle's inner
 * side, whatever its winding; the band is two more.
 */
export function rayPrism (o, d, a, b, c, zlo, zhi) {
  let t0 = 0
  let t1 = Infinity
  const slab = (p0, dp, lo, hi) => {
    if (Math.abs(dp) < 1e-12) return p0 >= lo && p0 <= hi
    let ta = (lo - p0) / dp
    let tb = (hi - p0) / dp
    if (ta > tb) [ta, tb] = [tb, ta]
    t0 = Math.max(t0, ta)
    t1 = Math.min(t1, tb)
    return t0 <= t1
  }
  if (!slab(o.z, d.z, zlo, zhi)) return null
  const wind = Math.sign((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) || 1
  for (const [p, q] of [[a, b], [b, c], [c, a]]) {
    // Inward normal of edge p->q.
    const nx = -(q.y - p.y) * wind
    const ny = (q.x - p.x) * wind
    const s0 = nx * (o.x - p.x) + ny * (o.y - p.y)   // >= 0 inside
    const ds = nx * d.x + ny * d.y
    if (Math.abs(ds) < 1e-12) { if (s0 < 0) return null; continue }
    const t = -s0 / ds
    if (ds > 0) t0 = Math.max(t0, t)
    else t1 = Math.min(t1, t)
    if (t0 > t1) return null
  }
  return t0 <= t1 ? [t0, t1] : null
}

/**
 * Optical depth along o + t d for t in [t0, t1], `d` UNIT length: density
 * `D`, halving every `falloff` units above `zb` (0 is uniform).
 */
export function opticalDepth (o, d, t0, t1, D, zb, falloff) {
  if (!(t1 > t0)) return 0
  if (!(falloff > 0)) return D * (t1 - t0)
  const k = Math.LN2 / falloff
  const a = -k * d.z
  const base = D * Math.exp(-k * (o.z - zb))
  if (Math.abs(a) < 1e-9) return base * Math.exp(a * (t0 + t1) / 2) * (t1 - t0)
  return base * (Math.exp(a * t1) - Math.exp(a * t0)) / a
}

/** The fog one prism adds: 1 - exp(-tau), capped. */
export function fogAlpha (tau, cap = 1) {
  return Math.min(cap, 1 - Math.exp(-Math.max(0, tau)))
}

// --- the GPU half -------------------------------------------------------------

const PARTICLE_VS = /* glsl */`
attribute vec2 corner;          // the quad's corner, -1..1
attribute vec4 seed;            // base x, base y (sim), z phase, random
uniform float time;
uniform vec2 band;              // absolute z, lo and hi
uniform vec3 vel;               // sim space: wind x, wind y, vertical
uniform float sway;
uniform float size;
uniform float len;
uniform float maxDrift;
varying vec2 vUv;
varying float vFade;
void main () {
  float h = max(band.y - band.x, 1e-3);
  float vz = vel.z;
  float speed = max(abs(vz), 1e-3);
  float cycle = h / speed;
  // Where in its fall this one is: the phase, moved by time, wrapped.
  float tc = mod(seed.z * cycle + time, cycle);
  float z = vz < 0.0 ? band.y - tc * speed : band.x + tc * speed;
  // Wind across one fall, centred on where it started, bounded so it stays
  // near its sector.
  vec2 drift = vel.xy * (tc - cycle * 0.5);
  float dl = length(drift);
  if (dl > maxDrift) drift *= maxDrift / dl;
  float ph = seed.w * 6.2831853;
  vec2 wob = sway * vec2(sin(time * 1.3 + ph), cos(time * 0.9 + ph * 1.7));
  vec3 sim = vec3(seed.xy + drift + wob, z);
  // Map-local, then wherever the map is (a region places levels side by side).
  vec3 p = (modelMatrix * vec4(sim.x, sim.z, -sim.y, 1.0)).xyz;
  // Fade in and out at the band's ends, so the wrap never pops.
  float edge = min(z - band.x, band.y - z) / max(h * 0.08, 1e-3);
  vFade = clamp(edge, 0.0, 1.0);
  vUv = corner;
  vec3 camRight = vec3(viewMatrix[0][0], viewMatrix[1][0], viewMatrix[2][0]);
  vec3 camUp = vec3(viewMatrix[0][1], viewMatrix[1][1], viewMatrix[2][1]);
  vec3 world;
  if (len > 0.0) {
    // A STREAK along its own velocity, as wide as its size, turned to face us.
    vec3 v3 = normalize(vec3(vel.x, vel.z, -vel.y) + vec3(0.0, -1e-4, 0.0));
    vec3 toCam = normalize(cameraPosition - p);
    vec3 across = normalize(cross(v3, toCam));
    world = p + v3 * (corner.y * 0.5 * len) + across * (corner.x * 0.5 * size);
  } else {
    world = p + (camRight * corner.x + camUp * corner.y) * (0.5 * size);
  }
  gl_Position = projectionMatrix * viewMatrix * vec4(world, 1.0);
}
`

const PARTICLE_FS = /* glsl */`
uniform vec3 color;
uniform float alpha;
uniform float soft;             // 1 a soft round flake, 0 a hard streak
varying vec2 vUv;
varying float vFade;
void main () {
  float r = length(vUv);
  float a = soft > 0.5 ? smoothstep(1.0, 0.2, r) : (1.0 - abs(vUv.x));
  a *= alpha * vFade;
  if (a < 0.004) discard;
  gl_FragColor = vec4(color, a);
}
`

const FOG_VS = /* glsl */`
attribute vec4 ab;              // corners a and b, sim xy
attribute vec4 cz;              // corner c, sim xy; band lo, hi
varying vec3 vWorld;
varying vec4 vAB;
varying vec4 vCZ;
void main () {
  vAB = ab; vCZ = cz;
  vec4 w = modelMatrix * vec4(position, 1.0);
  vWorld = w.xyz;
  gl_Position = projectionMatrix * viewMatrix * w;
}
`

const FOG_FS = /* glsl */`
uniform sampler2D sceneDepth;
uniform vec2 resolution;
uniform mat4 projInv;
uniform vec3 color;
uniform float density;
uniform float cap;
uniform float zb;
uniform float falloff;
uniform float noise;
uniform float drift;
uniform float time;
uniform vec3 offset;            // where the map is placed, THREE space
varying vec3 vWorld;
varying vec4 vAB;
varying vec4 vCZ;

float hash (vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float vnoise (vec3 p) {
  vec3 i = floor(p); vec3 f = fract(p); f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(hash(i), hash(i + vec3(1,0,0)), f.x), mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
             mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x), mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y), f.z);
}

void main () {
  // The ray, in simulation space: x, y on the ground, z up.
  // The prisms' corners are map-local; so is the ray, once the map's place is
  // taken off the eye. A direction is the same in both.
  vec3 o3 = cameraPosition - offset;
  vec3 d3 = normalize(vWorld - cameraPosition);
  vec3 o = vec3(o3.x, -o3.z, o3.y);
  vec3 d = vec3(d3.x, -d3.z, d3.y);
  vec2 A = vAB.xy, B = vAB.zw, C = vCZ.xy;
  float t0 = 0.0, t1 = 1e9;
  // The band.
  if (abs(d.z) > 1e-6) {
    float ta = (vCZ.z - o.z) / d.z, tb = (vCZ.w - o.z) / d.z;
    t0 = max(t0, min(ta, tb)); t1 = min(t1, max(ta, tb));
  } else if (o.z < vCZ.z || o.z > vCZ.w) discard;
  // The three edges, on the triangle's inner side whatever its winding.
  float wind = sign((B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y));
  if (wind == 0.0) wind = 1.0;
  vec2 P[3]; P[0] = A; P[1] = B; P[2] = C;
  for (int i = 0; i < 3; i++) {
    vec2 p = P[i], q = P[(i + 1) % 3];
    vec2 n = vec2(-(q.y - p.y), q.x - p.x) * wind;
    float s0 = dot(n, o.xy - p);
    float ds = dot(n, d.xy);
    if (abs(ds) < 1e-9) { if (s0 < 0.0) discard; continue; }
    float t = -s0 / ds;
    if (ds > 0.0) t0 = max(t0, t); else t1 = min(t1, t);
  }
  // Stopped by whatever the scene has in front: its depth, back to a distance.
  vec2 uv = gl_FragCoord.xy / resolution;
  float depth = texture2D(sceneDepth, uv).r;
  vec4 v = projInv * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
  float scene = length(v.xyz / v.w);
  // ONE FACE PER PRISM: the one at the ray's exit. Both sides are drawn, so
  // the face where the ray enters discards itself here -- winding-proof, and
  // still right with the camera inside, where there is no entry face at all.
  float tf = length(vWorld - cameraPosition);
  if (t1 <= t0 || tf < (t0 + min(t1, 1e8)) * 0.5) discard;
  t1 = min(t1, scene);
  if (t1 <= t0) discard;
  float tau;
  if (falloff > 0.0) {
    float k = 0.69314718 / falloff;
    float a = -k * d.z;
    float base = density * exp(-k * (o.z - zb));
    tau = abs(a) < 1e-6 ? base * exp(a * (t0 + t1) * 0.5) * (t1 - t0)
                        : base * (exp(a * t1) - exp(a * t0)) / a;
  } else {
    tau = density * (t1 - t0);
  }
  if (noise > 0.0) {
    vec3 mid = o + d * ((t0 + t1) * 0.5);
    float n = vnoise(mid * 0.45 + vec3(drift * time, drift * time * 0.6, drift * time * 0.3));
    tau *= max(0.0, 1.0 + noise * (2.0 * n - 1.0));
  }
  float a = min(cap, 1.0 - exp(-tau));
  if (a < 0.002) discard;
  gl_FragColor = vec4(color, a);
}
`

/**
 * Everything one map's effects draw: particle meshes into `scene`, fog prisms
 * into a scene of their own (drawn after the main pass, over the depth).
 *
 * An effect is lit by its sector's light level unless it glows. `touch` picks
 * the smaller particle budget.
 */
export class SectorFx {
  constructor ({ THREE, map, touch = false, budget = null, rng = mulberry(0x5eed) }) {
    this.THREE = THREE
    this.root = new THREE.Group()
    this.root.name = 'sectorfx'
    this.fogScene = new THREE.Scene()
    this.fogMaterials = []
    this.particleMaterials = []
    this.meshes = []
    this.time = { value: 0 }
    this.counts = []
    const fx = map.fx ?? []
    const particles = fx.filter((e) => PARTICLE_KINDS.includes(e.kind))
    const fogs = fx.filter((e) => e.kind === KIND.FOG)
    this.fogCount = fogs.length

    // PARTICLES: how many each wants, then the budget, then one mesh each.
    const shapes = new Map()
    const shapeOf = (s) => { if (!shapes.has(s)) shapes.set(s, sectorTriangles(map, s)); return shapes.get(s) }
    const wants = particles.map((e) => {
      const s = map.sectors[e.sector]
      const band = bandOf(e, s)
      return Math.max(0, Math.ceil((e.params.density ?? 0) * shapeOf(e.sector).area * (band.hi - band.lo)))
    })
    this.counts = particleCounts(wants, budget ?? (touch ? BUDGET.touch : BUDGET.desktop))
    particles.forEach((e, i) => {
      const n = this.counts[i]
      if (n <= 0) return
      const mesh = this.#particles(THREE, map, e, shapeOf(e.sector), n, rng)
      if (mesh) { this.root.add(mesh); this.meshes.push(mesh) }
    })

    // FOG: one geometry of prisms per effect.
    for (const e of fogs) {
      const mesh = this.#fog(THREE, map, e, shapeOf(e.sector))
      if (mesh) { this.fogScene.add(mesh); this.meshes.push(mesh) }
    }
    this.depthTarget = null
  }

  #particles (THREE, map, e, st, n, rng) {
    if (st.tris.length === 0) return null
    const s = map.sectors[e.sector]
    const band = bandOf(e, s)
    const p = e.params
    const quad = new THREE.InstancedBufferGeometry()
    quad.setAttribute('corner', new THREE.Float32BufferAttribute([-1, -1, 1, -1, 1, 1, -1, 1], 2))
    quad.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(12), 3))
    quad.setIndex([0, 1, 2, 0, 2, 3])
    const seeds = new Float32Array(n * 4)
    for (let i = 0; i < n; i++) {
      const pt = samplePoint(st, rng(), rng(), rng())
      seeds[i * 4] = pt.x
      seeds[i * 4 + 1] = pt.y
      seeds[i * 4 + 2] = rng()
      seeds[i * 4 + 3] = rng()
    }
    quad.setAttribute('seed', new THREE.InstancedBufferAttribute(seeds, 4))
    quad.instanceCount = n
    const glow = (p.glow ?? 0) > 0.5
    const lit = glow ? 1 : (s.light ?? 255) / 255
    // RAW display values, as the map's own shaders write: no colour-space
    // conversion either way, so a colour in the map file is the colour drawn.
    const col = new THREE.Color().setHex(p.color ?? 0xffffff, THREE.LinearSRGBColorSpace)
    col.multiplyScalar(lit)
    const up = e.kind === KIND.EMBERS
    const mat = new THREE.ShaderMaterial({
      uniforms: {
        time: this.time,
        band: { value: new THREE.Vector2(band.lo, band.hi) },
        vel: { value: new THREE.Vector3(p.windx ?? 0, p.windy ?? 0, up ? (p.speed ?? 0) : -(p.speed ?? 0)) },
        sway: { value: p.sway ?? 0 },
        size: { value: p.size ?? 0.03 },
        len: { value: p.length ?? 0 },
        maxDrift: { value: MAX_DRIFT },
        color: { value: col },
        alpha: { value: p.alpha ?? 1 },
        soft: { value: (p.length ?? 0) > 0 ? 0 : 1 }
      },
      vertexShader: PARTICLE_VS,
      fragmentShader: PARTICLE_FS,
      transparent: true,
      depthWrite: false,
      blending: glow ? THREE.AdditiveBlending : THREE.NormalBlending
    })
    this.particleMaterials.push(mat)
    const mesh = new THREE.Mesh(quad, mat)
    mesh.frustumCulled = false
    mesh.name = `fx:${e.kind}:${e.sector}`
    return mesh
  }

  #fog (THREE, map, e, st) {
    if (st.tris.length === 0) return null
    const s = map.sectors[e.sector]
    const band = bandOf(e, s)
    const p = e.params
    const pos = []
    const ab = []
    const cz = []
    const idx = []
    for (const [a, b, c] of st.tris) {
      const base = pos.length / 3
      // Six corners: the triangle at the band's foot and its head, in THREE space.
      for (const z of [band.lo, band.hi]) {
        for (const q of [a, b, c]) {
          pos.push(toThreeX(q.x), toThreeY(z), toThreeZ(q.y))
          ab.push(a.x, a.y, b.x, b.y)
          cz.push(c.x, c.y, band.lo, band.hi)
        }
      }
      // Both caps and three sides. Drawn double-sided; the shader keeps only
      // the face where the ray leaves, so their winding does not matter.
      const faces = [[0, 1, 2], [3, 4, 5], [0, 1, 4], [0, 4, 3], [1, 2, 5], [1, 5, 4], [2, 0, 3], [2, 3, 5]]
      for (const [i, j, k] of faces) idx.push(base + i, base + j, base + k)
    }
    const geo = new THREE.BufferGeometry()
    geo.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3))
    geo.setAttribute('ab', new THREE.Float32BufferAttribute(ab, 4))
    geo.setAttribute('cz', new THREE.Float32BufferAttribute(cz, 4))
    geo.setIndex(idx)
    const glow = Math.max(0, Math.min(1, p.glow ?? 0))
    const lit = glow + (1 - glow) * ((s.light ?? 255) / 255)
    const col = new THREE.Color().setHex(p.color ?? 0x8090a0, THREE.LinearSRGBColorSpace)
    col.multiplyScalar(lit)
    const mat = new THREE.ShaderMaterial({
      uniforms: {
        sceneDepth: { value: null },
        resolution: { value: new THREE.Vector2(1, 1) },
        projInv: { value: new THREE.Matrix4() },
        color: { value: col },
        density: { value: p.density ?? 0.15 },
        cap: { value: p.alpha ?? 1 },
        zb: { value: band.lo },
        falloff: { value: p.falloff ?? 0 },
        noise: { value: p.noise ?? 0 },
        drift: { value: p.drift ?? 0.3 },
        time: this.time,
        offset: { value: new THREE.Vector3() }
      },
      vertexShader: FOG_VS,
      fragmentShader: FOG_FS,
      transparent: true,
      depthWrite: false,
      depthTest: false,
      // Both sides, so the exit face is drawn however it is wound; the shader
      // discards the entry face, so no prism counts twice.
      side: THREE.DoubleSide
    })
    this.fogMaterials.push(mat)
    const mesh = new THREE.Mesh(geo, mat)
    mesh.frustumCulled = false
    mesh.name = `fog:${e.sector}`
    return mesh
  }

  /** Where the map is drawn, THREE space: its view's root position. */
  setOrigin (x, y, z) {
    this.root.position.set(x, y, z)
    for (const m of this.fogScene.children) m.position.set(x, y, z)
    for (const m of this.fogMaterials) m.uniforms.offset.value.set(x, y, z)
  }

  /** Advance the animation. One uniform, shared by every effect. */
  update (seconds) { this.time.value = seconds }

  get hasFog () { return this.fogCount > 0 }

  /**
   * The fog pass, after the main render: the scene's depth into a target --
   * the same scene, colour writes masked, so sprites keep their cut-outs -- then
   * the prisms over the picture, reading it. A map with no fog never gets here.
   *
   * Why not render the scene into a target once and copy it to the screen: the
   * map's shaders write display values and three's own materials are colour
   * managed, and only the screen treats both the same way. A depth-only pass
   * leaves the picture exactly what it was.
   */
  renderFog (renderer, scene, camera) {
    if (!this.hasFog) return
    const THREE = this.THREE
    const size = renderer.getDrawingBufferSize(new THREE.Vector2())
    if (!this.depthTarget || this.depthTarget.width !== size.x || this.depthTarget.height !== size.y) {
      this.depthTarget?.dispose()
      const dt = new THREE.DepthTexture(size.x, size.y)
      dt.type = THREE.UnsignedIntType
      this.depthTarget = new THREE.WebGLRenderTarget(size.x, size.y, { depthTexture: dt, depthBuffer: true })
    }
    const prevTarget = renderer.getRenderTarget()
    const prevAuto = renderer.autoClear
    // The frame's draw counts go on counting through these passes rather than
    // being reset by them, so the HUD reports the whole frame's cost.
    const prevInfo = renderer.info.autoReset
    renderer.info.autoReset = false
    const color = renderer.state.buffers.color
    renderer.setRenderTarget(this.depthTarget)
    renderer.clear(true, true, false)
    color.setMask(false)
    color.setLocked(true)
    renderer.render(scene, camera)
    color.setLocked(false)
    color.setMask(true)
    renderer.setRenderTarget(prevTarget)
    for (const m of this.fogMaterials) {
      m.uniforms.sceneDepth.value = this.depthTarget.depthTexture
      m.uniforms.resolution.value.set(size.x, size.y)
      m.uniforms.projInv.value.copy(camera.projectionMatrixInverse)
    }
    renderer.autoClear = false
    renderer.render(this.fogScene, camera)
    renderer.autoClear = prevAuto
    renderer.info.autoReset = prevInfo
  }

  dispose () {
    for (const m of this.meshes) { m.geometry.dispose(); m.material.dispose() }
    this.depthTarget?.dispose()
    this.root.removeFromParent()
    this.meshes = []
  }
}

/** A small seeded generator, so a map's particles land the same every load. */
export function mulberry (seed) {
  let a = seed >>> 0
  return () => {
    a = (a + 0x6D2B79F5) >>> 0
    let t = a
    t = Math.imul(t ^ (t >>> 15), t | 1)
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}
