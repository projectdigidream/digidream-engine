//
// The Phase 3 exit criterion, end to end.
//
// The plan's wording: "a hand-authored multi-level test map with a parametric
// lift, a scripted-discrete mover, a crusher, and a rail; anchored riding
// predicts cleanly in BOTH motion modes."
//
// The second mode is the interesting one. A parametric lift is predictable by
// construction -- the client evaluates the same function for a tick the server
// has not reached. A scripted-discrete mover is NOT: its offset is whatever a
// callback decided, and nothing describes it in advance. So the client falls
// back to anchor-relative prediction (design §3.2.5): it cannot know where the
// platform will be, but it knows exactly where it is standing ON the platform,
// and the engine hashes positions in that platform's frame so the ordinary
// exact-match machinery still applies.
//
// Which means both modes are checked with the SAME assertion -- zero
// mispredictions -- and the difference shows up as bandwidth, not as accuracy.
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
  axisFromNumber, fxToNumber, MSG
} from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34460)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34534)

// The control: drop the streamed SectorOffsets on the floor. The client then
// never learns where an unpredictable platform is, and anchor-relative
// prediction has nothing to be relative TO.
const IGNORE_OFFSETS = process.env.DNP_IGNORE_OFFSETS === '1'

// The control for slice 6. Sector motion is the MAP's business now, declared in
// maps/scripts/init.lua rather than by the engine. Run without a script and
// nothing should move -- which is what proves the takeover is real rather than
// a second implementation still running quietly alongside the first.
const NO_SCRIPT = process.env.DNP_NO_SCRIPT === '1'

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
  if (process.env.DNP_PHASE3_MAP) return process.env.DNP_PHASE3_MAP
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', arch, 'phase3.dnpm')
    if (existsSync(p)) return p
  }
  return null
}
const MAP_PATH = mapPath()
if (!MAP_PATH) { console.error('no baked phase3 map; run mapc maps/phase3.map first'); process.exit(2) }

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

async function ride ({ spawnTag, ticks, wtPort, linkPort }) {
  const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'),
                       ['--port', String(linkPort), '--map', MAP_PATH, '--spawn', spawnTag,
                      ...(NO_SCRIPT ? [] : ['--script', join(ROOT, 'maps', 'scripts', 'init.lua')])],
                       { stdio: ['ignore', 'inherit', 'pipe'] })
  let serverLog = ''
  server.stderr.on('data', (d) => { serverLog += d.toString() })
  const kill = () => { try { server.kill('SIGKILL') } catch {} }
  process.on('exit', kill)
  await waitFor(() => serverLog.includes('listening for edge'), 5000, 'server')

  const edge = new Edge({
    port: wtPort, host: '127.0.0.1', path: '/dnp', upstreamPort: linkPort,
    cert: readFileSync(join(CERTS, 'cert.pem')),
    privKey: readFileSync(join(CERTS, 'key.pem')),
    log: () => {}
  })
  await edge.start()
  await waitFor(() => serverLog.includes('edge connected'), 8000, 'edge')

  const wt = new WebTransport(`https://127.0.0.1:${wtPort}/dnp`, {
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
  if (!NO_SCRIPT) await waitFor(() => motionSets.length >= 3, 5000, 'motion sets')
  else await sleep(400)   // nothing to wait for; give the handshake a moment

  const { ok, map } = core.loadMap(acc.bytes())
  if (!ok || map.hash !== welcome.mapHash) throw new Error('map mismatch')

  const world = core.createWorld({ map })
  const agent = world.spawn(map.spawnForTag(spawnTag))
  for (const m of motionSets) world.applyMotionSet(m)

  const predictor = new Predictor(world, agent)
  let history = []
  // Counted PER SECTOR: this map has both kinds of mover, so a total would
  // credit the lift with the drifter's traffic.
  const offsetsBySector = new Map()
  const track = []

  const drain = () => {
    while (pending.length) {
      const s = pending.shift()
      track.push(fxToNumber(s.self.pos.z))
      for (const so of s.sectorOffsets ?? []) {
        offsetsBySector.set(so.sector, (offsetsBySector.get(so.sector) ?? 0) + 1)
        if (!IGNORE_OFFSETS) world.setDiscreteOffset(so.sector, so.offset)
      }
      predictor.reconcile(s)
    }
  }

  // Settle first, so the measured window is the ride and not the join.
  for (let i = 0; i < 60; i++) {
    const f = { buttons: 0, moveX: axisFromNumber(0), moveY: 0, aim: 0 }
    predictor.step(f)
    history = [f, ...history].slice(0, 12)
    await dw.write(encodeInputBundle({
      clientTick: predictor.tick,
      ackSnapshotTick: pending.at(-1)?.serverTick ?? predictor.lastSnapshot?.serverTick ?? 0,
      frames: history
    }))
    drain()
    await sleep(15)
  }
  const settled = predictor.state()
  predictor.stats.exactMatches = 0
  predictor.stats.mispredictions = 0
  predictor.stats.starvedDivergences = 0
  track.length = 0

  for (let i = 0; i < ticks; i++) {
    const f = { buttons: 0, moveX: axisFromNumber(0), moveY: 0, aim: 0 }
    predictor.step(f)
    history = [f, ...history].slice(0, 12)
    await dw.write(encodeInputBundle({
      clientTick: predictor.tick,
      ackSnapshotTick: pending.at(-1)?.serverTick ?? predictor.lastSnapshot?.serverTick ?? 0,
      frames: history
    }))
    drain()
    await sleep(15)
  }
  await sleep(300)
  drain()

  try { wt.close() } catch {}
  await edge.stop()
  server.kill('SIGTERM')
  await sleep(200)
  kill()

  return { serverLog, motionSets, stats: predictor.stats, track, settled, offsetsBySector }
}

if (NO_SCRIPT) {
  // Only this block runs: with no script there is nothing to ride.
  const r = await ride({ spawnTag: 'lift', ticks: 180, wtPort: WT_PORT, linkPort: LINK_PORT })
  check(r.motionSets.length === 0,
        `CONTROL: with no script, the server declares no movers (${r.motionSets.length})`)
  check(!r.serverLog.includes('moves (rise'),
        'CONTROL: and the engine no longer declares them itself')
  const span = Math.max(...r.track) - Math.min(...r.track)
  check(span < 0.01, `CONTROL: the lift does not move (${span.toFixed(3)} units)`)
  check(r.stats.mispredictions === 0, 'CONTROL: prediction stays exact on a still map')
  console.log(`\nphase3: ${checks} checks, ${fails} failures`)
  process.exit(fails === 0 ? 0 : 1)
}

console.log('\n--- mode 1: a PARAMETRIC lift (predictable, free to replicate) ---')
{
  const r = await ride({ spawnTag: 'lift', ticks: 300, wtPort: WT_PORT, linkPort: LINK_PORT })

  check(r.serverLog.includes('sector 1 moves (rise 3)'), 'the map declares a parametric lift')
  check(r.motionSets.find((m) => m.sector === 1)?.motionType === 1, 'and replicates it as parametric')
  check(r.settled.sector === 1, `the agent settled on it (sector ${r.settled.sector})`)

  const span = Math.max(...r.track) - Math.min(...r.track)
  console.log(`  rode ${span.toFixed(2)} units; exact ${r.stats.exactMatches} ` +
              `mispredicted ${r.stats.mispredictions} starved ${r.stats.starvedDivergences}`)
  check(span > 2, `carried through ${span.toFixed(2)} of its 3.00 units`)
  // A parametric mover costs NOTHING per tick: the client computes it. The
  // drifter next door is streamed all the while, which is what makes this a
  // comparison rather than an observation about a quiet map.
  check((r.offsetsBySector.get(1) ?? 0) === 0, 'and streamed zero per-tick offsets for the lift')
  check((r.offsetsBySector.get(2) ?? 0) > 50,
        `while the drifter next door cost ${r.offsetsBySector.get(2) ?? 0} offsets over the same window`)
  check(r.stats.mispredictions === 0, `ZERO mispredictions (got ${r.stats.mispredictions})`)
}

console.log('\n--- mode 2: a SCRIPTED-DISCRETE mover (unpredictable, streamed) ---')
{
  const r = await ride({ spawnTag: 'drifter', ticks: 300,
                         wtPort: WT_PORT + 1, linkPort: LINK_PORT + 1 })

  check(r.serverLog.includes('sector 2 drifts (scripted-discrete, range 2)'),
        'the map declares a scripted-discrete mover')
  check(r.settled.sector === 2, `the agent settled on it (sector ${r.settled.sector})`)

  const span = Math.max(...r.track) - Math.min(...r.track)
  console.log(`  drifted ${span.toFixed(2)} units; exact ${r.stats.exactMatches} ` +
              `mispredicted ${r.stats.mispredictions} starved ${r.stats.starvedDivergences}`)
  check(span > 0.1, `the platform actually moved (${span.toFixed(2)} units)`)

  // The cost of the mode, made visible: this one is streamed every snapshot,
  // where the parametric lift above sent nothing at all after the handshake.
  check((r.offsetsBySector.get(2) ?? 0) > 50,
        `its offset was streamed ${r.offsetsBySector.get(2) ?? 0} times`)

  if (IGNORE_OFFSETS) {
    // The control. Anchor-relative prediction is relative to an offset the
    // client is TOLD; discard the telling and it must break.
    check(r.stats.mispredictions > 0,
          `CONTROL: discarding the streamed offsets DOES break prediction ` +
          `(${r.stats.mispredictions} mispredictions)`)
  } else {
    // The claim: the same assertion as the predictable mode.
    check(r.stats.mispredictions === 0,
          `ZERO mispredictions on an UNPREDICTABLE platform (got ${r.stats.mispredictions})`)
  }
}

console.log(`\nphase3: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
