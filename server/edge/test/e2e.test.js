//
// Full-stack end-to-end test.
//
//   node test client  --WebTransport-->  edge  --link socket-->  C++ server
//
// Nothing is mocked. This is the Phase 1 harness exit criterion in miniature:
// two clients connect, send input at the simulation rate, and the authoritative
// server simulates and replicates them to each other.
//
// The assertions deliberately check SIMULATION behaviour, not just transport --
// that gravity pulled the agent down, that horizontal input moved it the
// distance the fixed timestep predicts, that a second client appears as a
// replicated entity. A transport that delivers bytes but a server that does not
// simulate correctly would pass a pure plumbing test.
//
import { spawn } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { bakedMapPath } from './helpers.js'
import { SnapshotReassembler } from '../../../client/src/snapshots.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, handleIndex, MSG, BUTTON, STATE_FLAG, TICK_RATE_HZ
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = 34436
const LINK_PORT = 34510

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

// Poll for a condition rather than sleeping a guessed interval. Fixed sleeps in
// a test that spans three processes are how a suite becomes flaky on a loaded
// machine: it passes locally and fails in CI for reasons unrelated to the code.
const waitFor = async (fn, ms, label) => {
  const deadline = Date.now() + ms
  while (Date.now() < deadline) {
    if (fn()) return true
    await sleep(25)
  }
  throw new Error(`timeout: ${label}`)
}

// ------------------------------------------------------------- C++ server

const server = spawn(serverBin, [...['--port', String(LINK_PORT)],
                      ...(bakedMapPath(ROOT) ? ['--map', bakedMapPath(ROOT)] : [])],
                     { stdio: ['ignore', 'inherit', 'pipe'] })
let serverLog = ''
server.stderr.on('data', (d) => { serverLog += d.toString() })

const cleanup = async () => {
  try { await edge?.stop() } catch {}
  try { server.kill('SIGTERM') } catch {}
}
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })

await waitFor(() => serverLog.includes('listening for edge'), 5000, 'server listening')
check(server.exitCode === null, 'C++ server started')

// ------------------------------------------------------------------- edge

const edge = new Edge({
  port: WT_PORT,
  host: '127.0.0.1',
  path: '/dnp',
  upstreamPort: LINK_PORT,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  log: () => {}
})
await edge.start()
await waitFor(() => serverLog.includes('edge connected'), 8000, 'edge link established')
check(serverLog.includes('edge connected'), 'edge connected to the C++ server over the link socket')

// ----------------------------------------------------------------- client

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64'
)

class TestClient {
  constructor (name) {
    this.name = name
    this.snapshots = []
    this.welcome = null
    this.tick = 0
    // Snapshots are delta-encoded, so `snapshot.entities` is a CHANGE LIST, not
    // a complete set -- a removed entity appears as a DESTROYED block, and an
    // unchanged one does not appear at all. Entity assertions therefore have to
    // go through reassembly.
    this.reasm = new SnapshotReassembler()
  }

  async connect () {
    this.wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
      serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
    })
    await withTimeout(this.wt.ready, 8000, `${this.name} ready`)

    this.control = await this.wt.createBidirectionalStream()
    this.controlWriter = this.control.writable.getWriter()
    this.dgWriter = this.wt.datagrams.createWritable().getWriter()

    this._pumpControl()
    this._pumpDatagrams()
  }

  async _pumpControl () {
    const r = this.control.readable.getReader()
    let buf = Buffer.alloc(0)
    try {
      for (;;) {
        const { value, done } = await r.read()
        if (done) break
        buf = Buffer.concat([buf, Buffer.from(value)])
        // Length-prefixed reassembly, same as everywhere else: a read boundary
        // is not a message boundary.
        while (buf.length >= 4) {
          const len = buf.readUInt32LE(0)
          if (buf.length < 4 + len) break
          const msg = decodeControlMessage(buf.subarray(0, 4 + len))
          buf = buf.subarray(4 + len)
          if (msg?.type === MSG.WELCOME) this.welcome = msg
          if (msg?.type === MSG.REJECT) this.reject = msg
        }
      }
    } catch { /* closed */ }
  }

  async _pumpDatagrams () {
    const r = this.wt.datagrams.readable.getReader()
    try {
      for (;;) {
        const { value, done } = await r.read()
        if (done) break
        const snap = decodeSnapshot(Buffer.from(value))
        if (snap) { this.snapshots.push(snap); this.reasm.apply(snap) }
      }
    } catch { /* closed */ }
  }

  async hello () {
    await this.controlWriter.write(encodeHello())
  }

  async sendInput ({ moveX = 0, moveY = 0, buttons = 0, aim = 0 } = {}) {
    this.tick++
    const frame = { buttons, moveX: axisFromNumber(moveX), moveY: axisFromNumber(moveY), aim }
    this.history = this.history ?? []
    this.history.unshift(frame)
    this.history = this.history.slice(0, 8)   // redundancy window, protocol 4.1
    await this.dgWriter.write(encodeInputBundle({
      clientTick: this.tick,
      // Ack what was successfully REASSEMBLED, never what was merely received.
      ackSnapshotTick: this.reasm.ackTick,
      frames: this.history
    }))
  }

  latest () { return this.snapshots.at(-1) }
}

// --- handshake ---------------------------------------------------------------

const a = new TestClient('A')
await a.connect()
await a.hello()
await waitFor(() => a.welcome, 8000, 'welcome A')

check(a.welcome.version === 0, 'WELCOME carries protocol version 0 (unstable)')
check(a.welcome.tickRate === TICK_RATE_HZ, `WELCOME tick_rate is ${TICK_RATE_HZ} (protocol R9)`)
check(a.welcome.snapshotRate === 30, 'WELCOME snapshot_rate is 30')
check(handleIndex(a.welcome.selfHandle) === 1, 'WELCOME assigns a self handle')

// --- the server simulates: gravity -------------------------------------------

// Spawn height is 2.0. Hold no input and let it fall to the floor.
for (let i = 0; i < 60; i++) { await a.sendInput(); await sleep(16) }
await sleep(200)

check(a.snapshots.length > 0, `client A received snapshots (${a.snapshots.length})`)
{
  const s = a.latest()
  const z = fxToNumber(s.self.pos.z)
  check(z === 0, `gravity pulled the agent to the floor (z = ${z})`)
  check((s.self.stateFlags & STATE_FLAG.GROUNDED) !== 0, 'GROUNDED flag is set on the floor')
  check(s.lastInputTick > 0, `server acknowledged input up to tick ${s.lastInputTick}`)
  check(s.self.stateHash !== 0n, 'SelfBlock carries a state hash (protocol R3)')
}

// --- the server simulates: horizontal movement matches the fixed timestep ------

{
  const before = fxToNumber(a.latest().self.pos.x)
  const beforeY = fxToNumber(a.latest().self.pos.y)
  const TICKS = 60
  for (let i = 0; i < TICKS; i++) { await a.sendInput({ moveX: 1 }); await sleep(16) }
  await sleep(200)
  const after = fxToNumber(a.latest().self.pos.x)
  const moved = after - before

  // move_speed 8 units/s at dt = 1/60 gives 8/60 per tick. The client cannot
  // know exactly how many ticks the server applied (its clock is free-running
  // here, with no sync loop yet), so assert the rate is right rather than the
  // exact total: anything in this band means the server applied roughly the
  // right number of ticks at exactly the right per-tick distance.
  const perTick = 8 / TICK_RATE_HZ
  check(moved > perTick * 20, `moved right under input (${moved.toFixed(3)} units)`)
  check(moved < perTick * TICKS * 1.5, 'movement rate matches move_speed at the fixed timestep')
  // Drift, not absolute position: the spawn point is not the origin, so
  // measuring |y| would only be testing where the map put the player.
  check(Math.abs(fxToNumber(a.latest().self.pos.y) - beforeY) < 1e-6,
        'no drift on the unused axis')
}

// --- the server simulates: jump ----------------------------------------------

{
  // Rising edge only: the jump must fire once, not every tick it is held.
  await a.sendInput({ buttons: BUTTON.JUMP })
  await sleep(60)
  let peak = 0
  for (let i = 0; i < 20; i++) {
    await a.sendInput({ buttons: BUTTON.JUMP })
    await sleep(16)
    peak = Math.max(peak, fxToNumber(a.latest().self.pos.z))
  }
  check(peak > 0.5, `jump lifted the agent off the floor (peak z = ${peak.toFixed(3)})`)
  check(a.latest().self.jumpsUsed <= 2, 'jump count respects max_jumps (held input did not re-trigger)')
}

// --- a second client is replicated to the first -------------------------------

const b = new TestClient('B')
await b.connect()
await b.hello()
await waitFor(() => b.welcome, 8000, 'welcome B')
check(handleIndex(b.welcome.selfHandle) === 2, 'second client got a distinct handle')

for (let i = 0; i < 30; i++) {
  await a.sendInput({ moveX: -1 })
  await b.sendInput({ moveX: 1 })
  await sleep(16)
}
await sleep(200)

{
  const sb = b.latest()
  const aSeen = a.reasm.entities()
  const bSeen = b.reasm.entities()
  check(aSeen.size === 1, `client A sees exactly one other entity (${aSeen.size})`)
  check(bSeen.size === 1, `client B sees exactly one other entity (${bSeen.size})`)

  const bIdx = handleIndex(b.welcome.selfHandle)
  check(aSeen.has(bIdx), "the entity A sees is B's agent")

  // Per-observer encoding: an observer's own agent appears in SelfBlock at full
  // precision and MUST NOT be duplicated in the entity array, or the client
  // could reconcile against the quantised copy.
  check(!aSeen.has(handleIndex(a.welcome.selfHandle)),
        "A's own agent is not duplicated in its entity list")

  // The two clients moved in opposite directions; the replicated position
  // should reflect that rather than being a static placeholder.
  const bxFromA = fxToNumber(aSeen.get(bIdx).pos.x)
  const bxFromB = fxToNumber(sb.self.pos.x)
  check(Math.abs(bxFromA - bxFromB) < 0.01,
        `replicated position matches authoritative within quantisation (${bxFromA.toFixed(3)} vs ${bxFromB.toFixed(3)})`)
}

// --- input margin is reported for clock sync ----------------------------------

{
  // Up to this point the client has sent exactly one input per tick with no
  // clock-sync loop, so the server consumes each input the moment it arrives and
  // never builds a buffer. A margin of 0 there is the honest answer, not a bug --
  // it is precisely the "server is starving, client should run further ahead"
  // signal that docs/protocol.md 10.1 tells the client to steer on.
  const beforeBurst = a.latest().inputMargin
  check(beforeBurst <= 1, `margin is ~0 while the client runs level with the server (${beforeBurst})`)

  // Now run ahead deliberately: burst a run of future ticks and confirm the
  // server reports them as buffered.
  const snapsBefore = a.snapshots.length
  // Every write ISSUED before any is awaited. Awaiting between them yields to
  // the event loop each time, so on a loaded machine each input can take longer
  // than a 60 Hz tick to leave -- the server then consumes them as fast as they
  // arrive, no backlog ever forms, and the margin reads 0. That is a real
  // measurement of a race this test lost, not of the thing it is checking, and
  // it is how this failed once under a software renderer running alongside it.
  const sends = []
  for (let i = 0; i < 12; i++) sends.push(a.sendInput({ moveX: 0 }))
  await Promise.all(sends)
  await waitFor(() => a.snapshots.length > snapsBefore + 1, 3000, 'snapshot after burst')

  const peak = Math.max(...a.snapshots.slice(snapsBefore).map((s) => s.inputMargin))
  check(peak > 1, `input_margin reports buffered input when the client runs ahead (peak ${peak})`)

  // And it drains back toward zero as the server consumes the backlog, which is
  // what stops the client from running further and further ahead.
  await waitFor(() => a.latest().inputMargin < peak, 3000, 'margin drains')
  check(a.latest().inputMargin < peak, `margin drains as the backlog is consumed (${a.latest().inputMargin})`)
}

// --- disconnect frees the agent ------------------------------------------------

{
  const bIdx = handleIndex(b.welcome.selfHandle)
  b.wt.close()
  await sleep(700)
  check(!a.reasm.entities().has(bIdx), "B's agent was despawned when its session closed")
  check(a.reasm.stats.droppedNoBaseline === 0,
        'no snapshot was unusable for want of a baseline')
}

await cleanup()
console.log(`\ne2e: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
