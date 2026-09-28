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
          linger: 0.2
        }
      : { id, type, colour: 0xffdd55, width: 0.04, life: 0.12 }
    const NUM = {
      width: 'width', life: 'life', life_var: 'lifeVar', count: 'count',
      speed: 'speed', speed_var: 'speedVar', cone: 'cone', gravity: 'gravity',
      drag: 'drag', size: 'size', size_end: 'sizeEnd',
      attach: 'attach', rate: 'rate', linger: 'linger'
    }
    for (let k = 3; k < tok.length; k += 2) {
      const key = tok[k]
      const v = tok[k + 1]
      if (v === undefined) { warnings.push(`line ${i + 1}: '${key}' has no value`); break }
      if (key === 'colour' || key === 'color') d.colour = parseInt(v.replace(/^#|^0x/, ''), 16)
      else if (key === 'colour_end' || key === 'color_end') d.colourEnd = parseInt(v.replace(/^#|^0x/, ''), 16)
      else if (key === 'blend') d.blend = v
      else if (NUM[key] !== undefined) d[NUM[key]] = Number(v)
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
 * `param` is the length in 1/256 world units, which is a hundredth of a Doom
 * unit at this scale and comfortably inside the u32 the event carries. `dir` is
 * a brad -- a turn in 2^32 -- so dividing by 2^32 gives turns directly and the
 * conversion cannot pick up a stray factor of pi.
 *
 * Extracted from main.js so it can be tested at all. main.js needs a DOM, a
 * WebGL context and a live session, so every line in it is a line no suite
 * runs -- and this particular calculation has a bad history: the sprite
 * billboard and the swing collider both shipped mirrored, both because an angle
 * convention was reasoned about instead of checked.
 */
export function tracerEnds (v, fxToFloat) {
  const len = v.param / 256
  const turns = (v.dir >>> 0) / 4294967296
  const a = turns * Math.PI * 2
  const x = fxToFloat(v.pos.x)
  const y = fxToFloat(v.pos.y)
  const z = fxToFloat(v.pos.z)
  return {
    from: { x, y, z },
    to: { x: x + Math.cos(a) * len, y: y + Math.sin(a) * len, z },
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
export class VfxView {
  constructor ({ THREE, scene, catalogue, poolSize = 48 }) {
    this.THREE = THREE
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
  attach (kind, entity, dirTurns, now) {
    const def = this.kinds.get(kind)
    if (!def || def.type !== 'particles') return
    const key = `${kind}:${entity}`
    const live = this.emitters.get(key)
    if (live) {
      live.until = now + (def.linger ?? 0.2)
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
      until: now + (def.linger ?? 0.2)
    })
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
