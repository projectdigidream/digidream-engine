//
// WALKING ACROSS A SEAM.
//
// Two maps, side by side on a metamap's grid, each with a corridor that ends at
// the cell boundary. The pitch composes them in world space: seam_b's origin is
// one pitch east of seam_a's, so seam_a's stub ending at local x = 40 and
// seam_b's beginning at local x = 0 are the same line. Neither map has ever
// seen the other.
//
// What this asserts, and they are three different claims:
//
//   1. the player who walked east is in the other map -- a second WELCOME, a
//      different map hash;
//   2. they arrived AT THE SAME WORLD POINT they left from. A seam is a change
//      of frame and nothing else, so the local coordinate jumps by exactly one
//      pitch and the world coordinate does not move at all. This is the one
//      that distinguishes a seam from a teleport;
//   3. they kept their momentum. A crossing that stops you dead is a loading
//      screen with the screen taken out.
//
// The CONTROL is the same walk with the metamap withheld. Without a pitch the
// cells are not composed in world space, there is no seam to cross, and the
// player walks off the end of the corridor and falls -- so a pass here has to
// mean the metamap did something.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import {
  encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot,
  axisFromNumber, fxToNumber, MSG
} from '../../../client/src/proto.js'
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

const args = ['--port', String(LINK_PORT), '--map', A, '--map', B, '--map', C]
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

// The last position reported while still in the first map, and the first
// reported after the second WELCOME.
//
// BY ARRIVAL, not by "whatever is latest when the welcome is noticed". The
// welcome comes up the control stream and snapshots come as datagrams, so the
// newest snapshot at the moment the welcome is seen is usually still one from
// the map just left -- and reading it as the arrival makes the crossing look
// like a jump of exactly one pitch. It passed for as long as the timing
// happened to put a fresh snapshot in between, which is not a property of the
// code under test.
const at = (p) => ({ x: fxToNumber(p.self.pos.x), y: fxToNumber(p.self.pos.y),
                     z: fxToNumber(p.self.pos.z), vx: fxToNumber(p.self.vel.x) })
let lastInA = null
let firstInB = null
let seenAtCross = -1
for (let i = 0; i < 700; i++) {
  await walkEast()
  await sleep(6)
  if (welcomes.length === 1) {
    const s = snapshots.at(-1)
    if (s?.self) { lastInA = at(s); seenAtCross = snapshots.length }
  } else if (welcomes.length >= 2 && !firstInB) {
    const s = snapshots.slice(seenAtCross).find((x) => x?.self)
    if (s) firstInB = at(s)
  }
}
await sleep(300)

console.log(`  welcomes ${welcomes.length}`)
if (lastInA) console.log(`  last in A: x=${lastInA.x.toFixed(3)} vx=${lastInA.vx.toFixed(3)}`)
if (firstInB) console.log(`  first in B: x=${firstInB.x.toFixed(3)} vx=${firstInB.vx.toFixed(3)}`)

console.log(`  neighbours announced: ${nbr.map((n) => `L${n.level}@(${n.dx},${n.dy})`).join(' ') || 'none'}`)
console.log(`  neighbours moved    : ${moves.map((m) => `L${m.level}@(${m.dx},${m.dy})`).join(' ') || 'none'}`)

if (NO_METAMAP) {
  // CONTROL: no pitch, so the cells are not composed and there is no seam. The
  // player walks off the end of the corridor and falls out of the world.
  check(welcomes.length === 1,
        `CONTROL: with no metamap nobody crosses (${welcomes.length} welcome)`)
  // CONTROL for the neighbour announcements too: with no layout there is no
  // cell to be next to, so a client is told about nothing. Without this, a
  // server that announced every level to everybody would pass the tests below.
  check(nbr.length === 0,
        `CONTROL: and no neighbour is announced (${nbr.length})`)
  check(lastInA !== null && lastInA.z < 0,
        `CONTROL: and they fall off the end instead (z=${lastInA ? lastInA.z.toFixed(2) : 'n/a'})`)
} else {
  // (4) THE FAR SIDE WAS ALREADY ON SCREEN.
  //
  // What makes a seam a seam rather than a loading screen: the level across the
  // boundary is announced on JOIN, with the offset that composes it, and its
  // geometry follows. Without this the corridor leads into black and the
  // crossing arrives as a cut.
  const onJoin = nbr.filter((n) => n.afterWelcomes === 1)
  check(onJoin.length === 2, `both cells next door are announced on join (${onJoin.length})`)
  const east = onJoin.find((n) => Math.abs(n.dx - PITCH) < 0.001 && Math.abs(n.dy) < 0.001)
  check(!!east, `one at exactly one pitch east (${onJoin.map((n) => `${n.dx},${n.dy}`).join(' ')})`)
  check(onJoin.every((n) => n.bytes > 0),
        `and the geometry follows each (${onJoin.map((n) => n.bytes).join(', ')} bytes)`)

  // (5) A CROSSING DOES NOT RE-SEND WHAT THE CLIENT ALREADY HAS.
  //
  // This is what makes a seam cheap rather than the most expensive thing in the
  // game. Walking one cell changes where every neighbour sits and which ones
  // they are hardly at all: seam_c is next to both halves, so it must arrive as
  // an offset and not as a map. Nine cells' geometry rebuilt to step through a
  // doorway is the loading screen a seam exists to abolish.
  const movedAfter = moves.filter((m) => m.afterWelcomes >= 2)
  const mappedAfter = nbr.filter((n) => n.afterWelcomes >= 2)
  check(movedAfter.length >= 1,
        `the retained cell is re-stated, not re-sent (${movedAfter.length} move(s))`)
  check(movedAfter.some((m) => m.level === onJoin.find((n) => n !== east)?.level ||
                               onJoin.some((n) => n.level === m.level && n !== east)),
        'and it is the one that was never entered')
  // NO MAP CROSSES THE WIRE AT ALL.
  //
  // The level entered was streamed as a neighbour on join and the level left
  // was the home map, and by the holding rule (docs/protocol.md 8.5) the client
  // has both until the server drops them. So the welcome says the map does not
  // follow, and the level left is MOVED to minus a pitch like any other cell
  // the client holds.
  //
  // This asserted one map (the level left) for a while, when "held" was the
  // server's guess about the client's bookkeeping and a wrong guess lost a cell
  // for the rest of the session. It is not a guess now; it is the protocol.
  check(mappedAfter.length === 0,
        `no neighbour map is streamed for a crossing (${mappedAfter.length})`)
  check(streamsAfter.filter((n) => n >= 2).length === 0,
        `and no home map either (${streamsAfter.filter((n) => n >= 2).length} stream(s) after the crossing)`)
  check(welcomes.length === 2 && welcomes[1].mapFollows === false,
        'the crossing welcome says its map does not follow')
  // CONTROL: the join's welcome did send one, so the flag is not stuck false.
  check(welcomes[0]?.mapFollows === true && streamsAfter.some((n) => n === 1),
        'CONTROL: while the join\'s welcome did, and its map came')
  check(welcomes.length === 2 && welcomes[1].homeLevel === east?.level,
        `and names the level entered (${welcomes[1]?.homeLevel} vs ${east?.level})`)
  const leftMoved = movedAfter.find((m) => m.level === welcomes[0]?.homeLevel)
  check(!!leftMoved && Math.abs(leftMoved.dx + PITCH) < 0.001,
        `the level just left is moved to minus a pitch (${leftMoved?.dx})`)
  // CONTROL: and the level just ENTERED is neither sent nor moved -- it is
  // underfoot, not next door.
  check(!mappedAfter.some((n) => n.level === east?.level) &&
        !movedAfter.some((m) => m.level === east?.level),
        'and the level just entered is neither sent nor moved')

  // (1) somewhere else.
  check(welcomes.length === 2, `the player crossed (${welcomes.length} welcomes)`)
  check(welcomes.length === 2 && welcomes[0].mapHash !== welcomes[1].mapHash,
        'into a different map')
  check(serverLog.includes('crossed the seam'), 'and the server says which way')

  // (2) THE SAME WORLD POINT. The local coordinate jumps by exactly one pitch,
  // which is what makes the world coordinate not move at all.
  if (lastInA && firstInB) {
    const worldA = lastInA.x + 0 * PITCH
    const worldB = firstInB.x + 1 * PITCH
    console.log(`  world x: ${worldA.toFixed(3)} -> ${worldB.toFixed(3)}`)
    check(Math.abs(worldB - worldA) < 1.5,
          `the world coordinate did not jump (${(worldB - worldA).toFixed(3)} units)`)
    // CONTROL for that: the LOCAL coordinate did jump, and by about a pitch --
    // otherwise "it did not move" is satisfied by a player who never left.
    check(lastInA.x - firstInB.x > PITCH * 0.8,
          `while the local one moved a pitch (${(lastInA.x - firstInB.x).toFixed(1)})`)

    // (3) still walking.
    check(firstInB.vx > 0.5, `and kept their momentum (vx ${firstInB.vx.toFixed(2)})`)
    check(firstInB.z > -1, `and did not fall (z ${firstInB.z.toFixed(2)})`)
  } else {
    check(false, 'positions either side of the seam were recorded')
  }
}

try { await cw.close() } catch {}
try { wt.close() } catch {}
await edge.stop()
server.kill('SIGKILL')
console.log(`\nseam: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
