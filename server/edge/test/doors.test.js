//
// Doors, switches and trip-wires, end to end (§3.2.2, §3.1.2).
//
// The claim: a player presses a button, a door opens, and every client -- the
// one that pressed it and any other -- predicts the moving ceiling exactly.
// That spans the whole stack, and each layer can fail silently, so the test
// asserts at each of them:
//
//   engine   the use reached a SIDE (core/tests/test_interact.cpp proves which)
//   Lua      the side was a door and the script started a motion
//   protocol the motion reached the client as SECTOR_MOTION_SET
//   client   the client's own copy of the geometry moved, and prediction of
//            the entity riding through it stayed exact
//
// Controls, because every one of those has a way to pass for the wrong reason:
//
//   DNP_NO_USE=1     the same walk with the button never pressed. The door must
//                    stay shut and the agent must be stopped by it. Without
//                    this, a door that opened on a timer would pass.
//   DNP_NO_SCRIPT=1  --no-scripts, so not even the map's own declared files
//                    load. Nothing moves, which is what proves the engine is
//                    reporting and not acting.
//   DNP_IGNORE_LUA=1 a client that ignores LUA_APPLIED must report the door as
//                    a DEFECT. Without this the flag could be carrying no
//                    information at all and nothing here would notice.
//
// The trip-wire is tested by the same run: crossing x = 17 lowers the lift in
// sector 4, and NOT pressing anything is the whole point of it.
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
  axisFromNumber, fxToNumber, MSG, BUTTON, ENT_FIELD, handleIndex
} from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34492)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34566)
const NO_USE = process.env.DNP_NO_USE === '1'
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
    const p = join(ROOT, 'build', arch, 'doors.dnpm')
    if (existsSync(p)) return p
  }
  return null
}
const MAP_PATH = mapPath()
if (!MAP_PATH) { console.error('no baked doors map'); process.exit(2) }

// The map under test, by name. Sector 1 is the door (shut: ceil == floor == 0);
// sector 4 is the lift, parked at 3.
const DOOR = 1
const LIFT = 4

// The body maps/doors.map declares: Doom's 16x56 player at 1/48. The east wing
// is 1.5 tall, so the door opens to 1.5 - 4/48 = 1.4167 and this body clears it
// by a quarter of a unit while the engine's default 2.0 does not fit at all.
const WANT_RADIUS = 0.3333
const WANT_HEIGHT = 1.1667
const DOOR_OPEN_H = 1.5 - 4 / 48

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

// No --script arguments: maps/doors.map declares what it needs and mapc put
// both files next to the baked map. --no-scripts is the operator's veto, and
// the control that proves the doors are the map's doing.
// --props with an absolute path, because the default is relative to the working
// directory and this test does not run from the repository root -- which is
// also true of any real deployment.
const args = ['--port', String(LINK_PORT), '--map', MAP_PATH, '--spawn', 'start',
              '--props', join(ROOT, 'assets', 'props.txt')]
if (NO_SCRIPT) args.push('--no-scripts')

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
const agent = world.spawn(map.spawnForTag('start'))
const predictor = new Predictor(world, agent)
predictor.ignoreLuaFlag = IGNORE_LUA

// The door as the client sees it, right now. Read through the same wasm module
// the client predicts with, from the same MapStorage sector motion writes into
// -- so this is exactly the geometry the renderer would draw, not a parallel
// copy that could agree by accident.
const liveSector = (i) => {
  core.M._dnp_map_sector(map.ptr, i, core.buf64, core.buf32)
  const b = core.buf64 / 8
  return { floorZ: fxToNumber(core.views.i64[b]), ceilZ: fxToNumber(core.views.i64[b + 1]) }
}
const ceilOf = (s) => liveSector(s).ceilZ
const floorOf = (s) => liveSector(s).floorZ
const agentX = () => fxToNumber(world.readAgent(agent).pos.x)

// The map's declared body, as the CLIENT received it. Replicated through
// PARAM_UPDATE like any other rule of physics -- a client predicting with a
// different body from the server's would disagree at every wall it slid along,
// and at this scale it would not fit through the map's own doors.
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

// Props the server told us about, by entity index, with the kind it declared.
//
// A prop is an ordinary entity on the wire -- the same block, the same delta
// rules, the same generational handle -- and what makes it a key rather than a
// player is display_id. This client has no table of kinds and does not need
// one; it is checking that the number arrives and that the entity behaves like
// an entity.
const propsSeen = new Map()
let keyIndex = null
let keyGone = false
const PROP_KEY_BLUE = 1        // assets/props.txt

const applied = new Set()
const drainMotions = () => {
  while (motionSets.length) {
    const m = motionSets.shift()
    world.applyMotionSet(m)
    applied.add(`${m.sector}:${m.startTick}:${m.durationTicks}`)
  }
}

let history = []
let maxDoorCeil = doorClosedCeil
let minLiftFloor = liftParkedFloor
let maxX = 0
// After the door has been fully open, does it come back down? An opening door
// is half a door. The wait is 90 ticks and the travel 32, so a door that is
// still up 300 ticks later is one whose deferred close never ran -- which is
// exactly what a wrong on_tick signature did, silently, while every other
// assertion in this file passed.
let ceilAfterOpen = null
let liftAfterDown = null

const verdicts = []
const drain = () => {
  while (pending.length) {
    const s = pending.shift()
    for (const e of s.entities ?? []) {
      const index = handleIndex(e.handle)
      if (e.mask & ENT_FIELD.DESTROYED) {
        if (index === keyIndex) keyGone = true
        continue
      }
      if (!e.displayId) continue
      propsSeen.set(index, e.displayId)
      // Tracked by the KIND it declared rather than by counting what arrived:
      // this map carries a barrel too, and a test that asserted "one prop"
      // was measuring the fixture rather than the replication.
      if (e.displayId === PROP_KEY_BLUE) keyIndex = index
    }
    const r = predictor.reconcile(s)
    if (r?.verdict) verdicts.push(r.verdict)
  }
}

// One tick of the client: predict, send, apply anything that arrived.
async function tickWith (buttons, moveX) {
  const f = { buttons, moveX: axisFromNumber(moveX), moveY: 0, aim: 0 }
  predictor.step(f)
  history = [f, ...history].slice(0, 12)
  await dw.write(encodeInputBundle({
    clientTick: predictor.tick,
    ackSnapshotTick: pending.at(-1)?.serverTick ?? 0,
    frames: history
  }))
  drainMotions()
  drain()
  maxDoorCeil = Math.max(maxDoorCeil, ceilOf(DOOR))
  minLiftFloor = Math.min(minLiftFloor, floorOf(LIFT))
  maxX = Math.max(maxX, agentX())
  // 8 ms, not 15. This loop steps the client once per iteration, so its tick
  // rate IS its loop rate -- and at ~66 Hz nominal it drops below the server's
  // 60 the moment the machine is busy. A client running behind the server never
  // predicts a tick the server has not already run, so it cannot mispredict at
  // all, and the control asserting that it DOES became a test of the machine's
  // mood rather than of the flag.
  await sleep(8)
}

// Walk east to the door and stop against it.
for (let i = 0; i < 120; i++) await tickWith(0, 1)
const xAtDoor = agentX()

// Press it. Once, and released -- a held button is one use, which is what
// core/tests/test_interact.cpp pins down.
const USE = NO_USE ? 0 : BUTTON.INTERACT
for (let i = 0; i < 4; i++) await tickWith(USE, 0)
for (let i = 0; i < 4; i++) await tickWith(0, 0)

// Give the door time to travel, still walking into it, so the moment it is high
// enough the agent goes through -- and then long enough for its wait to expire
// and it to shut again. Counted in TICKS but the door's schedule is in server
// ticks, so this has to cover open (32) + wait (90) + close (32) in wall clock
// at whatever rate this loop actually manages.
for (let i = 0; i < 520; i++) await tickWith(0, 1)
ceilAfterOpen = ceilOf(DOOR)
liftAfterDown = floorOf(LIFT)

// Back and forth across the trip-wire, which is repeatable.
//
// Not decoration: the IGNORE_LUA control needs the client to have predicted the
// exact tick a motion started before the message describing it arrives, and
// with only the four changes above that window is one tick wide and closes
// entirely when the suite is loaded and the client is running behind. Several
// more crossings spread over a second make it a property of the flag rather
// than of the machine's mood.
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
console.log(`  ${applied.size} motions applied; exact ${predictor.stats.exactMatches}, ` +
            `mispredicted ${predictor.stats.mispredictions}`)

check(xAtDoor > 9 && xAtDoor < 10, `the shut door stopped the agent at x=${xAtDoor.toFixed(2)}`)

if (NO_SCRIPT) {
  check(maxDoorCeil === 0, 'CONTROL: with no script the door never moves')
  check(applied.size === 0, 'CONTROL: and no motion is ever replicated')
  check(maxX < 10, 'CONTROL: and the agent never gets through')
} else if (NO_USE) {
  check(maxDoorCeil === 0, 'CONTROL: unpressed, the door never opens')
  check(maxX < 10, 'CONTROL: and the agent is still stopped by it')
  check(minLiftFloor === 3, 'CONTROL: and the trip-wire beyond it is never reached')
} else {
  check(Math.abs(maxDoorCeil - DOOR_OPEN_H) < 0.01,
        `the door opened to its lowest neighbouring ceiling less four Doom units ` +
        `(${maxDoorCeil.toFixed(3)}, wanted ${DOOR_OPEN_H.toFixed(3)})`)
  check(applied.size >= 1, `${applied.size} motions replicated as SECTOR_MOTION_SET`)
  check(maxX > 12, `the agent walked through it to x=${maxX.toFixed(2)}`)
  // Crossing x = 17 is a trip-wire, and nothing was pressed to do it.
  check(maxX > 17, 'and on across the trip-wire')
  check(minLiftFloor < 3, `the trip-wire lowered the lift to ${minLiftFloor.toFixed(3)}`)
  // The whole reason the motion is parametric: one message, and the client
  // computes every tick of it. If prediction drifted while the door moved, the
  // closed form is not being evaluated the same way on both sides.
  if (IGNORE_LUA) {
    check(predictor.stats.mispredictions > 0,
          `CONTROL: ignoring LUA_APPLIED reports the door as a DEFECT ` +
          `(${predictor.stats.mispredictions})`)
  } else {
    check(predictor.stats.mispredictions === 0,
          `ZERO mispredictions across an opening door (got ${predictor.stats.mispredictions})`)
    // The shape, as in lua.test.js: a change the client could not have known
    // about costs exactly one tick and then self-heals. A RUN of them would
    // mean the client never caught up -- a real fault wearing the flag as an
    // excuse.
    let worst = 0
    let run = 0
    for (const v of verdicts) {
      if (v === 'lua-applied') { run++; worst = Math.max(worst, run) } else run = 0
    }
    console.log(`  longest consecutive lua-applied run: ${worst}`)
    check(worst <= 2, `each geometry change self-heals within a tick (longest run ${worst})`)
  }
  // A prop reached the client as an ordinary entity, carrying what it IS.
  console.log(`  props seen: ${[...propsSeen].map(([i, k]) => `${i}:kind ${k}`).join(', ') || 'none'}`)
  check(propsSeen.size >= 1, `${propsSeen.size} props replicated`)
  check(keyIndex !== null, `the blue key declared its kind (entity ${keyIndex})`)
  // Its index is above the agents, which is what lets it share the entity
  // space with players and need no message of its own.
  check(keyIndex > 64, `in an entity slot above the agents (${keyIndex})`)
  // Walking into it picked it up, and the pickup reached the client as a
  // destruction rather than as an entity that quietly stopped being mentioned
  // -- absence means "unchanged" in a delta-encoded snapshot.
  check(keyGone, 'and walking into it destroyed it')
  check(serverLog.includes('picked up the blue key'), 'the script granted the key')

  // And the other half of a door.
  console.log(`  after the wait: door ceiling ${ceilAfterOpen.toFixed(3)}, ` +
              `lift floor ${liftAfterDown.toFixed(3)}`)
  check(ceilAfterOpen < maxDoorCeil,
        `the door shut itself again (ceiling back to ${ceilAfterOpen.toFixed(3)})`)
  check(liftAfterDown > minLiftFloor,
        `the lift rose again (floor back to ${liftAfterDown.toFixed(3)})`)
}

try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')
console.log(`\ndoors: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
