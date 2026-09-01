//
// Props you cannot walk through, and a swing that removes one.
//
// Three claims, and each is a different layer:
//
//   the catalogue   `solid` in a text file makes a body stop
//   the engine      a swing reports what was in its arc
//   the script      a barrel is what explodes; the engine never heard of one
//
// The controls are what separate them. Walking north and stopping proves
// nothing on its own -- a wall would do that -- so the run without an attack
// must stop in the same place and STAY there, and the run with one must get
// past. If both got past, `solid` would be doing nothing; if neither did, the
// attack would be.
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
import { parseProps } from '../../../client/src/render/props.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, MSG, BUTTON, ENT_FIELD, handleIndex
} from '../../../client/src/proto.js'
import { serverBin, doomProps } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34498)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34572)
// CONTROL: never press attack. The barrel must still be there at the end, and
// the agent must still be stuck against it.
const NO_ATTACK = process.env.DNP_NO_ATTACK === '1'

const PROP_BARREL = 10        // assets/props.txt

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

// The same catalogue the server reads, read for its simulation half: a solid
// prop has to exist in the client's world too, or the client walks through what
// the server stops it against.
const catalogue = parseProps(readFileSync(doomProps, 'utf8'))

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

const server = spawn(serverBin,
                     ['--port', String(LINK_PORT), '--map', MAP_PATH, '--spawn', 'start',
                      '--props', doomProps],
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
await withTimeout(wt.ready, 8000, 'ready')
const control = await wt.createBidirectionalStream()
const cw = control.writable.getWriter()
const dw = wt.datagrams.createWritable().getWriter()

let welcome = null
const acc = new MapAccumulator()
const snapshots = []
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
      if (s) snapshots.push(s)
    }
  } catch {}
})()

await cw.write(encodeHello())
await waitFor(() => welcome, 8000, 'welcome')
await waitFor(() => acc.complete, 8000, 'map')

const { ok, map } = core.loadMap(acc.bytes())
check(ok, 'map loaded')

// A predicting client, so the swing goes through reconciliation the way a real
// one does. That is the whole point of this half: the countdown lives in the
// controller state that a snapshot OVERWRITES, so a field the server does not
// send is silently reset every time one arrives.
const world = core.createWorld({ map })
const agent = world.spawn(map.spawnForTag('start'))
world.tick = welcome.serverTick
const predictor = new Predictor(world, agent)
let phase = 'approach'
const verdicts = []
let maxSeenLeft = 0
let sweepSamples = new Set()

// Track the barrel by the kind it declares, not by an index this test guessed.
let barrelIndex = null
let barrelGone = false
const propKind = new Map()
let selfY = 0
const drain = () => {
  while (snapshots.length) {
    const s = snapshots.shift()
    selfY = fxToNumber(s.self.pos.y)

    // Entities FIRST, reconciliation second.
    //
    // Reconciliation adopts the server's state and replays this client's inputs
    // through its own world -- so that world has to already know the barrel is
    // gone. Replaying against a prop the server destroyed pushes the body out
    // of something that is not there, diverges again, and does it on every
    // snapshot until the agent walks clear: eleven mispredictions in a row that
    // look like a determinism fault and are an ordering bug. main.js has always
    // ingested before reconciling; this harness had not.
    for (const e of s.entities ?? []) {
      const i = handleIndex(e.handle)
      if (e.mask & ENT_FIELD.DESTROYED) {
        if (i === barrelIndex) barrelGone = true
        world.removeProp(i)
        continue
      }
      if (e.displayId) propKind.set(i, e.displayId)
      if (e.displayId === PROP_BARREL) barrelIndex = i
      const kind = propKind.get(i)
      const def = kind ? catalogue.kinds.get(kind) : null
      if (def && (e.mask & ENT_FIELD.POS)) world.placeProp(i, kind, e.pos, def)
    }

    const r = predictor.reconcile(s)
    if (r?.verdict) {
      verdicts.push({ v: r.verdict, tick: s.serverTick, phase })
    }
  }
}

let tick = 0
let history = []
async function step (buttons, moveY) {
  tick++
  // Aim north, which is where the barrel is: +y in engine space.
  const f = {
    buttons,
    moveX: axisFromNumber(0),
    moveY: axisFromNumber(moveY),
    aim: Math.round((0.25) * 4294967296) >>> 0
  }
  history = [f, ...history].slice(0, 12)
  predictor.step(f)
  await dw.write(encodeInputBundle({
    clientTick: predictor.tick, ackSnapshotTick: snapshots.at(-1)?.serverTick ?? 0, frames: history
  }))
  drain()
  // What the client would DRAW: the countdown it holds after reconciling.
  const left = world.readAgent(agent).attackLeft
  maxSeenLeft = Math.max(maxSeenLeft, left)
  if (left > 0) sweepSamples.add(left)
  await sleep(8)
}

// Walk north into the barrel and stop against it.
for (let i = 0; i < 160; i++) await step(0, 1)
const stoppedAt = selfY
check(barrelIndex !== null, `the barrel replicated (entity ${barrelIndex})`)
check(stoppedAt > 6 && stoppedAt < 8, `a solid prop stopped the agent at y=${stoppedAt.toFixed(2)}`)

phase = 'swing'
// Swing at it.
const ATTACK = NO_ATTACK ? 0 : BUTTON.ATTACK_PRIMARY
for (let i = 0; i < 6; i++) await step(ATTACK, 0)
for (let i = 0; i < 8; i++) await step(0, 0)

phase = 'after'
// Then keep walking north. Past it, if it went.
for (let i = 0; i < 220; i++) await step(0, 1)
await sleep(300)
drain()

const endedAt = selfY
console.log(`  stopped at y=${stoppedAt.toFixed(2)}, ended at y=${endedAt.toFixed(2)}, ` +
            `barrel ${barrelGone ? 'destroyed' : 'still there'}`)
console.log(`  client's own predicted y=${fxToNumber(world.readAgent(agent).pos.y).toFixed(2)}` +
            `  (mispredicted ${predictor.stats.mispredictions})`)

// The swing the client can actually see, across reconciliation.
if (!NO_ATTACK) {
  const ticks = Number(world.getParamRaw(agent, world.paramId('attack_ticks')))
  console.log(`  client observed the swing at ${sweepSamples.size} distinct countdown values ` +
              `(peak ${maxSeenLeft} of ${ticks}); exact ${predictor.stats.exactMatches}, ` +
              `mispredicted ${predictor.stats.mispredictions}`)
  check(sweepSamples.size > ticks / 2,
        `the client sees the swing travel (${sweepSamples.size} of ${ticks - 1} steps)`)
  check(maxSeenLeft >= ticks - 2, `starting near the top of its travel (${maxSeenLeft})`)

  // Destroying an obstacle a body is leaning on invalidates every tick the
  // client had ALREADY predicted against it -- a burst as long as the client's
  // lead, arriving at once as those snapshots come back. Not a per-tick fault
  // and not avoidable: a client cannot know a script is about to remove what it
  // is standing against.
  //
  // So the assertion is the SHAPE, which is what distinguishes this from a
  // determinism problem: one burst, then agreement. A real divergence never
  // stops, and asserting zero here would be asserting that the client can
  // predict server-side gameplay.
  const bad = verdicts.filter((v) => v.v === 'mispredicted')
  const tail = verdicts.slice(-20).filter((v) => v.v === 'mispredicted').length
  console.log(`  ${bad.length} mispredictions, ${tail} of them in the last 20 snapshots`)
  check(bad.every((v) => v.phase === 'after'),
        'the only divergence is around the destruction')
  check(tail === 0, 'and it heals: client and server agree by the end')
  check(fxToNumber(world.readAgent(agent).pos.y) > 9,
        "and the client's own prediction walked past where the barrel stood")
}

if (NO_ATTACK) {
  check(!barrelGone, 'CONTROL: unattacked, the barrel is still there')
  check(endedAt < 8, `CONTROL: and the agent is still stuck behind it (y=${endedAt.toFixed(2)})`)
} else {
  check(barrelGone, 'the swing destroyed the barrel')
  check(serverLog.includes('') && endedAt > 9,
        `and the agent walked on past where it stood (y=${endedAt.toFixed(2)})`)
}

try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')
console.log(`\nattack: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
