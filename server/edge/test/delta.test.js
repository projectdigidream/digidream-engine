//
// Snapshot delta compression, end to end.
//
// The dangerous property of a delta encoder is that it fails QUIETLY. If the
// server ever deltas against state the client does not hold, entities freeze at
// stale positions, or inherit a dead entity's data, and nothing anywhere reports
// an error -- it just looks like a physics or interpolation bug much later.
//
// So correctness is asserted first and bandwidth second: a reconstructed entity
// set is compared against the authoritative position the OTHER client reports
// for itself, which is an independent source of truth rather than a re-derivation
// of the same data.
//
import { spawn } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { SnapshotReassembler } from '../../../client/src/snapshots.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, handleIndex, handleGeneration, MSG
} from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34442)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34516)
const LOSS = Number(process.env.DNP_LOSS_PCT ?? 0)

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const withTimeout = (p, ms, l) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`timeout: ${l}`)), ms))])
const waitFor = async (fn, ms, l) => {
  const end = Date.now() + ms
  while (Date.now() < end) { if (fn()) return true; await sleep(20) }
  throw new Error(`timeout: ${l}`)
}

const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'),
                     ['--port', String(LINK_PORT)], { stdio: ['ignore', 'inherit', 'pipe'] })
let serverLog = ''
server.stderr.on('data', (d) => { serverLog += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })
await waitFor(() => serverLog.includes('listening for edge'), 5000, 'server')

const edge = new Edge({
  port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  netsim: { lossPct: LOSS, seed: 5 },
  log: () => {}
})
await edge.start()
await waitFor(() => serverLog.includes('edge connected'), 8000, 'edge')

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

class Client {
  constructor (name) {
    this.name = name
    this.welcome = null
    this.tick = 0
    this.bytes = 0
    this.snapCount = 0
    this.reasm = new SnapshotReassembler()
    this.selfPos = null
    this.baselineTicksSeen = []
  }

  async connect () {
    this.wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
      serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
    })
    await withTimeout(this.wt.ready, 8000, `${this.name} ready`)
    this.control = await this.wt.createBidirectionalStream()
    this.cw = this.control.writable.getWriter()
    this.dw = this.wt.datagrams.createWritable().getWriter()
    this._pumpControl(); this._pumpDatagrams()
    await this.cw.write(encodeHello())
    await waitFor(() => this.welcome, 8000, `${this.name} welcome`)
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
          const m = decodeControlMessage(buf.subarray(0, 4 + len))
          buf = buf.subarray(4 + len)
          if (m?.type === MSG.WELCOME) this.welcome = m
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
        const raw = Buffer.from(value)
        const s = decodeSnapshot(raw)
        if (!s) continue
        this.bytes += raw.length
        this.snapCount++
        this.baselineTicksSeen.push(s.baselineTick)
        this.selfPos = s.self.pos
        this.reasm.apply(s)
      }
    } catch {}
  }

  async send ({ moveX = 0, moveY = 0 } = {}) {
    this.tick++
    const f = { buttons: 0, moveX: axisFromNumber(moveX), moveY: axisFromNumber(moveY), aim: 0 }
    this.hist = [f, ...(this.hist ?? [])].slice(0, 8)
    // Ack the newest snapshot successfully REASSEMBLED, not merely received.
    // Acking an unusable snapshot would tell the server we hold a baseline we
    // do not, and every subsequent delta would be computed against fiction.
    await this.dw.write(encodeInputBundle({
      clientTick: this.tick, ackSnapshotTick: this.reasm.ackTick, frames: this.hist
    }))
  }
}

const a = new Client('A'); await a.connect()
const b = new Client('B'); await b.connect()
check(true, 'two clients connected')

// --- drive them apart so entity state genuinely changes ----------------------

for (let i = 0; i < 120; i++) {
  await a.send({ moveX: 1, moveY: Math.sin(i / 10) })
  await b.send({ moveX: -1, moveY: Math.cos(i / 10) })
  await sleep(15)
}
await sleep(300)

// --- delta encoding is actually happening ------------------------------------

{
  const deltas = a.baselineTicksSeen.filter((t) => t !== 0).length
  const fulls = a.baselineTicksSeen.filter((t) => t === 0).length
  check(deltas > 0, `server sent delta snapshots (${deltas} delta, ${fulls} full)`)
  check(a.reasm.stats.delta > 0, `client reassembled ${a.reasm.stats.delta} deltas`)
  check(a.reasm.stats.droppedNoBaseline === 0,
        `no snapshot was dropped for a missing baseline (${a.reasm.stats.droppedNoBaseline})`)
}

// --- reconstruction is CORRECT, checked against an independent source --------

{
  const bIdx = handleIndex(b.welcome.selfHandle)
  const seen = a.reasm.entities().get(bIdx)
  check(seen !== undefined, "A's reassembled set contains B")
  check(seen?.generation === handleGeneration(b.welcome.selfHandle),
        'reassembled entity carries the right generation')

  // B's own SelfBlock is full precision and authoritative -- a completely
  // separate path from the delta stream A reconstructed. If the delta logic
  // were wrong, these would disagree.
  const ax = fxToNumber(seen.pos.x)
  const bx = fxToNumber(b.selfPos.x)
  const ay = fxToNumber(seen.pos.y)
  const by = fxToNumber(b.selfPos.y)
  check(Math.abs(ax - bx) < 0.05 && Math.abs(ay - by) < 0.05,
        `reassembled position matches authoritative (${ax.toFixed(3)},${ay.toFixed(3)}) vs (${bx.toFixed(3)},${by.toFixed(3)})`)
}

// --- an idle world costs almost nothing --------------------------------------

// This is the case delta compression exists for. With nobody moving, every
// entity is unchanged, so entities should stop being sent entirely and the
// snapshot should shrink to its fixed overhead.
{
  const bytesBefore = a.bytes
  const countBefore = a.snapCount
  for (let i = 0; i < 90; i++) { await a.send(); await b.send(); await sleep(15) }
  await sleep(300)

  const idleAvg = (a.bytes - bytesBefore) / Math.max(1, a.snapCount - countBefore)
  console.log(`\n  idle snapshot average: ${idleAvg.toFixed(1)} bytes`)

  // Fixed overhead is 129 bytes (docs/protocol.md 5.4); one entity block is 23.
  // Settled and unchanging, a snapshot should be at or near the floor -- so the
  // threshold is overhead plus less than one entity, which is what makes this
  // an assertion about entities being ELIDED rather than merely compressed.
  check(idleAvg < 150, `idle snapshots collapse to fixed overhead (${idleAvg.toFixed(1)} bytes < 150)`)
  check(a.reasm.stats.carriedForward > 0,
        `entities were carried forward rather than re-sent (${a.reasm.stats.carriedForward})`)
}

// --- a departing entity is destroyed exactly once ----------------------------

{
  const bIdx = handleIndex(b.welcome.selfHandle)
  b.wt.close()
  await sleep(700)
  check(!a.reasm.entities().has(bIdx), "B was removed from A's reassembled set")
  check(a.reasm.stats.droppedNoBaseline === 0, 'still no baseline gaps after a departure')
}

console.log(`\n  A received ${a.snapCount} snapshots, ${a.bytes} bytes total ` +
            `(${(a.bytes / a.snapCount).toFixed(1)} avg)`)
console.log(`  reassembly: ${a.reasm.stats.full} full, ${a.reasm.stats.delta} delta, ` +
            `${a.reasm.stats.carriedForward} carried forward, ` +
            `${a.reasm.stats.droppedNoBaseline} dropped`)
console.log(`RESULT ${JSON.stringify({ loss: LOSS, snapshots: a.snapCount, bytes: a.bytes, avg: +(a.bytes / a.snapCount).toFixed(1), ...a.reasm.stats })}`)

try { a.wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')

console.log(`\ndelta: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
