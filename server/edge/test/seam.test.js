//
// WALKING ACROSS A SEAM.
//
// Two maps, side by side on a metamap's grid, each with a corridor that ends at
// the cell boundary. The pitch composes them in world space: seam_b's origin is
// one pitch east of seam_a's, so seam_a's stub ending at local x = 40 and
// seam_b's beginning at local x = 0 are the same line. Neither map has ever
// seen the other.
//
// The two maps and the third beside them are ONE WORLD on the server (a
// region: every level on the metamap's grid). What this asserts:
//
//   1. walking east moves the player from level 0 into level 1 WITHOUT a
//      second WELCOME: the server says HOME, and nothing is re-placed;
//   2. the body's position is one continuous path in world coordinates --
//      through the boundary at x = 40 with no jump -- and it keeps its speed
//      and its handle, because it is the same body in the same world;
//   3. the neighbourhood is re-stated from the new home (moves, no maps);
//   4. the player's rules for the level walked into apply (docs/PLAYER.md):
//      a module says `player seam_b move_speed 5`, and the walk sends exactly
//      that -- not the jump_impulse line, which says the engine's own 9.
//
// The CONTROL is the same walk with the metamap withheld. Without a pitch the
// cells are not composed in world space, there is no region and no seam, and
// the player walks off the end of the corridor and falls -- so a pass here has
// to mean the metamap did something.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, MSG
} from '../../../client/src/proto.js'
import { Core } from '../../../client/src/core.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34498)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34572)
const NO_METAMAP = process.env.DNP_NO_METAMAP === '1'
const PITCH = 40

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

function built (name) {
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', arch, name)
    if (existsSync(p)) return p
  }
  return null
}
const A = built('seam_a.dnpm')
const B = built('seam_b.dnpm')
// The third cell is within a radius of 1 of both the others, so it is what a
// crossing RETAINS. See maps/seam_c.map.
const C = built('seam_c.dnpm')
if (!A || !B || !C) { console.error('no baked seam_a/seam_b/seam_c'); process.exit(2) }

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

// A module with rules for seam_b only (docs/PLAYER.md).
const modDir = mkdtempSync(join(tmpdir(), 'dnp-seam-'))
writeFileSync(join(modDir, 'module.txt'), 'name Seam\nid seam\nplayer seam_b move_speed 5\nplayer seam_b jump_impulse 9\n')
process.on('exit', () => rmSync(modDir, { recursive: true, force: true }))
const pworld = (await Core.load()).createWorld()
const args = ['--port', String(LINK_PORT), '--map', A, '--map', B, '--map', C, '--module', modDir]
if (!NO_METAMAP) args.push('--metamap', join(ROOT, 'maps', 'seam_metamap.txt'))

const server = spawn(serverBin, args, { stdio: ['ignore', 'ignore', 'pipe'] })
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

const welcomes = []
const paramUpdates = []
const homes = []
const snapshots = []
const nbr = []
const moves = []
const nbrDrops = []
let nbrBytes = 0
// Map streams begun (a chunk at offset 0), by how many welcomes preceded them.
const streamsAfter = []
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
        if (m?.type === MSG.WELCOME) welcomes.push(m)
        else if (m?.type === MSG.HOME) homes.push({ ...m, afterSnapshots: snapshots.length })
        else if (m?.type === MSG.PARAM_UPDATE) paramUpdates.push({ ...m, afterHomes: homes.length })
        // The cells next door, and the streams that follow them. Counted here
        // rather than reassembled: what this asserts is that the observer is
        // TOLD about the level across its boundary and where it sits, which is
        // everything the renderer needs and everything the server owes.
        else if (m?.type === MSG.NEIGHBOUR_MAP) {
          nbr.push({ ...m, afterWelcomes: welcomes.length })
          nbrBytes = 0
        }
        else if (m?.type === MSG.NEIGHBOUR_MOVE) moves.push({ ...m, afterWelcomes: welcomes.length })
        else if (m?.type === MSG.NEIGHBOUR_DROP) nbrDrops.push(m.level)
        else if (m?.type === MSG.MAP_DATA && m.offset === 0 && (streamsAfter.push(welcomes.length), false)) {}
        else if (m?.type === MSG.MAP_DATA && nbr.length) {
          nbrBytes += m.chunk.length
          if (nbr.at(-1).bytes === undefined && nbrBytes >= m.totalBytes) {
            nbr.at(-1).bytes = nbrBytes
          }
        }
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
await waitFor(() => welcomes.length >= 1, 8000, 'welcome')

let tick = 0
let history = []
async function walkEast () {
  tick++
  const lead = (snapshots.at(-1)?.serverTick ?? 0) + 8
  if (tick < lead) tick = lead
  const f = { buttons: 0, moveX: axisFromNumber(1), moveY: 0, aim: 0 }
  history = [f, ...history].slice(0, 12)
  await dw.write(encodeInputBundle({
    clientTick: tick,
    ackSnapshotTick: snapshots.at(-1)?.serverTick ?? 0,
    frames: history
  }))
}

// Every self position the server reported, in order, in world coordinates.
const path = []
for (let i = 0; i < 700; i++) {
  await walkEast()
  await sleep(6)
}
await sleep(300)
for (const snap of snapshots) {
  if (!snap?.self) continue
  path.push({ x: fxToNumber(snap.self.pos.x), z: fxToNumber(snap.self.pos.z),
              vx: fxToNumber(snap.self.vel.x), sector: snap.self.sector, tick: snap.serverTick })
}
console.log(`  welcomes ${welcomes.length}, homes ${homes.map((h) => h.level).join(' ') || 'none'}`)
console.log(`  x from ${path[0]?.x.toFixed(2)} to ${path.at(-1)?.x.toFixed(2)}`)
console.log(`  neighbours announced: ${nbr.map((n) => `L${n.level}@(${n.dx},${n.dy})`).join(' ') || 'none'}`)
console.log(`  neighbours moved    : ${moves.map((m) => `L${m.level}@(${m.dx},${m.dy})`).join(' ') || 'none'}`)

if (NO_METAMAP) {
  // CONTROL: no pitch, so no region and no seam. The player walks off the end
  // of the corridor and falls out of the world.
  check(homes.length === 0, `CONTROL: with no metamap nobody is re-homed (${homes.length})`)
  check(nbr.length === 0, `CONTROL: and no neighbour is announced (${nbr.length})`)
  check(!paramUpdates.some((u) => u.afterHomes > 0), 'CONTROL: and no rules change on the way')
  const lowest = Math.min(...path.map((p) => p.z))
  check(lowest < 0, `CONTROL: and they fall off the end instead (lowest z=${lowest.toFixed(2)})`)
} else {
  // (0) THE FAR SIDE WAS ALREADY THERE, announced on join with its offset.
  const onJoin = nbr.filter((n) => n.afterWelcomes === 1)
  check(onJoin.length === 2, `both cells next door are announced on join (${onJoin.length})`)
  const east = onJoin.find((n) => Math.abs(n.dx - PITCH) < 0.001 && Math.abs(n.dy) < 0.001)
  check(!!east, `one at exactly one pitch east (${onJoin.map((n) => `${n.dx},${n.dy}`).join(' ')})`)
  check(onJoin.every((n) => n.bytes > 0),
        `and the geometry follows each (${onJoin.map((n) => n.bytes).join(', ')} bytes)`)

  // (1) ONE WORLD: a HOME, not a second WELCOME.
  check(welcomes.length === 1, `nobody was re-placed (${welcomes.length} welcome)`)
  check(welcomes[0]?.region === true, 'the welcome says the level shares its world')
  check(homes.length >= 1 && homes[0].level === east?.level,
        `the server re-homed the player into the level east (${homes.map((h) => h.level).join(' ')})`)
  check(serverLog.includes('walked from level 0 into level 1'), 'and says so')

  // (2) ONE CONTINUOUS PATH through the boundary, in world coordinates.
  const across = path.filter((p) => p.x > PITCH + 1)
  check(path.length > 0 && across.length > 0,
        `the body is past the boundary at x=${PITCH} (reached ${path.at(-1)?.x.toFixed(2)})`)
  let worstStep = 0
  for (let i = 1; i < path.length; i++) {
    const dt = Math.max(1, path[i].tick - path[i - 1].tick)
    worstStep = Math.max(worstStep, Math.abs(path[i].x - path[i - 1].x) / dt)
  }
  // A walk is 8 units a second, 0.13 a tick. Anything like a pitch is a jump.
  check(worstStep < 0.5, `and never jumped (worst ${worstStep.toFixed(3)} a tick)`)
  const nearSeam = path.filter((p) => Math.abs(p.x - PITCH) < 4)
  check(nearSeam.length > 0 && nearSeam.every((p) => p.vx > 0.5),
        `it kept its speed through the boundary (vx ${nearSeam.map((p) => p.vx.toFixed(1)).slice(0, 4).join(' ')})`)
  check(path.every((p) => p.z > -1), 'and did not fall')
  // The same world names the sector by the level it is in: chunk key 0, then 1.
  const keys = [...new Set(path.filter((p) => p.sector >= 0).map((p) => p.sector >>> 16))]
  check(keys.includes(0) && keys.includes(1),
        `the sector the body stands in is named by its level (${keys.join(' then ')})`)

  // (3) THE NEIGHBOURHOOD IS RE-STATED FROM THE NEW HOME, and nothing streamed.
  const movedAfter = moves.filter((m) => m.afterWelcomes === 1)
  const left = movedAfter.find((m) => m.level === welcomes[0]?.homeLevel)
  check(!!left && Math.abs(left.dx + PITCH) < 0.001,
        `the level left is moved to minus a pitch (${left?.dx})`)
  check(nbr.filter((n) => n.afterWelcomes >= 1).length === onJoin.length,
        'and no neighbour map is streamed for the walk')
  check(streamsAfter.filter((n) => n === 1).length === 1 + onJoin.length,
        `nor any home map (${streamsAfter.length} stream(s), all at the join)`)
  check(!movedAfter.some((m) => m.level === east?.level),
        'and the level walked into is not moved: it is underfoot')

  // (4) THE RULES OF THE LEVEL WALKED INTO (docs/PLAYER.md).
  const speed = pworld.paramId('move_speed')
  const onJoin4 = paramUpdates.find((u) => u.afterHomes === 0)
  const joinSpeed = Number(onJoin4?.fields.find((f) => f.id === speed)?.raw) / 2 ** 32
  check(joinSpeed === 8, `CONTROL: seam_a's player runs at the engine's 8 (${joinSpeed})`)
  const walked = paramUpdates.filter((u) => u.afterHomes > 0)
  const sent = walked.flatMap((u) => u.fields)
  check(walked.length === 1 && sent.length === 1 && sent[0].id === speed && Number(sent[0].raw) / 2 ** 32 === 5,
        `walking into seam_b sends its move_speed 5, and only that (${sent.map((f) => `${f.id}=${Number(f.raw) / 2 ** 32}`).join(' ')})`)
}

try { await cw.close() } catch {}
try { wt.close() } catch {}
await edge.stop()
server.kill('SIGKILL')
console.log(`\nseam: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
