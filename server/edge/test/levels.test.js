//
// Changing map for ONE client (design §3.3's shape, without the second server).
//
// The server hosts one simulation per map and moves a session between them, so
// this asserts three separate things that are easy to confuse:
//
//   1. the player who touched the exit is now somewhere else -- a different
//      map hash, a different sector count, a different spawn;
//   2. the player who did NOT touch it is still exactly where they were, and
//      still receiving snapshots of the map they are standing in;
//   3. the mover can still predict. A map change resets the world under a
//      client, and a client whose predictor kept its old baseline would
//      mispredict forever afterwards -- which would look like a determinism
//      bug and is really a bookkeeping one.
//
// (2) is the whole point of the exercise. A server-wide map change would pass
// (1) and (3) and be a completely different feature.
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
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, MSG, BUTTON
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'
import { fpsFrame } from './helpers.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34496)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34570)
// CONTROL: with only one level loaded there is nowhere to go, so the exit must
// leave the player exactly where they are. Without it, "the map changed" could
// be any reset at all rather than a move to the NEXT level.
const ONE_LEVEL = process.env.DNP_ONE_LEVEL === '1'

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

function mapPath (name) {
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', arch, name)
    if (existsSync(p)) return p
  }
  return null
}
const A = mapPath('level_a.dnpm')
const B = mapPath('level_b.dnpm')
if (!A || !B) { console.error('no baked level_a/level_b'); process.exit(2) }

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

// No --script: maps/level_a.map declares doom.lua and its own line table.
const args = ['--port', String(LINK_PORT), '--map', A]
if (!ONE_LEVEL) args.push('--map', B)

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

// One connected player, tracking every welcome it receives rather than only the
// first -- a second WELCOME is the server saying "you are somewhere else now".
async function connect (name) {
  const wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
    serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
  })
  await withTimeout(wt.ready, 8000, `${name} ready`)
  const control = await wt.createBidirectionalStream()
  const cw = control.writable.getWriter()
  const dw = wt.datagrams.createWritable().getWriter()

  const P = {
    name, wt, cw, dw,
    welcomes: [],
    acc: new MapAccumulator(),
    maps: [],           // one accumulator per welcome
    snapshots: [],
    tick: 0,
    history: []
  }

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
          if (m?.type === MSG.WELCOME) { P.welcomes.push(m); P.maps.push(new MapAccumulator()) }
          else if (m?.type === MSG.MAP_DATA) P.maps[P.maps.length - 1]?.push(m)
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
        if (s) P.snapshots.push(s)
      }
    } catch {}
  })()

  await cw.write(encodeHello())
  await waitFor(() => P.welcomes.length >= 1, 8000, `${name} welcome`)
  await waitFor(() => P.maps[0].complete, 8000, `${name} map`)
  return P
}

async function send (P, buttons, moveX) {
  P.tick++
  // Kept LEADING the newest snapshot, which is what a predicting client's own
  // clock does. This counter only advances when send() is called, so a window
  // where the test deliberately sends nothing leaves it behind the server by
  // however long that window lasted -- and every frame after it is then dropped
  // as too old, which is a property of the test and not of the engine.
  const lead = (P.snapshots.at(-1)?.serverTick ?? 0) + 8
  if (P.tick < lead) P.tick = lead
  // As this run's scheme sends it (helpers.js): the same intent in either.
  const f = fpsFrame({ buttons, moveX: axisFromNumber(moveX), moveY: 0, aim: 0 })
  P.history = [f, ...P.history].slice(0, 12)
  await P.dw.write(encodeInputBundle({
    clientTick: P.tick,
    ackSnapshotTick: P.snapshots.at(-1)?.serverTick ?? 0,
    frames: P.history
  }))
}

const mover = await connect('mover')
const stayer = await connect('stayer')

const hashOf = (P, i) => P.welcomes[i].mapHash
const sectorsOf = (P, i) => {
  const { ok, map } = core.loadMap(P.maps[i].bytes())
  return ok ? map.sectorCount : -1
}

check(hashOf(mover, 0) === hashOf(stayer, 0), 'both players start in the same map')
check(sectorsOf(mover, 0) === 1, `level A has one sector (${sectorsOf(mover, 0)})`)

// Both walk east; only the mover presses use at the wall.
for (let i = 0; i < 90; i++) { await send(mover, 0, 1); await send(stayer, 0, 1); await sleep(12) }
const stayerBefore = stayer.snapshots.at(-1)
for (let i = 0; i < 6; i++) { await send(mover, BUTTON.INTERACT, 0); await send(stayer, 0, 0); await sleep(12) }
for (let i = 0; i < 60; i++) { await send(mover, 0, 0); await send(stayer, 0, 0); await sleep(12) }
await sleep(400)

console.log(`  mover got ${mover.welcomes.length} welcome(s), stayer got ${stayer.welcomes.length}`)

if (ONE_LEVEL) {
  check(mover.welcomes.length === 1,
        'CONTROL: with nowhere to go, the exit does not move anyone')
  check(serverLog.includes('finished the last level'),
        'CONTROL: and the server says so rather than failing silently')
} else {
  // (1) the mover is somewhere else.
  check(mover.welcomes.length === 2, `the mover was moved (${mover.welcomes.length} welcomes)`)
  if (mover.welcomes.length === 2) {
    await waitFor(() => mover.maps[1].complete, 5000, 'second map')
    check(hashOf(mover, 1) !== hashOf(mover, 0), 'to a map with a different hash')
    check(sectorsOf(mover, 1) === 2, `with two sectors, not one (${sectorsOf(mover, 1)})`)
  }

  // (2) THE POINT: the other player did not move.
  check(stayer.welcomes.length === 1, `the stayer stayed (${stayer.welcomes.length} welcome)`)
  const stayerAfter = stayer.snapshots.at(-1)
  check(stayerAfter && stayerAfter.serverTick > (stayerBefore?.serverTick ?? 0),
        'and is still being simulated in the map it is standing in')
  const sx = fxToNumber(stayerAfter.self.pos.x)
  check(sx > 16, `still at the east wall of level A (x=${sx.toFixed(2)})`)

  // (2b) AND EVERY LEVEL IS ON THE SAME CLOCK.
  //
  // Empty levels are not simulated -- hosting a 32-level module meant stepping
  // 3,800 monsters nobody was near -- but they DO keep counting ticks
  // (World::step_idle). An earlier attempt skipped the tick entirely and was
  // reverted: a level whose clock stops is thousands of ticks behind by the
  // time somebody arrives, and a client crossing that gap jumps its clock
  // backwards far enough that the server's input ring seeds itself from a
  // straggler and discards every real frame after it. Reported as "player
  // inputs stop working after transitioning to a map".
  //
  // So the level being entered must be at roughly the tick of the one being
  // left, not at zero.
  if (mover.welcomes.length === 2) {
    const aTick = stayerAfter?.serverTick ?? 0
    const bTick = mover.welcomes[1].serverTick
    check(bTick > aTick * 0.5,
          `the level entered is on the same clock as the one left (${bTick} vs ${aTick})`)
  }

  // (3) the mover is a working client again: snapshots keep arriving, and their
  // ticks belong to the new level's world.
  const after = mover.snapshots.filter((s) => s.serverTick > 0).length
  check(after > 0, `${after} snapshots since the change`)

  // NOBODY WALKS OFF ON THEIR OWN.
  //
  // take() repeats the last input across a gap, which is right for jitter and
  // wrong across a map change: the last frame before the exit is whatever the
  // player was doing when they touched it, and the client then resets its clock
  // to the new world's tick -- so its fresh frames arrive outside the window
  // the ring expects and are dropped, while the server goes on simulating
  // "walking east" forever. Reported as "something taking control, and the
  // player character just moves on its own in a chosen direction".
  //
  // Send NOTHING after the change and watch: a body that drifts is a body being
  // driven by input nobody sent.
  // The newest self position this client has been told about.
  const selfX = (P) => {
    for (let i = P.snapshots.length - 1; i >= 0; i--) {
      if (P.snapshots[i].self) return fxToNumber(P.snapshots[i].self.pos.x)
    }
    return 0
  }
  // THE CLIENT RESETS ITS CLOCK on a map change -- main.js rebuilds the world
  // at the new level's tick and the predictor with it. Without doing the same
  // here, this test keeps a clock that never jumps and the server's input ring
  // goes on accepting its frames, which is precisely the case that works.
  mover.tick = (mover.snapshots.at(-1)?.serverTick ?? 0) + 8
  mover.history = []

  const startX = selfX(mover)
  for (let i = 0; i < 90; i++) await sleep(12)
  const driftX = selfX(mover) - startX
  check(Math.abs(driftX) < 0.25,
        `and does not drift when no input is sent (moved ${driftX.toFixed(2)})`)

  // THE STRAGGLERS, and this test passed for a long time without them.
  //
  // A real client sends sixty frames a second and cannot know it has changed
  // level until the WELCOME arrives, so a handful of frames stamped on the OLD
  // world's clock are always in flight behind it. send() only ever raises its
  // tick to lead the newest snapshot, never lowers it, so these still carry
  // level A's clock -- which is exactly what a browser puts on the wire.
  //
  // Without them the change is unnaturally clean: the ring is empty, the first
  // frame to arrive is already on the new clock, and it re-seeds correctly. One
  // straggler is enough to re-seed it a whole level's worth of ticks into the
  // future instead, after which every real frame is discarded as too old and
  // the player is deaf while their last pre-exit input repeats for ever.
  //
  // That was reported as "player inputs stop working after transitioning to a
  // map", and it only became reliable once empty levels stopped ticking -- the
  // two clocks used to be close enough that the buffer absorbed the jump.
  for (let i = 0; i < 4; i++) { await send(mover, 0, 1); await sleep(12) }

  // The control, and the one that actually caught this: it CAN still move.
  //
  // A buffer stalled on a gap it will never fill is deaf AND repeating -- the
  // drift assertion above passes on a session that has stopped listening
  // entirely, because a zeroed repeat does not drift either. Only this one
  // separates "recovered" from "silenced".
  for (let i = 0; i < 60; i++) { await send(mover, 0, 1); await sleep(12) }
  check(selfX(mover) - startX > 0.5,
        `but still moves when input IS sent (${(selfX(mover) - startX).toFixed(2)})`)
}

for (const P of [mover, stayer]) { try { P.wt.close() } catch {} }
await edge.stop()
server.kill('SIGTERM')
console.log(`\nlevels: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
