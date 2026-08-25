//
// Client-side prediction and reconciliation (docs/protocol.md section 10).
//
// The client simulates its own agent ahead of the server by running the SAME
// code the server runs, compiled to wasm. On each snapshot it adopts the
// authoritative state and replays the inputs the server has not yet
// acknowledged.
//
// The property that makes this design work, and the one this file is built to
// measure: because the core is fixed-point and deterministic, a replay from the
// same state with the same inputs must reproduce the previous prediction
// EXACTLY. There is no numerical noise floor. So a mismatch is never "normal
// drift" to be smoothed away -- it means the server applied something the
// client did not know about, or the two ran different inputs, or the two builds
// disagree. All three are worth knowing about, and all three are invisible in a
// float-based engine where small corrections happen constantly.
//
import { STATE_FLAG } from './proto.js'

const HISTORY = 256   // ticks of input kept for replay; ~4s at 60 Hz

export class Predictor {
  /**
   * @param {import('./core.js').WasmWorld} world  local sim, one agent
   * @param {number} agent                          local agent index
   */
  constructor (world, agent) {
    this.world = world
    this.agent = agent

    this.tick = 0                 // client input tick, ahead of the server
    this.inputs = new Map()       // tick -> input frame
    this.predicted = new Map()    // tick -> agent hash after applying that tick

    this.synced = false           // adopted an authoritative state yet?
    this.lastAppliedTick = 0

    // Clock sync (protocol 10.1). The client steers its tick offset to keep a
    // small positive margin of buffered input at the server.
    this.targetMargin = 2
    this.tickAdjust = 0           // ticks to skip (-) or duplicate (+) next frame

    this.lastSnapshot = null      // { serverTick, lastInputTick }, for starvation detection

    this.stats = {
      ticks: 0,
      snapshots: 0,
      replays: 0,
      replayedTicks: 0,
      exactMatches: 0,
      mispredictions: 0,        // genuine: same inputs, different answer
      starvedDivergences: 0,    // expected: the server simulated input we never sent
      serverRepeats: 0,
      hardResyncs: 0,
      noHistory: 0
    }
  }

  /** Advance one simulation tick with the given (already quantised) input. */
  step (frame) {
    this.tick += 1
    this.inputs.set(this.tick, frame)

    this.world.setInput(this.agent, frame)
    this.world.step()

    const s = this.world.readAgent(this.agent)
    this.predicted.set(this.tick, s.hash)
    this.stats.ticks += 1

    this._trim()
    return s
  }

  _trim () {
    const cutoff = this.tick - HISTORY
    if (cutoff <= 0) return
    for (const t of this.inputs.keys()) if (t <= cutoff) this.inputs.delete(t)
    for (const t of this.predicted.keys()) if (t <= cutoff) this.predicted.delete(t)
  }

  /**
   * Apply an authoritative snapshot: adopt, then replay.
   *
   * The snapshot's SelfBlock is the state AFTER the server applied input tick
   * `lastInputTick`, so history is keyed by the client's OWN input tick and no
   * mapping between the two clocks is needed. That is deliberate: a
   * client-tick-to-server-tick mapping is an extra thing to get wrong, and it
   * would have to be re-derived every time the clock-sync loop nudged.
   */
  reconcile (snapshot) {
    this.stats.snapshots += 1
    const t = snapshot.lastInputTick

    const authoritative = {
      pos: snapshot.self.pos,
      vel: snapshot.self.vel,
      facing: snapshot.self.facing,
      stateFlags: snapshot.self.stateFlags,
      jumpsUsed: snapshot.self.jumpsUsed,
      // jump_held is not replicated: it is derived from the input the server
      // applied, which the client has in its own history.
      jumpHeld: (this.inputs.get(t)?.buttons & 0x0001) !== 0,
      active: true
    }

    if (!this.synced) {
      this.world.writeAgent(this.agent, authoritative)
      this.synced = true
      this.lastAppliedTick = t
      this._replayFrom(t)
      return { firstSync: true }
    }

    if (snapshot.flags & 0x0001) {          // RESYNC_REQUIRED
      this.world.writeAgent(this.agent, authoritative)
      this.inputs.clear()
      this.predicted.clear()
      this.tick = t
      this.lastAppliedTick = t
      this.stats.hardResyncs += 1
      return { hardResync: true }
    }

    // Did the server run more ticks than it consumed inputs? If so it starved
    // and repeated the previous frame (server/src/session.hpp), simulating input
    // the client never sent and cannot have replayed. A hash mismatch is then
    // EXPECTED and is not evidence of a determinism problem -- so it is counted
    // separately, otherwise ordinary network jitter would be indistinguishable
    // from a real desync and the zero-mispredictions claim would be untestable.
    let serverRepeats = 0
    if (this.lastSnapshot) {
      const ticksAdvanced = snapshot.serverTick - this.lastSnapshot.serverTick
      const inputsAdvanced = t - this.lastSnapshot.lastInputTick
      serverRepeats = Math.max(0, ticksAdvanced - inputsAdvanced)
    }
    this.lastSnapshot = { serverTick: snapshot.serverTick, lastInputTick: t }
    this.stats.serverRepeats += serverRepeats

    // Compare the server's hash against what we predicted for that same input
    // tick. This is the measurement R3 exists for.
    let verdict = 'no-history'
    const mine = this.predicted.get(t)
    if (mine !== undefined) {
      if (mine === snapshot.self.stateHash) {
        this.stats.exactMatches += 1
        verdict = 'exact'
      } else if (serverRepeats > 0) {
        this.stats.starvedDivergences += 1
        verdict = 'starved'
      } else {
        this.stats.mispredictions += 1
        verdict = 'mispredicted'
      }
    } else {
      this.stats.noHistory += 1
    }

    // Adopt and replay regardless of the verdict. Even on an exact match this
    // is correct and nearly free -- and skipping it when the hashes agree would
    // mean the one path that matters is the one never exercised.
    this.world.writeAgent(this.agent, authoritative)
    this.lastAppliedTick = t
    this._replayFrom(t)

    this._steerClock(snapshot.inputMargin)
    return { verdict }
  }

  _replayFrom (t) {
    this.stats.replays += 1
    for (let k = t + 1; k <= this.tick; k++) {
      const frame = this.inputs.get(k)
      if (!frame) continue
      this.world.setInput(this.agent, frame)
      this.world.step()
      this.predicted.set(k, this.world.readAgent(this.agent).hash)
      this.stats.replayedTicks += 1
    }
  }

  // Steer the client's tick offset so the server keeps a small buffer of input
  // in hand. One tick at a time, never a snap: a client that jumps its clock
  // produces a burst of mispredictions indistinguishable from a determinism bug.
  _steerClock (margin) {
    if (margin < this.targetMargin) this.tickAdjust = 1
    else if (margin > this.targetMargin + 2) this.tickAdjust = -1
    else this.tickAdjust = 0
  }

  /** How many extra (or fewer) ticks to run this frame. Consumed by the caller. */
  takeClockAdjust () {
    const a = this.tickAdjust
    this.tickAdjust = 0
    return a
  }

  state () { return this.world.readAgent(this.agent) }
  grounded () { return (this.state().stateFlags & STATE_FLAG.GROUNDED) !== 0 }
}
