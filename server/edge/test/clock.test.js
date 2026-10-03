//
// THE CLIENT STEERS ITS CLOCK (main.js, Predictor._steerClock): the server
// holds the few ticks of our input it needs, and no more. Every tick it holds
// is 1/60 s between a press and its effect, and nothing used to read the
// predictor's adjustment: the lead was whatever it was at the start plus every
// catch-up burst after a hitch, and only ever grew.
//
//   A client paced as main.js paces it (a 60 Hz wall-clock accumulator) is
//   pushed 15 ticks ahead, as a hitch or a backgrounded tab does. With the
//   steering, the server's held margin comes back to the target within two
//   seconds, and the time from an input to the snapshot that applies it drops
//   with it. CONTROL: the same client not steering stays ahead.
//
// (What follows is the session weapons.test.js uses, trimmed.)
//
// YOUR OWN WEAPON, PREDICTED (docs/WEAPON_PREDICTION.md, phases C and D), end
// to end against a real server running DOOM II's weapons as the game's scripts.
//
// The weapon machine runs in the core on both sides: the server's weapon
// runtime steps it in on_tick, and this client's wasm world steps it at the
// same point of its own tick, from the table the server sent (WEAPON_DEFS).
// Its state is in the prediction hash and in the self block, so the claim is
// the one prediction.test.js makes for movement, and it is as absolute: firing,
// holding, tapping, switching and running dry, ZERO mispredictions.
//
//   fire      press, hold, tap, walk while firing: ammunition goes down, the
//             phases change, and every snapshot agrees with what this client
//             predicted for that tick.
//   switch    a number key is a weapon wanted in the input, for one tick:
//             predicted, and the server switches on the same tick.
//   drawn     what the client drew for its own shot the tick it predicted it --
//             a tracer per pellet, the muzzle flash -- is what the server
//             sends for it, field for field. The server is asked to send the
//             shooter its own copies back for this (DNP_ECHO_PREDICTED=1);
//             it otherwise does not, so they are never drawn twice:
//   no echo   a second run, the ordinary way: the shooter is sent none of its
//             own predicted effects.
//   rockets   a third run with every weapon given (DNP_GIVE_ALL=1): our
//             rockets flown here (render/ghostmissiles.js) burst where the
//             server's do, and the server's blast and rocket are recognised
//             as copies of ours so neither is drawn twice.
//   knock     in the same run, the bodies our rockets shove are shoved here
//             at once (render/knock.js), the way the server then moves them.
//   present   in the second run, a moving body near us drawn at the present
//             (render/nearnow.js) is nearer where the server has it then than
//             the interpolated picture is.
//   jump      a fourth run, first-person: a rocket at your own feet throws you
//             on the tick its ghost bursts, on the server's bits -- the jump
//             is predicted, so not one comparison needs excusing. CONTROL:
//             without the predicted shove the server's has to be adopted.
//   control   the same client without the table -- its machine does not run --
//             mispredicts as soon as it fires. Without this, zero would only
//             say the weapon state is not being compared at all.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { Core } from '../../../client/src/core.js'
import { MapAccumulator } from '../../../client/src/maploader.js'
import { Predictor } from '../../../client/src/predictor.js'
import { parseProps } from '../../../client/src/render/props.js'
import { GhostMissiles } from '../../../client/src/render/ghostmissiles.js'
import { KnockLayer, shoveOf } from '../../../client/src/render/knock.js'
import { NearPresent } from '../../../client/src/render/nearnow.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  numberToFx, MSG, BUTTON, NO_WEAPON, ENT_FIELD
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'
import { fpsFrame, mirrorEntities } from './helpers.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const REPO = join(ROOT, '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34472)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34546)
const FPS = process.env.DNP_CONTROLS === 'fps'

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

const MAP = join(REPO, 'modules', 'doom2', 'maps', 'map01.dnpm')
if (!existsSync(MAP)) { console.error('no baked modules/doom2/maps/map01.dnpm'); process.exit(2) }

const core = await Core.load()
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
// What a body is, for mirroring the others: the catalogue the server loads.
const catalogue = parseProps(readFileSync(join(ROOT, 'assets', 'props.txt'), 'utf8') + '\n' +
                             readFileSync(join(REPO, 'modules', 'doom2', 'assets', 'props.txt'), 'utf8'))
const mirrorBase = core.M._dnp_mirror_base()

// A snapshot carries an effect's position quantised (bytes.hpp fx_to_fxq, 2^-12
// of a unit, truncated towards zero); a comparison applies the same.
const q = (raw) => (raw < 0n ? -((-raw) >> 20n) : raw >> 20n) << 20n
const vfxKey = (v) => `${v.kind}/${q(v.pos.x)}/${q(v.pos.y)}/${q(v.pos.z)}/${v.dir >>> 0}/${v.param >>> 0}`

// ------------------------------------------------------------- one session
//
// A server, an edge, and prediction.test.js's client plus what main.js does
// with WEAPON_DEFS (the bytes to the wasm world, with our body's index ON THE
// SERVER, whose random stream the spread is drawn from) and with other bodies
// (mirrored, so a predicted shot stops where the server's does).

async function session ({ echo, giveAll = false, fps = false, netsim = null }) {
  const args = ['--port', String(LINK_PORT), '--module', 'doom2', '--map', MAP]
  if (FPS || fps) args.push('--controls', 'fps')
  if (process.env.DIV) args.push('--snapshot-divisor', process.env.DIV)
  const env = { ...process.env, DNP_ECHO_PREDICTED: echo ? '1' : '0', DNP_GIVE_ALL: giveAll ? '1' : '0' }
  const server = spawn(serverBin, args, { cwd: ROOT, env, stdio: ['ignore', 'pipe', 'pipe'] })
  let log = ''
  server.stdout.on('data', (d) => { log += d.toString() })
  server.stderr.on('data', (d) => { log += d.toString() })
  process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })
  await waitFor(() => log.includes('listening for edge'), 15000, 'server listening')
  const edge = new Edge({
    port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
    cert: readFileSync(join(CERTS, 'cert.pem')),
    privKey: readFileSync(join(CERTS, 'key.pem')),
    log: () => {},
    netsim: netsim ?? undefined
  })
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge link')

  const c = {
    snapshots: [], pending: [], params: [], welcome: null, defs: null,
    mapAcc: new MapAccumulator(), history: [], predictedVfx: [], kinds: new Map()
  }
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
            // Applied AND its epoch adopted, as main.js does: the weapons change
            // the swing's parameters, and a predictor left on epoch 0 files every
            // mismatch after that as "rules in flight" instead of comparing it.
            if (c.world) { c.world.applyParamUpdate(c.agent, m); c.predictor.paramEpoch = m.epoch }
            else c.params.push(m)
          } else if (m?.type === MSG.WEAPON_DEFS) {
            c.defs = m.bytes
            if (c.world) c.applyDefs()
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
        if (s) { s.arrived = performance.now(); c.snapshots.push(s); c.pending.push(s) }
      }
    } catch {}
  })()

  await cw.write(encodeHello())
  await waitFor(() => c.welcome, 8000, 'welcome')
  await waitFor(() => c.mapAcc.complete || c.mapAcc.failed, 15000, 'map')
  const { ok: mapOk, map } = core.loadMap(c.mapAcc.bytes())
  if (!mapOk) throw new Error('client rejected the map')
  c.world = core.createWorld({ map })
  c.agent = c.world.spawn(map.spawnForTag('start') ?? { x: 0n, y: 0n, z: 0n })
  c.serverAgent = (c.welcome.selfHandle & 0x00ffffff) - 1
  c.predictor = new Predictor(c.world, c.agent)
  for (const m of c.params.splice(0)) { c.world.applyParamUpdate(c.agent, m); c.predictor.paramEpoch = m.epoch }
  c.applyDefs = () => { c.defsOk = c.world.setWeapons(c.defs, c.agent, c.serverAgent) }
  if (c.defs) c.applyDefs()
  // Our rockets, flown as main.js flies them; what they drew, with when.
  c.ghostBlasts = []
  c.ghosts = new GhostMissiles({
    world: c.world,
    agent: c.agent,
    draw: { make: () => ({}), place () {}, burst () {}, remove () {},
            blast: (v) => c.ghostBlasts.push({ v, at: Date.now() }) },
    // As main.js: our own blast's shove, in the step about to run.
    selfBlast: (impact, blast) => {
      if (c.noSelfShove) return
      const v = c.world.blastShove(impact, c.world.readAgent(c.agent).pos, blast)
      if (v) { c.predictor.addImpulse(c.predictor.tick + 1, v); c.shoves++ }
    }
  })
  c.shoves = 0
  // As main.js: our blast's shove of the bodies around it, by the server's
  // rule, at their last known positions, landing at the server tick of the
  // step the burst lands in.
  c.knock = new KnockLayer()
  c.bodies = new Map()          // index -> { pos, kind, track: [{ tick, x, y }] }
  c.ghosts.onBlast = (impact, blast) => {
    const ls = c.predictor.lastSnapshot
    if (!ls) return
    const S = c.predictor.tick + 1 + (ls.serverTick - ls.lastInputTick)
    for (const [index, b] of c.bodies) {
      if (index === c.serverAgent || !b.pos) continue
      const mass = b.kind ? c.world.mobMass(b.kind) : 100
      if (!mass) continue
      const def = b.kind ? catalogue.kinds.get(b.kind) : { radius: 0.5, height: 2 }
      if (!def) continue
      const sh = shoveOf(c.world, impact, b.pos, blast, mass, def)
      if (sh) c.knock.add(index, S, sh.v, sh.path)
    }
  }
  c.launches = 0

  c.frame = async (input) => {
    let f = {
      buttons: input.buttons ?? 0,
      moveX: core.axisFromFxRaw(numberToFx(input.moveX ?? 0)),
      moveY: core.axisFromFxRaw(numberToFx(input.moveY ?? 0)),
      aim: input.aim ?? 0,
      pitch: input.pitch ?? 0,
      weapon: input.weapon ?? NO_WEAPON
    }
    if (FPS) f = { ...fpsFrame(f), weapon: f.weapon }
    // As main.js: our missiles fly before the step, launches join after it.
    c.ghosts.tick()
    c.predictor.step(f)
    for (const v of c.predictor.weapon?.vfx ?? []) c.predictedVfx.push(v)
    for (const l of c.predictor.weapon?.launches ?? []) { c.ghosts.launch(l); c.launches++ }
    c.history.unshift(f)
    c.history = c.history.slice(0, 12)
    c.sentAt = c.sentAt ?? new Map()
    c.sentAt.set(c.predictor.tick, performance.now())
    await c.dg.write(encodeInputBundle({
      clientTick: c.predictor.tick,
      ackSnapshotTick: c.snapshots.at(-1)?.serverTick ?? 0,
      frames: c.history
    }))
  }
  c.drain = () => {
    while (c.pending.length) {
      const s = c.pending.shift()
      mirrorEntities(c.world, s, {
        catalogue, kinds: c.kinds, mirrorBase, selfIndex: c.serverAgent,
        bodyRadius: Number(c.world.getParamRaw(c.agent, 0)) / 4294967296,
        bodyHeight: Number(c.world.getParamRaw(c.agent, 1)) / 4294967296,
        ENT_FIELD, handleIndex: (h) => (h & 0x00ffffff) - 1
      })
      c.predictor.reconcile(s)
      for (const e of s.entities ?? []) {
        const index = (e.handle & 0x00ffffff) - 1
        if (e.mask & ENT_FIELD.DESTROYED) { c.bodies.delete(index); continue }
        const b = c.bodies.get(index) ?? { track: [] }
        if (e.displayId) b.kind = e.displayId
        if ((e.mask & ENT_FIELD.POS) && e.pos) {
          b.pos = e.pos
          b.track.push({ tick: s.serverTick, x: Number(e.pos.x) / 4294967296, y: Number(e.pos.y) / 4294967296 })
          if (b.track.length > 400) b.track.shift()
        }
        c.bodies.set(index, b)
      }
      c.onSnapshot?.(s)
      c.guessNow?.()
    }
  }
  c.zero = () => {
    for (const k of ['exactMatches', 'mispredictions', 'starvedDivergences', 'luaApplied', 'paramsStale']) {
      c.predictor.stats[k] = 0
    }
  }
  c.report = (what) => {
    const st = c.predictor.stats
    console.log(`  ${what}: exact ${st.exactMatches}  mispredicted ${st.mispredictions}  ` +
                `starved ${st.starvedDivergences}  lua ${st.luaApplied}  stale ${st.paramsStale}`)
    return st
  }
  // Every tick's predicted weapon, to show a run did something.
  c.seen = { phases: new Set(), currents: new Set(), ammoLow: Infinity, ammoStart: null }
  c.play = async (ticks, script) => {
    for (let i = 0; i < ticks; i++) {
      const input = script(i)
      await c.frame(input)
      c.drain()
      const k = c.world.readWeapon(c.agent)
      c.seen.phases.add(`${k.armed}/${k.phase}`)
      c.seen.currents.add(k.current)
      if (c.seen.ammoStart === null) c.seen.ammoStart = k.ammo.reduce((a, b) => a + b, 0)
      c.seen.ammoLow = Math.min(c.seen.ammoLow, k.ammo.reduce((a, b) => a + b, 0))
      // As main.js now does: a tick more, or one fewer.
      const adj = c.predictor.takeClockAdjust()
      if (adj > 0) await c.frame({ ...input, weapon: NO_WEAPON })
      if (adj < 0) { i++; continue }
      await sleep(15)
    }
    await sleep(250)
    c.drain()
  }
  c.close = async () => {
    try { c.wt.close() } catch {}
    await edge.stop()
    server.kill('SIGTERM')
    await sleep(300)
  }

  for (let i = 0; i < 40; i++) { await c.frame({}); c.drain(); await sleep(16) }
  await waitFor(() => c.predictor.synced && c.defs, 8000, 'first sync and the weapon table')
  c.slots = []
  for (let n = 1; n <= 9; n++) {
    const w = c.world.weaponSlot(c.agent, n)
    if (w !== NO_WEAPON) c.slots.push(w)
  }
  return c
}


const DT = 1000 / 60
const med = (xs) => xs.length ? [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)] : NaN

// One run: optionally a 15-tick burst ahead, then `ticks` at 60 Hz, paced as
// main.js paces it, steering or not, against a given target.
// Reported over the last second (margin, latency) and over the whole run
// (ticks the server repeated because our input was late: starvation).
async function run ({ steer = true, target = 2, burst = 15, ticks = 180, netsim = null } = {}) {
  const c = await session({ echo: false, netsim })
  c.predictor.targetMargin = target
  for (let i = 0; i < burst; i++) await c.frame({})       // a hitch's catch-up burst
  let acc = 0
  let last = performance.now()
  let n = 0
  const startSnap = c.snapshots.length
  const repeats0 = c.predictor.stats.serverRepeats
  while (n < ticks) {
    await sleep(4)
    const now = performance.now()
    acc += now - last
    last = now
    const adj = c.predictor.takeClockAdjust()
    if (steer) {
      if (adj > 0) acc += DT * adj
      else if (adj < 0) acc = Math.max(0, acc + DT * adj)
    }
    let budget = 8
    while (acc >= DT && budget-- > 0) { await c.frame({}); acc -= DT; n++ }
    if (acc > DT) acc = 0
    c.drain()
  }
  await sleep(300)
  c.drain()
  const snaps = c.snapshots.slice(startSnap)
  const late = snaps.slice(-30)                         // the last second
  const lat = []
  for (const s of late) {
    const t = c.sentAt.get(s.lastInputTick)
    if (t !== undefined) lat.push(s.arrived - t)
  }
  const out = { margin: med(late.map((s) => s.inputMargin)), latency: med(lat),
                target: c.predictor.targetMargin, repeats: c.predictor.stats.serverRepeats - repeats0 }
  await c.close()
  return out
}

// THE BURST: steering brings the lead back; not steering, it stays.
const on = await run()
const off = await run({ steer: false })
console.log(`  after a burst -- steering: held margin ${on.margin} ticks, input to its snapshot ${on.latency.toFixed(1)} ms; ` +
            `not steering: ${off.margin} ticks, ${off.latency.toFixed(1)} ms`)
check(on.margin <= 3, `steered, the server holds a few ticks of input (${on.margin})`)
check(off.margin >= 10, `CONTROL: not steered, the burst's lead stays (${off.margin})`)
check(on.latency < off.latency - 100, `and the time from a press to its effect falls with it (${on.latency.toFixed(0)} vs ${off.latency.toFixed(0)} ms)`)

console.log(`\nclock: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
