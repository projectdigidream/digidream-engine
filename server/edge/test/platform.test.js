//
// Predicting an agent that rides a moving platform.
//
// This is the payoff of design §3.1.2's parametric/discrete split. The server
// sends the motion PARAMETERS once; the client evaluates the same function for
// ticks the server has not reached, so an anchored entity predicts normally and
// the platform costs nothing per tick.
//
// Run with DNP_IGNORE_MOTION=1 to skip applying the parameters. That is not a
// debugging convenience -- it is the control case. If prediction stays exact
// without them, the replication is not doing anything and the test proves
// nothing.
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
import { MapAccumulator } from '../../../client/src/maploader.js'
import { bakedMapPath } from './helpers.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, MSG
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34448)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34522)
const IGNORE_MOTION = process.env.DNP_IGNORE_MOTION === '1'

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

const MAP_PATH = bakedMapPath(ROOT)
if (!MAP_PATH) { console.error('no baked map; run mapc first'); process.exit(2) }

const server = spawn(serverBin,
                     // Spawn directly onto the lift. Walking onto one is a
                     // separate concern (and only possible while it is within
                     // step height of the ground); what is being tested here is
                     // predicting the ride.
                     ['--port', String(LINK_PORT), '--map', MAP_PATH, '--spawn', 'ledge',
                      '--script', join(ROOT, 'maps', 'scripts', 'init.lua')],
                     { stdio: ['ignore', 'inherit', 'pipe'] })
let serverLog = ''
server.stderr.on('data', (d) => { serverLog += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })
await waitFor(() => serverLog.includes('listening for edge'), 5000, 'server')
check(serverLog.includes('moves (rise'), 'the map declares a moving sector')

const edge = new Edge({
  port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  log: () => {}
})
await edge.start()
await waitFor(() => serverLog.includes('edge connected'), 8000, 'edge')

const core = await Core.load()
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

const wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
  serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
})
await withTimeout(wt.ready, 8000, 'client ready')
const control = await wt.createBidirectionalStream()
const cw = control.writable.getWriter()
const dw = wt.datagrams.createWritable().getWriter()

let welcome = null
const acc = new MapAccumulator()
const motionSets = []
const pending = []

;(async () => {
  const r = control.readable.getReader()
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
        if (m?.type === MSG.WELCOME) welcome = m
        else if (m?.type === MSG.MAP_DATA) acc.push(m)
        else if (m?.type === MSG.SECTOR_MOTION_SET) motionSets.push(m)
      }
    }
  } catch {}
})()

;(async () => {
  const r = wt.datagrams.readable.getReader()
  try {
    for (;;) {
      const { value, done } = await r.read()
      if (done) break
      const s = decodeSnapshot(Buffer.from(value))
      if (s) pending.push(s)
    }
  } catch {}
})()

await cw.write(encodeHello())
await waitFor(() => welcome, 8000, 'welcome')
await waitFor(() => acc.complete, 8000, 'map')
await waitFor(() => motionSets.length > 0, 5000, 'motion sets')
check(motionSets.length > 0, `received ${motionSets.length} SECTOR_MOTION_SET message(s)`)
check(motionSets[0].motionType === 1, 'the mover is parametric (predictable)')

const { ok, map } = core.loadMap(acc.bytes())
check(ok && map.hash === welcome.mapHash, 'map loaded and hash matches')

const world = core.createWorld({ map })
const agent = world.spawn(map.spawnForTag('ledge'))

// The control case. Without the parameters the client's copy of the sector
// simply never moves, while the server's does.
let applied = 0
if (!IGNORE_MOTION) {
  for (const m of motionSets) if (world.applyMotionSet(m)) applied++
}
check(IGNORE_MOTION || applied === motionSets.length,
      `applied ${applied}/${motionSets.length} motions${IGNORE_MOTION ? ' (SKIPPED: control case)' : ''}`)

const predictor = new Predictor(world, agent)
let tick = 0
let history = []

const frame = async (moveX) => {
  tick++
  const f = { buttons: 0, moveX: axisFromNumber(moveX), moveY: 0, aim: 0 }
  predictor.step(f)
  history = [f, ...history].slice(0, 12)
  await dw.write(encodeInputBundle({
    clientTick: predictor.tick,
    ackSnapshotTick: pending.at(-1)?.serverTick ?? predictor.lastSnapshot?.serverTick ?? 0,
    frames: history
  }))
}
let authMinZ = 1e9
let authMaxZ = -1e9
let sampling = false

const drain = () => {
  while (pending.length) {
    const s = pending.shift()
    if (sampling) {
      // Sampled from the AUTHORITATIVE state, not the prediction. In the
      // control case the client's platform never moves, so measuring its own
      // predicted z would report "the platform did not move" -- which is true
      // of the broken client and false of the world.
      const z = fxToNumber(s.self.pos.z)
      authMinZ = Math.min(authMinZ, z)
      authMaxZ = Math.max(authMaxZ, z)
    }
    // Scripted-discrete offsets, if any. Parametric movers are absent here by
    // construction -- the client computes those itself.
    for (const so of s.sectorOffsets ?? []) world.setDiscreteOffset(so.sector, so.offset)
    predictor.reconcile(s)
  }
}

// Settle onto the lift.
for (let i = 0; i < 60; i++) { await frame(0); drain(); await sleep(15) }
predictor.stats.exactMatches = 0
predictor.stats.mispredictions = 0
predictor.stats.starvedDivergences = 0

const onLift = predictor.state()
console.log(`\n  standing at x=${fxToNumber(onLift.pos.x).toFixed(2)}, sector ${onLift.sector}, z=${fxToNumber(onLift.pos.z).toFixed(3)}`)
check(onLift.sector === 1, `agent is on the moving sector (sector ${onLift.sector})`)
check(onLift.grounded, 'and is standing on it')

// Now ride it. The floor moves under the agent every tick; the client is
// predicting that motion locally from the parameters.
sampling = true
for (let i = 0; i < 420; i++) {
  await frame(0)
  drain()
  await sleep(15)
}
await sleep(300)
drain()

const st = predictor.stats
console.log(`  authoritative z ranged ${authMinZ.toFixed(3)} .. ${authMaxZ.toFixed(3)}`)
console.log(`  exact ${st.exactMatches}  mispredicted ${st.mispredictions}  starved ${st.starvedDivergences}\n`)

// Derived from the motion the server declared, not a magic number: the agent
// should be carried through most of the lift's actual travel.
const declaredTravel = Math.abs(fxToNumber(motionSets[0].to.z) - fxToNumber(motionSets[0].from.z))
check(authMaxZ - authMinZ > declaredTravel * 0.8,
      `the platform carried the agent through its travel ` +
      `(${(authMaxZ - authMinZ).toFixed(2)} of ${declaredTravel.toFixed(2)} units)`)
// Comparisons, not successes: in the control case every one of them fails, and
// requiring successes here would report the control's expected failure as a
// second, misleading failure.
const compared = st.exactMatches + st.mispredictions + st.starvedDivergences
check(compared > 50, `reconciliation compared ${compared} snapshots`)

if (IGNORE_MOTION) {
  // The control case must FAIL. If prediction stays exact without the
  // parameters, replicating them is doing nothing and this whole test is
  // vacuous.
  check(st.mispredictions > 0,
        `CONTROL: ignoring motion parameters DOES break prediction (${st.mispredictions} mispredictions)`)
} else {
  check(st.mispredictions === 0,
        `ZERO mispredictions while riding a moving platform (got ${st.mispredictions})`)
}

try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')

console.log(`platform: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
