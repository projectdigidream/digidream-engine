//
// REGION LEVELS' SCRIPTS (docs/REGION_GAPS.md §2-§4), against a real server.
//
// The seam fixture's three cells (maps/seam_metamap.txt): seam_a, seam_b one
// pitch east, seam_c beside them. Every level runs the graph library and a
// small script, written below, that:
//
//   - says where its own spawn point "start" is (§3: in a region, the lookups
//     used to ask the region's store, which has no spawns, and answered nil);
//   - loads a graph with `when_player_arrives` / `when_player_leaves` logging
//     the level, and in seam_b a teleport to the spawn tag "start" on arrival
//     (§3: such a graph used to be refused at load);
//   - in seam_c, fails to load until the test rewrites it (§4: a failed region
//     level used to leave its chunk behind and could never be entered again).
//
// The player joins in seam_a and walks east into seam_b. What this asserts:
//
//   §2  seam_b's arrival comes AFTER the server re-homed the player there, not
//       when the level woke with the player still next door; seam_a sees the
//       player leave; seam_c, never entered, sees no arrival at all. CONTROL:
//       seam_a's arrival on join is seen, so the graphs do run.
//   §3  seam_b's spawn point is its own (34, 20), and the arrival's teleport
//       lands at its world position, one pitch east: x = 74.
//   §4  seam_c fails at the join with its script's error; rewritten, the next
//       look at residency (the walk) loads it, and "does not fit" never shows.
//       CONTROL: the first attempt really failed.
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
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const REPO = join(ROOT, '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34502)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34576)
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
const C = built('seam_c.dnpm')
if (!A || !B || !C) { console.error('no baked seam_a/seam_b/seam_c'); process.exit(2) }

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

const dir = mkdtempSync(join(tmpdir(), 'dnp-region-gaps-'))
process.on('exit', () => rmSync(dir, { recursive: true, force: true }))
const GAPS = join(dir, 'gaps.lua')
const gaps = (seamCFails) => `
local L = dnp.level()
${seamCFails ? 'if L == 2 then error("gaps: seam_c refuses to load") end' : ''}
local sp = dnp.spawn_point("start")
print(("[gaps] level %d spawn %s"):format(L, sp and (tostring(sp.x) .. "," .. tostring(sp.y)) or "nil"))
local nodes = {
  { id = "a", type = "when_player_arrives" },
  { id = "say", type = "action", a = "log", text = "arrived in " .. L },
  { id = "l", type = "when_player_leaves" },
  { id = "bye", type = "action", a = "log", text = "left " .. L },
}
local links = { { "a.then", "say.in" }, { "l.then", "bye.in" } }
if L == 1 then
  nodes[#nodes + 1] = { id = "go", type = "teleport", spawn = "start" }
  links[#links + 1] = { "say.then", "go.in" }
end
graphs.load({ graph = 1, name = "gaps" .. L, nodes = nodes, links = links }, "gaps" .. L)
`
writeFileSync(GAPS, gaps(true))

const S = join(ROOT, 'maps', 'scripts')
const args = ['--port', String(LINK_PORT), '--map', A, '--map', B, '--map', C,
              '--metamap', join(ROOT, 'maps', 'seam_metamap.txt'),
              '--props', join(REPO, 'modules', 'doom2', 'assets', 'props.txt'),
              '--script', join(S, 'trigger_vocab.lua'), '--script', join(S, 'triggers.lua'),
              '--script', join(S, 'graph_vocab.lua'), '--script', join(S, 'graphs.lua'),
              '--script', GAPS]
const server = spawn(serverBin, args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
let log = ''
server.stdout.on('data', (d) => { log += d.toString() })
server.stderr.on('data', (d) => { log += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })

try {
  await waitFor(() => log.includes('listening for edge'), 8000, 'server')
  const edge = new Edge({
    port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
    cert: readFileSync(join(CERTS, 'cert.pem')),
    privKey: readFileSync(join(CERTS, 'key.pem')),
    log: () => {}
  })
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge')

  const wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
    serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
  })
  await withTimeout(wt.ready, 8000, 'ready')
  const control = await wt.createBidirectionalStream()
  const cw = control.writable.getWriter()
  const dw = wt.datagrams.createWritable().getWriter()
  const welcomes = []
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
          if (m?.type === MSG.WELCOME) welcomes.push(m)
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
  await waitFor(() => /arrived in 0/.test(log), 4000, 'arrival in seam_a')

  // §4, the first attempt: seam_c is next door, so the join loads it, and its
  // script refuses.
  const failedFirst = /seam_c refuses to load/.test(log)
  writeFileSync(GAPS, gaps(false))

  let tick = 0
  let history = []
  const walkEast = async () => {
    tick++
    const lead = (snapshots.at(-1)?.serverTick ?? 0) + 8
    if (tick < lead) tick = lead
    const f = { buttons: 0, moveX: axisFromNumber(1), moveY: 0, aim: 0 }
    history = [f, ...history].slice(0, 12)
    await dw.write(encodeInputBundle({
      clientTick: tick, ackSnapshotTick: snapshots.at(-1)?.serverTick ?? 0, frames: history
    }))
  }
  for (let i = 0; i < 700 && !/arrived in 1/.test(log); i++) { await walkEast(); await sleep(6) }
  for (let i = 0; i < 60; i++) { await walkEast(); await sleep(6) }
  await sleep(300)

  const at = (re) => { const m = re.exec(log); return m ? m.index : -1 }

  console.log('§2 arrivals')
  check(at(/arrived in 0/) >= 0, 'CONTROL: seam_a sees the player arrive at the join')
  const walked = at(/walked from level 0 into level 1/)
  const arrived1 = at(/arrived in 1/)
  check(walked >= 0, 'the player walks from seam_a into seam_b')
  check(arrived1 > walked, `seam_b's arrival comes after the walk into it, not before (${arrived1} vs ${walked})`)
  check((log.match(/arrived in 1/g) ?? []).length === 1, 'and only once')
  check(at(/left 0/) > walked, 'seam_a sees the player leave')
  check(!/arrived in 2/.test(log), 'seam_c, never entered, sees no arrival')

  console.log('§3 spawn points')
  check(/\[gaps\] level 1 spawn 34\.0+,20\.0+/.test(log),
        `seam_b's script sees its own spawn point, in its own terms (${log.match(/\[gaps\] level 1 spawn (\S+)/)?.[1]})`)
  check(!/no spawn point tagged/.test(log), 'and the graph teleporting to it is not refused')
  const xs = snapshots.filter((s) => s?.self).map((s) => fxToNumber(s.self.pos.x))
  let jump = null
  for (let i = 1; i < xs.length; i++) if (xs[i] - xs[i - 1] > 10) { jump = [xs[i - 1], xs[i]]; break }
  check(jump !== null && Math.abs(jump[1] - (PITCH + 34)) < 0.5,
        `the arrival's teleport lands on the spawn, one pitch east (${jump ? jump.map((x) => x.toFixed(2)).join(' -> ') : 'no jump'})`)

  console.log('§4 a failed level, fixed')
  check(failedFirst, 'CONTROL: seam_c failed to load at the join, with its own error')
  check(/level 2 joined the region/.test(log.slice(at(/seam_c refuses to load/))),
        'rewritten, it joins the region on the next look at residency')
  check(!/does not fit in the region/.test(log), 'and "does not fit" never shows')

  try { await cw.close() } catch {}
  try { wt.close() } catch {}
  await edge.stop()
} catch (e) {
  check(false, `the run finished (${e.message})`)
  console.log(log.split('\n').slice(-30).join('\n'))
}
server.kill('SIGKILL')
console.log(`\nregion_gaps: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
