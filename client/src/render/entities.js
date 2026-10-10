//
// Remote entities: the players and monsters this client does NOT control
// (design §4, docs/protocol.md §11).
//
// They are interpolated, not predicted, and the asymmetry is the whole point.
// The local agent is predicted because its inputs are known before the server
// has seen them. A remote entity's inputs are not knowable at all -- predicting
// one means guessing what another person will press, and being wrong about that
// looks far worse than being slightly behind.
//
// So a remote entity is drawn in the PAST: at a time a fixed delay behind the
// newest snapshot, between two states that have both already arrived. That
// delay is what buys smoothness across a dropped datagram, and it is why one
// dropped snapshot is invisible rather than a stutter.
//

// How far behind the newest snapshot to render. Two snapshot intervals: enough
// that a single dropped datagram still has a later sample to interpolate
// toward, and no more, because every millisecond here is a millisecond of
// additional apparent lag on everyone else.
export const INTERP_DELAY_TICKS = 4

// Decoded straight from the wire rather than pre-chewed by proto.js: a handle
// IS an index and a generation, and unpacking it where it is used keeps the
// decoder a faithful transcription of the format.
import { ENT_FIELD, handleIndex, handleGeneration } from '../proto.js'

export class EntityView {
  /**
   * `makeMesh(kind)` builds the visual for an entity. `kind` is the wire's
   * display_id: 0 for a player, and for anything else the number the prop
   * catalogue keys on. A builder that does not recognise a kind returns null
   * and gets the default body, which is a visible wrong rather than an
   * invisible one -- an entity you cannot see is indistinguishable from one the
   * server never sent.
   */
  constructor ({ makeMesh, scene, onProp = null, onPropGone = null,
                 onBody = null, onBodyGone = null }) {
    this.makeMesh = makeMesh
    this.scene = scene
    // Told about props so the caller can mirror them into a simulation. A
    // remote PLAYER is interpolated and never simulated locally; a solid prop
    // has to be, or the client walks through it.
    this.onProp = onProp
    this.onPropGone = onPropGone
    // Bodies -- players and monsters -- so the caller can mirror them as
    // colliders. A remote body is interpolated and never simulated, but it is
    // still something to walk into.
    this.onBody = onBody
    this.onBodyGone = onBodyGone
    // handle -> { mesh, samples: [{tick, x, y, z, facing}], generation, kind }
    this.entities = new Map()
  }

  /**
   * Fold one snapshot's entity blocks in.
   *
   * A snapshot is a DELTA against a baseline, so an entity that did not change
   * is simply absent -- absence means "unchanged", never "gone". Only an
   * explicit DESTROYED mask removes one (docs/protocol.md §6).
   */
  ingest (snapshot, { fxToFloat, toThree }) {
    for (const e of snapshot.entities ?? []) {
      const index = handleIndex(e.handle)
      const generation = handleGeneration(e.handle)

      if (e.mask & ENT_FIELD.DESTROYED) {
        const gone = this.entities.get(index)
        if (gone?.kind && this.onPropGone) this.onPropGone(index)
        if (gone && this.onBodyGone) this.onBodyGone(index)
        this.#remove(index)
        continue
      }

      let ent = this.entities.get(index)
      if (ent && ent.generation !== generation) {
        // A recycled slot is a DIFFERENT entity (protocol R8). Keeping its
        // samples would interpolate the newcomer from wherever its predecessor
        // died -- which reads as an enemy teleporting across the map at the
        // exact moment it is replaced.
        this.#remove(index)
        ent = null
      }
      if (!ent) {
        // DISPLAY_ID arrives with the first block that carries a new entity
        // (docs/protocol.md §5.4), so the visual is chosen once, here, from
        // what the entity IS -- not guessed from how it moves.
        const kind = e.displayId ?? 0
        // The avatar arrives with the same first block, for the same reason: a
        // player's costume is what it IS to the renderer, and choosing it once
        // here is cheaper than asking on every frame.
        const avatar = e.avatar ?? 0
        ent = { mesh: this.makeMesh(kind, avatar), samples: [], generation, kind, avatar }
        this.scene.add(ent.mesh)
        this.entities.set(index, ent)
      }

      // A new animation state is a declaration, never a frame number: the
      // client owns the timing (design §4). Recorded on the entity so a
      // renderer can pick it up without re-reading the snapshot.
      if (e.mask & ENT_FIELD.ANIMATION_STATE) ent.animationState = e.animationState ?? 0
      // A costume can change mid-life -- a class swap. Recorded here; the mesh
      // is NOT rebuilt, because a change of art is a change of mesh and that is
      // a remove-and-respawn the caller can do rather than a surprise this loop
      // performs mid-interpolation.
      if (e.mask & ENT_FIELD.AVATAR) ent.avatar = e.avatar ?? 0
      // How far into its swing (ticks left; 0 none), delta-encoded like the
      // rest, so the last value stands until it changes -- and BEFORE the
      // position early-return, because a monster swinging on the spot sends
      // ATTACK with no POS. With `attackAt`, the client time it was told, so a
      // renderer can tell a fresh swing from one it already drew.
      if (e.mask & ENT_FIELD.ATTACK) {
        const left = e.attackLeft ?? 0
        if (left > (ent.attackLeft ?? 0)) ent.swingStartedAt = performance.now()
        ent.attackLeft = left
        ent.attackAt = performance.now()
      }

      // SOLIDITY, BEFORE THE POSITION EARLY-RETURN.
      //
      // `solid` is delta-encoded like everything else: it arrives on the tick
      // it CHANGES and never again, so the last value has to be remembered
      // rather than read from this message.
      //
      // AND IT ARRIVES ON A TICK WITH NO POSITION. A monster that dies stops
      // moving, so the block announcing it is a corpse carries SOLID and
      // ANIMATION_STATE and no POS at all. Handling solidity after the POS
      // return meant that block was skipped: the death animation played (that
      // is applied above), the body was drawn on the floor, and the collider
      // mirrored into this client's world stayed solid for ever. The player
      // then walked into an invisible corpse the server walks straight
      // through, which is a misprediction that reads as an invisible wall
      // exactly where something died.
      //
      // It only happened SOMETIMES because a corpse that settles in the same
      // tick puts POS back in the mask and takes the old path.
      if (e.mask & ENT_FIELD.SOLID) {
        ent.solid = e.solid !== 0
        // A body that has just stopped being solid needs no position: the
        // mirror is removed. One that has just become solid does, and the last
        // known one is the right answer -- it has not moved, or POS would be
        // here too.
        if (!(e.mask & ENT_FIELD.POS) && this.onBody && (ent.lastPos || !ent.solid)) {
          this.onBody(index, ent.kind, ent.lastPos, ent.solid !== false)
        }
      }

      // No POS in the mask means the position did not change. Absence is
      // information in a delta-encoded snapshot (docs/protocol.md §5.3), so
      // there is nothing to record -- and holding at the last sample is
      // exactly what a stationary entity should do.
      if (!(e.mask & ENT_FIELD.POS)) continue

      // The raw fx position, kept for the solidity path above: the samples
      // below are converted to three-space and rounded, and the simulation
      // needs engine space.
      ent.lastPos = e.pos

      // Props go into the simulation as well as onto the screen, at the raw fx
      // position rather than the converted one: the simulation works in engine
      // space, and converting for the renderer and back would round.
      if (ent.kind && this.onProp) this.onProp(index, ent.kind, e.pos)
      // The KIND, not a size: how big a thing is comes from the catalogue for
      // a monster and from the map for a player, and neither is this view's
      // business to know.
      if (this.onBody) this.onBody(index, ent.kind, e.pos, ent.solid !== false)

      const last = ent.samples.at(-1)
      const p = toThree(fxToFloat(e.pos.x), fxToFloat(e.pos.y), fxToFloat(e.pos.z))
      // The newest position as the server holds it, exact, for questions the
      // core answers about it (a predicted blast's reach, render/knock.js).
      ent.fxPos = e.pos
      ent.samples.push({
        tick: snapshot.serverTick,
        x: p.x, y: p.y, z: p.z,
        facing: e.facing ?? last?.facing ?? 0
      })
      // Two seconds of history is far more than the delay needs; the cap is so
      // a long session does not grow one buffer per entity forever.
      if (ent.samples.length > 120) ent.samples.shift()
    }
  }

  #remove (index) {
    const ent = this.entities.get(index)
    if (!ent) return
    this.scene.remove(ent.mesh)
    ent.mesh.geometry?.dispose?.()
    // The MATERIAL only when it belongs to this entity, which is not always.
    //
    // This used to dispose whatever it found, with a comment saying "one is
    // allocated per entity". True of the per-PNG sprite path and FALSE of the
    // atlas path, where one material is shared by every body drawn from a sheet
    // -- so destroying one of them disposed the material the others were still
    // using. It presented as a shader recompile rather than a blank, which is
    // why nobody chased it; it became reachable the moment a PLAYER was drawn
    // from a shared sheet.
    //
    // `owned` is set by whoever cloned it. Leaving a per-entity material behind
    // leaks a GPU program slot per corpse over a long session, so the flag has
    // to be set rather than the disposal dropped.
    //
    // NOT THE TEXTURE, either way. `material.map` is shared with every other
    // prop of the same kind and with the preloaded frame set, so disposing it
    // here would blank every barrel in the map the instant one of them was
    // destroyed -- which reads as a texture-loading bug and would be hunted as
    // one.
    const mat = ent.mesh.material
    if (mat && !mat.userData?.shared) mat.dispose?.()
    this.entities.delete(index)
  }

  /**
   * Place every entity for a render time of `newestTick - INTERP_DELAY_TICKS`.
   *
   * Fractional ticks, because the display's frame rate has no relationship to
   * the tick rate and snapping to whole ticks would reintroduce exactly the
   * stutter the delay is here to remove.
   */
  update (newestTick, alpha = 0) {
    const renderTick = newestTick + alpha - INTERP_DELAY_TICKS
    // Kept: the tick a shot aimed now is aimed at (an input frame's `view`).
    this.renderTick = renderTick
    for (const ent of this.entities.values()) {
      const s = ent.samples
      if (s.length === 0) continue

      // Newest sample at or before the render time, and the one after it.
      let i = s.length - 1
      while (i > 0 && s[i].tick > renderTick) i--
      const a = s[i]
      const b = s[i + 1] ?? null

      if (!b || b.tick <= a.tick) {
        // Nothing to interpolate toward: hold at the last known state rather
        // than extrapolating. A guess about someone else's movement that turns
        // out wrong has to be taken back, and a visible correction is worse
        // than a few frames of standing still.
        ent.mesh.position.set(a.x, a.y + (ent.mesh.userData.yOffset ?? 0), a.z)
        ent.facing = a.facing
        continue
      }

      const t = Math.max(0, Math.min(1, (renderTick - a.tick) / (b.tick - a.tick)))
      ent.mesh.position.set(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t + (ent.mesh.userData.yOffset ?? 0),
        a.z + (b.z - a.z) * t
      )
      // Facing is NOT interpolated between the two samples: it is a brad, and
      // lerping one across the wrap point sends a monster spinning the long way
      // round. It only chooses which of eight sprites to draw, so the nearer
      // sample's value is both correct enough and free of that failure.
      ent.facing = (t < 0.5 ? a : b).facing
    }
  }

  get count () { return this.entities.size }

  /**
   * Forget every entity and take its mesh out of the scene.
   *
   * For a map change: the entity indices in the next map's snapshots address
   * different players entirely, so a view that kept its old ones would
   * interpolate a stranger's body from wherever the last map left it toward
   * wherever this one starts.
   */
  clear () {
    for (const index of [...this.entities.keys()]) this.#remove(index)
  }
}
