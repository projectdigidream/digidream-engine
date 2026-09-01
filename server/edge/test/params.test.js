//
// Replicated movement parameters (docs/protocol.md §8.4), end to end.
//
// The parameter table is design §7.2's "only scriptable surface of the physics
// core". Everything else Lua will touch is state; this is the RULES. That
// distinction is the whole test: a wrong position is corrected by the next
// snapshot, whereas a wrong `move_speed` makes every subsequent tick wrong in a
// way no state correction repairs -- and it looks exactly like a determinism
// regression, which is the most expensive thing to misdiagnose in this engine.
//
// Run with DNP_IGNORE_PARAMS=1 for the control: the client drops the updates on
// the floor and must then mispredict continuously and never recover.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { Core } from '../../../client/src/core.js'
import { Predictor } from '../../../client/src/predictor.js'
import { MapAccumulator } from '../../../client/src/maploader.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, MSG
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34470)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34544)
const IGNORE_PARAMS = process.env.DNP_IGNORE_PARAMS === '1'

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

function mapPath () {
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', arch, 'phase3.dnpm')
    if (existsSync(p)) return p
  }
  return null
}
const MAP_PATH = mapPath()
if (!MAP_PATH) { console.error('no baked phase3 map'); process.exit(2) }

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

const server = spawn(serverBin,
                     ['--port', String(LINK_PORT), '--map', MAP_PATH,
                      '--spawn', 'yard', '--param-test'],
                     { stdio: ['ignore', 'inherit', 'pipe'] })
let serverLog = ''
server.stderr.on('data', (d) => { serverLog += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })
await waitFor(() => serverLog.includes('listening for edge'), 5000, 'server')

const edge = new Edge({
  port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  log: () => {}
})
await edge.start()
await waitFor(() => serverLog.includes('edge connected'), 8000, 'edge')

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
const paramUpdates = []
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
        else if (m?.type === MSG.PARAM_UPDATE) paramUpdates.push(m)
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
await waitFor(() => paramUpdates.length > 0, 5000, 'the FULL parameter set')

const { ok, map } = core.loadMap(acc.bytes())
check(ok && map.hash === welcome.mapHash, 'map loaded')

const world = core.createWorld({ map })
const agent = world.spawn(map.spawnForTag('yard'))
for (const m of motionSets) world.applyMotionSet(m)

// The join update must be FULL: a client with no history has nothing to patch.
const first = paramUpdates[0]
check((first.flags & 1) !== 0, 'the first update is FULL')
check(first.fields.length === core.paramCount,
      `and carries every field (${first.fields.length} of ${core.paramCount})`)
check(first.effectiveTick !== undefined, 'and names the tick it took effect on')

const predictor = new Predictor(world, agent)
let applied = 0
const drainParams = () => {
  while (paramUpdates.length) {
    const m = paramUpdates.shift()
    if (IGNORE_PARAMS) continue          // the control
    applied += world.applyParamUpdate(agent, m)
    predictor.paramEpoch = m.epoch
  }
}
drainParams()
// Against the engine's own count, not a literal. "29" was true until
// MoveParams grew the swing parameters, at which point the assertion still
// passed its own reading and no longer meant "every field".
check(IGNORE_PARAMS || applied === core.paramCount,
      `applied ${applied} of ${core.paramCount} fields from the FULL set`)

// move_speed is what --param-test toggles.
const SPEED_ID = 2
const speedAtStart = world.getParamRaw(agent, SPEED_ID)
check(IGNORE_PARAMS || speedAtStart === 8n * 4294967296n,
      `move_speed starts at ${Number(speedAtStart) / 4294967296}`)

let history = []
const drain = () => {
  while (pending.length) {
    const s = pending.shift()
    for (const so of s.sectorOffsets ?? []) world.setDiscreteOffset(so.sector, so.offset)
    predictor.reconcile(s)
  }
}

// Shuttle left and right, rather than holding one direction.
//
// Holding right parks the agent against the yard wall within a second, and a
// stationary agent moves at the same speed whatever move_speed says -- so the
// control could not tell a client with the wrong parameter from one with the
// right one. The parameter has to actually influence the result, continuously,
// or the whole test measures nothing.
const shuttle = (i) => axisFromNumber(Math.floor(i / 30) % 2 === 0 ? 1 : -1)

for (let i = 0; i < 60; i++) {
  const f = { buttons: 0, moveX: shuttle(i), moveY: 0, aim: 0 }
  predictor.step(f)
  history = [f, ...history].slice(0, 12)
  await dw.write(encodeInputBundle({
    clientTick: predictor.tick,
    ackSnapshotTick: pending.at(-1)?.serverTick ?? 0,
    frames: history
  }))
  drainParams(); drain()
  await sleep(15)
}
predictor.stats.exactMatches = 0
predictor.stats.mispredictions = 0
predictor.stats.paramsStale = 0
predictor.stats.paramsStuck = 0

for (let i = 0; i < 420; i++) {
  const f = { buttons: 0, moveX: shuttle(i), moveY: 0, aim: 0 }
  predictor.step(f)
  history = [f, ...history].slice(0, 12)
  await dw.write(encodeInputBundle({
    clientTick: predictor.tick,
    ackSnapshotTick: pending.at(-1)?.serverTick ?? 0,
    frames: history
  }))
  drainParams(); drain()
  await sleep(15)
}
await sleep(300)
drainParams(); drain()

const st = predictor.stats
console.log(`  exact ${st.exactMatches}  mispredicted ${st.mispredictions}  ` +
            `params-stale ${st.paramsStale}  params-STUCK ${st.paramsStuck}  ` +
            `starved ${st.starvedDivergences}`)
console.log(`  server changed move_speed ${(serverLog.match(/move_speed ->/g) ?? []).length} times`)

check((serverLog.match(/move_speed ->/g) ?? []).length >= 2,
      'the server changed the parameter more than once during the run')

const compared = st.exactMatches + st.mispredictions + st.paramsStale +
                 st.paramsStuck + st.starvedDivergences
check(compared > 50, `reconciliation compared ${compared} snapshots`)

if (IGNORE_PARAMS) {
  // The control, and the shape of it is the interesting part.
  //
  // Naively this asserted `mispredictions > 0` and FAILED -- with zero. A
  // client that never applies parameters is permanently a generation behind,
  // so the classifier truthfully reports "stale" on every single comparison
  // and never reports a defect. The classification was right and the control
  // was measuring the wrong thing: it let a client running entirely different
  // physics look healthy.
  //
  // What actually distinguishes the two cases is DURATION, exactly as
  // docs/protocol.md §10.1.2 says -- in flight is transient, and this never
  // stops. So the predictor escalates a long stale run to `params-stuck`, and
  // that is what must fire here.
  check(st.paramsStuck > 0,
        `CONTROL: discarding PARAM_UPDATE is caught as params-STUCK (${st.paramsStuck})`)
  check(st.paramsStale + st.paramsStuck > 50,
        `CONTROL: and it never recovers (${st.paramsStale + st.paramsStuck} disagreeing snapshots)`)
} else {
  check(st.mispredictions === 0,
        `ZERO mispredictions across ${(serverLog.match(/move_speed ->/g) ?? []).length} parameter changes ` +
        `(got ${st.mispredictions})`)
  // The epoch earns its byte here: mismatches while an update is in flight are
  // expected and must be CLASSIFIED, not counted as defects...
  check(st.paramsStale > 0,
        `${st.paramsStale} in-flight mismatches classified params-stale, not defects`)
  // ...but they must also be transient. A healthy client is never stuck.
  check(st.paramsStuck === 0, `and none of them got stuck (${st.paramsStuck})`)
}

try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')
console.log(`\nparams: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
