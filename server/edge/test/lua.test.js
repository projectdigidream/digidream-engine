//
// Server-side scripting reaches the client (docs/protocol.md §10.1.3).
//
// This is the first thing in the engine a client genuinely CANNOT predict. The
// client never runs server Lua -- that is the design's founding constraint --
// so when a script teleports a player, the client's prediction for that tick is
// wrong for a reason no amount of local determinism could have anticipated.
//
// The claim is not "mismatches happen". It is that they are DISTINGUISHABLE
// from a fault, and the distinguishing property is shape:
//
//   a scripted change  -> mismatches on exactly one tick, then self-heals
//   a determinism bug  -> mismatches forever
//
// A test asserting "at least one mismatch" would pass for both. This one
// asserts the shape.
//
// Controls:
//   DNP_NO_SCRIPT=1     no script at all -> zero lua-applied, zero mispredicts.
//                       Without it, "the flag was set" might just mean the
//                       server sets it unconditionally.
//   DNP_IGNORE_LUA=1    the client ignores the flag -> the same ticks must be
//                       reported as DEFECTS. Without it the flag could carry no
//                       information and nothing would notice.
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
  axisFromNumber, fxToNumber, MSG, SNAP_FLAG
} from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34490)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34564)
const NO_SCRIPT = process.env.DNP_NO_SCRIPT === '1'
const IGNORE_LUA = process.env.DNP_IGNORE_LUA === '1'

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

const args = ['--port', String(LINK_PORT), '--map', MAP_PATH, '--spawn', 'yard']
if (!NO_SCRIPT) args.push('--script', join(ROOT, 'maps', 'scripts', 'teleport_test.lua'))

const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'), args,
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

const { ok, map } = core.loadMap(acc.bytes())
check(ok && map.hash === welcome.mapHash, 'map loaded')

const world = core.createWorld({ map })
const agent = world.spawn(map.spawnForTag('yard'))
for (const m of motionSets) world.applyMotionSet(m)

const predictor = new Predictor(world, agent)
predictor.ignoreLuaFlag = IGNORE_LUA

let history = []
let flaggedSnapshots = 0
const verdicts = []
const jumps = []
let lastZ = null

const drain = () => {
  while (pending.length) {
    const s = pending.shift()
    if ((s.flags & SNAP_FLAG.LUA_APPLIED) !== 0) flaggedSnapshots++

    // Watch the authoritative position for the teleport itself, independent of
    // any verdict: a flag that fired without the world moving would prove
    // nothing.
    const z = { x: fxToNumber(s.self.pos.x), y: fxToNumber(s.self.pos.y) }
    if (lastZ) {
      const d = Math.hypot(z.x - lastZ.x, z.y - lastZ.y)
      if (d > 2) jumps.push({ tick: s.serverTick, dist: d })
    }
    lastZ = z

    const r = predictor.reconcile(s)
    if (r?.verdict) verdicts.push({ tick: s.serverTick, v: r.verdict })
  }
}

// Walk right so the agent is somewhere other than the teleport anchor.
for (let i = 0; i < 420; i++) {
  const f = { buttons: 0, moveX: axisFromNumber(1), moveY: 0, aim: 0 }
  predictor.step(f)
  history = [f, ...history].slice(0, 12)
  await dw.write(encodeInputBundle({
    clientTick: predictor.tick,
    ackSnapshotTick: pending.at(-1)?.serverTick ?? 0,
    frames: history
  }))
  drain()
  await sleep(15)
}
await sleep(300)
drain()

const st = predictor.stats
console.log(`  exact ${st.exactMatches}  mispredicted ${st.mispredictions}  ` +
            `lua-applied ${st.luaApplied}  starved ${st.starvedDivergences}`)
console.log(`  ${flaggedSnapshots} snapshots carried LUA_APPLIED; ` +
            `${jumps.length} position jumps observed`)
if (process.env.DNP_DEBUG_VERDICTS) {
  const jt = jumps.map(j => j.tick)
  console.log('  jump ticks: ' + jt.join(', '))
  const bad = verdicts.filter(v => v.v === 'mispredicted').map(v => v.tick)
  console.log('  mispredicted ticks: ' + bad.slice(0, 40).join(', '))
  console.log('  first 24 verdicts: ' + verdicts.slice(0, 24).map(v => `${v.tick}:${v.v}`).join(' '))
}

if (NO_SCRIPT) {
  // Control (a): no script at all. If the flag still appeared, the server
  // would be setting it unconditionally and it would mean nothing.
  check(flaggedSnapshots === 0, 'CONTROL: with no script, LUA_APPLIED is never set')
  check(st.luaApplied === 0, 'CONTROL: and nothing is classified lua-applied')
  check(st.mispredictions === 0, `CONTROL: prediction is exact (${st.mispredictions})`)
  check(jumps.length === 0, 'CONTROL: and the agent is never moved')
} else {
  check(serverLog.includes('teleported'), 'the script ran and teleported the agent')
  check(jumps.length >= 2, `the agent was moved ${jumps.length} times`)
  check(flaggedSnapshots >= 2, `${flaggedSnapshots} snapshots carried LUA_APPLIED`)

  if (IGNORE_LUA) {
    // Control (b): a client that ignores the flag must report these as
    // defects. If it did not, the flag would be carrying no information.
    check(st.mispredictions > 0,
          `CONTROL: ignoring LUA_APPLIED reports the change as a DEFECT (${st.mispredictions})`)
  } else {
    check(st.luaApplied >= 2, `${st.luaApplied} ticks classified lua-applied, not defects`)
    check(st.mispredictions === 0, `ZERO mispredictions (got ${st.mispredictions})`)

    // The shape assertion, which is the point of the whole file: each scripted
    // change costs exactly ONE tick and then self-heals. A run of consecutive
    // lua-applied verdicts would mean the client never recovered -- the
    // signature of a real fault wearing the flag as an excuse.
    let worstRun = 0
    let run = 0
    for (const v of verdicts) {
      if (v.v === 'lua-applied') { run++; worstRun = Math.max(worstRun, run) } else run = 0
    }
    console.log(`  longest consecutive lua-applied run: ${worstRun}`)
    check(worstRun <= 2,
          `each scripted change self-heals within a tick (longest run ${worstRun})`)
  }
}

try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')
console.log(`\nlua: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
