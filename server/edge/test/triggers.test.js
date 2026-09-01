//
// The trigger runtime, end to end: maps/doors_trig.map.
//
// THE CLAIM. A map's behaviour written as a TRIGGER TABLE -- data, walked by
// maps/scripts/triggers.lua -- does exactly what the same map's Doom line table
// does, on geometry that is identical to the vertex.
//
// That is why this file asserts the same NUMBERS as doors.test.js rather than
// numbers of its own: the door reaches 1.5 - 4/48, the agent is stopped at the
// same x, the lift lowers off the same trip-wire, and prediction stays exact
// through all of it. maps/doors.map keeps its line table and doors.test.js
// keeps running against it unchanged, so the two implementations are compared
// rather than one replacing the other -- and scripts/check_doors_twin.sh fails
// the moment the two maps stop being the same map, which is the only thing that
// makes the comparison mean anything.
//
// What is being proved that doors.test.js does not prove:
//
//   the vocabulary is expressive enough for a map that already works -- a
//   toggling door, a two-sided trip-wire, a shared action run from both, a
//   deferred close, an exit switch, a pickup and a barrel
//
//   `run_after` is a tick deadline and not a wait: the door's close is queued
//   from the moment the ceiling SETTLES, and it arrives
//
//   `sector_offset_at_least` reads the door's own offset, so "is it open" is
//   the same question the client can answer
//
// What is NOT proved here, and is proved in server/tests/test_lua_triggers.cpp
// instead: every way a trigger table can be wrong. This test can stand up one
// map; that one can enumerate forty broken tables in a second.
//
// Controls, because each claim above has a way to pass for the wrong reason:
//
//   DNP_NO_USE=1     the same walk with the button never pressed. The door must
//                    stay shut and the agent must be stopped by it. Without it,
//                    a door on a timer would pass.
//   DNP_NO_SCRIPT=1  --no-scripts, so not even the map's declared files load.
//                    Nothing moves, which is what proves the engine reports and
//                    the triggers act.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { parseProps } from '../../../client/src/render/props.js'
import { mirrorEntities } from './helpers.js'
import { Core } from '../../../client/src/core.js'
import { Predictor } from '../../../client/src/predictor.js'
import { MapAccumulator } from '../../../client/src/maploader.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, MSG, BUTTON, ENT_FIELD, handleIndex
} from '../../../client/src/proto.js'
import { serverBin, doomProps } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34700)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34710)
const NO_USE = process.env.DNP_NO_USE === '1'
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
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', arch, 'doors_trig.dnpm')
    if (existsSync(p)) return p
  }
  return null
}
const MAP_PATH = mapPath()
if (!MAP_PATH) { console.error('no baked doors_trig map'); process.exit(2) }

// The same sectors as doors.test.js, because it is the same map. Sector 1 is
// the door (shut: ceil == floor == 0); sector 4 is the lift, parked at 3.
const DOOR = 1
const LIFT = 4

// The body maps/doors_trig.map declares, and the height its door opens to:
// 1.5 less four Doom units at 1/48. These constants are doors.test.js's,
// deliberately -- if the trigger table produced a door a rounding step away
// from the line table's, this is where it would show.
const WANT_RADIUS = 0.3333
const WANT_HEIGHT = 1.1667
const DOOR_OPEN_H = 1.5 - 4 / 48

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

// No --script arguments: the map declares the vocabulary, the runtime and its
// own table, and mapc put all three next to the baked output.
const args = ['--port', String(LINK_PORT), '--map', MAP_PATH, '--spawn', 'start',
              '--props', doomProps]
if (NO_SCRIPT) args.push('--no-scripts')

const server = spawn(serverBin, args,
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
const agent = world.spawn(map.spawnForTag('start'))
const predictor = new Predictor(world, agent)

// The door as the client sees it, read through the same wasm module the client
// predicts with -- so this is the geometry the renderer would draw, not a
// parallel copy that could agree by accident.
const liveSector = (i) => {
  core.M._dnp_map_sector(map.ptr, i, core.buf64, core.buf32)
  const b = core.buf64 / 8
  return { floorZ: fxToNumber(core.views.i64[b]), ceilZ: fxToNumber(core.views.i64[b + 1]) }
}
const ceilOf = (s) => liveSector(s).ceilZ
const floorOf = (s) => liveSector(s).floorZ
const agentX = () => fxToNumber(world.readAgent(agent).pos.x)

const RADIUS_ID = 0
const HEIGHT_ID = 1
const clientRadius = Number(world.getParamRaw(agent, RADIUS_ID)) / 4294967296
const clientHeight = Number(world.getParamRaw(agent, HEIGHT_ID)) / 4294967296

const doorClosedCeil = ceilOf(DOOR)
const liftParkedFloor = floorOf(LIFT)
check(doorClosedCeil === 0, `the door starts shut (ceiling at ${doorClosedCeil})`)
check(Math.abs(clientRadius - WANT_RADIUS) < 0.001 && Math.abs(clientHeight - WANT_HEIGHT) < 0.001,
      `the client got the map's body (radius ${clientRadius.toFixed(4)}, ` +
      `height ${clientHeight.toFixed(4)})`)
check(liftParkedFloor === 3, `the lift starts parked (floor at ${liftParkedFloor})`)

const propsSeen = new Map()
let keyIndex = null
let keyGone = false
const PROP_KEY_BLUE = 1        // assets/props.txt

const catalogue = parseProps(readFileSync(doomProps, 'utf8'))
const mirrorBase = core.M._dnp_mirror_base()
const selfIndex = (welcome.selfHandle & 0x00ffffff) - 1

const applied = new Set()
const drainMotions = () => {
  while (motionSets.length) {
    const m = motionSets.shift()
    world.applyMotionSet(m)
    applied.add(`${m.sector}:${m.startTick}:${m.durationTicks}`)
  }
}

let history = []
let leadSum = 0
let leadN = 0
let maxDoorCeil = doorClosedCeil
let minLiftFloor = liftParkedFloor
let maxX = 0
// The other half of a door. An opening door is half a door, and a `run_after`
// that never arrived is exactly what "the door stayed open" looks like from
// inside the game -- a feature nobody implemented rather than a deadline that
// was dropped.
let ceilAfterOpen = null
let liftAfterDown = null

let newestTick = 0
const drain = () => {
  while (pending.length) {
    const s = pending.shift()
    newestTick = Math.max(newestTick, s.serverTick)
    for (const e of s.entities ?? []) {
      const index = handleIndex(e.handle)
      if (e.mask & ENT_FIELD.DESTROYED) {
        if (index === keyIndex) keyGone = true
        continue
      }
      if (!e.displayId) continue
      propsSeen.set(index, e.displayId)
      if (e.displayId === PROP_KEY_BLUE) keyIndex = index
    }
    mirrorEntities(world, s, {
      catalogue, kinds: propsSeen, mirrorBase, selfIndex,
      bodyRadius: clientRadius, bodyHeight: clientHeight, ENT_FIELD, handleIndex
    })
    const r = predictor.reconcile(s)
    if (r?.verdict) { /* recorded through predictor.stats below */ }
  }
}

// A predicting client MUST lead the server, or it never predicts a tick the
// server has not already run and therefore cannot mispredict at all -- which
// silently turns the zero-mispredictions claim into a test of how busy the
// machine is. Same band, and the same reason, as doors.test.js.
const TARGET_LEAD = 6
const MAX_LEAD = 14

async function tickWith (buttons, moveX) {
  const f = { buttons, moveX: axisFromNumber(moveX), moveY: 0, aim: 0 }
  const send = () => {
    predictor.step(f)
    history = [f, ...history].slice(0, 12)
    return dw.write(encodeInputBundle({
      clientTick: predictor.tick,
      ackSnapshotTick: newestTick,
      frames: history
    }))
  }
  if (newestTick > 0 && predictor.tick > newestTick + MAX_LEAD) {
    drainMotions()
    drain()
    await sleep(8)
    return
  }
  await send()
  for (let i = 0; i < 4 && newestTick > 0 && predictor.tick < newestTick + TARGET_LEAD; i++) {
    await send()
  }
  drainMotions()
  drain()
  if (newestTick > 0) { leadSum += predictor.tick - newestTick; leadN++ }
  maxDoorCeil = Math.max(maxDoorCeil, ceilOf(DOOR))
  minLiftFloor = Math.min(minLiftFloor, floorOf(LIFT))
  maxX = Math.max(maxX, agentX())
  await sleep(8)
}

// Walk east to the door and stop against it.
for (let i = 0; i < 120; i++) await tickWith(0, 1)
const xAtDoor = agentX()

// Press it. Once, and released -- a held button is one use.
const USE = NO_USE ? 0 : BUTTON.INTERACT
for (let i = 0; i < 4; i++) await tickWith(USE, 0)
for (let i = 0; i < 4; i++) await tickWith(0, 0)

// Long enough for the door to open, for its ninety-tick wait to expire from the
// moment it SETTLED, and for it to travel back down.
for (let i = 0; i < 520; i++) await tickWith(0, 1)
ceilAfterOpen = ceilOf(DOOR)
liftAfterDown = floorOf(LIFT)

// Back and forth across the trip-wire, which is repeatable. Both directions are
// separate triggers on separate side indices, both running the one named
// action -- so this also exercises `run`.
for (let pass = 0; pass < 3; pass++) {
  for (let i = 0; i < 70; i++) await tickWith(0, -1)
  for (let i = 0; i < 70; i++) await tickWith(0, 1)
}
await sleep(200)
drainMotions(); drain()
await sleep(200)
drainMotions(); drain()

console.log(`  agent stopped at x=${xAtDoor.toFixed(2)}, reached x=${maxX.toFixed(2)}`)
console.log(`  door ceiling reached ${maxDoorCeil.toFixed(3)}, lift floor reached ${minLiftFloor.toFixed(3)}`)
console.log(`  mean lead over the newest snapshot: ${(leadSum / Math.max(1, leadN)).toFixed(1)} ticks`)
console.log(`  ${applied.size} motions applied; exact ${predictor.stats.exactMatches}, ` +
            `mispredicted ${predictor.stats.mispredictions}`)

check(xAtDoor > 9 && xAtDoor < 10, `the shut door stopped the agent at x=${xAtDoor.toFixed(2)}`)

if (NO_SCRIPT) {
  check(maxDoorCeil === 0, 'CONTROL: with no script the door never moves')
  check(applied.size === 0, 'CONTROL: and no motion is ever replicated')
  check(maxX < 10, 'CONTROL: and the agent never gets through')
  check(!serverLog.includes('triggers on'), 'CONTROL: and no trigger table was loaded')
} else if (NO_USE) {
  check(maxDoorCeil === 0, 'CONTROL: unpressed, the door never opens')
  check(maxX < 10, 'CONTROL: and the agent is still stopped by it')
  check(minLiftFloor === 3, 'CONTROL: and the trip-wire beyond it is never reached')
} else {
  // The table loaded and, more to the point, loaded WHOLE. Validation is fail-
  // closed: one bad argument anywhere and nothing at all is registered, so a
  // count here is the difference between "the triggers ran" and "the triggers
  // were refused and the door happens not to be under test".
  check(/11 triggers on 4 events/.test(serverLog),
        'the trigger table loaded whole (11 triggers on 4 events)')
  check(!/problems in the trigger table/.test(serverLog),
        'and the runtime found nothing wrong with it')

  check(Math.abs(maxDoorCeil - DOOR_OPEN_H) < 0.01,
        `the door opened to its lowest neighbouring ceiling less four Doom units ` +
        `(${maxDoorCeil.toFixed(3)}, wanted ${DOOR_OPEN_H.toFixed(3)}) -- the same ` +
        `height doors.test.js measures on the line-table version of this map`)
  check(applied.size >= 1, `${applied.size} motions replicated as SECTOR_MOTION_SET`)
  check(maxX > 12, `the agent walked through it to x=${maxX.toFixed(2)}`)
  check(maxX > 17, 'and on across the trip-wire, having pressed nothing')
  check(minLiftFloor < 3, `the trip-wire lowered the lift to ${minLiftFloor.toFixed(3)}`)

  // The whole reason the motion is parametric: one message, and the client
  // computes every tick of it. A trigger table changes who decided to move the
  // ceiling; it must not change what crosses the wire.
  check(predictor.stats.mispredictions === 0,
        `ZERO mispredictions across a trigger-opened door (got ${predictor.stats.mispredictions})`)

  console.log(`  props seen: ${[...propsSeen].map(([i, k]) => `${i}:kind ${k}`).join(', ') || 'none'}`)
  check(propsSeen.size >= 1, `${propsSeen.size} props replicated`)
  check(keyIndex !== null, `the blue key declared its kind (entity ${keyIndex})`)
  // A pickup authored as a trigger rather than as a function: on_touch, a
  // condition on the catalogue name, and despawn who='target'.
  check(keyGone, 'and a trigger picked it up')
  check(serverLog.includes('picked up the blue key'), 'saying so in the log')
  // The escape hatch, against the real bindings rather than a test double: a
  // `lua` action read a property, wrote it back and reached dnp through the
  // same sandbox every other script uses.
  check(serverLog.includes('custom code counted key 1'),
        'and custom code ran, reaching the real bindings and the property store')

  console.log(`  after the wait: door ceiling ${ceilAfterOpen.toFixed(3)}, ` +
              `lift floor ${liftAfterDown.toFixed(3)}`)
  // run_after is a tick deadline, and this is the only place it is proved
  // against a real clock: the close was queued from the moment the ceiling
  // settled, and it arrived.
  check(ceilAfterOpen < maxDoorCeil,
        `the deferred close ran -- the door shut itself again ` +
        `(ceiling back to ${ceilAfterOpen.toFixed(3)})`)
  check(liftAfterDown > minLiftFloor,
        `and the lift rose again (floor back to ${liftAfterDown.toFixed(3)})`)
}

try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')
console.log(`\ntriggers: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
