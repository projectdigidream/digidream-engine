//
// Transient visuals: tracers, and whatever comes after them.
//
// A tracer is NOT an entity. It has no identity, nothing refers to it, and a
// client that missed one has missed nothing it needs to stay consistent -- so
// it arrives as a fire-and-forget event on the snapshot (docs/protocol.md §9.2)
// rather than as a thing that is spawned and destroyed sixty times a second to
// draw a line lasting a tenth of a second.
//
// WHAT A KIND LOOKS LIKE is in assets/props.txt, alongside the props, and in
// neither binary:
//
//   vfx <kind> tracer [colour <hex>] [width <w>] [life <seconds>]
//   vfx <kind> particles [count <n>] [life <s>] [life_var <s>] [speed <u/s>]
//                        [speed_var <u/s>] [cone <turns>] [gravity <u/s/s>]
//                        [drag <per second>] [size <u>] [size_end <u>]
//                        [colour <hex>] [colour_end <hex>] [blend additive|alpha]
//                        [shape soft|disc|square|hexagon|star] [sprite <name>]
//                        [carry 0|1]
//
// WHAT A PARTICLE IS DRAWN AS: `shape` -- `soft`, the soft round dot every
// particle was before there was a choice, and the default, so a line that
// says nothing draws exactly as it did; or a hard-edged disc, square, hexagon
// or five-pointed star, made here in code -- or `sprite`, one of the module's
// own pictures, found as the HUD's image element finds one (/sprites/<name>.png,
// then /textures/<name>.png). The sprite wins when a line has both, and the
// shape is what draws while the picture loads, or if it is not there. Either is
// tinted by the colour ramp, and its own alpha cuts its outline under either
// blend. Neither applies to a tracer.
//
// A TRAIL is a kind kept coming out of a body for as long as it has one -- a
// missile's `trail` field, any entity's e:set_trail -- at the kind's own `rate`
// (VfxView.follow). `carry 1` makes the specks move with the body that let them
// out, a flame around a fireball, rather than being left behind where they
// came out, a trail of smoke; it applies to any emitter on a body.
//
// The engine attaches no meaning to the number at all. It reports "effect 1
// happened here, pointing that way, with this parameter"; the file says what a
// 1 is, and the parameter is its length -- or, for particles, the radius of the
// volume they come out of.
//
// PARTICLES ARE ENTIRELY THE CLIENT'S, and that is the whole design. The server
// says an effect happened somewhere; it does not simulate a single particle,
// none of them cross the wire, and none of them is in any hash. So a burst
// costs one event however many specks it draws, two clients may show slightly
// different sparks, and neither is wrong -- the same standing this engine gives
// the animation cross-fade.
//
// WHICH ALSO MEANS THERE IS NOTHING NEW TO ATTACH THEM TO. A hit, a rocket
// bursting, a sword swinging and a monster reaching frame 4 of its attack all
// already call `dnp.vfx`: the mob and weapon vocabularies have a `vfx` action,
// missiles have `on_hit`, and hits run through mobs.apply_hit. A particle kind
// is a number you put in one of those.
//
// ONE-SHOT BURSTS, not looping emitters. A vfx event has no end and nothing
// refers to it, so there is nothing to stop -- a torch that smokes forever is a
// thing attached to an ENTITY with a lifetime, which is a different feature and
// is not this one.
//

/**
 * How long a trail goes on after the last frame that asked for it (follow):
 * long enough to ride out a slow frame, short enough that a missile that has
 * burst lets out nothing more anybody sees.
 */
export const FOLLOW_GRACE = 0.06

/** What a particle can be drawn as, `soft` first: the default. */
export const PARTICLE_SHAPES = ['soft', 'disc', 'square', 'hexagon', 'star']

// A picture's name: what the HUD's image element takes, a frame key or a path
// under /sprites/ (`fx/spark`) -- never out of it.
const SPRITE_NAME = /^(?!.*\.\.)[A-Za-z0-9_][A-Za-z0-9_./-]*$/

/** Read the `vfx` records out of the catalogue text. */
export function parseVfx (text) {
  const kinds = new Map()
  const warnings = []
  text.split('\n').forEach((raw, i) => {
    const line = raw.split('#')[0].trim()
    if (!line) return
    const tok = line.split(/\s+/)
    if (tok[0] !== 'vfx') return
    const id = Number(tok[1])
    if (!Number.isInteger(id) || id <= 0) {
      warnings.push(`line ${i + 1}: '${tok[1]}' is not a vfx kind`)
      return
    }
    const type = tok[2] ?? 'tracer'
    const d = type === 'particles'
      ? {
          id,
          type,
          // Unity's ParticleSystem in the parts that earn their keep here: a
          // burst count, a lifetime with spread, a speed with spread, a cone to
          // throw them into, gravity and drag, and size and colour ramps.
          // `cone` is in TURNS like every other angle in this engine: 0 is a
          // beam straight along the event's direction, 0.25 a hemisphere, 0.5 a
          // full sphere.
          count: 16,
          life: 0.5,
          lifeVar: 0.25,
          speed: 6,
          speedVar: 3,
          cone: 0.25,
          gravity: 18,
          drag: 1.5,
          size: 0.1,
          sizeEnd: 0,
          colour: 0xffaa33,
          colourEnd: 0x662200,
          // ADDITIVE BY DEFAULT, and it is not only a look: under additive
          // blending black IS invisible, so the per-particle colour ramp doubles
          // as the fade and one vertex attribute does both. Sparks, muzzle
          // flashes and blood all want it. `alpha` is for smoke, where fading
          // to black would be soot rather than absence -- there the whole burst
          // fades together on the material, because per-point opacity needs a
          // custom shader and a shader is a lot to carry for smoke.
          blend: 'additive',
          // ATTACHED EMITTERS. `attach 1` makes the event's `param` an ENTITY
          // INDEX rather than an emission radius, and turns the burst into an
          // emitter that follows that entity: new particles come out of where
          // it is NOW, not where it was when the event was sent.
          //
          // `rate` is particles a second and `linger` is how long it keeps
          // going after the last event. There is no stop message and there does
          // not need to be: a weapon that repeats, like a chainsaw's `cut`,
          // re-sends its event every few ticks, and each one pushes the
          // deadline out. Let go of the trigger and it lapses.
          attach: 0,
          rate: 40,
          linger: 0.2,
          // The specks move with the body they came out of (see the top).
          carry: 0,
          // What each particle is drawn as (see the top of this file).
          shape: 'soft',
          sprite: null
        }
      : { id, type, colour: 0xffdd55, width: 0.04, life: 0.12 }
    const NUM = {
      width: 'width', life: 'life', life_var: 'lifeVar', count: 'count',
      speed: 'speed', speed_var: 'speedVar', cone: 'cone', gravity: 'gravity',
      drag: 'drag', size: 'size', size_end: 'sizeEnd',
      attach: 'attach', rate: 'rate', linger: 'linger', carry: 'carry'
    }
    for (let k = 3; k < tok.length; k += 2) {
      const key = tok[k]
      const v = tok[k + 1]
      if (v === undefined) { warnings.push(`line ${i + 1}: '${key}' has no value`); break }
      if (key === 'colour' || key === 'color') d.colour = parseInt(v.replace(/^#|^0x/, ''), 16)
      else if (key === 'colour_end' || key === 'color_end') d.colourEnd = parseInt(v.replace(/^#|^0x/, ''), 16)
      else if (key === 'blend') d.blend = v
      else if (key === 'shape' || key === 'sprite') {
        if (type !== 'particles') warnings.push(`line ${i + 1}: '${key}' is for particles; a tracer is a line`)
        else if (key === 'shape' && !PARTICLE_SHAPES.includes(v)) {
          warnings.push(`line ${i + 1}: shape '${v}' is not one of ${PARTICLE_SHAPES.join(', ')}`)
        } else if (key === 'sprite' && !SPRITE_NAME.test(v)) {
          warnings.push(`line ${i + 1}: sprite '${v}' is not a picture's name`)
        } else d[key] = v
      } else if (NUM[key] !== undefined) d[NUM[key]] = Number(v)
      else warnings.push(`line ${i + 1}: unknown vfx attribute '${key}'`)
    }
    kinds.set(id, d)
  })
  return { kinds, warnings }
}

/**
 * The two ends of a tracer, in world coordinates.
 *
 * The event carries where the shot STARTED, which way it went, and how long it
 * was -- not where it ended. A client given only a direction would draw a line
 * to infinity through whatever the shot hit, and one given only the far end
 * would not know where it came from.
 *
 * `param` carries TWO numbers, both in 1/256 world units -- a hundredth of a
 * Doom unit at this scale -- packed into the one u32 the event has: the
 * horizontal length in the low sixteen bits, and the RISE over that length in
 * the high sixteen, offset by half its range so it can be negative.
 *
 * The rise is there because the event has one angle and it is a heading, and a
 * shot in this engine can now slope: there is no vertical aim input, so every
 * bullet fired at something on a step or in a pit is auto-aimed upwards or
 * down. Drawn from the heading alone the tracer would be flat while the damage
 * landed on the balcony, which is the failure this repository keeps finding --
 * the shot is right and the picture says it is not.
 *
 * A rise rather than a pitch angle, because a pitch needs a tangent to become
 * an endpoint and the tangent is unbounded looking straight down.
 *
 * `dir` is a brad -- a turn in 2^32 -- so dividing by 2^32 gives turns directly
 * and the conversion cannot pick up a stray factor of pi.
 *
 * Extracted from main.js so it can be tested at all. main.js needs a DOM, a
 * WebGL context and a live session, so every line in it is a line no suite
 * runs -- and this particular calculation has a bad history: the sprite
 * billboard and the swing collider both shipped mirrored, both because an angle
 * convention was reasoned about instead of checked.
 */
export function tracerEnds (v, fxToFloat) {
  const p = (v.param >>> 0)
  const len = (p & 0xffff) / 256
  const rise = ((p >>> 16) - 32768) / 256
  const turns = (v.dir >>> 0) / 4294967296
  const a = turns * Math.PI * 2
  const x = fxToFloat(v.pos.x)
  const y = fxToFloat(v.pos.y)
  const z = fxToFloat(v.pos.z)
  return {
    from: { x, y, z },
    to: { x: x + Math.cos(a) * len, y: y + Math.sin(a) * len, z: z + rise },
    length: len
  }
}

/**
 * How visible an effect is, `age` seconds in.
 *
 * Linear, and 0 once it has outlived its life. A pure function so the decay can
 * be asserted without a GPU -- and because "it fades" is the one thing about a
 * tracer that is easy to get subtly wrong: a shot that never quite disappears
 * leaves the screen full of them, and one that vanishes on the frame it arrives
 * is a shot nobody sees.
 */
export function fade (age, life) {
  if (!(life > 0)) return 0
  // NOT YET BORN IS FULLY VISIBLE, not gone.
  //
  // This returned 0 here, and 0 is what update() retires on -- so a tracer with
  // a negative age was destroyed on the spot. Which sounds impossible until you
  // notice the two clocks: frame(now) receives the timestamp of when the frame
  // STARTED, spawns happen later inside that frame from performance.now(), and
  // update() then ran with the earlier stamp. Every tracer was created and
  // retired within one frame, forever -- seen 25, live 0, pool 48, and nothing
  // ever on screen.
  //
  // The clocks are one clock now (see the caller), so this should not arise.
  // It stays because conflating "before it existed" with "after it died" is
  // wrong on its own terms, and because that conflation cost four rounds of
  // looking in the wrong place.
  if (age < 0) return 1
  const f = 1 - age / life
  return f > 0 ? f : 0
}

/**
 * One burst's worth of particles, ready to integrate.
 *
 * PURE, and takes its own random source, so a test can hand it a fixed one and
 * assert the cone actually constrains the directions. `rand` returns [0, 1).
 *
 * `dirTurns` is the event's heading and `radius` the volume they come out of --
 * the event's `param`, in 1/256 units, so an explosion can fill its own blast
 * radius rather than all of it starting at one point. Zero is a point source.
 *
 * Emitted into a CONE about the heading, in three space, because that is where
 * they will be drawn and converting per particle per frame would be the same
 * arithmetic sixty times a second. `cone` of 0.5 turns is the whole sphere, and
 * the polar angle is distributed by acos so the directions are even on it
 * rather than bunched at the pole -- the naive version puts most of an
 * explosion in a ring, which reads as a smoke signal.
 */
export function emitBurst (def, at, dirTurns, radius = 0, rand = Math.random) {
  const out = []
  const n = Math.max(0, Math.floor(def.count ?? 0))
  const yaw = dirTurns * Math.PI * 2
  const maxPolar = Math.min(1, Math.max(0, (def.cone ?? 0.25) * 2)) * Math.PI
  for (let i = 0; i < n; i++) {
    // Around the heading: polar from the axis, azimuth around it.
    const polar = Math.acos(1 - rand() * (1 - Math.cos(maxPolar)))
    const az = rand() * Math.PI * 2
    // The cone's axis is the event's heading, laid flat: engine (x, y) maps to
    // three (x, -y), so a heading of `yaw` is (cos yaw, 0, -sin yaw).
    const ax = Math.cos(yaw); const az0 = -Math.sin(yaw)
    // A frame about that axis. The axis is horizontal by construction, so world
    // up is never parallel to it and the cross product is always defined.
    const ux = 0; const uy = 1; const uz = 0
    // right = axis x up
    const rx = 0 * az0 - 0 * uy
    const ry = az0 * ux - ax * uz
    const rz = ax * uy - 0 * ux
    const rl = Math.hypot(rx, ry, rz) || 1
    const bx = rx / rl; const by = ry / rl; const bz = rz / rl
    // up2 = right x axis
    const cx = by * az0 - bz * 0
    const cy = bz * ax - bx * az0
    const cz = bx * 0 - by * ax
    const sp = Math.sin(polar); const cp = Math.cos(polar)
    const ca = Math.cos(az); const sa = Math.sin(az)
    const dx = ax * cp + (bx * ca + cx * sa) * sp
    const dy = 0 * cp + (by * ca + cy * sa) * sp
    const dz = az0 * cp + (bz * ca + cz * sa) * sp
    const speed = Math.max(0, (def.speed ?? 0) + ((rand() * 2 - 1) * (def.speedVar ?? 0)))
    const life = Math.max(0.01, (def.life ?? 0.5) + ((rand() * 2 - 1) * (def.lifeVar ?? 0)))
    // Scattered through the emission volume, not onto its shell: cube-rooting a
    // uniform draw is what makes a blast look filled instead of hollow.
    const r = radius > 0 ? radius * Math.cbrt(rand()) : 0
    out.push({
      x: at.x + dx * r, y: at.y + dy * r, z: at.z + dz * r,
      vx: dx * speed, vy: dy * speed, vz: dz * speed,
      age: 0, life
    })
  }
  return out
}

/**
 * Advance a burst by `dt` seconds, dropping what has expired.
 *
 * Gravity is -Y because three's up is Y, and this engine's z maps to it.
 * Drag is exponential rather than linear so a large `dt` -- a dropped frame, a
 * tab regaining focus -- cannot push a speed through zero and send a spark
 * backwards, which a naive `v -= v * drag * dt` does the moment drag*dt > 1.
 */
export function stepParticles (parts, dt, def) {
  if (!(dt > 0)) return parts
  const keep = Math.exp(-(def.drag ?? 0) * dt)
  const g = (def.gravity ?? 0) * dt
  for (let i = parts.length - 1; i >= 0; i--) {
    const p = parts[i]
    p.age += dt
    if (p.age >= p.life) { parts.splice(i, 1); continue }
    p.vx *= keep; p.vy *= keep; p.vz *= keep
    p.vy -= g
    p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt
  }
  return parts
}

/** Lerp two packed 0xRRGGBB colours, component-wise. */
export function rampColour (from, to, t) {
  const f = t < 0 ? 0 : t > 1 ? 1 : t
  const r = ((from >> 16) & 255) + (((to >> 16) & 255) - ((from >> 16) & 255)) * f
  const g = ((from >> 8) & 255) + (((to >> 8) & 255) - ((from >> 8) & 255)) * f
  const b = (from & 255) + ((to & 255) - (from & 255)) * f
  return { r: r / 255, g: g / 255, b: b / 255 }
}

/**
 * Draws transient effects and lets them expire.
 *
 * A small pool of reusable meshes rather than one allocation per shot: a
 * firefight is a few dozen tracers a second, and building a geometry for each
 * would make the garbage collector part of the frame budget.
 */
/**
 * A SOFT ROUND SPRITE, the alpha every particle is drawn through: 1 in the
 * middle, falling smoothly to 0 at the rim. Without it a point is a hard
 * square, invisible on a blood speck and a grey tile on a puff of smoke.
 * Pure: the RGBA bytes, `n` pixels square.
 */
export function softDisc (n = 32) {
  const px = new Uint8Array(n * n * 4)
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      const dx = (x + 0.5) / n * 2 - 1
      const dy = (y + 0.5) / n * 2 - 1
      const r = Math.min(1, Math.hypot(dx, dy))
      const a = (1 - r * r) * (1 - r * r)        // smooth to the rim
      const k = (y * n + x) * 4
      px[k] = 255; px[k + 1] = 255; px[k + 2] = 255; px[k + 3] = Math.round(255 * a)
    }
  }
  return px
}

// Whether a point (x, y), each -1..1 with y up, is inside a hard shape. The
// star is five-pointed, one point up, its inner corners at 0.38 of its reach --
// a regular pentagram's -- tested as a ten-sided polygon.
const STAR = Array.from({ length: 10 }, (_, k) => {
  const a = Math.PI / 2 + k * Math.PI / 5
  const r = k % 2 === 0 ? 0.95 : 0.95 * 0.382
  return [r * Math.cos(a), r * Math.sin(a)]
})
const INSIDE = {
  disc: (x, y) => x * x + y * y <= 0.92 * 0.92,
  square: (x, y) => Math.abs(x) <= 0.8 && Math.abs(y) <= 0.8,
  // Flat-topped, corners at 0.92.
  hexagon: (x, y) => Math.abs(y) <= 0.92 * Math.sqrt(3) / 2 &&
                     Math.sqrt(3) * Math.abs(x) + Math.abs(y) <= 0.92 * Math.sqrt(3),
  star: (x, y) => {
    let inside = false
    for (let i = 0, j = STAR.length - 1; i < STAR.length; j = i++) {
      const [xi, yi] = STAR[i]
      const [xj, yj] = STAR[j]
      if ((yi > y) !== (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside
    }
    return inside
  }
}

/**
 * A PARTICLE'S SHAPE, as RGBA bytes `n` pixels square: `soft` is softDisc, the
 * rest hard-edged -- opaque inside, clear outside, the rim anti-aliased by four
 * by four samples a pixel so a hard shape is crisp rather than jagged. White, so
 * the colour ramp is the colour. Rows run bottom to top, as a DataTexture's do,
 * so the star points up on screen. Pure; an unknown shape is `soft`.
 */
export function particleShape (shape, n = 32) {
  const inside = INSIDE[shape]
  if (!inside) return softDisc(n)
  const px = new Uint8Array(n * n * 4)
  const S = 4
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      let hit = 0
      for (let sy = 0; sy < S; sy++) {
        for (let sx = 0; sx < S; sx++) {
          if (inside((x + (sx + 0.5) / S) / n * 2 - 1, (y + (sy + 0.5) / S) / n * 2 - 1)) hit++
        }
      }
      const k = (y * n + x) * 4
      px[k] = 255; px[k + 1] = 255; px[k + 2] = 255; px[k + 3] = Math.round(255 * hit / (S * S))
    }
  }
  return px
}

/**
 * Where a module's picture is looked for: as the HUD's image element looks, a
 * sprite of that name and then a texture of it. A name that already ends in
 * .png or .webp is that file.
 */
export const defaultImageUrls = (name) => {
  const file = /\.(png|webp)$/i.test(name) ? name : `${name}.png`
  return [`/sprites/${file}`, `/textures/${file}`]
}

export class VfxView {
  /**
   * `resolveImage(name)` says where a `sprite` picture is: a URL, a list to try
   * in order, or null for none. The default is defaultImageUrls; the studio,
   * whose pictures are blob URLs, passes its own. `loader` is a TextureLoader
   * (made from THREE when absent).
   */
  constructor ({ THREE, scene, catalogue, poolSize = 48, resolveImage = defaultImageUrls, loader = null }) {
    this.THREE = THREE
    // One texture per shape, made the first time a kind asks for it, the soft
    // one at once. Optional: a stub THREE without textures (a test) draws
    // square points, as before.
    this.disc = null
    this.shapes = new Map()
    if (THREE.DataTexture) {
      this.disc = this.#shapeTexture('soft')
    }
    this.resolveImage = resolveImage
    this.loader = loader ?? (THREE.TextureLoader ? new THREE.TextureLoader() : null)
    // Each sprite picture once, by name: { texture, state: loading|ready|missing }.
    this.pictures = new Map()
    this.scene = scene
    this.kinds = catalogue?.kinds ?? new Map()
    this.live = []          // { mesh, born, life }
    this.pool = []
    this.warned = new Set()
    // Bursts are not pooled the way tracers are: each carries its own buffer
    // sized to its own count, and a pool of those is a pool of sizes. They are
    // few and short -- a cap is what keeps a firefight from allocating without
    // bound, and dropping one during a firefight is invisible.
    this.parts = []
    this.maxBursts = 24
    this.lastNow = null
    // Attached emitters, keyed by kind AND entity so one chainsaw does not
    // cancel another's. Re-triggering refreshes the deadline rather than
    // stacking a second emitter on the same body.
    this.emitters = new Map()

    // The pictures the catalogue names, asked for now, so the first burst of a
    // kind is usually drawn with its own.
    for (const def of this.kinds.values()) if (def.sprite) this.#picture(def.sprite)

    for (let i = 0; i < poolSize; i++) {
      // A unit-length box along +X, scaled per shot. A line primitive would be
      // one pixel wide whatever the distance, which reads as a scratch on the
      // screen rather than as something in the world.
      const geo = new THREE.BoxGeometry(1, 1, 1)
      geo.translate(0.5, 0, 0)      // origin at the muzzle end, so scale.x is length
      const mat = new THREE.MeshBasicMaterial({
        color: 0xffffff, transparent: true, opacity: 1, depthWrite: false
      })
      const mesh = new THREE.Mesh(geo, mat)
      mesh.visible = false
      scene.add(mesh)
      this.pool.push(mesh)
    }
  }

  #shapeTexture (shape) {
    const THREE = this.THREE
    if (!THREE.DataTexture) return null
    const key = INSIDE[shape] ? shape : 'soft'
    let t = this.shapes.get(key)
    if (!t) {
      t = new THREE.DataTexture(particleShape(key, 32), 32, 32, THREE.RGBAFormat)
      t.magFilter = THREE.LinearFilter
      t.minFilter = THREE.LinearFilter
      t.needsUpdate = true
      this.shapes.set(key, t)
    }
    return t
  }

  // A module's picture by name, loaded once: the texture when it has arrived,
  // null while it is on its way or when it is nowhere. Each URL resolveImage
  // gives is tried in order, as the HUD tries /sprites/ then /textures/.
  #picture (name) {
    let p = this.pictures.get(name)
    if (!p) {
      p = { texture: null, state: 'loading' }
      this.pictures.set(name, p)
      const got = this.resolveImage ? this.resolveImage(name) : null
      const urls = got == null ? [] : [].concat(got)
      const THREE = this.THREE
      const next = (i) => {
        if (i >= urls.length || !this.loader) {
          p.state = 'missing'
          console.warn(`[vfx] no picture '${name}' for a particle; drawing its shape`)
          return
        }
        this.loader.load(urls[i], (tex) => {
          // As the engine's other pictures are: no colour conversion, and
          // nearest up close so pixel art stays pixel art.
          if (THREE.NoColorSpace !== undefined) tex.colorSpace = THREE.NoColorSpace
          if (THREE.NearestFilter !== undefined) tex.magFilter = THREE.NearestFilter
          p.texture = tex
          p.state = 'ready'
          this.#pictureArrived(name, tex)
        }, undefined, () => next(i + 1))
      }
      next(0)
    }
    return p.state === 'ready' ? p.texture : null
  }

  // What a kind's particles are drawn through: its sprite once that has
  // arrived, else its shape.
  #mapFor (def) {
    if (def.sprite) {
      const t = this.#picture(def.sprite)
      if (t) return t
    }
    return this.#shapeTexture(def.shape ?? 'soft')
  }

  // A picture that arrived after bursts of its kind began: they wear it from now.
  #pictureArrived (name, tex) {
    for (const b of [...this.parts, ...this.emitters.values()]) {
      if (b.def.sprite !== name) continue
      b.points.material.map = tex
      b.points.material.needsUpdate = true
    }
  }

  /**
   * `from` and `to` are in three space; `now` is the clock the fade runs on.
   *
   * Taking both ends rather than an origin and a direction: the server already
   * knows where the shot stopped -- on a wall, or on whoever it hit -- and a
   * tracer drawn to a computed infinity would pass through them.
   */
  spawn (kind, from, to, now) {
    const def = this.kinds.get(kind)
    if (!def) {
      if (!this.warned.has(kind)) {
        this.warned.add(kind)
        console.warn(`[vfx] no catalogue entry for kind ${kind}`)
      }
      return
    }
    if (def.type === 'particles') return this.#burst(def, from, to)
    const mesh = this.pool.pop()
    // Out of pool: drop it. A transient that had to wait is a transient drawn
    // at the wrong moment, and dropping one during a firefight is invisible.
    if (!mesh) return

    const dx = to.x - from.x
    const dy = to.y - from.y
    const dz = to.z - from.z
    const len = Math.hypot(dx, dy, dz)
    if (!(len > 0)) return
    mesh.position.set(from.x, from.y, from.z)
    // Rotate +X onto the shot with a quaternion, not with Euler angles.
    //
    // Two hand-derived rotations in this renderer have shipped mirrored --
    // the sprite billboard and the swing collider -- both because an axis
    // convention was reasoned about rather than computed. setFromUnitVectors
    // asks the question directly: take this vector to that one. There is no
    // order to get wrong and no sign to invert.
    this._from ??= new this.THREE.Vector3(1, 0, 0)
    this._to ??= new this.THREE.Vector3()
    this._to.set(dx / len, dy / len, dz / len)
    mesh.quaternion.setFromUnitVectors(this._from, this._to)
    mesh.scale.set(len, def.width, def.width)
    mesh.material.color.setHex(def.colour)
    mesh.material.opacity = 1
    mesh.visible = true
    this.live.push({ mesh, born: now, life: def.life })
  }

  /**
   * A burst of particles, from the same `spawn` every other effect arrives by.
   *
   * `from` is where it happened and `to` carries the direction and the emission
   * radius, because that is what the caller already computed for a tracer --
   * see tracerEnds. A point source is from === to, which is what an event with
   * a zero `param` gives.
   */
  #burst (def, from, to) {
    if (this.parts.length >= this.maxBursts) return
    const dx = to.x - from.x
    const dz = to.z - from.z
    const radius = Math.hypot(dx, dz)
    // Three space again: the heading is atan2 over (x, -z), the inverse of the
    // mapping the caller used. Zero radius leaves no direction to recover, and
    // a cone of 0.5 turns does not need one.
    const turns = radius > 0 ? Math.atan2(-dz, dx) / (Math.PI * 2) : 0
    const parts = emitBurst(def, from, turns, radius)
    if (parts.length === 0) return

    const THREE = this.THREE
    const geo = new THREE.BufferGeometry()
    geo.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(parts.length * 3), 3))
    geo.setAttribute('color', new THREE.Float32BufferAttribute(new Float32Array(parts.length * 3), 3))
    const mat = new THREE.PointsMaterial({
      size: def.size ?? 0.1,
      ...(this.disc ? { map: this.#mapFor(def) } : {}),
      vertexColors: true,
      transparent: true,
      depthWrite: false,
      sizeAttenuation: true,
      blending: def.blend === 'alpha' ? THREE.NormalBlending : THREE.AdditiveBlending
    })
    const points = new THREE.Points(geo, mat)
    points.frustumCulled = false
    this.scene.add(points)
    this.parts.push({ def, parts, points, maxLife: Math.max(...parts.map((p) => p.life)) })
  }

  /** What a kind is, so a caller can branch before it computes a tracer's ends. */
  kindOf (kind) { return this.kinds.get(kind) }

  /**
   * Keep an attached emitter alive, creating it if it is not already running.
   *
   * `entity` is the index the event carried in `param`; the caller resolves it
   * to a position every frame through `update`'s `locate`. Nothing is drawn
   * here -- an emitter with no located entity emits nothing and quietly lapses,
   * which is what happens when whatever it was following dies.
   */
  attach (kind, entity, dirTurns, now, { key: who = entity, linger = null } = {}) {
    const def = this.kinds.get(kind)
    if (!def || def.type !== 'particles') return
    const key = `${kind}:${who}`
    const keep = linger ?? (def.linger ?? 0.2)
    const live = this.emitters.get(key)
    if (live) {
      live.until = now + keep
      live.dir = dirTurns
      return
    }
    if (this.emitters.size >= this.maxBursts) return
    // Capacity for as many as can be alive at once, plus a second's slack, so
    // the buffer is allocated ONCE and emission appends into it. A growing
    // buffer per frame is the allocation this whole file avoids.
    const maxLife = (def.life ?? 0.5) + (def.lifeVar ?? 0)
    const cap = Math.min(512, Math.ceil((def.rate ?? 40) * (maxLife + 1)))
    const THREE = this.THREE
    const geo = new THREE.BufferGeometry()
    geo.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(cap * 3), 3))
    geo.setAttribute('color', new THREE.Float32BufferAttribute(new Float32Array(cap * 3), 3))
    const mat = new THREE.PointsMaterial({
      size: def.size ?? 0.1,
      ...(this.disc ? { map: this.#mapFor(def) } : {}),
      vertexColors: true,
      transparent: true,
      depthWrite: false,
      sizeAttenuation: true,
      blending: def.blend === 'alpha' ? THREE.NormalBlending : THREE.AdditiveBlending
    })
    const points = new THREE.Points(geo, mat)
    points.frustumCulled = false
    this.scene.add(points)
    this.emitters.set(key, {
      def, entity, dir: dirTurns, points, parts: [], cap, carry: 0,
      until: now + keep, lastAt: null
    })
  }

  /**
   * A TRAIL: keep `kind` coming out of `entity` -- a missile's `trail`, any
   * body's set_trail -- for THIS frame. Called every frame for as long as it
   * should; the first frame it is not, nothing new comes out (within
   * FOLLOW_GRACE) and the specks already out live their own life. So a
   * missile that bursts or goes simply stops being followed. The kind need not
   * say `attach 1`: this is what attaches it. `key` tells one body in a slot
   * from the next one there (index/generation), so a slot reused by another
   * missile does not carry the last one's specks along.
   */
  follow (kind, entity, dirTurns, now, key = entity) {
    this.attach(kind, entity, dirTurns, now, { key, linger: FOLLOW_GRACE })
  }

  /**
   * Fade everything live, and return expired meshes to the pool.
   *
   * `locate(entity)` answers where an attached emitter's body is right now, or
   * null if it is gone. Supplied by the caller because only it knows how to
   * find one -- the local player and a remote entity come from different
   * places, and this file should not learn the difference.
   */
  update (now, locate = null) {
    for (let i = this.live.length - 1; i >= 0; i--) {
      const e = this.live[i]
      const f = fade(now - e.born, e.life)
      if (f <= 0) {
        e.mesh.visible = false
        this.pool.push(e.mesh)
        this.live.splice(i, 1)
        continue
      }
      e.mesh.material.opacity = f
    }

    // Particles run on a DELTA, not on a timestamp: they integrate, and a burst
    // that joined between two frames has no age to measure against this one.
    const dt = this.lastNow === null ? 0 : Math.min(0.1, now - this.lastNow)
    this.lastNow = now
    for (let i = this.parts.length - 1; i >= 0; i--) {
      const b = this.parts[i]
      stepParticles(b.parts, dt, b.def)
      if (b.parts.length === 0) {
        this.scene.remove(b.points)
        b.points.geometry.dispose()
        b.points.material.dispose()
        this.parts.splice(i, 1)
        continue
      }
      const pos = b.points.geometry.attributes.position
      const col = b.points.geometry.attributes.color
      for (let k = 0; k < b.parts.length; k++) {
        const p = b.parts[k]
        pos.array[k * 3] = p.x; pos.array[k * 3 + 1] = p.y; pos.array[k * 3 + 2] = p.z
        const t = p.age / p.life
        const c = rampColour(b.def.colour, b.def.colourEnd ?? 0, t)
        // The ramp IS the fade under additive blending, where black is
        // invisible. Under alpha it is not, so there the whole burst fades on
        // the material instead -- per-point opacity wants a custom shader, and
        // a shader is a lot to carry for smoke.
        col.array[k * 3] = c.r; col.array[k * 3 + 1] = c.g; col.array[k * 3 + 2] = c.b
      }
      // The buffers were sized for the burst and particles retire out of it, so
      // the draw range is what hides the tail rather than a rebuilt buffer.
      b.points.geometry.setDrawRange(0, b.parts.length)
      pos.needsUpdate = true
      col.needsUpdate = true
      const age = b.maxLife > 0 ? 1 - Math.max(...b.parts.map((p) => 1 - p.age / p.life)) : 1
      b.points.material.size = (b.def.size ?? 0.1) +
        (((b.def.sizeEnd ?? 0) - (b.def.size ?? 0.1)) * Math.min(1, Math.max(0, age)))
      if (b.def.blend === 'alpha') b.points.material.opacity = 1 - Math.min(1, Math.max(0, age))
    }

    for (const [key, em] of [...this.emitters]) {
      // WHERE THE BODY IS NOW. This is the whole point of an attached emitter:
      // a burst spawns where the event said and a moving player runs out from
      // under it, leaving the sparks behind in a line. Emitting from the
      // located position each frame keeps them on the saw.
      const at = locate ? locate(em.entity) : null
      // CARRIED: every speck out takes the body's movement since the last
      // frame, so they stay where they are relative to it -- a flame on a
      // fireball -- rather than where they came out.
      if (at && em.def.carry) {
        if (em.lastAt) {
          const dx = at.x - em.lastAt.x; const dy = at.y - em.lastAt.y; const dz = at.z - em.lastAt.z
          for (const p of em.parts) { p.x += dx; p.y += dy; p.z += dz }
        }
        em.lastAt = { x: at.x, y: at.y, z: at.z }
      }
      if (at && now < em.until) {
        em.carry += (em.def.rate ?? 40) * dt
        const want = Math.floor(em.carry)
        if (want > 0) {
          em.carry -= want
          const room = em.cap - em.parts.length
          const n = Math.max(0, Math.min(want, room))
          if (n > 0) {
            // emitBurst's `count` is the burst size, so it is asked for exactly
            // as many as this frame earned rather than the kind's own number.
            for (const p of emitBurst({ ...em.def, count: n }, at, em.dir, 0)) {
              em.parts.push(p)
            }
          }
        }
      }
      stepParticles(em.parts, dt, em.def)
      if (em.parts.length === 0 && (!at || now >= em.until)) {
        this.scene.remove(em.points)
        em.points.geometry.dispose()
        em.points.material.dispose()
        this.emitters.delete(key)
        continue
      }
      const pos = em.points.geometry.attributes.position
      const col = em.points.geometry.attributes.color
      for (let k = 0; k < em.parts.length; k++) {
        const p = em.parts[k]
        pos.array[k * 3] = p.x; pos.array[k * 3 + 1] = p.y; pos.array[k * 3 + 2] = p.z
        const c = rampColour(em.def.colour, em.def.colourEnd ?? 0, p.age / p.life)
        col.array[k * 3] = c.r; col.array[k * 3 + 1] = c.g; col.array[k * 3 + 2] = c.b
      }
      em.points.geometry.setDrawRange(0, em.parts.length)
      pos.needsUpdate = true
      col.needsUpdate = true
    }
  }

  get emitterCount () { return this.emitters.size }

  get burstCount () { return this.parts.length }
}
