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
//
// The engine attaches no meaning to the number at all. It reports "effect 1
// happened here, pointing that way, with this parameter"; the file says what a
// 1 is, and the parameter is its length.
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
    const d = { id, type: tok[2] ?? 'tracer', colour: 0xffdd55, width: 0.04, life: 0.12 }
    for (let k = 3; k < tok.length; k += 2) {
      const v = tok[k + 1]
      if (v === undefined) { warnings.push(`line ${i + 1}: '${tok[k]}' has no value`); break }
      if (tok[k] === 'colour' || tok[k] === 'color') d.colour = parseInt(v.replace(/^#|^0x/, ''), 16)
      else if (tok[k] === 'width') d.width = Number(v)
      else if (tok[k] === 'life') d.life = Number(v)
      else warnings.push(`line ${i + 1}: unknown vfx attribute '${tok[k]}'`)
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

  /** Fade everything live, and return expired meshes to the pool. */
  update (now) {
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
  }
}
