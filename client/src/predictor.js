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
import { STATE_FLAG, SNAP_FLAG } from './proto.js'

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
    // tick -> { hash, epoch }. The epoch matters as much as the hash: a tick
    // predicted under one parameter generation and confirmed under another was
    // computed from different constants on the two sides, and comparing hashes
    // alone reports that as a defect. docs/protocol.md §8.4 says a client MUST
    // NOT apply parameters retroactively to ticks before effective_tick; this
    // is how that is honoured when the change lands mid-replay.
    this.predicted = new Map()

    this.synced = false           // adopted an authoritative state yet?
    this.lastAppliedTick = 0

    // Clock sync (protocol 10.1). The client steers its tick offset to keep a
    // small positive margin of buffered input at the server.
    this.targetMargin = 2
    this.tickAdjust = 0           // ticks to skip (-) or duplicate (+) next frame

    this.lastSnapshot = null      // { serverTick, lastInputTick }, for starvation detection

    // Which generation of the movement parameters we have applied
    // (docs/protocol.md §8.4). Compared against the snapshot's, because the
    // reliable and unreliable channels are not ordered against each other and a
    // snapshot computed with new rules can arrive before the rules do.
    this.paramEpoch = 0

    // Consecutive snapshots whose epoch disagreed with ours.
    //
    // A parameter update in flight is transient by definition: the reliable
    // channel delivers, and the disagreement stops. One that never stops means
    // the update was lost or is being ignored, and THAT is a defect -- a client
    // silently running different physics from the server. Without this counter
    // such a client looks healthy, because every mismatch it produces is
    // truthfully classified as "stale" forever.
    this.paramsStaleRun = 0
    this.paramsStaleLimit = 12

    // Test hook for the control: a client that ignores LUA_APPLIED must report
    // the scripted change as a defect. If it did not, the flag would be
    // carrying no information and could be deleted.
    this.ignoreLuaFlag = false

    this.stats = {
      ticks: 0,
      snapshots: 0,
      replays: 0,
      replayedTicks: 0,
      exactMatches: 0,
      mispredictions: 0,        // genuine: same inputs, different answer
      starvedDivergences: 0,    // expected: the server simulated input we never sent
      paramsStale: 0,           // expected: our rules are one generation behind
      paramsStuck: 0,           // NOT expected: they have been behind too long
      luaApplied: 0,            // expected: the server ran gameplay logic we cannot predict
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
    this.predicted.set(this.tick, { hash: s.hash, epoch: this.paramEpoch })
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

    const sb = snapshot.self
    const held = this.inputs.get(t)?.buttons ?? 0
    const authoritative = {
      pos: sb.pos,
      vel: sb.vel,
      facing: sb.facing,
      stateFlags: sb.stateFlags,
      mode: sb.controllerMode,
      jumpsUsed: sb.jumpsUsed,
      dashesUsed: sb.dashesUsed,
      sector: sb.sector,
      dashTimer: sb.dashTimer,
      dashCooldown: sb.dashCooldown,
      hoverFuel: sb.hoverFuel,
      flightFuel: sb.flightFuel,
      grappleAnchor: sb.grappleAnchor,
      grappleDist: sb.grappleDist,
      grounded: (sb.stateFlags & STATE_FLAG.GROUNDED) !== 0,
      // The sector this entity rides, and whether it is riding it. Both are
      // controller state carried between ticks, so a replay starting from the
      // wrong anchor diverges until the entity happens to land again.
      anchor: sb.anchor,
      anchored: (sb.stateFlags & STATE_FLAG.ANCHORED) !== 0,
      climbing: (sb.stateFlags & STATE_FLAG.CLIMBING) !== 0,
      grappling: (sb.stateFlags & STATE_FLAG.GRAPPLING) !== 0,

      // Held-button flags are NOT replicated: they are edge-detection state,
      // and the client already holds the input it sent for the acknowledged
      // tick. Deriving them keeps bytes off every snapshot.
      //
      // Every edge-triggered button has to appear here. One that does not is
      // silently cleared on each reconcile, and then re-fires on the tick after
      // -- a door worked once a snapshot, a swing started thirty times a
      // second. interact and attack were both missing.
      jumpHeld: (held & 0x0001) !== 0,
      dashHeld: (held & 0x0002) !== 0,
      grappleHeld: (held & 0x0020) !== 0,
      interactHeld: (held & 0x0040) !== 0,
      attackHeld: (held & 0x0080) !== 0,

      // The swing countdown IS replicated, because it cannot be derived: the
      // server decides when a swing ends, and the client holds only the button
      // that started it. Without it every reconcile reset the countdown to
      // zero, so the collider was drawn for the two frames between snapshots
      // and never seen to travel -- which reads as a swing that does not move.
      attackLeft: sb.attackLeft ?? 0,
      attackCooldownLeft: sb.attackCooldownLeft ?? 0,
      active: true
    }

    if (!this.synced) {
      this.world.writeAgent(this.agent, authoritative)
      // Adopt the server's clock along with its state. Parametric sector motion
      // is a pure function of the tick, so a client running its own zero-based
      // counter would evaluate every moving platform at the wrong point in its
      // schedule.
      this.world.tick = snapshot.serverTick
      this.synced = true
      this.lastAppliedTick = t
      this._replayFrom(t)
      return { firstSync: true }
    }

    if (snapshot.flags & 0x0001) {          // RESYNC_REQUIRED
      this.world.writeAgent(this.agent, authoritative)
      this.world.tick = snapshot.serverTick
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
    // The FIRST comparable snapshot has nothing to measure advancement against.
    //
    // Starvation is detected by comparing how far the server's clock moved with
    // how far its consumption of our input moved, and both need a previous
    // snapshot. On the first one the answer is not "zero", it is "unknown" --
    // and a client that has just joined is precisely when the server is most
    // likely to have run ticks with no input from it, because our first
    // datagram has not finished its round trip.
    //
    // Treated as starved rather than as a defect, because calling it a defect
    // asserts something this data cannot support. It costs one snapshot of
    // detector sensitivity per session and removes a false positive that
    // appeared in roughly one netcode run in three.
    const firstComparison = this.lastSnapshot === null
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
    const snapEpoch = snapshot.self.paramEpoch ?? 0

    // The stale RUN tracks delivery -- is our current parameter set behind the
    // server's, and for how long. That is a different question from the
    // per-tick epoch below, which asks whether one particular comparison is
    // apples to apples.
    if (snapEpoch === this.paramEpoch) this.paramsStaleRun = 0

    const mine = this.predicted.get(t)
    if (mine !== undefined) {
      if (mine.hash === snapshot.self.stateHash) {
        this.stats.exactMatches += 1
        verdict = 'exact'
      } else if (serverRepeats > 0 || firstComparison) {
        this.stats.starvedDivergences += 1
        verdict = 'starved'
      } else if (!this.ignoreLuaFlag && (snapshot.flags & SNAP_FLAG.LUA_APPLIED) !== 0) {
        // Server-side gameplay logic touched THIS entity since the last
        // snapshot we received (docs/protocol.md §10.1.3). The client never
        // runs server Lua, by design, so this tick was never predictable.
        //
        // Expected -- but the SHAPE is what makes it distinguishable from a
        // real fault: a scripted change mismatches on one tick and then
        // self-heals, because the adopt-and-replay below re-derives everything
        // after it. A determinism regression never stops.
        this.stats.luaApplied += 1
        verdict = 'lua-applied'
      } else if (mine.epoch !== snapEpoch || snapEpoch !== this.paramEpoch) {
        // Either this tick was predicted under a different parameter
        // generation than the server used for it, or our current set is still
        // behind. Both mean the two sides computed correct physics from
        // different constants (docs/protocol.md §10.1.2), which is not a
        // defect -- but is counted apart from starvation, because the two have
        // opposite fixes: starvation is a clock-sync problem and this is a
        // delivery problem.
        this.paramsStaleRun += 1
        if (this.paramsStaleRun > this.paramsStaleLimit) {
          // Long past "in flight". The update is not coming, so this client is
          // running different physics from the server and saying nothing about
          // it. Escalate rather than keep excusing it.
          this.stats.paramsStuck += 1
          verdict = 'params-stuck'
        } else {
          this.stats.paramsStale += 1
          verdict = 'params-stale'
        }
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
    // The snapshot's state is the world AFTER server tick `serverTick`, so the
    // replay below must resume from exactly there for sector motion to line up.
    this.world.tick = snapshot.serverTick
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
      this.predicted.set(k, { hash: this.world.readAgent(this.agent).hash, epoch: this.paramEpoch })
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
