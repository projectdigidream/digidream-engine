//
// The BROWSER client's own code path, against the real server.
//
// Every other e2e test here reimplements the handshake, the control-stream
// framing and the input loop locally. That was fine while the only client was a
// test, and it stopped being fine the moment there was a real one: those
// reimplementations can all be correct while client/src/net.js is broken, and
// nothing would say so.
//
// So this drives the ACTUAL modules the browser loads -- Connection, Predictor,
// EntityView -- and asserts the same property the rest of the suite does:
// prediction is exact, with a control that proves the zero is a measurement.
//
import { spawn } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { serverBin, doomProps } from './serverbin.js'

// net.js reaches for the global, exactly as a browser provides it. Installed
// here rather than parameterised: a constructor argument would be a seam that
// exists only for tests, and the browser path would then be the untested one.
globalThis.WebTransport = WebTransport

const { Core } = await import('../../../client/src/core.js')
const { Connection } = await import('../../../client/src/net.js')
const { Predictor } = await import('../../../client/src/predictor.js')
const { MapAccumulator } = await import('../../../client/src/maploader.js')
const { EntityView } = await import('../../../client/src/render/entities.js')
const { MSG, axisFromNumber, fxToNumber, ENT_FIELD, handleIndex } =
  await import('../../../client/src/proto.js')
const { parseProps } = await import('../../../client/src/render/props.js')
const { bakedMapPath, mirrorEntities } = await import('./helpers.js')

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34461)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34531)

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, l) => {
  const end = Date.now() + ms
  while (Date.now() < end) { if (fn()) return true; await sleep(20) }
  throw new Error(`timeout: ${l}`)
}

const MAP_PATH = bakedMapPath(ROOT)
if (!MAP_PATH) { console.error('no baked map; run mapc first'); process.exit(2) }

const server = spawn(serverBin,
                     ['--port', String(LINK_PORT), '--map', MAP_PATH],
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

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

// One client, built the way client/src/main.js builds one.
async function connectClient (name) {
  const c = {
    name, welcome: null, motionSets: [], paramUpdates: [], snapshots: [],
    acc: new MapAccumulator()
  }
  c.conn = new Connection({
    url: `https://127.0.0.1:${WT_PORT}/dnp`,
    certHash,
    onControl (m) {
      if (m.type === MSG.WELCOME) c.welcome = m
      else if (m.type === MSG.MAP_DATA) c.acc.push(m)
      else if (m.type === MSG.SECTOR_MOTION_SET) c.motionSets.push(m)
      else if (m.type === MSG.PARAM_UPDATE) c.paramUpdates.push(m)
    },
    onSnapshot (s) { c.snapshots.push(s) },
    onClose () {}
  })
  await c.conn.open()
  await waitFor(() => c.welcome, 8000, `${name} welcome`)
  await waitFor(() => c.acc.complete, 8000, `${name} map`)

  const { ok, map } = core.loadMap(c.acc.bytes())
  if (!ok) throw new Error(`${name}: map rejected`)
  // Its own entity index, so it does not mirror a collider on top of itself,
  // and a place to remember each entity's kind -- display_id is delta-encoded
  // and arrives once, on the tick the entity appears.
  c.selfIndex = (c.welcome.selfHandle & 0x00ffffff) - 1
  c.kinds = new Map()
  c.map = map
  c.world = core.createWorld({ map })
  for (const m of c.motionSets) c.world.applyMotionSet(m)
  c.agent = c.world.spawn(map.spawnForTag('start'))
  c.predictor = new Predictor(c.world, c.agent)
  for (const m of c.paramUpdates) {
    c.world.applyParamUpdate(c.agent, m)
    c.predictor.paramEpoch = m.epoch
  }
  c.paramUpdates.length = 0
  c.world.tick = c.welcome.serverTick
  c.history = []
  return c
}

console.log('\n--- the browser client connects and predicts ---')
const a = await connectClient('A')
check(a.welcome !== null, 'Connection completed the handshake and got WELCOME')
check(a.acc.complete, `and reassembled the map (${a.acc.total} bytes)`)
check(a.map.hash === a.welcome.mapHash,
      `client and server agree on the map hash (${a.map.hash.toString(16)})`)

// A second client, so the entity path has something to draw.
const b = await connectClient('B')
check(b.welcome !== null, 'a second client connects too')

// EntityView with a stand-in scene: the interpolation and lifetime rules are
// what is under test, not Three.js.
const scene = { objects: new Set(), add (o) { this.objects.add(o) }, remove (o) { this.objects.delete(o) } }
const others = new EntityView({
  makeMesh: () => ({ position: { set () {} }, userData: {} }),
  scene
})
const CONV = { fxToFloat: (v) => Number(v) / 4294967296, toThree: (x, y, z) => ({ x, y: z, z: -y }) }

// Colliders for the OTHER bodies, so a predicting client stops where the server
// will stop it. Both clients spawn on the same tag, so they start concentric and
// the engine separates them on the first tick -- a client that has not mirrored
// the other one predicts straight through it and is corrected for as long as
// they overlap. Exactly what main.js does; a harness that skipped it would be
// measuring a client simpler than the real one.
const catalogue = parseProps(readFileSync(doomProps, 'utf8'))
const mirrorBase = core.M._dnp_mirror_base()

function drive (c, moveX, feedEntities = false) {
  const f = { buttons: 0, moveX: axisFromNumber(moveX), moveY: 0, aim: 0 }
  c.predictor.step(f)
  c.history = [f, ...c.history].slice(0, 8)
  c.conn.send(c.predictor.tick, c.predictor.lastSnapshot?.serverTick ?? 0, c.history)
  while (c.snapshots.length) {
    const s = c.snapshots.shift()
    for (const so of s.sectorOffsets ?? []) c.world.setDiscreteOffset(so.sector, so.offset)
    if (feedEntities) others.ingest(s, CONV)
    mirrorEntities(c.world, s, {
      catalogue, kinds: c.kinds, mirrorBase, selfIndex: c.selfIndex, ENT_FIELD, handleIndex
    })
    c.predictor.reconcile(s)
  }
}

// Settle, then measure.
for (let i = 0; i < 60; i++) { drive(a, 0); drive(b, 0); await sleep(15) }
a.predictor.stats.exactMatches = 0
a.predictor.stats.mispredictions = 0
a.predictor.stats.starvedDivergences = 0

// A walks WEST and B east -- apart, not into each other.
//
// Both clients spawn on the same tag, so they start concentric and the engine's
// tie-break separates them on the first tick: A, the lower index, goes west.
// Driving A east from there means driving it back through B, and two bodies
// grinding against each other is a deliberately different claim from this one.
// It is also the one case prediction cannot be exact about -- the remote body
// is interpolated, so a client's copy of where it is trails the server's by the
// interpolation delay, and the two disagree about where the stop is for as long
// as the contact lasts. What this test measures is the netcode path; body
// contact is core/tests/test_bodies.cpp's subject.
for (let i = 0; i < 240; i++) {
  drive(a, -1, true)
  drive(b, 1)
  await sleep(15)
}
await sleep(200)
drive(a, 0, true)

const st = a.predictor.stats
console.log(`  exact ${st.exactMatches}  mispredicted ${st.mispredictions}  starved ${st.starvedDivergences}`)
check(st.exactMatches > 50, `reconciliation compared ${st.exactMatches} snapshots`)
check(st.mispredictions === 0, `ZERO mispredictions through client/src/net.js (got ${st.mispredictions})`)

const moved = fxToNumber(a.predictor.state().pos.x)
check(Math.abs(moved - 10) > 1, `the agent actually moved (x=${moved.toFixed(2)}), so the zero is not idleness`)

check(others.count === 1, `EntityView tracked the other player (${others.count})`)
check(scene.objects.size === 1, 'and added exactly one mesh to the scene')

// The control: a client that never reads its snapshots cannot possibly be
// exact, so a suite reporting zero mispredictions for one that does is
// measuring something.
const c = await connectClient('C')
for (let i = 0; i < 120; i++) {
  const f = { buttons: 0, moveX: axisFromNumber(1), moveY: 0, aim: 0 }
  c.predictor.step(f)
  c.history = [f, ...c.history].slice(0, 8)
  c.conn.send(c.predictor.tick, 0, c.history)
  c.snapshots.length = 0          // discard every snapshot
  await sleep(15)
}
check(c.predictor.stats.exactMatches === 0,
      'CONTROL: a client that discards its snapshots matches nothing (0)')

try { a.conn.close(); b.conn.close(); c.conn.close() } catch {}
await edge.stop()
server.kill('SIGTERM')

console.log(`\nlive: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
