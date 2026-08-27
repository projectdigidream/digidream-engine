//
// What a prop looks like, read from the same file the server reads.
//
// assets/props.txt has two kinds of record and two readers. The server takes
// the `prop` lines, which say how big a thing is; this takes the `sprite` and
// `mesh` lines, which say how to draw it. Neither reader understands the
// other's records and both skip what they do not recognise, so adding a visual
// does not restart the server and changing a radius does not rebuild assets.
//
// Adding a prop is editing that file and naming it from a map's Lua. There is
// deliberately no table of kinds in this file, and none in the engine: a number
// baked into two programs is a number that has to be changed in two programs.
//
// ANIMATION: the server declares a STATE and never a frame or a time (design
// §4). A sprite renderer swaps frames on its own clock; a mesh renderer will
// cross-fade on its own. That division is why a slow client looks slow rather
// than wrong, and why the simulation cannot be made to disagree with itself
// about what tick a foot lands on.
//

/**
 * Parse the catalogue.
 *
 * Tolerant by construction: an unknown record is skipped, an unknown attribute
 * is skipped, and a malformed line is reported and skipped. This file is edited
 * by hand and by artists, and a parser that refuses the whole thing over one
 * typo teaches people not to touch it.
 */
export function parseProps (text) {
  const kinds = new Map()   // id -> { id, sprite?: {...}, mesh?: {...} }
  const warnings = []

  const def = (id) => {
    if (!kinds.has(id)) kinds.set(id, { id, states: new Map() })
    return kinds.get(id)
  }

  text.split('\n').forEach((raw, i) => {
    const line = raw.split('#')[0].trim()
    if (!line) return
    const tok = line.split(/\s+/)

    // `prop` lines are the simulation's half. Skipped in silence: they are none
    // of the renderer's business, and warning about them would train everyone
    // to ignore this array.
    if (tok[0] === 'prop') return

    if (tok[0] !== 'sprite' && tok[0] !== 'mesh') return

    const id = Number(tok[1])
    if (!Number.isInteger(id) || id <= 0) {
      warnings.push(`line ${i + 1}: '${tok[1]}' is not a prop id`)
      return
    }
    const d = def(id)
    d.type = tok[0]

    // One state per line, defaulting to state 0 so the common case -- a thing
    // with one look -- needs no state keyword at all.
    let state = 0
    let fps = 0
    let frames = null
    let model = null
    let clip = null

    for (let k = 2; k < tok.length; k++) {
      const key = tok[k]
      if (key === 'billboard') { d.billboard = true; continue }
      const val = tok[k + 1]
      if (val === undefined) { warnings.push(`line ${i + 1}: '${key}' has no value`); break }
      k++
      switch (key) {
        case 'size':  d.size = Number(val); break
        case 'state': state = Number(val); break
        case 'fps':   fps = Number(val); break
        case 'frames': frames = val.split(',').filter(Boolean); break
        case 'model': model = val; break
        case 'clip':  clip = val; break
        default: warnings.push(`line ${i + 1}: unknown attribute '${key}'`)
      }
    }

    if (d.type === 'sprite') {
      if (!frames || frames.length === 0) {
        warnings.push(`line ${i + 1}: sprite ${id} state ${state} has no frames`)
        return
      }
      d.states.set(state, { frames, fps })
    } else {
      d.states.set(state, { model, clip, fps })
    }
  })

  return { kinds, warnings }
}

/**
 * Which frame of a state is showing at time `t`.
 *
 * A pure function of the animation's own clock, so it is testable without a
 * GPU and identical for every entity in that state -- which is what makes a row
 * of keys bob together instead of shimmering.
 *
 * `fps` of 0 means a single held frame. Not a special case in the caller: a
 * one-frame animation at any rate is the same frame forever, and expressing
 * "still" as "zero rate" keeps one path.
 */
export function frameAt (state, seconds) {
  const n = state.frames?.length ?? 0
  if (n <= 1 || !state.fps) return 0
  const i = Math.floor(seconds * state.fps) % n
  return i < 0 ? i + n : i
}

/**
 * Builds the visual for a prop kind, and keeps its animation running.
 *
 * Meshes are not implemented yet and that is deliberate rather than forgotten:
 * the catalogue already carries `mesh` records, the registry already reads
 * them, and what is missing is a loader for a model format this project has not
 * chosen. A kind declaring one draws the fallback and says so once, which is a
 * better answer than pretending it worked.
 */
export class PropRegistry {
  constructor ({ THREE, textures, catalogue }) {
    this.THREE = THREE
    this.textures = textures          // name -> THREE.Texture
    this.kinds = catalogue?.kinds ?? new Map()
    this.warned = new Set()
    this.animated = []                // { mesh, def, state }
  }

  /** A THREE object for this kind, or null to fall back to the default body. */
  make (kind) {
    const def = this.kinds.get(kind)
    if (!def) return null

    if (def.type === 'mesh') {
      if (!this.warned.has(kind)) {
        this.warned.add(kind)
        console.warn(`[props] kind ${kind} is a mesh; mesh rendering is not implemented yet`)
      }
      return null
    }

    const state = def.states.get(0) ?? [...def.states.values()][0]
    if (!state) return null

    const first = this.textures.get(state.frames[0])
    const size = def.size ?? 1
    const THREE = this.THREE

    // A billboarded plane rather than THREE.Sprite: a Sprite is always screen
    // aligned including its up vector, which under this camera's tilt makes a
    // key lie down as you rotate. A plane rotated about Y only stays upright in
    // the world and turns to face the viewer, which is what a Doom sprite does.
    const geo = new THREE.PlaneGeometry(size, size)
    const mat = new THREE.MeshBasicMaterial({
      map: first ?? null,
      color: first ? 0xffffff : 0xff00ff,
      transparent: true,
      // A sprite is mostly empty, and its empty parts must not write depth or
      // they punch holes in whatever is behind them.
      alphaTest: 0.5,
      side: THREE.DoubleSide
    })
    const mesh = new THREE.Mesh(geo, mat)
    mesh.userData.propKind = kind
    // Feet on the ground: the simulation's position is the bottom of the thing,
    // and a plane's origin is its middle.
    mesh.userData.yOffset = size / 2

    this.animated.push({ mesh, def, stateId: 0 })
    return mesh
  }

  /** Called when the server declares a new animation state for an entity. */
  setState (mesh, stateId) {
    const a = this.animated.find((x) => x.mesh === mesh)
    if (a) a.stateId = stateId
  }

  /**
   * Advance every prop's animation to `seconds`.
   *
   * One clock for all of them rather than one each: props of a kind animate in
   * lockstep in Doom, and independent phases would look like a bug nobody could
   * name.
   */
  update (seconds) {
    for (const a of this.animated) {
      if (!a.mesh.parent) continue                 // removed from the scene
      const state = a.def.states.get(a.stateId) ?? a.def.states.get(0)
      if (!state?.frames) continue
      const tex = this.textures.get(state.frames[frameAt(state, seconds)])
      if (tex && a.mesh.material.map !== tex) {
        a.mesh.material.map = tex
        a.mesh.material.color.setHex(0xffffff)
        a.mesh.material.needsUpdate = true
      }
    }
  }

  /** Forget meshes that have left the scene, so a long session does not grow. */
  prune () {
    this.animated = this.animated.filter((a) => a.mesh.parent)
  }
}
