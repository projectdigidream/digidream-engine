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
  if (age < 0) return 0
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
