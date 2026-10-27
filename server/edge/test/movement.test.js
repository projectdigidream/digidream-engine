//
// THE MOVEMENT STATES ON A REAL SERVER (docs/MOVEMENT.md): modules/movetest's
// course, a bay at a time, by a client that predicts with the real Predictor --
// the server's MOVE_DEFS text in its core, the module's `player` rules applied
// from PARAM_UPDATE, every snapshot reconciled.
//
// The claim, for each bay: the body does what the bay is for, AND the client
// predicted all of it -- zero mispredictions, a script's changes excused as the
// script's (LUA_APPLIED). That is the half no unit test can see: a field the
// controller carries and the snapshot does not was found exactly this way, by
// hand, at the top of every climb (engine ed0620b). This is that run, kept.
//
//   walkway  a dash (a layered move) and a slide off a run
//   pool     wade the margin, swim the pit, dive, and come up -- not out
//   mud      the script switches the dash off in it, and on again out of it
//   ledge    the glide, switched off until the ledge, clears the gap
//   vine     climb it (climb_auto, from the module) and mantle at the top
//
// CONTROLS, or zero mispredictions proves nothing:
//   - the pool with the client given NO movement definitions: it predicts
//     plain walking where the server swims, and must mispredict;
//   - the vine with the climb normal dropped from every snapshot, as before
//     ed0620b: the mantle must mispredict.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
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
  axisFromNumber, fxToNumber, MSG, BUTTON
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'
import { isOwnParams } from '../../../client/src/remoteparams.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const MOD = join(ROOT, '..', 'modules', 'movetest')
const CERTS = join(HERE, '..', 'certs')
const LINK = Number(process.env.DNP_LINK_PORT ?? 34583)
const WT = Number(process.env.DNP_WT_PORT ?? 34481)

if (!existsSync(join(MOD, 'maps', 'move.dnpm'))) {
  console.log('movement: skipped (no ../modules/movetest)')
  process.exit(0)
}

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, l) => {
  const end = Date.now() + ms
  while (Date.now() < end) { if (fn()) return; await sleep(20) }
  throw new Error(`timeout: ${l}`)
}

const certHash = Buffer.from(JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

// The course's names, by index (module.txt).
const STATE = { default: 0, slide: 1, ice: 2, mud: 3, wade: 4, swim: 5, glide: 6, cling: 7, stunned: 8 }
const MOVE = { dash: 1 }           // a running move's number is its index + 1
const EAST = 0
const NORTH = 2 ** 30

// A predicting client's lead over the newest snapshot (doors.test.js says why
// both ends of the band matter).
const TARGET_LEAD = 6
const MAX_LEAD = 14

/**
 * One server, one client, at a spawn tag; `drive(c)` steps it. Returns what
 * the snapshots showed and the predictor's stats. `noDefs` withholds the
 * movement definitions from the client; `dropClimbNormal` takes the climb
 * normal out of every snapshot before reconciling, as clients were before it
 * was sent.
 */
async function session (spawnTag, drive, { noDefs = false, dropClimbNormal = false } = {}) {
  // Its own save folder: the course is a module with a game, and a save left by
  // one session must not place the next one anywhere but its spawn.
  const saves = mkdtempSync(join(tmpdir(), 'dnp-movement-'))
  const server = spawn(serverBin, ['--port', String(LINK), '--map', join(MOD, 'maps', 'move.dnpm'), '--module', MOD,
                                   '--spawn', spawnTag, '--state-dir', saves],
                       { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
  let log = ''
  server.stderr.on('data', (d) => { log += d }); server.stdout.on('data', (d) => { log += d })
  try {
    await waitFor(() => log.includes('listening for edge'), 8000, 'server')
  } catch (e) { server.kill('SIGKILL'); throw new Error(`${e.message}\n${log.slice(-600)}`) }
  const edge = new Edge({ port: WT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK,
                          cert: readFileSync(join(CERTS, 'cert.pem')), privKey: readFileSync(join(CERTS, 'key.pem')), log: () => {} })
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge')
  const wt = new WebTransport(`https://127.0.0.1:${WT}/dnp`, { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
  await wt.ready
  const control = await wt.createBidirectionalStream()
  const cw = control.writable.getWriter()
  const dw = wt.datagrams.createWritable().getWriter()

  let welcome = null
  let moveDefs = null
  const acc = new MapAccumulator()
  const params = []
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
          else if (m?.type === MSG.PARAM_UPDATE) params.push(m)
          else if (m?.type === MSG.MOVE_DEFS) moveDefs = m.text
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
        if (s?.self) pending.push(s)
      }
    } catch {}
  })()

  await cw.write(encodeHello())
  await waitFor(() => welcome && acc.complete && moveDefs !== null, 8000, 'welcome, map and movement')
  const { ok, map } = core.loadMap(acc.bytes())
  if (!ok || map.hash !== welcome.mapHash) throw new Error('map mismatch')
  const world = core.createWorld({ map })
  const agent = world.spawn(map.spawnForTag(spawnTag))
  const why = noDefs ? null : world.setMoveDefs(moveDefs)
  if (why) throw new Error(`the client's core refused the module's movement: ${why}`)
  const self = (welcome.selfHandle & 0x00ffffff) - 1
  const applyParams = () => { for (const m of params.splice(0)) if (isOwnParams(m, self)) { world.applyParamUpdate(agent, m); predictor.paramEpoch = m.epoch } }
  const predictor = new Predictor(world, agent)
  applyParams()

  let history = []
  let newest = 0
  const seen = []                    // the authoritative self, every snapshot
  const drain = () => {
    while (pending.length) {
      const s = pending.shift()
      newest = Math.max(newest, s.serverTick)
      seen.push(s.self)
      if (dropClimbNormal) delete s.self.climbNormal
      predictor.reconcile(s)
    }
  }
  const tick = async (f) => {
    const frame = { buttons: f.buttons ?? 0, moveX: axisFromNumber(f.x ?? 0), moveY: axisFromNumber(f.y ?? 0), aim: f.aim ?? 0 }
    const send = () => {
      applyParams()
      predictor.step(frame)
      history = [frame, ...history].slice(0, 12)
      return dw.write(encodeInputBundle({ clientTick: predictor.tick, ackSnapshotTick: newest, frames: history }))
    }
    if (newest > 0 && predictor.tick > newest + MAX_LEAD) { drain(); await sleep(8); return }
    await send()
    for (let i = 0; i < 4 && newest > 0 && predictor.tick < newest + TARGET_LEAD; i++) await send()
    drain()
    await sleep(8)
  }
  // N INPUTS, which is N server ticks -- not N iterations, which the lead cap
  // makes anything from one to five sends each.
  const hold = async (n, f = {}) => { const until = predictor.tick + n; while (predictor.tick < until) await tick(f) }
  // Settle: the join, its PARAM_UPDATE, a few snapshots, before anything is judged.
  await hold(40)
  const first = seen[0]
  const st0 = { ...predictor.stats }
  const mark = seen.length
  const last = () => seen[seen.length - 1]
  await drive({ hold, last })
  await hold(30)
  const st = predictor.stats
  const out = {
    seen: seen.slice(mark),
    mispredicted: st.mispredictions - st0.mispredictions,
    lua: st.luaApplied - st0.luaApplied,
    exact: st.exactMatches - st0.exactMatches,
    end: last(),
    first
  }
  try { wt.close() } catch {}
  await edge.stop()
  server.kill('SIGTERM')
  await sleep(300)
  return out
}

const fx = (v) => fxToNumber(v)
const states = (r) => new Set(r.seen.map((s) => s.moveState))
const report = (name, r) => console.log(`  ${name}: exact ${r.exact}, mispredicted ${r.mispredicted}, lua ${r.lua}`)

// --- the walkway: a dash and a slide ----------------------------------------
console.log('\n--- the walkway: a dash, a slide ---')
{
  const r = await session('start', async (c) => {
    await c.hold(20, { x: 1, aim: EAST })                                // run east
    await c.hold(1, { x: 1, aim: EAST, buttons: BUTTON.DASH })           // SHIFT
    await c.hold(40, { x: 1, aim: EAST })                                // up to speed again
    await c.hold(1, { x: 1, aim: EAST, buttons: BUTTON.CROUCH })         // C: a slide
    await c.hold(60, { aim: EAST })
  })
  report('walkway', r)
  check(r.seen.some((s) => s.moves[0].move === MOVE.dash), 'SHIFT dashed: the layered move ran on the server')
  check(states(r).has(STATE.slide), 'C off a run slid')
  // Never found here, the glide stays off in every snapshot: `flags off`, as the wire carries it.
  const glideBit = 1 << (STATE.glide - 1)
  check(r.seen.length > 20 && r.seen.every((x) => (x.statesOff & glideBit) !== 0), 'the glide starts switched off (module.txt `flags off`), and stays off here')
  check(r.mispredicted === 0, `and the client predicted it all (mispredicted ${r.mispredicted})`)
}

// --- the pool: wade, swim, dive, come up ---------------------------------------
const pool = async (c) => {
  await c.hold(240, { y: 0.5, aim: NORTH })                            // over the margin, into the pit
  await c.hold(60, { aim: NORTH })
  await c.hold(100, { aim: NORTH, buttons: BUTTON.CROUCH })            // down
  await c.hold(240, { aim: NORTH })                                    // let go: up on buoyancy
}
console.log('\n--- the pool: wade, swim, a dive ---')
{
  const r = await session('pool', pool)
  report('pool', r)
  const s = states(r)
  check(s.has(STATE.wade) && s.has(STATE.swim), 'waded the margin and swam the pit')
  const swimming = r.seen.filter((x) => x.moveState === STATE.swim)
  const low = Math.min(...swimming.map((x) => fx(x.pos.z)))
  const high = Math.max(...swimming.slice(swimming.findIndex((x) => fx(x.pos.z) === low)).map((x) => fx(x.pos.z)))
  check(low < -2.5, `dived deep (${low.toFixed(2)})`)
  check(high < 0.4, `and came up to the surface, not out of it (feet at most ${high.toFixed(2)}; the surface is 0.4)`)
  check(r.mispredicted === 0, `predicted all of it, depth included (mispredicted ${r.mispredicted})`)
}
{
  const r = await session('pool', pool, { noDefs: true })
  report('pool, no definitions', r)
  check(r.mispredicted > 0, `CONTROL: a client without the movement definitions mispredicts the pool (${r.mispredicted})`)
}

// --- the mud: the dash switched off, and on -------------------------------------
console.log('\n--- the mud: the script switches the dash ---')
{
  let inMud = null
  let outMud = null
  const r = await session('mud', async (c) => {
    const dashed = async (aim) => {
      let n = 0
      await c.hold(1, { aim, buttons: BUTTON.DASH })
      for (let t = 0; t < 20; t++) { await c.hold(1, { aim }); if (c.last().moves[0].move === MOVE.dash) n++ }
      return n
    }
    await c.hold(100, { y: 1, aim: NORTH })                            // into the mud
    await c.hold(20, { aim: NORTH })
    inMud = { off: c.last().movesOff, ticks: await dashed(NORTH) }
    await c.hold(200, { y: -1, aim: (3 * 2 ** 30) >>> 0 })             // back out, south
    await c.hold(20, { aim: EAST })
    outMud = { off: c.last().movesOff, ticks: await dashed(EAST) }
  })
  report('mud', r)
  check(states(r).has(STATE.mud), 'in the mud')
  check(inMud.off === 1 && inMud.ticks === 0, `the script switched the dash off there: SHIFT did nothing (moves_off ${inMud.off})`)
  check(outMud.off === 0 && outMud.ticks > 0, `and on again out of it: SHIFT dashed (moves_off ${outMud.off})`)
  check(r.lua >= 2, `both switches came as the script's (lua ${r.lua})`)
  check(r.mispredicted === 0, `and nothing else mispredicted (${r.mispredicted})`)
}

// --- the ledge: the glide, found ---------------------------------------------------
console.log('\n--- the ledge: the glide, found, over the gap ---')
{
  const r = await session('ledge', async (c) => {
    await c.hold(50, { x: 1, aim: EAST })                              // the script has found it; run east
    await c.hold(190, { x: 1, aim: EAST, buttons: BUTTON.JUMP })       // jump, held
    await c.hold(60, { aim: EAST })
  })
  report('ledge', r)
  const end = r.end
  check(states(r).has(STATE.glide), 'found on the ledge, it glided')
  check(fx(end.pos.x) > 74 && Math.abs(fx(end.pos.z) - 2) < 0.01, `over the gap onto the landing (x ${fx(end.pos.x).toFixed(1)}, z ${fx(end.pos.z).toFixed(2)})`)
  check(r.mispredicted === 0, `predicted (mispredicted ${r.mispredicted}, lua ${r.lua})`)
}

// --- the vine: climb and mantle ------------------------------------------------------
// The stick into the vines (north) and the aim EAST: a climber is turned to
// face the wall whatever the aim says, by the wall's normal -- so a client that
// had not got the normal would turn with the aim instead, and disagree.
const vine = async (c) => {
  await c.hold(420, { y: 1, aim: EAST })
  await c.hold(60, { aim: EAST })
}
console.log('\n--- the vine: climbed, and over the top ---')
{
  const r = await session('vine', vine)
  report('vine', r)
  check(r.seen.some((s) => (s.stateFlags & 2) !== 0), 'pushing into the vines climbed them (climb_auto, from the module)')
  check(Math.abs(fx(r.end.pos.z) - 10) < 0.01 && fx(r.end.pos.y) > 11, `and mantled onto the lookout (y ${fx(r.end.pos.y).toFixed(2)}, z ${fx(r.end.pos.z).toFixed(2)})`)
  check(r.mispredicted === 0, `predicted to the top (mispredicted ${r.mispredicted})`)
}
{
  const r = await session('vine', vine, { dropClimbNormal: true })
  report('vine, no climb normal', r)
  check(r.mispredicted > 0, `CONTROL: a client that loses the climb normal mispredicts the mantle (${r.mispredicted})`)
}

console.log(`\nmovement: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
