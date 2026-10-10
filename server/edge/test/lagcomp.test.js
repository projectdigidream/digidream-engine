//
// LAG COMPENSATION (docs/WEAPON_PREDICTION.md, phase E), end to end.
//
// A body walks to and fro across a room. A client on a 140 ms round trip aims
// exactly where it DRAWS that body -- the newest snapshot it holds, less the
// interpolation delay, as client/src/render/entities.js draws it -- says in
// the frame which tick that was (InputFrame::view), and fires a single exact
// ray (maps/scripts/lagcomp_test.lua). The server reports each shot as HIT or
// MISS.
//
//   on        the server judges the shot against the walker where this
//             client saw it: the shots land.
//   capped    on a 600 ms round trip the walker was seen further back than
//             the cap (MAX_REWIND_TICKS, 250 ms) lets a shot be judged: the
//             same aim mostly misses, as it should -- a slow link does not get
//             to shoot into the distant past.
//   no view   compensation on, but frames that name no tick (view 0): judged
//             as now, so they miss -- the server goes by what the client says
//             it drew, not by anything it infers.
//   control   the same, with compensation off (--lag-comp 0): the server
//             judges against where the walker is NOW, which by the time the
//             shot arrives is a body's width or more along -- the shots miss.
//             Without this, "they land" would say nothing about rewinding.
//
import { spawn } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { Core } from '../../../client/src/core.js'
import { MapAccumulator } from '../../../client/src/maploader.js'
import { Predictor } from '../../../client/src/predictor.js'
import { INTERP_DELAY_TICKS } from '../../../client/src/render/entities.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  MSG, BUTTON, NO_WEAPON, ENT_FIELD, viewByte
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'
import { bakedMapPath } from './helpers.js'
import { isOwnParams } from '../../../client/src/remoteparams.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const REPO = join(ROOT, '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34454)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34528)
const LATENCY_MS = Number(process.env.DNP_LATENCY_MS ?? 70)
const SHOTS = 24

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, label) => {
  const deadline = Date.now() + ms
  while (Date.now() < deadline) { if (fn()) return true; await sleep(20) }
  throw new Error(`timeout: ${label}`)
}

const MAP = bakedMapPath(ROOT)
if (!MAP) { console.error('no baked test map'); process.exit(2) }
const core = await Core.load()
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const SCRIPTS = ['mob_vocab.lua', 'mobs.lua', 'weapon_vocab.lua', 'weapons.lua', 'lagcomp_test.lua']
  .flatMap((s) => ['--script', join(ROOT, 'maps', 'scripts', s)])

async function run (lagComp, latencyMs = LATENCY_MS, sayView = true) {
  const args = ['--port', String(LINK_PORT), '--map', MAP, '--controls', 'fps',
                '--props', join(REPO, 'modules', 'doom2', 'assets', 'props.txt'),
                '--lag-comp', String(lagComp), ...SCRIPTS]
  const server = spawn(serverBin, args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
  let log = ''
  server.stdout.on('data', (d) => { log += d.toString() })
  server.stderr.on('data', (d) => { log += d.toString() })
  process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })
  await waitFor(() => log.includes('listening for edge'), 15000, 'server listening')
  const edge = new Edge({
    port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
    cert: readFileSync(join(CERTS, 'cert.pem')),
    privKey: readFileSync(join(CERTS, 'key.pem')),
    netsim: { latencyMs, jitterMs: 0, lossPct: 0, reorderPct: 0, seed: 3 },
    log: () => {}
  })
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge link')

  const c = { snapshots: [], pending: [], params: [], welcome: null, defs: null,
              mapAcc: new MapAccumulator(), history: [], walker: new Map() }
  c.wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
    serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
  })
  await c.wt.ready
  const control = await c.wt.createBidirectionalStream()
  const cw = control.writable.getWriter()
  c.dg = c.wt.datagrams.createWritable().getWriter()
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
          if (m?.type === MSG.WELCOME) c.welcome = m
          else if (m?.type === MSG.MAP_DATA) c.mapAcc.push(m)
          else if (m?.type === MSG.PARAM_UPDATE) {
            if (c.world && isOwnParams(m, (c.welcome.selfHandle & 0x00ffffff) - 1)) { c.world.applyParamUpdate(c.agent, m); c.predictor.paramEpoch = m.epoch }
            else c.params.push(m)
          } else if (m?.type === MSG.WEAPON_DEFS) {
            c.defs = m.bytes
            if (c.world) c.world.setWeapons(c.defs, c.agent, c.serverAgent)
          }
        }
      }
    } catch {}
  })()
  ;(async () => {
    const r = c.wt.datagrams.readable.getReader()
    try {
      for (;;) {
        const { value, done } = await r.read()
        if (done) break
        const s = decodeSnapshot(Buffer.from(value))
        if (s) { c.snapshots.push(s); c.pending.push(s) }
      }
    } catch {}
  })()

  await cw.write(encodeHello())
  await waitFor(() => c.welcome, 8000, 'welcome')
  await waitFor(() => c.mapAcc.complete || c.mapAcc.failed, 8000, 'map')
  const { map } = core.loadMap(c.mapAcc.bytes())
  c.world = core.createWorld({ map })
  c.agent = c.world.spawn(map.spawnForTag('start'))
  c.serverAgent = (c.welcome.selfHandle & 0x00ffffff) - 1
  c.predictor = new Predictor(c.world, c.agent)
  for (const m of c.params.splice(0)) { if (!isOwnParams(m, (c.welcome.selfHandle & 0x00ffffff) - 1)) continue; c.world.applyParamUpdate(c.agent, m); c.predictor.paramEpoch = m.epoch }
  if (c.defs) c.world.setWeapons(c.defs, c.agent, c.serverAgent)

  // The walker's positions by snapshot tick: the one other body.
  const drain = () => {
    while (c.pending.length) {
      const s = c.pending.shift()
      for (const e of s.entities ?? []) {
        const index = (e.handle & 0x00ffffff) - 1
        if (index === c.serverAgent || !(e.mask & ENT_FIELD.POS)) continue
        c.walkerIndex = c.walkerIndex ?? index
        if (index === c.walkerIndex) c.walker.set(s.serverTick, e.pos)
      }
      // A position not resent is unchanged: carry it forward.
      if (c.walkerIndex !== undefined && !c.walker.has(s.serverTick)) {
        const prev = [...c.walker.keys()].filter((t) => t < s.serverTick).sort((a, b) => b - a)[0]
        if (prev !== undefined) c.walker.set(s.serverTick, c.walker.get(prev))
      }
      c.predictor.reconcile(s)
    }
  }
  const frame = async (f) => {
    c.predictor.step(f)
    c.history.unshift(f)
    c.history = c.history.slice(0, 12)
    await c.dg.write(encodeInputBundle({
      clientTick: c.predictor.tick,
      ackSnapshotTick: c.snapshots.at(-1)?.serverTick ?? 0,
      frames: c.history
    }))
  }

  for (let i = 0; i < 60; i++) { await frame({ weapon: NO_WEAPON, view: 0 }); drain(); await sleep(16) }
  const EYE_H = Number(c.world.getParamRaw(c.agent, 1)) / 4294967296 * core.eyeFraction
  const WALKER_H = 56 / 48   // the catalogue's zombieman
  await waitFor(() => c.predictor.synced && c.walkerIndex !== undefined, 8000, 'sync and the walker')

  // AIM WHERE IT IS DRAWN: the walker at the newest snapshot this client holds,
  // less the interpolation delay -- and the frame says so.
  let shots = 0
  for (let i = 0; i < SHOTS * 30 + 30; i++) {
    const newest = c.snapshots.at(-1).serverTick
    const there = c.walker.get(newest - INTERP_DELAY_TICKS)
    const me = c.predictor.state().pos
    let aim = 0
    let pitch = 0
    if (there) {
      const a = Math.atan2(Number(there.y - me.y), Number(there.x - me.x))
      aim = Math.round((a / (2 * Math.PI)) * 4294967296) >>> 0
      // And DOWN to its middle, from the eye a first-person shot leaves from
      // (the core's EYE_FRACTION): the room's body is taller than a zombie.
      const fx = (v) => Number(v) / 4294967296
      const eye = fx(me.z) + EYE_H
      const mid = fx(there.z) + WALKER_H / 2
      const run = Math.hypot(fx(there.x - me.x), fx(there.y - me.y))
      pitch = Math.round(Math.atan2(mid - eye, run) / (2 * Math.PI) * 65536)
    }
    const fire = there !== undefined && i % 30 === 15 && shots < SHOTS
    if (fire) shots++
    await frame({ buttons: fire ? BUTTON.ATTACK_PRIMARY : 0, aim, pitch, weapon: NO_WEAPON,
                  view: sayView ? viewByte(newest - INTERP_DELAY_TICKS) : 0 })
    drain()
    await sleep(16)
  }
  await sleep(600)
  const hits = (log.match(/\[lagcomp\] HIT/g) ?? []).length
  const misses = (log.match(/\[lagcomp\] MISS/g) ?? []).length
  try { c.wt.close() } catch {}
  await edge.stop()
  server.kill('SIGTERM')
  await sleep(300)
  return { hits, misses, shots }
}

console.log(`  a ${LATENCY_MS * 2} ms round trip, ${SHOTS} shots at a walking body, aimed where it is drawn`)
const on = await run(15)
console.log(`  compensated:   ${on.hits} hit, ${on.misses} missed of ${on.shots}`)
const off = await run(0)
console.log(`  uncompensated: ${off.hits} hit, ${off.misses} missed of ${off.shots}`)
const far = await run(15, 300)
const blind = await run(15, LATENCY_MS, false)
console.log(`  no view tick:  ${blind.hits} hit, ${blind.misses} missed of ${blind.shots}`)
console.log(`  600 ms, capped: ${far.hits} hit, ${far.misses} missed of ${far.shots}`)

check(on.hits + on.misses >= SHOTS * 0.9 && off.hits + off.misses >= SHOTS * 0.9,
      `the server judged the shots (${on.hits + on.misses} and ${off.hits + off.misses} of ${SHOTS})`)
check(on.hits >= SHOTS * 0.8, `compensated, the shots land where they were aimed (${on.hits} of ${SHOTS})`)
check(off.hits <= SHOTS * 0.3, `CONTROL: uncompensated, the same shots mostly miss (${off.hits} of ${SHOTS})`)
check(blind.hits + blind.misses >= SHOTS * 0.9 && blind.hits <= SHOTS * 0.3,
      `CONTROL: frames that name no tick are judged as now, and miss (${blind.hits} of ${blind.hits + blind.misses})`)
check(far.hits + far.misses >= SHOTS * 0.9 && far.hits <= SHOTS * 0.3,
      `capped: a 600 ms link cannot shoot into the distant past (${far.hits} of ${far.hits + far.misses})`)

console.log(`\nlagcomp: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
