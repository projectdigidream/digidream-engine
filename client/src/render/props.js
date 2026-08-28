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

    // `prop` lines are the SIMULATION's half. The renderer does not draw from
    // them -- but the client has to mirror a solid prop into its own world or
    // it walks through what the server stops it against, so the physical facts
    // are parsed here and handed to the simulation rather than being read from
    // a second copy of this file.
    if (tok[0] === 'prop') {
      const pid = Number(tok[1])
      if (!Number.isInteger(pid) || pid <= 0) return
      const d = def(pid)
      d.name = tok[2]
      d.radius = 0.5
      d.height = 1
      d.flags = 0
      for (let k = 3; k < tok.length; k++) {
        // Bare flags, matching sim::prop_flag. Numbers rather than names on the
        // wasm side, so they are written out once, here, next to the words the
        // file uses.
        if (tok[k] === 'solid') { d.flags |= 1; continue }
        if (tok[k] === 'usable') { d.flags |= 2; continue }
        const v = tok[k + 1]
        if (v === undefined) break
        k++
        if (tok[k - 1] === 'radius') d.radius = Number(v)
        else if (tok[k - 1] === 'height') d.height = Number(v)
      }
      return
    }

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
    let once = false
    let fps = 0
    let frames = null
    let model = null
    let clip = null

    for (let k = 2; k < tok.length; k++) {
      const key = tok[k]
      if (key === 'billboard') { d.billboard = true; continue }
      // EIGHT VIEWING ANGLES. The frame names on this line are then given
      // WITHOUT their rotation digit -- POSSA, not POSSA1 -- and the renderer
      // appends 1..8 according to where the thing is facing relative to the
      // camera. Doom's own sprites are stored this way and it is what makes a
      // monster read as facing somewhere: without it an imp walking away from
      // you looks exactly like an imp walking towards you.
      if (key === 'rot8') { d.rot8 = true; continue }
      // PLAY ONCE, then hold the last frame -- per STATE, not per kind, so a
      // thing can loop while it walks and stop when it dies.
      //
      // A death or an explosion is an event, not a loop, and without this the
      // engine's only options are to cycle it forever or to remove the entity
      // the moment it ends. A barrel has been re-exploding on a loop since
      // bursts were added, which nobody noticed because it vanishes shortly
      // after.
      if (key === 'once') { once = true; continue }
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
      d.states.set(state, { frames, fps, once })
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
/**
 * Which of the eight views to show, 1..8, Doom's numbering.
 *
 * Rotation 1 is the thing seen from the FRONT -- facing the camera -- and they
 * go anticlockwise from there in eighths of a turn. So the index comes from the
 * angle between where the thing is facing and where the camera is looking at
 * it, not from either angle alone: a monster facing north looks different
 * depending on which side of it you stand.
 *
 * Both angles are in turns, and the arithmetic is done in turns for the same
 * reason the engine uses brad -- a turn wraps exactly at 1 and there is no
 * factor of pi to get the wrong way round. Two rotations in this renderer have
 * already shipped mirrored from exactly that mistake.
 */
export function rotationFor (facingTurns, cameraToThingTurns) {
  // No offset. Rotation 1 is the view where the thing faces the camera, and
  // "faces the camera" is when its facing and the direction from it TO the
  // camera are the SAME -- the first version added half a turn here on the
  // reasoning that they were opposite, which swapped every monster's front for
  // its back. Checked at the cardinal points rather than argued about, because
  // this renderer has now shipped two mirrored angle conventions.
  const rel = (cameraToThingTurns - facingTurns) % 1
  const wrapped = rel < 0 ? rel + 1 : rel
  // Rounded, not truncated: each of the eight views should own the 45 degrees
  // CENTRED on its angle, so the boundary falls at 22.5 and not at 0.
  return (Math.round(wrapped * 8) % 8) + 1
}

/** The texture name for a frame, given the view. */
export function frameName (def, state, index, rotation) {
  const base = state.frames[index]
  return def?.rot8 ? `${base}${rotation}` : base
}

export function frameAt (state, seconds) {
  const n = state.frames?.length ?? 0
  if (n <= 1 || !state.fps) return 0
  const i = Math.floor(seconds * state.fps)
  // `once` HOLDS the last frame instead of wrapping. That is what leaves a
  // corpse on the floor rather than a monster dying over and over, and what
  // stops a barrel re-exploding for as long as it exists.
  if (state.once) return i <= 0 ? 0 : (i >= n ? n - 1 : i)
  const w = i % n
  return w < 0 ? w + n : w
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
    // mesh -> { def, stateId }. A Map rather than an array searched linearly,
    // because the animation state is synced from the entity view every frame
    // and a scan per prop per frame is a scan that grows with the map.
    this.animated = new Map()
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

    this.animated.set(mesh, { def, stateId: 0 })
    return mesh
  }

  /**
   * Called when the server declares a new animation state for an entity.
   *
   * Nothing called this at all when it was written, so a barrel switched to its
   * explosion state on the server and went on showing its idle frames until it
   * vanished. The state arrived, was recorded on the entity, and stopped there
   * -- the two halves of the renderer each did their job and were not
   * introduced.
   */
  setState (mesh, stateId) {
    const a = this.animated.get(mesh)
    if (!a || a.stateId === stateId) return
    a.stateId = stateId
    // Restart the animation from its first frame rather than continuing the
    // shared clock's phase. An explosion that begins a third of the way through
    // is not an explosion, and unlike an idle loop it is meant to be seen once
    // from the beginning.
    a.startedAt = null
  }

  /**
   * Advance every prop's animation to `seconds`.
   *
   * One clock for all of them rather than one each: props of a kind animate in
   * lockstep in Doom, and independent phases would look like a bug nobody could
   * name.
   */
  update (seconds, cameraPos = null) {
    for (const [mesh, a] of this.animated) {
      if (!mesh.parent) continue                   // removed from the scene
      const state = a.def.states.get(a.stateId) ?? a.def.states.get(0)
      if (!state?.frames) continue
      // Idle states share one clock so props of a kind bob in lockstep; a state
      // the server switched INTO runs from its own start, because it is an
      // event rather than a loop.
      if (a.startedAt == null) a.startedAt = a.stateId === 0 ? 0 : seconds

      // Which of the eight views, for a sprite that has them. The angle is
      // between where the thing is FACING and where the camera is standing
      // relative to it -- the mesh's own yaw is not it, because a billboard is
      // always turned to face the viewer and so carries no information about
      // which way the thing is pointing.
      let rot = 1
      if (a.def.rot8 && cameraPos) {
        const dx = cameraPos.x - mesh.position.x
        const dz = cameraPos.z - mesh.position.z
        // Screen space is x/-z for world x/y (see main.js), and atan2 returns
        // radians, so this is converted to turns before it meets a facing.
        const toCamera = Math.atan2(-dz, dx) / (Math.PI * 2)
        rot = rotationFor(mesh.userData.facingTurns ?? 0, toCamera)
      }
      const idx = frameAt(state, seconds - a.startedAt)
      const tex = this.textures.get(frameName(a.def, state, idx, rot))
      if (tex && mesh.material.map !== tex) {
        mesh.material.map = tex
        mesh.material.color.setHex(0xffffff)
        mesh.material.needsUpdate = true
      }
    }
  }

  /** Forget meshes that have left the scene, so a long session does not grow. */
  prune () {
    for (const mesh of [...this.animated.keys()]) {
      if (!mesh.parent) this.animated.delete(mesh)
    }
  }
}
