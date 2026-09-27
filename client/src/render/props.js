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
import { frameKey, quadFor, uvFor } from './atlas.js'
import { CONDITIONS, defaultStates } from './animstate.js'

/**
 * Put one atlas frame's corners into a plane's attributes.
 *
 * FEET AT THE ORIGIN. quadFor gives the quad's size and where its CENTRE sits
 * relative to the feet; the four corners follow from that. Because the offsets
 * are written into the geometry rather than into the mesh's position, the mesh
 * stays exactly where the simulation puts the entity, and entities.js needs to
 * know nothing about any of this.
 *
 * PlaneGeometry's vertices are top-left, top-right, bottom-left, bottom-right,
 * and uvFor returns its four corners in that same order -- which is why the two
 * loops below can share an index. Getting the two orders out of step draws the
 * right rectangle of the sheet onto the wrong corners, which shears the sprite.
 */
export function writeAtlasFrame (geometry, atlas, key, worldHeight = atlas.worldHeight) {
  const q = quadFor(atlas, key, worldHeight)
  const uv = uvFor(atlas, key)
  if (!q || !uv) return false
  const pos = geometry.attributes?.position
  const tex = geometry.attributes?.uv
  if (!pos || !tex) return false

  const x0 = q.dx - q.w / 2
  const x1 = q.dx + q.w / 2
  const yBottom = q.dy - q.h / 2
  const yTop = q.dy + q.h / 2
  pos.setXYZ(0, x0, yTop, 0)
  pos.setXYZ(1, x1, yTop, 0)
  pos.setXYZ(2, x0, yBottom, 0)
  pos.setXYZ(3, x1, yBottom, 0)
  pos.needsUpdate = true
  for (let i = 0; i < 4; i++) tex.setXY(i, uv[i * 2], uv[i * 2 + 1])
  tex.needsUpdate = true
  // The bounding sphere is derived from the positions and is what frustum
  // culling tests, so a stale one culls a sprite that is on screen.
  geometry.computeBoundingSphere?.()
  return true
}

/**
 * A kind's drawing scale: 1 unless the catalogue says otherwise.
 *
 * Clamped, and the clamp is the point. `scale 0` is a prop that draws as
 * nothing with no error anywhere, which is indistinguishable from missing art
 * and is the failure this whole file is careful about elsewhere; a negative
 * one mirrors the quad. Both are typos rather than intentions.
 */
export function propScale (def) {
  const s = Number(def?.scale)
  return Number.isFinite(s) && s > 0 ? s : 1
}

export function parseProps (text) {
  const kinds = new Map()   // id -> { id, sprite?: {...}, mesh?: {...} }
  const warnings = []
  // Which record draws a player, by avatar NUMBER. A map rather than one entry,
  // because different players can be drawn differently: the server names a
  // number per body and this is what turns it into art.
  const avatars = new Map()

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
        // HOW BIG IT IS DRAWN, as a multiplier on whatever its picture would
        // otherwise be -- the `size` on a sprite line, or the worldHeight the
        // sheet declares for an atlas. A multiplier rather than a size, which
        // is the whole reason it can sit here and serve both: it is not a
        // second opinion about a number the sheet already carries, and the
        // `atlas` line's comment below refuses a `size` for exactly that
        // reason. It does NOT touch radius or height -- what a thing collides
        // as is the simulation's business and is replicated; how big it looks
        // is not.
        else if (tok[k - 1] === 'scale') d.scale = Number(v)
      }
      return
    }

    // An `atlas` line draws this kind from a ThreeSprite export: one packed
    // sheet and a manifest, rather than one PNG per frame per angle.
    //
    //   atlas <id> sheet <name> [state <n>] anim <animation>
    //
    // DELIBERATELY SHORT. The manifest already states how tall the sprite
    // stands, how many views it has, how fast each animation runs and whether
    // it loops -- so this line does not restate any of them. A `size` here
    // would be a second opinion about a number the sheet already carries, and
    // the two would drift the first time somebody re-exported.
    // WHICH RECORD DRAWS A PLAYER (design 4.1).
    //
    //   avatar <id> [blend <seconds>]
    //   avatar <id> when <condition> state <n>
    //
    // Kind 0 is structurally "no prop" in both readers -- this parser refuses
    // id <= 0 below and the server calls it the same thing -- and a player IS
    // kind 0 on the wire, which cannot change: an agent's `kind` is in
    // World::agent_hash and absent from SelfBlock, so a server that gave a
    // player kind 91 would mispredict every tick with nothing able to heal it.
    //
    // So the mapping lives HERE instead, which is better than the workaround it
    // replaces: "a player is not a prop" stays true everywhere else in the
    // client, the art is an ordinary prop/sprite/atlas record set needing no new
    // grammar, the frame preload walks it for free, and a module ships its own
    // avatar with no id baked into any code.
    if (tok[0] === 'avatar') {
      const id = Number(tok[1])
      if (!Number.isInteger(id) || id <= 0) {
        warnings.push(`line ${i + 1}: '${tok[1]}' is not a prop id`)
        return
      }
      // WHICH avatar this is. `as <n>` names it; without one it is avatar 0, the
      // default a server that says nothing gets. Read before the rest of the
      // line so `when` and `blend` land on the right one whatever the order.
      let slot = 0
      for (let k = 2; k < tok.length - 1; k++) if (tok[k] === 'as') slot = Number(tok[k + 1])
      if (!Number.isInteger(slot) || slot < 0) {
        warnings.push(`line ${i + 1}: '${slot}' is not an avatar number`)
        return
      }
      // The condition table is SEEDED with the convention rather than left
      // empty, so `avatar <id>` alone animates. `when` lines then override.
      if (!avatars.has(slot)) avatars.set(slot, { slot, kind: id, blend: 0, when: defaultStates() })
      const avatar = avatars.get(slot)
      // Last wins, and it is a warning rather than an error: a module loaded
      // after another legitimately replaces an avatar.
      if (avatar.kind !== id) {
        warnings.push(`line ${i + 1}: avatar ${slot} was kind ${avatar.kind}, now ${id}; the later one wins`)
        avatar.kind = id
      }
      let when = null
      for (let k = 2; k < tok.length; k++) {
        const val = tok[k + 1]
        if (val === undefined) { warnings.push(`line ${i + 1}: '${tok[k]}' has no value`); break }
        k++
        switch (tok[k - 1]) {
          case 'as': break   // already read above
          case 'blend': avatar.blend = Number(val); break
          case 'when':
            if (!CONDITIONS.includes(val)) {
              warnings.push(`line ${i + 1}: '${val}' is not an animation condition`)
            }
            when = val
            break
          case 'state':
            if (when) avatar.when[when] = Number(val)
            else warnings.push(`line ${i + 1}: 'state' without a 'when' before it`)
            break
          default: warnings.push(`line ${i + 1}: unknown attribute '${tok[k - 1]}'`)
        }
      }
      return
    }

    if (tok[0] === 'atlas') {
      const id = Number(tok[1])
      if (!Number.isInteger(id) || id <= 0) {
        warnings.push(`line ${i + 1}: '${tok[1]}' is not a prop id`)
        return
      }
      const d = def(id)
      d.type = 'atlas'
      let state = 0
      let anim = null
      for (let k = 2; k < tok.length; k++) {
        const val = tok[k + 1]
        if (val === undefined) { warnings.push(`line ${i + 1}: '${tok[k]}' has no value`); break }
        k++
        switch (tok[k - 1]) {
          case 'sheet': d.sheet = val; break
          case 'state': state = Number(val); break
          case 'anim': anim = val; break
          case 'blend': d.blend = Number(val); break
          default: warnings.push(`line ${i + 1}: unknown attribute '${tok[k - 1]}'`)
        }
      }
      if (!d.sheet) { warnings.push(`line ${i + 1}: atlas ${id} names no sheet`); return }
      if (!anim) { warnings.push(`line ${i + 1}: atlas ${id} state ${state} names no animation`); return }
      d.states.set(state, { anim })
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
        // SECONDS to cross-fade between this kind's states. ABSENT MEANS ZERO
        // means the hard cut this renderer has always done, byte for byte --
        // which is why it is opt-in and why the Doom catalogue never opts in. A
        // dissolve between two frames drawn for a hard cut at 6 fps is a double
        // exposure that reads as a renderer fault.
        case 'blend': d.blend = Number(val); break
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

  // An avatar naming a record nobody wrote would hand make() an unknown kind and
  // get the fallback body with nothing said. Refuse it here, where the line is
  // still identifiable.
  for (const [slot, a] of [...avatars]) {
    if (!kinds.has(a.kind)) {
      warnings.push(`avatar ${slot} names kind ${a.kind}, which no record declares`)
      avatars.delete(slot)
    }
  }

  return { kinds, warnings, avatars }
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
export function rotationFor (facingTurns, cameraToThingTurns, views = 8) {
  // No offset. Rotation 1 is the view where the thing faces the camera, and
  // "faces the camera" is when its facing and the direction from it TO the
  // camera are the SAME -- the first version added half a turn here on the
  // reasoning that they were opposite, which swapped every monster's front for
  // its back. Checked at the cardinal points rather than argued about, because
  // this renderer has now shipped two mirrored angle conventions.
  const rel = (cameraToThingTurns - facingTurns) % 1
  const wrapped = rel < 0 ? rel + 1 : rel
  // Rounded, not truncated: each view should own the arc CENTRED on its angle,
  // so with eight the boundary falls at 22.5 degrees and not at 0.
  //
  // `views` IS A PARAMETER BECAUSE THERE IS NOW A SECOND SOURCE OF SPRITES.
  // Doom stores eight rotations and that is still the default. A ThreeSprite
  // atlas may hold any number from 1 to 64, and its numbering is the same one --
  // view 1 faces the camera, then anticlockwise, which its own documentation
  // says was chosen to match Doom. So the atlas path passes its count here
  // rather than carrying a second copy of this arithmetic, which is the whole
  // reason this function takes an argument instead of the atlas growing its own.
  return (Math.round(wrapped * views) % views) + 1
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
/**
 * Which view of a mesh faces the camera, from the facing smuggled in userData.
 *
 * Smuggled because a billboard overwrites `mesh.rotation.y` every frame, so the
 * facing cannot live on the object itself -- main.js writes `facingTurns`
 * alongside.
 *
 * ONE of these. The four lines were already written twice, for the per-PNG path
 * and the atlas path, and a player would have made three copies of an arithmetic
 * whose comments record two earlier versions shipping mirrored. It is also the
 * single place a brad interpolation would go if a mesh ever needs facing to be
 * smooth rather than one of eight.
 */
export function viewFor (mesh, cameraPos, views = 8) {
  const dx = cameraPos.x - mesh.position.x
  const dz = cameraPos.z - mesh.position.z
  // Screen space is x/-z for world x/y (see main.js), and atan2 returns radians,
  // so this is converted to turns before it meets a facing.
  const toCamera = Math.atan2(-dz, dx) / (Math.PI * 2)
  return rotationFor(mesh.userData.facingTurns ?? 0, toCamera, views)
}

export class PropRegistry {
  constructor ({ THREE, textures, catalogue, atlases = new Map() }) {
    this.THREE = THREE
    this.textures = textures          // name -> THREE.Texture
    // sheet name -> { atlas, texture, material }. One material per SHEET, not
    // per entity: every prop drawn from one export samples the same image, so
    // they can share it, and sharing is what lets the renderer batch them.
    this.atlases = atlases
    this.kinds = catalogue?.kinds ?? new Map()
    // Avatar number -> the record that draws a player with it. From `avatar`
    // lines; see parseProps. Held rather than resolved at construction because
    // the catalogue knows it and the registry is what can build it.
    this.avatars = catalogue?.avatars ?? new Map()
    this.warned = new Set()
    // mesh -> { def, stateId }. A Map rather than an array searched linearly,
    // because the animation state is synced from the entity view every frame
    // and a scan per prop per frame is a scan that grows with the map.
    this.animated = new Map()
  }

  /**
   * A THREE object for a PLAYER, or null when no avatar is declared.
   *
   * A player is kind 0 on the wire and kind 0 is structurally "no prop", so this
   * is the one lookup that does not go through a kind the server knows about.
   * Everything after it is the ordinary path -- the same make(), the same atlas
   * handling, the same animation registry -- because a player's art is an
   * ordinary record and only its ADDRESSING is special.
   */
  makeAvatar (slot = 0) {
    // Falling back to avatar 0 rather than to nothing: a server naming an avatar
    // this catalogue does not ship should draw SOMETHING, and the default is a
    // better something than the capsule. A module that ships none gets the
    // capsule, which is the old behaviour unchanged.
    const a = this.avatars.get(slot) ?? this.avatars.get(0)
    if (!a) return null
    const mesh = this.make(a.kind)
    // The avatar's own blend wins over the record's, because a player may share
    // art with something else -- the placeholder does exactly that -- and how a
    // PLAYER transitions is a decision about the player.
    const rec = mesh ? this.animated.get(mesh) : null
    if (rec && a.blend > 0) rec.blend = a.blend
    return mesh
  }

  /** The avatar record behind a number, for its `when` table and its blend. */
  avatarFor (slot = 0) {
    return this.avatars.get(slot) ?? this.avatars.get(0) ?? null
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

    if (def.type === 'atlas') return this.#makeAtlas(kind, def)

    const first = this.textures.get(state.frames[0])
    // The kind's scale, applied once here. Guarded rather than trusted: a
    // catalogue with `scale 0` would make every one of these vanish with
    // nothing saying why, and a negative one turns the quad inside out.
    const size = (def.size ?? 1) * propScale(def)
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

    this.animated.set(mesh, { def, stateId: 0, blend: def.blend ?? 0, from: null })
    return mesh
  }

  /**
   * A sprite drawn out of a ThreeSprite sheet.
   *
   * Two differences from the per-frame-PNG path above, both of them the point:
   *
   * THE MATERIAL IS SHARED. Every prop from one sheet samples one image, so
   * they share one material. The old path gives each entity its own, which on a
   * busy level is four hundred materials for four hundred textures.
   *
   * THE GEOMETRY CARRIES THE FRAME. A packed frame is TRIMMED to its opaque
   * pixels, so its quad is a different size and sits at a different height for
   * every frame -- a crouch is shorter than a stand, and an outstretched arm is
   * off-centre. Rather than scale the mesh and push the offset through
   * `yOffset` (which entities.js applies, and which is one number where two are
   * needed), the four corners are written in the entity's own space with the
   * FEET AT THE ORIGIN. `yOffset` is therefore 0 here and the mesh sits exactly
   * where the simulation says the thing is.
   */
  #makeAtlas (kind, def) {
    const entry = this.atlases.get(def.sheet)
    if (!entry) {
      if (!this.warned.has(kind)) {
        this.warned.add(kind)
        console.warn(`[props] kind ${kind} draws from sheet '${def.sheet}', which was not loaded`)
      }
      return null
    }
    const THREE = this.THREE
    // 1x1 and then rewritten every time the frame changes. The size passed here
    // never survives; what matters is that the attributes exist to write into.
    const geo = new THREE.PlaneGeometry(1, 1)
    const mesh = new THREE.Mesh(geo, entry.material)
    mesh.userData.propKind = kind
    mesh.userData.yOffset = 0
    this.animated.set(mesh, { def, stateId: 0, blend: def.blend ?? 0, from: null, sheet: def.sheet, frameKey: null })
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
  setState (mesh, stateId, seconds = null) {
    const a = this.animated.get(mesh)
    if (!a || a.stateId === stateId) return

    // THE OUTGOING STATE, kept only when this record opted into a blend.
    //
    // Its clock is NOT reset. Without `fromStarted` the outgoing quad freezes on
    // frame 0 of the state it is fading OUT of -- which is worse than a hard cut,
    // because a frozen ghost reads as a stuck renderer where a cut reads as an
    // animation. A `once` state fading out is then correct for free: its clock
    // keeps running and frameAt holds its last frame, which is what leaves a
    // corpse on the floor.
    //
    // A transition that starts mid-fade REPLACES the outgoing state rather than
    // stacking another ghost behind it. Three states in quick succession leave
    // one ghost, not three -- and a stacking version would leak a material per
    // transition.
    const blend = a.blend ?? 0
    if (blend > 0 && seconds !== null && a.startedAt !== null) {
      a.from = a.stateId
      a.fromStarted = a.startedAt
      a.blendAt = seconds
      a.blendFor = blend
    }

    a.stateId = stateId
    // Restart the animation from its first frame rather than continuing the
    // shared clock's phase. An explosion that begins a third of the way through
    // is not an explosion, and unlike an idle loop it is meant to be seen once
    // from the beginning.
    a.startedAt = null
  }

  /**
   * How far through a cross-fade this mesh is: 0 outgoing, 1 fully arrived.
   *
   * A DURATION rather than the exponential ease the camera uses, and the reason
   * is concrete: an exponential never arrives, and a cross-fade must terminate
   * so the outgoing clock can be dropped and its cloned material disposed.
   */
  mixAt (a, seconds) {
    if (a.from === undefined || a.from === null) return 1
    if (!(a.blendFor > 0)) return 1
    const t = (seconds - a.blendAt) / a.blendFor
    if (t >= 1) {
      // Retire: the ghost is gone and its clock with it.
      a.from = null
      a.fromStarted = null
      return 1
    }
    return t < 0 ? 0 : t
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
      const mix = this.mixAt(a, seconds)
      if (a.def.type === 'atlas') { this.#updateAtlas(mesh, a, seconds, cameraPos, mix); continue }
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
      if (a.def.rot8 && cameraPos) rot = viewFor(mesh, cameraPos)
      const idx = frameAt(state, seconds - a.startedAt)
      const tex = this.textures.get(frameName(a.def, state, idx, rot))
      if (tex && mesh.material.map !== tex) {
        mesh.material.map = tex
        mesh.material.color.setHex(0xffffff)
        mesh.material.needsUpdate = true
      }

      // The outgoing state, drawn behind and fading. Its own clock, still
      // running -- see setState.
      if (mix < 1) {
        const from = a.def.states.get(a.from) ?? a.def.states.get(0)
        const ftex = from?.frames
          ? this.textures.get(frameName(a.def, from, frameAt(from, seconds - a.fromStarted), rot))
          : null
        this.#fade(mesh, a, mix, ftex)
      } else {
        this.#unfade(mesh, a)
      }
    }
  }

  /**
   * Draw the outgoing state behind the incoming one, both part-transparent.
   *
   * THE TRAP HERE is alphaTest. A sprite's material clips every fragment below
   * 0.5 alpha so its empty corners do not punch depth holes -- and with an
   * opacity of 0.3 that clips EVERY fragment, so the thing being faded in is
   * invisible for the whole fade and then appears. So both quads drop to a
   * near-zero alphaTest while the blend runs, and #unfade puts it back.
   *
   * The ghost is a child so it inherits position, rotation and scale without
   * this having to track any of them -- a fade that drifted away from the body
   * it belongs to would be worse than no fade.
   */
  #fade (mesh, a, mix, ghostTex) {
    const THREE = this.THREE
    if (!a.ghost) {
      // THE MESH'S OWN MATERIAL MAY BE SHARED. The atlas path hands every entity
      // drawn from a sheet the SAME material, so writing opacity onto it would
      // fade every body of that kind at once. So a fading mesh gets its own
      // clone for the duration, and the shared one is put back afterwards.
      if (!mesh.material.userData?.owned) {
        a.sharedMaterial = mesh.material
        mesh.material = mesh.material.clone()
        mesh.material.userData.owned = true
      }
      // The ghost needs its OWN geometry as well as its own material: the atlas
      // path rewrites the quad every frame for each frame's trim, so sharing it
      // would drag the ghost onto the incoming frame's shape.
      const g = new THREE.Mesh(mesh.geometry.clone(), mesh.material.clone())
      // OWNED, and this flag is load-bearing. Disposal walks meshes on the way
      // out, and until now it disposed whatever it found -- true of the per-PNG
      // path where one material is allocated per entity, and FALSE of the atlas
      // path. Only what this method cloned may ever be disposed.
      g.material.userData.owned = true
      g.material.depthWrite = false
      g.renderOrder = (mesh.renderOrder ?? 0) - 1
      if (ghostTex) g.material.map = ghostTex
      mesh.add(g)
      a.ghost = g
    }
    // The per-PNG path keeps the outgoing state ANIMATING, because there that
    // costs one texture swap. The atlas path holds the frame it left on: its
    // geometry is per-frame, so following it would mean cloning a quad every
    // frame of every fade, and over a tenth of a second the difference is not
    // visible. Stated rather than left to be noticed.
    if (ghostTex && a.ghost.material.map !== ghostTex) {
      a.ghost.material.map = ghostTex
      a.ghost.material.needsUpdate = true
    }
    // THE TRAP. A sprite's material clips every fragment below 0.5 alpha so its
    // empty corners do not punch depth holes -- and with an opacity of 0.3 that
    // clips EVERY fragment, so the thing fading IN is invisible for the whole
    // fade and then appears. Both quads drop to a near-zero alphaTest while the
    // blend runs; #unfade puts it back.
    mesh.material.transparent = true
    mesh.material.alphaTest = 0.01
    mesh.material.opacity = mix
    a.ghost.material.transparent = true
    a.ghost.material.alphaTest = 0.01
    a.ghost.material.opacity = 1 - mix
  }

  /** Put the mesh back the way a sprite is meant to be, shared material and all. */
  #unfade (mesh, a) {
    if (!a.ghost) return
    mesh.remove(a.ghost)
    a.ghost.geometry.dispose()
    a.ghost.material.dispose()
    a.ghost = null
    if (a.sharedMaterial) {
      // The clone this mesh borrowed for the fade, and only that.
      mesh.material.dispose()
      mesh.material = a.sharedMaterial
      a.sharedMaterial = null
    } else {
      mesh.material.opacity = 1
      mesh.material.alphaTest = 0.5
    }
  }

  /**
   * One atlas-backed sprite, for this instant.
   *
   * The timing rule is frameAt(), the SAME one the per-PNG path uses -- the
   * manifest's fps and loop flag are shaped into what it expects rather than a
   * second clock being written here. The view rule is rotationFor(), likewise,
   * with the sheet's own view count.
   *
   * Writing the corners is skipped unless the frame actually changed, for the
   * reason the animated-texture code gives: at 144 Hz against an 8 fps
   * animation, seventeen of every eighteen writes would be setting the values
   * that are already there.
   */
  #updateAtlas (mesh, a, seconds, cameraPos, mix = 1) {
    const entry = this.atlases.get(a.sheet)
    if (!entry) return
    const state = a.def.states.get(a.stateId) ?? a.def.states.get(0)
    if (!state?.anim) return
    const anim = entry.atlas.byName.get(state.anim)
    if (!anim) {
      if (!this.warned.has(`${a.sheet}/${state.anim}`)) {
        this.warned.add(`${a.sheet}/${state.anim}`)
        console.warn(`[props] sheet '${a.sheet}' has no animation '${state.anim}'`)
      }
      return
    }
    if (a.startedAt == null) a.startedAt = a.stateId === 0 ? 0 : seconds

    let view = 1
    if (entry.atlas.views.count > 1 && cameraPos) {
      view = viewFor(mesh, cameraPos, entry.atlas.views.count)
    }

    // frameAt wants { frames, fps, once }. `loop` and `once` are the same fact
    // stated the other way up.
    // The fade, BEFORE the early return below: a frame that has not changed
    // still has an opacity that has, and returning first would freeze the blend
    // at whatever it was when the frame last moved.
    if (mix < 1) this.#fade(mesh, a, mix, null)
    else this.#unfade(mesh, a)

    const idx = frameAt({ frames: anim.frames, fps: anim.fps, once: !anim.loop }, seconds - a.startedAt)
    const key = frameKey(entry.atlas, state.anim, idx, view)
    if (!key || key === a.frameKey) return
    a.frameKey = key
    // The sheet says how tall the sprite stands; the kind's scale multiplies
    // it. Passed on every frame because writeAtlasFrame rewrites the quad
    // anyway -- each frame has its own trim and so its own size.
    writeAtlasFrame(mesh.geometry, entry.atlas, key,
                    entry.atlas.worldHeight * propScale(a.def))
  }

  /** Forget meshes that have left the scene, so a long session does not grow. */
  prune () {
    for (const mesh of [...this.animated.keys()]) {
      if (!mesh.parent) this.animated.delete(mesh)
    }
  }
}
