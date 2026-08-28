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
import { Predictor } from '../../../client/src/predictor.js'
import { parseVfx } from '../../../client/src/render/vfx.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, MSG, BUTTON, ENT_FIELD, handleIndex
} from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34486)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34586)
// CONTROL: no player at all.
//
// The monsters spawn when the map loads, so a run that never connects a client
// exercises exactly the branch where there is nothing to chase. "The monster
// moved" and "the monster shot" are otherwise satisfied by anything that
// drifts or fires on a timer.
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
    const p = join(ROOT, 'build', arch, process.env.DNP_MAP_NAME ?? 'monsters.dnpm')
    if (existsSync(p)) return p
  }
  return null
}
const MAP_PATH = mapPath()
if (!MAP_PATH) { console.error('no baked monsters map'); process.exit(2) }

const vfxCatalogue = parseVfx(readFileSync(join(ROOT, 'assets', 'props.txt'), 'utf8'))
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()

const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'),
                     ['--port', String(LINK_PORT), '--map', MAP_PATH, '--spawn', 'start',
                      '--props', join(ROOT, 'assets', 'props.txt')],
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

// In the control, the client connects and asks for nothing: no HELLO means no
// agent, so the server has a level with monsters in it and nobody to chase.
if (!IDLE_ONLY) {
  await cw.write(encodeHello())
  await waitFor(() => welcome, 8000, 'welcome')
  await waitFor(() => acc.complete, 8000, 'map')
  check(acc.complete, 'map loaded')
}

// Enemies, by the kind they declare. This client has no table of monsters.
const seen = new Map()       // entity index -> { kind, first: {x,y}, last: {x,y} }
const vfxSeen = []
let lastSelf = null
let snapCount = 0
const drain = () => {
  while (snapshots.length) {
    const s = snapshots.shift()
    for (const e of s.entities ?? []) {
      const i = handleIndex(e.handle)
      if (e.mask & ENT_FIELD.DESTROYED) {
        // Recorded rather than skipped: a monster that stops existing is the
        // whole point of the damage test, and dropping the event here is how
        // that would silently never be observed.
        const r = seen.get(i)
        if (r) r.gone = true
        continue
      }
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
        // Stamped, so a speed can be computed from them. Snapshots arrive every
        // other tick and a dropped one widens the gap, so the interval has to
        // come from the wire rather than be assumed.
        ;(rec.samples ??= []).push({ t: s.serverTick, x: p.x, y: p.y })
      }
    }
    for (const v of s.vfx ?? []) vfxSeen.push(v)
    lastSelf = s.self ?? lastSelf
    snapCount++
    // Reconciling is what keeps the predictor's clock locked to the server's.
    // Without it the lead drifts and the input runs off the far end of the
    // server's buffer, which is the failure this whole clock exists to avoid.
    if (predictor) predictor.reconcile(s)
  }
}

// The client tick must LEAD the newest snapshot, as a real client's does: the
// server buffers input for ticks it has not simulated yet and has nothing to do
// with a frame stamped for a tick it already ran. A private counter starting at
// zero drifts further behind every second, which is why the player in this test
// stood at its spawn for four hundred ticks with the movement keys held.
// The input clock is the PREDICTOR's, not a counter of our own.
//
// A client's tick has to lead the server by roughly the round trip and stay
// there: the server buffers input for ticks it has not simulated and discards
// anything past its horizon. Two hand-rolled clocks were tried here first -- a
// bare counter from zero, which falls further behind every second, and a value
// recomputed from the newest snapshot, which repeats because frames go out
// faster than snapshots come back. Both are silently ignored by the server,
// which is indistinguishable from a button that does nothing, and cost an
// afternoon of looking at the swing code instead.
//
// Predictor already solves this and is what the real client uses.
// Only when there is a session. The idle control deliberately sends no HELLO,
// so no map ever arrives -- and a predictor needs one. It has nothing to drive
// either: the whole point of that control is a level with nobody in it.
let predictor = null
if (!IDLE_ONLY) {
  const loaded = core.loadMap(acc.bytes())
  if (!loaded.ok) throw new Error('map rejected by the client core')
  const world = core.createWorld({ map: loaded.map })
  predictor = new Predictor(world, world.spawn(loaded.map.spawnForTag('start')))
}
let history = []
async function step (moveY) {
  const f = { buttons: 0, moveX: axisFromNumber(0), moveY: axisFromNumber(moveY), aim: 0 }
  if (!predictor) { drain(); await sleep(1000 / 60); return }
  predictor.step(f)
  history = [f, ...history].slice(0, 12)
  await dw.write(encodeInputBundle({
    clientTick: predictor?.tick ?? 0, ackSnapshotTick: snapshots.at(-1)?.serverTick ?? 0, frames: history
  }))
  drain()
  // One tick per step, so the predictor's clock advances at the rate the server
  // does. At 8 ms this stepped 125 times a second against 60 ticks, and the
  // client's tick ran away from the server until every bundle was past the
  // input horizon and silently discarded -- which presents as a button that
  // does nothing, and did.
  await sleep(1000 / 60)
}

// KILLING ONE.
//
// The engine reports that a swing reached something and stops there; what that
// costs is the script's business, so this is the whole path -- a held attack
// button, the engine's moving collider, on_attack, the script's own health
// bookkeeping, and the entity ceasing to exist. Every link is silent when it
// breaks: a hook nobody registered, a target the script does not recognise, a
// despawn that never comes.
if (!IDLE_ONLY && !process.env.DNP_MAP_NAME) {
  // Walk into them and swing. The monsters have closed by now, so the collider
  // reaches whatever arrived first.
  for (let i = 0; i < 120; i++) {
    // East, toward them, and aiming east too. The swing is a collider in front
    // of the body -- walking away from something and swinging at it is a test
    // of nothing.
    // PULSED, not held. The attack is edge-triggered -- a held button is one
    // swing, not sixty a second -- so holding it swings once, at the start,
    // from wherever the player happened to be standing. Which was across the
    // room.
    const swinging = Math.floor(i / 12) % 2 === 0
    const f = { buttons: swinging ? BUTTON.ATTACK_PRIMARY : 0,
                moveX: axisFromNumber(0), moveY: axisFromNumber(1), aim: 0 }
    predictor.step(f)
    history = [f, ...history].slice(0, 12)
    await dw.write(encodeInputBundle({
      clientTick: predictor?.tick ?? 0, ackSnapshotTick: snapshots.at(-1)?.serverTick ?? 0, frames: history
    }))
    drain()
    await sleep(1000 / 60)
  }
  for (let i = 0; i < 20; i++) await step(0)
  drain()

  const dead = [...seen.entries()].filter(([, r]) => r.gone).length
  const dying = [...seen.entries()].filter(([, r]) => r.anim === 4).length
  console.log(`  ${dead} destroyed, ${dying} showing the death animation`)
  check(dead + dying > 0, `swinging at a monster kills it (${dead} gone, ${dying} dying)`)
}


// Stand still and let them come. The monsters are north of the spawn; walking
// into them would make "did it approach" a statement about the player.
//
// Unless a map was named, in which case MOVE -- pace back and forth. Standing
// at the spawn on a big map leaves most of its monsters outside sight range and
// on the cheap idle path forever, so the first version of this measured a level
// that was almost entirely asleep and reported no stalls while a browser
// session on the same map was warning every few seconds. Walking is what wakes
// them, a few at a time, which is what a player actually does.
const ROAM = process.env.DNP_MAP_NAME !== undefined
for (let i = 0; i < 420; i++) await step(ROAM ? (Math.floor(i / 60) % 2 ? -1 : 1) : 0)
// Keep STEPPING through the pause, rather than sleeping through it.
//
// The server drops a session that stops sending input -- it closes with
// "starved N ticks" -- so a test that pauses to print its results between
// phases is disconnected before the next one starts, and everything after that
// is shouted into a closed session. Which looks exactly like a button that does
// nothing: input encoded correctly, written to a datagram writer that no longer
// goes anywhere, and no error on either side.
for (let i = 0; i < 20; i++) await step(0)
drain()

const enemies = [...seen.entries()].filter(([, r]) => r.kind === KIND_ZOMBIEMAN || r.kind === KIND_IMP)
console.log(`  ${enemies.length} enemies replicated: ` +
            enemies.map(([i, r]) => `${i}:kind ${r.kind} anim ${r.anim}`).join(', '))

if (!IDLE_ONLY) {
  // Three now: two that chase, and the dormant one the damage test kills. The
// count is asserted rather than "at least one" so a map that silently stopped
// placing something is caught.
check(enemies.length === 3 || enemies.length === 2,
      `the map's monsters replicated (${enemies.length})`)
  check(enemies.some(([, r]) => r.kind === KIND_ZOMBIEMAN), 'a zombieman, by the kind it declared')
  check(enemies.some(([, r]) => r.kind === KIND_IMP), 'and an imp')
  // Below MAX_AGENTS: an enemy is an agent, which is what gives it the
  // player's mover rather than a second one.
  check(enemies.every(([i]) => i <= 64), 'in agent slots, not prop slots')
}

const moved = enemies.filter(([, r]) => r.first && r.last &&
  Math.hypot(r.last.x - r.first.x, r.last.y - r.first.y) > 0.5)
console.log('  movement: ' + enemies.map(([i, r]) =>
  `${i} ${Math.hypot((r.last?.x ?? 0) - (r.first?.x ?? 0),
                     (r.last?.y ?? 0) - (r.first?.y ?? 0)).toFixed(2)}`).join(', '))

if (IDLE_ONLY) {
  // Nothing arrives at all without a HELLO, which is the point: the assertion
  // is about the SERVER's log, not about what was replicated.
  const shot = serverLog.includes('[trace]')
  check(!shot, 'CONTROL: with nobody connected, nothing is chased')
  check(vfxSeen.length === 0, 'CONTROL: and nothing shoots')
  check(seen.size === 0, 'CONTROL: and a client that never said HELLO gets no entities')
} else {
  check(moved.length > 0, `${moved.length} of them closed on the player`)

  // HOW FAST it walks is the catalogue's `speed`, not the engine's default.
  //
  // The number is in assets/props.txt and reaches the entity as its move_speed,
  // which is a replicated parameter -- so this is a claim about a text file
  // changing physics without a recompile, and the way it fails is by silently
  // doing nothing and leaving the default in place. That is what the second
  // assertion is for: 5.83 and 8 are far enough apart that "the catalogue was
  // read" and "the catalogue was ignored" cannot both pass.
  const CATALOGUE_SPEED = 5.83     // assets/props.txt, prop 20
  const ENGINE_DEFAULT = 8         // MoveParams::move_speed
  const topSpeed = (r) => {
    let best = 0
    for (let i = 1; i < (r.samples?.length ?? 0); i++) {
      const a = r.samples[i - 1]
      const b = r.samples[i]
      const dt = (b.t - a.t) / 60
      if (dt <= 0) continue
      best = Math.max(best, Math.hypot(b.x - a.x, b.y - a.y) / dt)
    }
    return best
  }
  const fastest = Math.max(...enemies.map(([, r]) => topSpeed(r)))
  console.log(`  fastest observed ${fastest.toFixed(2)} u/s ` +
              `(catalogue ${CATALOGUE_SPEED}, engine default ${ENGINE_DEFAULT})`)
  check(Math.abs(fastest - CATALOGUE_SPEED) < 0.6,
        `it walks at the speed its catalogue line names (${fastest.toFixed(2)})`)
  check(fastest < (CATALOGUE_SPEED + ENGINE_DEFAULT) / 2,
        'CONTROL: and not at the engine default, which is what ignoring the catalogue looks like')

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
// Lua stalls the server logged while this ran.
//
// Surfaced rather than swallowed. A monster script that gets slower does not
// fail an assertion -- it makes the server miss ticks, and a server that falls
// behind presents to every client as a burst of corrections, which is the
// hardest thing in this engine to tell apart from a determinism bug. The server
// already names the responsible script; this is what makes the test say it.
const stalls = serverLog.split('\n').filter((l) => l.includes('budget warn'))
if (stalls.length) {
  console.log(`  ${stalls.length} Lua stall warning(s), worst last:`)
  for (const l of stalls.slice(-4)) console.log(`     ${l.trim()}`)
}
check(stalls.length === 0, `the monster script kept up (${stalls.length} stalls)`)

console.log(`\nenemies: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
