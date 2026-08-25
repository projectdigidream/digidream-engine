//
// Snapshot reassembly: turning delta-encoded snapshots back into a full entity
// set (docs/protocol.md 5.3).
//
// A delta snapshot carries only the entities that changed since a baseline the
// server believes this client holds. Everything omitted is carried forward
// unchanged. The client therefore has to keep the reconstructed entity set for
// each snapshot it has applied, because any of them may be the baseline the
// server chooses next.
//
// Two rules do most of the work here:
//
//   * A snapshot whose baseline the client does NOT have is unusable and must
//     be dropped WITHOUT acking it. Applying it against the wrong baseline
//     would silently produce a wrong world; refusing it makes the server fall
//     back to a full snapshot, which always works. Dropping is the safe
//     direction and it is self-healing.
//
//   * A handle whose generation differs from the one held is a DIFFERENT entity
//     in a recycled slot (protocol R8), not the same one moved. Merging fields
//     onto the old record would give a freshly-spawned entity the dead one's
//     position and interpolation history.
//
import { ENT_FIELD } from './proto.js'

const HISTORY = 64   // snapshots retained as possible baselines (~2s at 30 Hz)

export class SnapshotReassembler {
  constructor () {
    this.history = new Map()   // serverTick -> Map<index, entity>
    this.current = new Map()   // newest reconstructed entity set
    this.ackTick = 0           // newest snapshot successfully applied

    this.stats = {
      applied: 0,
      full: 0,
      delta: 0,
      droppedNoBaseline: 0,
      carriedForward: 0,
      regenerated: 0
    }
  }

  /**
   * @returns {{ok: boolean, reason?: string, entities?: Map}}
   */
  apply (snapshot) {
    let base
    if (snapshot.baselineTick === 0) {
      base = new Map()
      this.stats.full += 1
    } else {
      base = this.history.get(snapshot.baselineTick)
      if (!base) {
        // The server deltaed against something we no longer hold -- normally
        // because the snapshot that would have established it was lost. Do not
        // ack it: the server then sees a stale ack, finds it outside its ring,
        // and sends a full snapshot.
        this.stats.droppedNoBaseline += 1
        return { ok: false, reason: 'no-baseline' }
      }
      this.stats.delta += 1
    }

    const out = new Map()
    for (const [idx, e] of base) out.set(idx, { ...e })
    const carriedIn = out.size

    for (const e of snapshot.entities) {
      const idx = e.handle & 0x00ffffff
      const gen = (e.handle >>> 24) & 0xff

      if (e.mask & ENT_FIELD.DESTROYED) {
        out.delete(idx)
        continue
      }

      let cur = out.get(idx)
      if (!cur || cur.generation !== gen) {
        // Recycled slot: a new entity, not a moved one.
        if (cur) this.stats.regenerated += 1
        cur = { generation: gen, pos: null, facing: 0, animationState: 0 }
      }

      if (e.mask & ENT_FIELD.POS) cur.pos = e.pos
      if (e.mask & ENT_FIELD.FACING) cur.facing = e.facing
      if (e.mask & ENT_FIELD.ANIMATION_STATE) cur.animationState = e.animationState
      if (e.mask & ENT_FIELD.VISUAL_PARAMS) cur.visual = e.visual
      if (e.mask & ENT_FIELD.ANCHOR) cur.anchor = e.anchor
      if (e.mask & ENT_FIELD.CONTROLLER_MODE) cur.controllerMode = e.controllerMode
      if (e.mask & ENT_FIELD.DISPLAY_ID) cur.displayId = e.displayId

      out.set(idx, cur)
    }

    this.stats.carriedForward += Math.max(0, carriedIn - snapshot.entities.length)
    this.stats.applied += 1

    this.history.set(snapshot.serverTick, out)
    this.current = out
    this.ackTick = snapshot.serverTick

    // Prune. Retaining every snapshot forever would be a slow leak in a
    // long-lived session, and anything older than the ack round trip can never
    // be chosen as a baseline again.
    if (this.history.size > HISTORY) {
      const cutoff = snapshot.serverTick - HISTORY
      for (const t of this.history.keys()) if (t <= cutoff) this.history.delete(t)
    }

    return { ok: true, entities: out }
  }

  entities () { return this.current }
}
