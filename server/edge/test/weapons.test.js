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
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34452)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34526)
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

async function session ({ echo, giveAll = false, fps = false }) {
  const args = ['--port', String(LINK_PORT), '--module', 'doom2', '--map', MAP]
  if (FPS || fps) args.push('--controls', 'fps')
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
    log: () => {}
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
        if (s) { c.snapshots.push(s); c.pending.push(s) }
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
      if (c.predictor.takeClockAdjust() > 0) await c.frame({ ...input, weapon: NO_WEAPON })
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

// The firing, in phases of a hundred ticks: held, taps, bursts walking, fast
// taps walking back, held throughout, idle -- and a switch every 70 ticks.
const firing = (slots) => (i) => {
  const p = Math.floor(i / 100)
  const buttons =
    p === 0 ? (i < 60 ? BUTTON.ATTACK_PRIMARY : 0)
    : p === 1 ? ((i % 12) < 2 ? BUTTON.ATTACK_PRIMARY : 0)
    : p === 2 ? ((i % 30) < 20 ? BUTTON.ATTACK_PRIMARY : 0)
    : p === 3 ? ((i % 7) < 3 ? BUTTON.ATTACK_PRIMARY : 0)
    : p === 4 ? BUTTON.ATTACK_PRIMARY
    : 0
  const weapon = (i % 70 === 35) ? slots[Math.floor(i / 70) % slots.length] : NO_WEAPON
  return { buttons, weapon, moveX: p === 2 ? 0.5 : p === 3 ? -0.5 : 0, aim: (i * 3001) >>> 0 }
}

// ===================================================== run 1: the measurement

{
  const c = await session({ echo: true })
  check(c.defs !== null && c.defsOk === true, `the server sent its weapon table (${c.defs?.length} bytes) and this build reads it`)
  check(c.world.hasWeapons, 'the client world predicts a weapon')
  const armed = c.snapshots.at(-1)?.self.weapon
  check(armed !== undefined && armed.have !== 0, 'the self block carries the weapon machine')
  check(c.slots.length >= 2, `more than one weapon held to switch between (${c.slots.length}: ${c.slots.join(', ')})`)

  c.zero()
  c.predictedVfx.length = 0
  const firstSnapshot = c.snapshots.length
  await c.play(600, firing(c.slots))
  const st = c.report('firing and switching')
  check(st.exactMatches > 30, `compared ${st.exactMatches} snapshots against the prediction`)
  check(st.mispredictions === 0, `ZERO mispredictions firing, holding, tapping and switching (got ${st.mispredictions})`)
  check(c.seen.ammoStart - c.seen.ammoLow > 0, `ammunition was spent (${c.seen.ammoStart - c.seen.ammoLow})`)
  check(c.seen.phases.size > 4, `the weapons went through ${c.seen.phases.size} weapon/phase pairs`)
  check(c.seen.currents.size >= 2, `the weapon in hand changed (${[...c.seen.currents].join(', ')})`)
  // And on the server: the weapon in hand changed there too, on the ticks the
  // hashes above say it did.
  const serverCurrents = new Set(c.snapshots.map((x) => x.self.weapon?.current).filter((x) => x !== undefined))
  check(serverCurrents.size >= 2, `the server switched as well (${[...serverCurrents].join(', ')})`)

  // DRAWN AT ONCE, AND THE SAME: every tracer and flash this client drew from
  // its prediction is one the server sent for that shot, field for field.
  const sent = new Set(c.snapshots.slice(firstSnapshot).flatMap((s) => (s.vfx ?? []).map(vfxKey)))
  const mine = c.predictedVfx
  const matched = mine.filter((v) => sent.has(vfxKey(v))).length
  console.log(`  drawn: ${mine.length} predicted effects, ${matched} of them exactly as the server sent them`)
  check(mine.length > 5, `the client drew its own shots at once (${mine.length} effects)`)
  // Exactly, for a shot at a wall or the open; a pellet meeting a monster can
  // differ, since this client holds it where it was last seen.
  check(matched >= mine.length * 0.9, `what it drew is what the server sent for it (${matched} of ${mine.length})`)
  // CONTROL: the comparison can fail -- a tracer one brad off is not found.
  check(mine.length === 0 || !sent.has(vfxKey({ ...mine[0], dir: (mine[0].dir + 1) >>> 0 })),
        'CONTROL: a tracer one brad off is not counted as sent')

  // CONTROL: the same client, its machine stopped. A table it cannot read
  // leaves the world predicting nothing -- and firing then has to mispredict.
  check(c.world.setWeapons(new Uint8Array([1, 2, 3]), c.agent, c.serverAgent) === false,
        'CONTROL: a table this build cannot read is refused')
  c.zero()
  await c.play(240, (i) => ({ buttons: (i % 20) < 10 ? BUTTON.ATTACK_PRIMARY : 0,
                              weapon: (i === 30) ? c.slots[0] : (i === 120 ? c.slots[1] : NO_WEAPON) }))
  const ctl = c.report('CONTROL: no weapon table')
  check(ctl.mispredictions > 0, `CONTROL: without the machine running, firing mispredicts (${ctl.mispredictions})`)
  await c.close()
}

// ====================================================== run 2: never twice

{
  const c = await session({ echo: false })
  // PRESENT: for every moving body near us, at each snapshot, where the
  // present-drawn picture and the interpolated one put it at the server tick
  // our prediction is on -- judged when the server's sample for that tick
  // arrives.
  const np = new NearPresent()
  const guesses = []
  c.guessNow = () => {
    const ls = c.predictor.lastSnapshot
    if (!ls) return
    const now = c.predictor.tick + (ls.serverTick - ls.lastInputTick)
    const me = c.world.readAgent(c.agent).pos
    const mx = Number(me.x) / 4294967296
    const my = Number(me.y) / 4294967296
    for (const [index, b] of c.bodies) {
      if (index === c.serverAgent || b.track.length < 3) continue
      const n = b.track.at(-1)
      if (Math.hypot(n.x - mx, n.y - my) > 10) continue
      const samples = b.track.map((t) => ({ tick: t.tick, x: t.x, y: t.y, z: 0 }))
      const p = np.present(index, samples, now, 1 / 60)
      // The interpolated picture: INTERP_DELAY_TICKS (4) behind the newest.
      const rt = n.tick - 4
      const i = b.track.findLastIndex((t) => t.tick <= rt)
      if (i < 0) continue
      const a = b.track[i]
      const z = b.track[i + 1] ?? a
      const f = z.tick > a.tick ? (rt - a.tick) / (z.tick - a.tick) : 0
      guesses.push({ index, tick: Math.round(now), p, q: { x: a.x + (z.x - a.x) * f, y: a.y + (z.y - a.y) * f } })
    }
  }
  c.predictedVfx.length = 0
  const firstSnapshot = c.snapshots.length
  await c.play(200, (i) => ({ buttons: (i % 24) < 3 ? BUTTON.ATTACK_PRIMARY : 0, aim: (i * 7001) >>> 0 }))
  const sent = new Set(c.snapshots.slice(firstSnapshot).flatMap((s) => (s.vfx ?? []).map(vfxKey)))
  const echoed = c.predictedVfx.filter((v) => sent.has(vfxKey(v))).length
  console.log(`  no echo: ${c.predictedVfx.length} predicted effects, ${echoed} sent back`)
  check(c.predictedVfx.length > 5, `the client drew its own shots (${c.predictedVfx.length})`)
  check(echoed === 0, `the server sent none of them back to be drawn twice (${echoed})`)
  c.guessNow = null
  const errP = []
  const errQ = []
  for (const g of guesses) {
    const t = c.bodies.get(g.index)?.track.find((s) => s.tick === g.tick)
    if (!t) continue
    const eq = Math.hypot(g.q.x - t.x, g.q.y - t.y)
    if (eq < 0.01) continue           // it was not moving: nothing to judge
    errP.push(Math.hypot(g.p.x - t.x, g.p.y - t.y))
    errQ.push(eq)
  }
  const med = (xs) => xs.length ? [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)] : 0
  console.log(`  present: ${errP.length} judged; median error drawn at the present ${med(errP).toFixed(3)}, ` +
              `interpolated ${med(errQ).toFixed(3)} units`)
  check(errP.length > 20, `moving bodies near us were judged (${errP.length})`)
  check(med(errP) < med(errQ) * 0.6, 'drawn at the present, a body is nearer where the server has it now')
  await c.close()
}

// ======================================================= run 3: rockets

{
  const c = await session({ echo: false, giveAll: true })
  const rocketKind = [...catalogue.kinds.entries()].find(([, d]) => d.name === 'rocket')?.[0]
  check(rocketKind !== undefined, `the catalogue has a rocket (${rocketKind})`)
  // Which slot launches one: switch to each and press once.
  let launcher = null
  for (const w of c.slots) {
    await c.play(40, (i) => ({ weapon: i === 0 ? w : NO_WEAPON }))
    const before = c.launches
    await c.play(30, (i) => ({ buttons: i < 2 ? BUTTON.ATTACK_PRIMARY : 0 }))
    if (c.launches > before) { launcher = w; break }
  }
  check(launcher !== null, `a weapon in hand predicts a launch (${launcher})`)

  // Fired at the room in eight directions, one rocket each, and every server
  // effect and rocket the session sees kept with when it arrived.
  const served = []
  const rockets = new Map()      // server entity (its handle) -> { hidden, seen }
  c.onSnapshot = (s) => {
    for (const v of s.vfx ?? []) served.push({ v, at: Date.now(), swallowed: c.ghosts.swallow(v) })
    for (const e of s.entities ?? []) {
      const index = (e.handle & 0x00ffffff) - 1
      if (c.kinds.get(index) !== rocketKind || !(e.mask & ENT_FIELD.POS)) continue
      const key = e.handle >>> 0
      const r = rockets.get(key) ?? { hidden: 0, seen: 0 }
      r.seen++
      if (c.ghosts.hides(key, rocketKind, { x: Number(e.pos.x) / 4294967296, y: Number(e.pos.y) / 4294967296 })) r.hidden++
      rockets.set(key, r)
    }
  }
  c.ghostBlasts.length = 0
  const launched = c.launches
  for (let k = 0; k < 8; k++) {
    const aim = (k * 536870912) >>> 0
    await c.play(70, (i) => ({ buttons: i < 2 ? BUTTON.ATTACK_PRIMARY : 0, aim }))
  }
  await c.play(120, () => ({}))
  const fired = c.launches - launched
  const ours = c.ghostBlasts.filter((b) => b.v.kind === c.ghostBlasts[0]?.v.kind)
  console.log(`  rockets: ${fired} launched here, ${ours.length} burst here, ` +
              `${served.length} effects served (${served.filter((x) => x.swallowed).length} swallowed), ` +
              `${rockets.size} server rockets seen`)
  check(fired >= 6, `rockets were launched by the prediction (${fired})`)
  check(ours.length >= fired - 1, `and burst here, on their own (${ours.length} of ${fired})`)
  // WHERE: each ghost blast has a served blast of the same kind beside it.
  const near = (a, b) => Math.hypot(Number(a.pos.x - b.pos.x), Number(a.pos.y - b.pos.y),
                                     Number(a.pos.z - b.pos.z)) / 4294967296
  let agreed = 0
  let earlier = 0
  for (const g of ours) {
    const m = served.find((x) => x.v.kind === g.v.kind && near(x.v, g.v) < 0.5)
    if (m) { agreed++; if (g.at <= m.at) earlier++ }
  }
  check(agreed >= ours.length * 0.75, `they burst where the server's did (${agreed} of ${ours.length} within half a unit)`)
  check(earlier >= agreed * 0.75, `and no later than the server's blast arrived (${earlier} of ${agreed})`)
  // NEVER TWICE: the served copies were swallowed, the server's rockets hidden.
  const swallowed = served.filter((x) => x.swallowed && x.v.kind === ours[0]?.v.kind).length
  check(swallowed >= agreed * 0.75, `the server's copies of those blasts were swallowed (${swallowed})`)
  const hidden = [...rockets.values()].filter((r) => r.hidden === r.seen).length
  check(rockets.size > 0 && hidden >= rockets.size * 0.75,
        `the server's rockets were recognised as ours and hidden (${hidden} of ${rockets.size})`)
  // KNOCK: every body our rockets shoved here, against how the server moved
  // it -- in the predicted direction, by a fair part of the predicted distance
  // (the monster's own steering takes some back, which this does not model).
  let judged = 0
  let agreed2 = 0
  const ratios = []
  for (const sh of c.knock.shoves) {
    const b = c.bodies.get(sh.key)
    const p20 = sh.path[Math.min(20, sh.path.length - 1)]
    const dist = Math.hypot(p20.x, p20.y)
    if (!b || dist < 0.5) continue
    const before = [...b.track].reverse().find((t) => t.tick <= sh.at)
    const after = b.track.find((t) => t.tick >= sh.at + 20)
    if (!before || !after) continue
    judged++
    const along = ((after.x - before.x) * p20.x + (after.y - before.y) * p20.y) / dist
    if (along > dist * 0.4) agreed2++
    ratios.push(along / dist)
  }
  ratios.sort((a, b) => a - b)
  const median = ratios.length ? ratios[Math.floor(ratios.length / 2)] : 0
  console.log(`  knock: ${c.knock.shoves.length} bodies shoved here, ${judged} judged, ${agreed2} moved the same way on the server; ` +
              `the server's distance over the prediction's, median ${median.toFixed(2)}`)
  check(c.knock.shoves.length > 0, `our rockets shoved bodies here at once (${c.knock.shoves.length})`)
  check(judged > 0 && agreed2 >= judged * 0.75, `and the server moved them the same way (${agreed2} of ${judged})`)
  check(median > 0.6 && median < 1.4, `about as far (median ${median.toFixed(2)} of the predicted distance)`)

  // CONTROL: a blast somewhere else is not swallowed.
  const far = ours[0] ? { ...ours[0].v, pos: { ...ours[0].v.pos, x: ours[0].v.pos.x + (40n << 32n) } } : null
  check(far !== null && !c.ghosts.swallow(far), 'CONTROL: a blast forty units away is not taken for ours')
  await c.close()
}

// ===================================================== run 4: rocket jump

{
  const c = await session({ echo: false, giveAll: true, fps: true })
  let launcher = null
  for (const w of c.slots) {
    await c.play(40, (i) => ({ weapon: i === 0 ? w : NO_WEAPON }))
    const before = c.launches
    await c.play(30, (i) => ({ buttons: i < 2 ? BUTTON.ATTACK_PRIMARY : 0, aim: 0 }))
    if (c.launches > before) { launcher = w; break }
  }
  check(launcher !== null, `first person, a weapon in hand launches (${launcher})`)
  const DOWN = -12000                      // about 66 degrees below level
  // Judged over the JUMP: from the trigger until thirty ticks on, which takes
  // in the rocket's flight, the shove and the rise, and every snapshot about
  // them. Not the landing: map01's spawn is crowded with wandering bodies that
  // the client holds where it last saw them, and coming down among them
  // mispredicts for reasons that have nothing to do with the rocket.
  const jump = async () => {
    const z0 = c.world.readAgent(c.agent).pos.z
    let top = z0
    let rose = null
    let window = null
    await c.play(150, (i) => {
      const z = c.world.readAgent(c.agent).pos.z
      if (z > top) top = z
      if (rose === null && z > z0 + (1n << 28n)) rose = i
      if (i === 10) c.zero()
      if (i === 40) window = { ...c.predictor.stats }
      return { buttons: i === 10 ? BUTTON.ATTACK_PRIMARY : 0, pitch: DOWN, aim: 0 }
    })
    return { rose, height: Number(top - z0) / 4294967296, st: window }
  }
  const shoves0 = c.shoves
  const p = await jump()
  console.log(`  rocket jump, predicted: rose ${p.height.toFixed(2)} units, ${p.rose === null ? 'never' : (p.rose - 10) + ' ticks after the trigger'}; ` +
              `exact ${p.st.exactMatches} lua ${p.st.luaApplied} mispredicted ${p.st.mispredictions}`)
  check(c.shoves > shoves0, 'the ghost burst at our feet and shoved us here')
  check(p.height > 1, `we were thrown up (${p.height.toFixed(2)} units)`)
  check(p.st.mispredictions === 0 && p.st.luaApplied === 0,
        `and the server agreed to the bit: nothing to excuse (lua ${p.st.luaApplied}, mispredicted ${p.st.mispredictions})`)
  // CONTROL: the same jump with the shove left to the server.
  await c.play(120, () => ({}))
  c.noSelfShove = true
  const n = await jump()
  console.log(`  CONTROL, unpredicted: rose ${n.height.toFixed(2)} units, ${n.rose === null ? 'never' : (n.rose - 10) + ' ticks after the trigger'}; ` +
              `lua ${n.st.luaApplied}`)
  check(n.st.luaApplied > 0, 'CONTROL: without it, the server\'s shove has to be adopted')
  check(p.rose !== null && n.rose !== null && p.rose < n.rose,
        `CONTROL: and it is felt later (${n.rose - 10} ticks against ${p.rose - 10})`)
  await c.close()
}

console.log(`\nweapons: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
