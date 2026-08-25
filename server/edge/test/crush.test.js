//
// Crushing and horizontal sweep, end to end (design §3.1, §3.1.3).
//
// Two halves, because the two hazards land on opposite sides of the prediction
// boundary and that is the whole point of separating them:
//
//   the piston   changes the client's POSITION, so the client must predict it.
//                Verified the only way that means anything: zero mispredictions
//                while being shoved, with a control (DNP_IGNORE_MOTION=1) that
//                must break.
//
//   the crusher  changes nothing the client can see. Detection is native and
//                the consequence is a Lua callback (§3.1), so what is verified
//                is that the authoritative server raised the event at all --
//                through a real baked map, a real tag, and a real tick loop.
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
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34450)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34524)
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

// The crush map, baked by mapc. Deliberately not the shared test map: this one
// exists to be dangerous, and the other is compared byte for byte by tests that
// would rather it stayed boring.
function crushMapPath () {
  if (process.env.DNP_CRUSH_MAP) return process.env.DNP_CRUSH_MAP
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', arch, 'crush.dnpm')
    if (existsSync(p)) return p
  }
  return null
}

const MAP_PATH = crushMapPath()
if (!MAP_PATH) { console.error('no baked crush map; run mapc maps/crush.map first'); process.exit(2) }

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

// Boot a server on the crush map at a named spawn, attach an edge and one
// predicting client, run `ticks` frames, and hand back everything observed.
async function session ({ spawnTag, ticks, wtPort, linkPort }) {
  const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'),
                       ['--port', String(linkPort), '--map', MAP_PATH, '--spawn', spawnTag],
                       { stdio: ['ignore', 'inherit', 'pipe'] })
  let serverLog = ''
  server.stderr.on('data', (d) => { serverLog += d.toString() })
  const killServer = () => { try { server.kill('SIGKILL') } catch {} }
  process.on('exit', killServer)
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
  await waitFor(() => motionSets.length >= 2, 5000, 'motion sets')

  const { ok, map } = core.loadMap(acc.bytes())
  if (!ok || map.hash !== welcome.mapHash) throw new Error('map mismatch')

  const world = core.createWorld({ map })
  const agent = world.spawn(map.spawnForTag(spawnTag))
  let applied = 0
  if (!IGNORE_MOTION) {
    for (const m of motionSets) if (world.applyMotionSet(m)) applied++
  }

  const predictor = new Predictor(world, agent)
  let history = []
  const track = []

  for (let i = 0; i < ticks; i++) {
    const f = { buttons: 0, moveX: axisFromNumber(0), moveY: 0, aim: 0 }
    predictor.step(f)
    history = [f, ...history].slice(0, 12)
    await dw.write(encodeInputBundle({
      clientTick: predictor.tick,
      ackSnapshotTick: pending.at(-1)?.serverTick ?? predictor.lastSnapshot?.serverTick ?? 0,
      frames: history
    }))
    while (pending.length) {
      const s = pending.shift()
      // Sampled from the AUTHORITATIVE state. Reading the client's own
      // prediction would report "nothing moved" in the control case, which is
      // true of the broken client and false of the world.
      track.push({ x: fxToNumber(s.self.pos.x), y: fxToNumber(s.self.pos.y) })
      for (const so of s.sectorOffsets ?? []) world.setDiscreteOffset(so.sector, so.offset)
      predictor.reconcile(s)
    }
    await sleep(15)
  }
  await sleep(300)

  try { wt.close() } catch {}
  await edge.stop()
  server.kill('SIGTERM')
  await sleep(200)
  killServer()

  return { serverLog, motionSets, applied, stats: predictor.stats, track, world, agent }
}

// ------------------------------------------------- the piston: predicted shove

console.log('\n--- the piston sweeps the room ---')
{
  // Settle first, then measure: the run below starts with the client already
  // reconciled, so the stats describe the shove and not the join.
  const r = await session({ spawnTag: 'start', ticks: 420, wtPort: WT_PORT, linkPort: LINK_PORT })

  check(r.serverLog.includes('sector 2 sweeps (slides 20 south)'),
        'the map declares a sweeping footprint')
  const sweeper = r.motionSets.find((m) => m.sector === 2)
  check(sweeper?.motionType === 1, 'the piston is parametric (predictable)')
  check(sweeper?.motionTarget === 0, 'and moves as a volume (target: both surfaces)')
  check(IGNORE_MOTION || r.applied === r.motionSets.length,
        `applied ${r.applied}/${r.motionSets.length} motions${IGNORE_MOTION ? ' (SKIPPED: control case)' : ''}`)

  const ys = r.track.map((p) => p.y)
  const minY = Math.min(...ys)
  const maxY = Math.max(...ys)
  console.log(`  authoritative y ranged ${minY.toFixed(3)} .. ${maxY.toFixed(3)}`)
  console.log(`  exact ${r.stats.exactMatches}  mispredicted ${r.stats.mispredictions}` +
              `  starved ${r.stats.starvedDivergences}`)

  // Shoved from the spawn at y=10 down toward the far wall. Derived from the
  // declared travel, not a magic number: the piston's face starts at y=20 and
  // ends at y=1, so a body of radius 0.5 that started at y=10 is pushed to 0.5.
  check(maxY - minY > 8, `the piston pushed the agent ${(maxY - minY).toFixed(2)} units`)
  check(minY > 0.4 && minY < 0.6, `and stopped it against the far wall at y=${minY.toFixed(3)}`)

  check(r.serverLog.includes('pinned by sector 2'),
        'the server reported a PIN once the room ran out (§3.1.3)')

  const compared = r.stats.exactMatches + r.stats.mispredictions + r.stats.starvedDivergences
  check(compared > 50, `reconciliation compared ${compared} snapshots`)
  if (IGNORE_MOTION) {
    // The control. A client that never learned the piston's path cannot know it
    // is being shoved, so prediction MUST break. If it does not, replicating
    // the parameters is doing nothing and this whole file proves nothing.
    check(r.stats.mispredictions > 0,
          `CONTROL: ignoring motion parameters DOES break prediction (${r.stats.mispredictions} mispredictions)`)
  } else {
    check(r.stats.mispredictions === 0,
          `ZERO mispredictions while being shoved by a moving footprint (got ${r.stats.mispredictions})`)
  }
}

// ------------------------------------------------ the crusher: server detection

console.log('\n--- the crusher closes on a standing player ---')
{
  const r = await session({ spawnTag: 'trap', ticks: 420,
                            wtPort: WT_PORT + 1, linkPort: LINK_PORT + 1 })

  check(r.serverLog.includes('sector 1 crushes (ceiling drops 7)'),
        'the map declares a crusher')
  const crusher = r.motionSets.find((m) => m.sector === 1)
  // Target 2 = ceiling only. This is the field that makes crushing possible at
  // all: a sector that moves as a volume keeps a constant gap forever.
  check(crusher?.motionTarget === 2, 'and it moves its CEILING only (target 2)')

  check(/tick \d+: agent 0 crushed by sector 1 \(depth [\d.]+\)/.test(r.serverLog),
        'the server detected the crush')
  const depth = Number(r.serverLog.match(/crushed by sector 1 \(depth ([\d.]+)\)/)?.[1] ?? 0)
  // Body 2.0 in a gap that closes to 1.0, so the shortfall tops out at 1.0.
  check(depth > 0 && depth <= 1.0, `reported shortfall ${depth} is within the gap it closed`)

  // Ping-pong: the ceiling goes back up, and the entity stops being crushed.
  // Without this the event would be indistinguishable from a latch that fires
  // once and never clears.
  check(/agent 0 released after \d+ ticks/.test(r.serverLog),
        'and reported the release when the ceiling lifted')

  // Nothing about this reaches the client, and prediction is unaffected by it.
  check(r.stats.mispredictions === 0 || IGNORE_MOTION,
        `prediction is unaffected by the crush (${r.stats.mispredictions} mispredictions)`)
}

console.log(`\ncrush: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
