//
// Client-side prediction and reconciliation, end to end against the real server.
//
// This is the test the whole fixed-point contract was built to make possible.
// The client runs the SAME simulation code as the server, compiled to wasm, and
// replays unacknowledged input after each snapshot. Because there is no
// numerical noise floor, the correct number of mispredictions under clean
// conditions is not "small" -- it is ZERO, exactly, and anything else is a real
// defect rather than drift to be tuned away.
//
// The assertions are therefore absolute where they can be. A test that accepted
// "mostly matching" would pass with a genuine desync in it.
//
import { spawn } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { Core } from '../../../client/src/core.js'
import { Predictor } from '../../../client/src/predictor.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  fxToNumber, numberToFx, MSG, BUTTON
} from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
// Ports and impairment come from the environment so tools/netsim/matrix.js can
// drive this same test across a condition matrix without forking it.
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34438)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34512)
const NETSIM = {
  latencyMs: Number(process.env.DNP_LATENCY_MS ?? 0),
  jitterMs: Number(process.env.DNP_JITTER_MS ?? 0),
  lossPct: Number(process.env.DNP_LOSS_PCT ?? 0),
  reorderPct: Number(process.env.DNP_REORDER_PCT ?? 0),
  seed: Number(process.env.DNP_SEED ?? 1)
}
const TICKS = Number(process.env.DNP_TICKS ?? 420)

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const withTimeout = (p, ms, label) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`timeout: ${label}`)), ms))])
const waitFor = async (fn, ms, label) => {
  const deadline = Date.now() + ms
  while (Date.now() < deadline) {
    if (fn()) return true
    await sleep(20)
  }
  throw new Error(`timeout: ${label}`)
}

// ------------------------------------------------------------------- processes

const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'),
                     ['--port', String(LINK_PORT)], { stdio: ['ignore', 'inherit', 'pipe'] })
let serverLog = ''
server.stderr.on('data', (d) => { serverLog += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })

await waitFor(() => serverLog.includes('listening for edge'), 5000, 'server listening')

const edge = new Edge({
  port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  netsim: NETSIM,
  log: () => {}
})
await edge.start()
await waitFor(() => serverLog.includes('edge connected'), 8000, 'edge link')

// --------------------------------------------------------------- wasm core

console.log(`\n  conditions: ${edge.up.describe()}  (seed ${NETSIM.seed}, ${TICKS} ticks)\n`)

const core = await Core.load()
check(true, 'wasm simulation core loaded')

// Sanity: the same input sequence from the same state must give the same result
// twice. If this fails, nothing downstream is meaningful.
{
  const w1 = core.createWorld()
  const w2 = core.createWorld()
  const a1 = w1.spawn({ z: numberToFx(2) })
  const a2 = w2.spawn({ z: numberToFx(2) })
  for (let i = 0; i < 200; i++) {
    const f = { buttons: i % 17 === 0 ? BUTTON.JUMP : 0, moveX: (i * 997) % 32767, moveY: -(i * 331) % 32767, aim: i * 7919 }
    w1.setInput(a1, f); w1.step()
    w2.setInput(a2, f); w2.step()
  }
  check(w1.readAgent(a1).hash === w2.readAgent(a2).hash,
        'wasm core is self-consistent over 200 ticks')
  w1.destroy(); w2.destroy()
}

// ----------------------------------------------------------------- client

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

class PredictingClient {
  constructor () {
    this.snapshots = []
    this.welcome = null
    this.pending = []
  }

  async connect () {
    this.wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
      serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
    })
    await withTimeout(this.wt.ready, 8000, 'client ready')
    this.control = await this.wt.createBidirectionalStream()
    this.controlWriter = this.control.writable.getWriter()
    this.dgWriter = this.wt.datagrams.createWritable().getWriter()
    this._pumpControl()
    this._pumpDatagrams()
    await this.controlWriter.write(encodeHello())
    await waitFor(() => this.welcome, 8000, 'welcome')

    // The client's own world holds only its own agent. Remote agents are
    // interpolated from snapshots, never simulated locally -- their positions
    // are stale by construction (docs/protocol.md 11).
    this.world = core.createWorld()
    this.agent = this.world.spawn({ z: numberToFx(2) })
    this.predictor = new Predictor(this.world, this.agent)
  }

  async _pumpControl () {
    const r = this.control.readable.getReader()
    let buf = Buffer.alloc(0)
    try {
      for (;;) {
        const { value, done } = await r.read()
        if (done) break
        buf = Buffer.concat([buf, Buffer.from(value)])
        while (buf.length >= 4) {
          const len = buf.readUInt32LE(0)
          if (buf.length < 4 + len) break
          const msg = decodeControlMessage(buf.subarray(0, 4 + len))
          buf = buf.subarray(4 + len)
          if (msg?.type === MSG.WELCOME) this.welcome = msg
        }
      }
    } catch {}
  }

  async _pumpDatagrams () {
    const r = this.wt.datagrams.readable.getReader()
    try {
      for (;;) {
        const { value, done } = await r.read()
        if (done) break
        const s = decodeSnapshot(Buffer.from(value))
        if (s) { this.snapshots.push(s); this.pending.push(s) }
      }
    } catch {}
  }

  /** One client frame: predict locally, then send. */
  async frame (rawInput) {
    // Quantise through the CORE's function, then predict with the quantised
    // value (protocol R1). Predicting with the raw analogue reading and sending
    // the quantised one is the classic way to diverge on every single tick.
    const frame = {
      buttons: rawInput.buttons ?? 0,
      moveX: core.axisFromFxRaw(numberToFx(rawInput.moveX ?? 0)),
      moveY: core.axisFromFxRaw(numberToFx(rawInput.moveY ?? 0)),
      aim: rawInput.aim ?? 0
    }

    this.predictor.step(frame)

    this.history = this.history ?? []
    this.history.unshift(frame)
    this.history = this.history.slice(0, 12)

    await this.dgWriter.write(encodeInputBundle({
      clientTick: this.predictor.tick,
      ackSnapshotTick: this.snapshots.at(-1)?.serverTick ?? 0,
      frames: this.history
    }))
  }

  /** Apply everything that arrived since the last call. */
  drain () {
    const out = []
    while (this.pending.length) out.push(this.predictor.reconcile(this.pending.shift()))
    return out
  }
}

const c = new PredictingClient()
await c.connect()
check(true, 'predicting client connected and got WELCOME')

// --- warm up: let the first snapshot establish the authoritative baseline -----

for (let i = 0; i < 30; i++) { await c.frame({}); c.drain(); await sleep(16) }
await waitFor(() => c.predictor.synced, 5000, 'first sync')
check(c.predictor.synced, 'client adopted authoritative state on first snapshot')

// --- the measurement ---------------------------------------------------------

// Run a long, varied session: movement, jumps, direction changes -- the kinds of
// input that exercise edge detection and the grounded transition, not just
// steady-state coasting.
c.predictor.stats.exactMatches = 0
c.predictor.stats.mispredictions = 0
c.predictor.stats.starvedDivergences = 0
c.predictor.stats.serverRepeats = 0
c.predictor.stats.noHistory = 0

for (let i = 0; i < TICKS; i++) {
  const phase = Math.floor(i / 60)
  const input = {
    moveX: [0, 1, -1, 0.5, -0.25, 1, 0][phase % 7],
    moveY: [0, 0, 0.5, -1, 0, 0.75, -0.5][phase % 7],
    buttons: (i % 47 === 0) ? BUTTON.JUMP : 0,
    aim: (i * 7919) >>> 0
  }
  await c.frame(input)
  c.drain()

  // Honour the clock-sync loop so the server keeps a small input buffer and
  // does not starve. Starvation makes the server repeat a frame the client
  // never replays, which IS a legitimate divergence -- and would muddy exactly
  // the measurement being taken here.
  const adjust = c.predictor.takeClockAdjust()
  if (adjust > 0) await c.frame(input)

  await sleep(15)
}
await sleep(300)
c.drain()

const st = c.predictor.stats
console.log(`\n  ticks ${st.ticks}  snapshots ${st.snapshots}  replays ${st.replays} (${st.replayedTicks} ticks)`)
console.log(`  exact ${st.exactMatches}  mispredicted ${st.mispredictions}  ` +
            `starved ${st.starvedDivergences} (server repeated ${st.serverRepeats} frames)  ` +
            `no-history ${st.noHistory}\n`)

const ns = edge.netsimReport()
console.log(`  netsim uplink   ${ns.up}`)
console.log(`  netsim downlink ${ns.down}\n`)

// Under loss the client simply receives fewer snapshots, so the number of
// comparisons scales down with the conditions. Requiring a fixed count would
// make the test fail for the wrong reason at high loss.
check(st.exactMatches > 20, `reconciliation compared ${st.exactMatches} snapshots against local prediction`)

// THE assertion. Not "few", not "within tolerance" -- zero. Fixed-point
// determinism means client and server compute identical results from identical
// inputs, so any mismatch here is a real defect.
// Divergences caused by server starvation are excluded and reported separately:
// there the server simulated a repeated input frame the client never sent, so a
// mismatch is correct behaviour, not a determinism failure.
check(st.mispredictions === 0,
      `ZERO genuine mispredictions over ${st.ticks} ticks (got ${st.mispredictions})`)
check(st.exactMatches > st.starvedDivergences,
      `starvation stays the exception (${st.starvedDivergences} starved vs ${st.exactMatches} exact)`)

check(st.hardResyncs === 0, 'no hard resyncs were needed')
console.log(`\nRESULT ${JSON.stringify({
  conditions: edge.up.describe(),
  seed: NETSIM.seed,
  ticks: st.ticks,
  exact: st.exactMatches,
  mispredicted: st.mispredictions,
  starved: st.starvedDivergences,
  serverRepeats: st.serverRepeats,
  replayedTicks: st.replayedTicks,
  hardResyncs: st.hardResyncs,
  dropped: edge.up.stats.dropped + edge.down.stats.dropped
})}`)
check(st.replayedTicks > 0, `replay actually ran (${st.replayedTicks} ticks re-simulated)`)

// --- the prediction is also CORRECT, not merely self-consistent ---------------

// A client that never sent input and never simulated would also report zero
// mispredictions. Confirm the predicted state is genuinely tracking the server.
{
  const predicted = c.predictor.state()
  const authoritative = c.snapshots.at(-1).self
  const dx = Math.abs(fxToNumber(predicted.pos.x) - fxToNumber(authoritative.pos.x))
  const dy = Math.abs(fxToNumber(predicted.pos.y) - fxToNumber(authoritative.pos.y))
  console.log(`  predicted (${fxToNumber(predicted.pos.x).toFixed(3)}, ${fxToNumber(predicted.pos.y).toFixed(3)}) ` +
              `vs authoritative (${fxToNumber(authoritative.pos.x).toFixed(3)}, ${fxToNumber(authoritative.pos.y).toFixed(3)})`)
  // They differ by the input the server has not applied yet, which is bounded by
  // move_speed * (ticks in flight). It must not be zero (that would mean nothing
  // was predicted) nor large (that would mean prediction had run away).
  // The lead is bounded by move_speed * ticks-in-flight, so it must scale with
  // the round trip rather than being a fixed number.
  const inFlightTicks = 4 + (NETSIM.latencyMs * 2 / 1000) * 60
  const bound = (8 / 60) * inFlightTicks + 2
  check(dx < bound && dy < bound,
        `predicted position leads the authoritative one by a bounded amount (${dx.toFixed(2)}, ${dy.toFixed(2)} < ${bound.toFixed(2)})`)
  check(Math.abs(fxToNumber(predicted.pos.x)) > 0.1, 'the agent actually moved (prediction is not trivially idle)')
}

// --- a deliberately induced divergence MUST be detected ----------------------

// Without this, "zero mispredictions" could just mean the detector never fires.
//
// The injected fault is a MISMATCHED INPUT, not corrupted state. Corrupting
// local state does not work as a test: reconciliation adopts the authoritative
// state and re-derives every predicted hash from it, so the corruption is healed
// before any snapshot can be compared against it -- which is exactly what
// reconciliation is for. Poisoning the client's stored copy of an input it
// already sent survives that, because the replay keeps re-simulating the wrong
// frame. It is also the failure mode most likely to happen for real: protocol R1
// violations, where the client predicts with a different input than it sent.
{
  const before = c.predictor.stats.mispredictions + c.predictor.stats.starvedDivergences

  await c.frame({ moveX: 0.5 })
  const t = c.predictor.tick
  const poisoned = { ...c.predictor.inputs.get(t) }
  poisoned.moveX += 1        // one Q1.15 step: the smallest possible disagreement
  c.predictor.inputs.set(t, poisoned)

  for (let i = 0; i < 60; i++) { await c.frame({ moveY: 0.3 }); c.drain(); await sleep(15) }
  await sleep(500)
  c.drain()

  const after = c.predictor.stats.mispredictions + c.predictor.stats.starvedDivergences
  check(after > before,
        `a ONE-STEP input disagreement is detected (${after - before} divergences)`)
}

try { c.wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')

console.log(`\nprediction: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
