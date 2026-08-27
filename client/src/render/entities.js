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
  constructor ({ makeMesh, scene }) {
    this.makeMesh = makeMesh
    this.scene = scene
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

      if (e.mask & ENT_FIELD.DESTROYED) { this.#remove(index); continue }

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
        ent = { mesh: this.makeMesh(kind), samples: [], generation, kind }
        this.scene.add(ent.mesh)
        this.entities.set(index, ent)
      }

      // A new animation state is a declaration, never a frame number: the
      // client owns the timing (design §4). Recorded on the entity so a
      // renderer can pick it up without re-reading the snapshot.
      if (e.mask & ENT_FIELD.ANIMATION_STATE) ent.animationState = e.animationState ?? 0

      // No POS in the mask means the position did not change. Absence is
      // information in a delta-encoded snapshot (docs/protocol.md §5.3), so
      // there is nothing to record -- and holding at the last sample is
      // exactly what a stationary entity should do.
      if (!(e.mask & ENT_FIELD.POS)) continue

      const last = ent.samples.at(-1)
      const p = toThree(fxToFloat(e.pos.x), fxToFloat(e.pos.y), fxToFloat(e.pos.z))
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
        continue
      }

      const t = Math.max(0, Math.min(1, (renderTick - a.tick) / (b.tick - a.tick)))
      ent.mesh.position.set(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t + (ent.mesh.userData.yOffset ?? 0),
        a.z + (b.z - a.z) * t
      )
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
