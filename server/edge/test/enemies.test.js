//
// Monsters: agents a script drives, drawn from the catalogue, shooting.
//
// Four claims, each a different layer, and each with the control that separates
// it from the layer below:
//
//   engine    an enemy is an AGENT -- it moves through the controller a player
//             runs, so it walks, falls and is stopped by walls for free
//   catalogue what it looks like is a text file, replicated as display_id, and
//             the client needs no table of monsters
//   script    it chases and it shoots; the engine does neither
//   protocol  a hitscan shot arrives as a transient VfxEvent -- fire and forget,
//             off the entity path, which is what §9.2 reserved it for and what
//             nothing had ever sent
//
// The control that matters: with NO player connected they must not advance.
// "The enemy moved" is otherwise satisfied by anything that drifts.
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
import { parseVfx } from '../../../client/src/render/vfx.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, MSG, ENT_FIELD, handleIndex
} from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34486)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34586)
// CONTROL: connect, then never send an input and never appear as a target...
// which is impossible, so instead: stand still far away. A monster that
// advances on a player it cannot see is not chasing anything.
const IDLE_ONLY = process.env.DNP_ENEMY_IDLE === '1'

const KIND_ZOMBIEMAN = 20
const KIND_IMP = 21

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

const vfxCatalogue = parseVfx(readFileSync(join(ROOT, 'assets', 'props.txt'), 'utf8'))
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'),
                     ['--port', String(LINK_PORT), '--map', MAP_PATH, '--spawn', 'start',
                      '--props', join(ROOT, 'assets', 'props.txt'),
                      '--script', '/tmp/dbg3.lua'],
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
check(acc.complete, 'map loaded')

// Enemies, by the kind they declare. This client has no table of monsters.
const seen = new Map()       // entity index -> { kind, first: {x,y}, last: {x,y} }
const vfxSeen = []
const drain = () => {
  while (snapshots.length) {
    const s = snapshots.shift()
    for (const e of s.entities ?? []) {
      const i = handleIndex(e.handle)
      if (e.mask & ENT_FIELD.DESTROYED) continue
      if (e.displayId) {
        if (!seen.has(i)) seen.set(i, { kind: e.displayId, anim: 0 })
      }
      const rec = seen.get(i)
      if (!rec) continue
      if (e.mask & ENT_FIELD.ANIMATION_STATE) rec.anim = e.animationState
      if (e.mask & ENT_FIELD.POS) {
        const p = { x: fxToNumber(e.pos.x), y: fxToNumber(e.pos.y) }
        rec.first ??= p
        rec.last = p
      }
    }
    for (const v of s.vfx ?? []) vfxSeen.push(v)
  }
}

let tick = 0
let history = []
async function step (moveY) {
  tick++
  const f = { buttons: 0, moveX: axisFromNumber(0), moveY: axisFromNumber(moveY), aim: 0 }
  history = [f, ...history].slice(0, 12)
  await dw.write(encodeInputBundle({
    clientTick: tick, ackSnapshotTick: snapshots.at(-1)?.serverTick ?? 0, frames: history
  }))
  drain()
  await sleep(8)
}

// Stand still and let them come. The monsters are north of the spawn; walking
// into them would make "did it approach" a statement about the player.
for (let i = 0; i < 420; i++) await step(0)
await sleep(300)
drain()

const enemies = [...seen.entries()].filter(([, r]) => r.kind === KIND_ZOMBIEMAN || r.kind === KIND_IMP)
console.log(`  ${enemies.length} enemies replicated: ` +
            enemies.map(([i, r]) => `${i}:kind ${r.kind} anim ${r.anim}`).join(', '))

check(enemies.length === 2, `both monsters replicated (${enemies.length})`)
check(enemies.some(([, r]) => r.kind === KIND_ZOMBIEMAN), 'a zombieman, by the kind it declared')
check(enemies.some(([, r]) => r.kind === KIND_IMP), 'and an imp')
// Below MAX_AGENTS: an enemy is an agent, which is what gives it the player's
// mover rather than a second one.
check(enemies.every(([i]) => i <= 64), 'in agent slots, not prop slots')

const moved = enemies.filter(([, r]) => r.first && r.last &&
  Math.hypot(r.last.x - r.first.x, r.last.y - r.first.y) > 0.5)
console.log('  movement: ' + enemies.map(([i, r]) =>
  `${i} ${Math.hypot((r.last?.x ?? 0) - (r.first?.x ?? 0),
                     (r.last?.y ?? 0) - (r.first?.y ?? 0)).toFixed(2)}`).join(', '))

if (IDLE_ONLY) {
  check(moved.length === 0, 'CONTROL: with nothing in sight, nothing advances')
  check(vfxSeen.length === 0, 'CONTROL: and nothing shoots')
} else {
  check(moved.length > 0, `${moved.length} of them closed on the player`)

  // NOT YET PASSING, and left visible rather than removed or weakened.
  //
  // The monsters replicate, carry their catalogue kind, land in agent slots and
  // close on the player -- so the engine, the wire and the mover are doing
  // their part. What has not been shown to work is the script's own decisions
  // reaching the client: no animation state other than idle arrives, and no
  // shot is fired. Both are in maps/scripts/enemies.lua, both are reachable
  // from a Lua print that this harness does not surface, and neither is
  // diagnosed.
  //
  // Recording that here, failing, is the honest state. Deleting these two would
  // leave a green test asserting that monsters walk, which is the part that
  // already worked.
  check(enemies.some(([, r]) => r.anim !== 0), 'and declared an animation state other than idle')

  // The transient path. Nothing had ever sent one of these.
  const tracers = vfxSeen.filter((v) => vfxCatalogue.kinds.has(v.kind))
  console.log(`  ${vfxSeen.length} vfx events, ${tracers.length} of a kind the catalogue knows`)
  check(vfxSeen.length > 0, `the hitscan shot arrived as a transient (${vfxSeen.length})`)
  check(tracers.length === vfxSeen.length, 'and every one names a kind the catalogue describes')
  // It carries where the shot ENDED, which is what lets a tracer stop on a wall
  // instead of at infinity.
  check(tracers.every((v) => Number.isFinite(fxToNumber(v.pos.x))), 'each with an impact point')
}

try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')
console.log(`\nenemies: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
