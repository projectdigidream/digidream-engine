//
// A GRAPH SENDS A PLAYER TO ANOTHER MAP, AT A CHOSEN SPAWN POINT
// (docs/GRAPHS.md, `change_map`; entity:change_map(level, spawn_tag)).
//
// Map A's graph sends whoever arrives to map B, by NAME, at the spawn point
// tagged `arrive` -- away from B's start. The second WELCOME says where the
// player was put. CONTROL: a tag map B does not have arrives at its start, and
// the server says so.
//
import { spawn, execFileSync } from 'node:child_process'
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { decodeControlMessage, encodeHello, MSG, fxToNumber } from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34498)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34572)
const ARCH = process.arch === 'x64' ? 'x86_64' : 'arm64'
const MAPC = join(ROOT, 'build', ARCH, 'mapc')

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, label) => {
  const end = Date.now() + ms
  while (Date.now() < end) { if (fn()) return; await sleep(20) }
  throw new Error(`timeout: ${label}`)
}
const certHash = Buffer.from(JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const S = join(ROOT, 'maps', 'scripts')

// Two maps, baked: A with the graph runtime and a graph, B with an extra spawn.
function bake (spawnTag) {
  const dir = mkdtempSync(join(tmpdir(), 'maptravel-'))
  writeFileSync(join(dir, 'a.map'), `body radius 0.3333 height 1.1667
script ${S}/trigger_vocab.lua
script ${S}/triggers.lua
script ${S}/graph_vocab.lua
script ${S}/graphs.lua
script ${dir}/travel.graph.json
sector floor 0 ceil 6 tag 1 light 220 floortex FLOOR4_8 ceiltex CEIL3_5
  vertex 0 0    wall tex STARTAN3
  vertex 20 0   wall tex STARTAN3
  vertex 20 20  wall tex STARTAN3
  vertex 0 20   wall tex STARTAN3
end
spawn start 16 10 1
`)
  writeFileSync(join(dir, 'travel.graph.json'), JSON.stringify({
    graph: 1, name: 'travel',
    nodes: [{ id: 'in', type: 'when_player_arrives' },
            { id: 'go', type: 'change_map', level: 'b', spawn: spawnTag }],
    links: [['in.then', 'go.in']]
  }))
  writeFileSync(join(dir, 'b.map'), `body radius 0.3333 height 1.1667
sector floor 0 ceil 6 tag 1 light 180 floortex FLOOR4_8 ceiltex CEIL3_5
  vertex 0 0    wall tex BRICK5
  vertex 30 0   wall tex BRICK5
  vertex 30 7   neighbor 1
  vertex 30 14  wall tex BRICK5
  vertex 0 14   wall tex BRICK5
end
sector floor 0 ceil 6 tag 2 light 120 floortex FLOOR4_8 ceiltex CEIL3_5
  vertex 30 0   wall tex BRICK5
  vertex 40 0   wall tex BRICK5
  vertex 40 7   wall tex BRICK5
  vertex 30 7   neighbor 0
end
spawn start 5 7 1
spawn arrive 35 3 1
`)
  execFileSync(MAPC, [join(dir, 'a.map'), join(dir, 'a.dnpm')])
  execFileSync(MAPC, [join(dir, 'b.map'), join(dir, 'b.dnpm')])
  return dir
}

// One run: a server on A and B, one player, every welcome kept.
async function run (spawnTag) {
  const dir = bake(spawnTag)
  const server = spawn(serverBin, ['--port', String(LINK_PORT), '--map', join(dir, 'a.dnpm'), '--map', join(dir, 'b.dnpm')],
                       { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
  let log = ''
  server.stdout.on('data', (d) => { log += d })
  server.stderr.on('data', (d) => { log += d })
  process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })
  await waitFor(() => log.includes('listening for edge'), 8000, 'server')
  const edge = new Edge({ port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
                          cert: readFileSync(join(CERTS, 'cert.pem')), privKey: readFileSync(join(CERTS, 'key.pem')), log: () => {} })
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge')
  const wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
  await wt.ready
  const control = await wt.createBidirectionalStream()
  const cw = control.writable.getWriter()
  const welcomes = []
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
  await cw.write(encodeHello())
  await waitFor(() => welcomes.length >= 2, 8000, 'a second welcome: the map change').catch(() => {})
  await sleep(200)
  try { wt.close() } catch {}
  await edge.stop()
  server.kill('SIGTERM')
  await sleep(300)
  rmSync(dir, { recursive: true, force: true })
  const at = welcomes[1]?.arrival
  return { welcomes: welcomes.length, at: at && { x: fxToNumber(at.x), y: fxToNumber(at.y) }, log }
}

const tagged = await run('arrive')
console.log(`  to 'arrive': ${tagged.welcomes} welcomes, arrived at ${JSON.stringify(tagged.at)}`)
check(tagged.welcomes === 2, 'the graph moved the player to the other map')
check(tagged.at && Math.abs(tagged.at.x - 35) < 0.01 && Math.abs(tagged.at.y - 3) < 0.01,
      'at the spawn point tagged `arrive`, not at the map\'s start')
const untagged = await run('nowhere')
console.log(`  to 'nowhere': arrived at ${JSON.stringify(untagged.at)}`)
check(untagged.at && Math.abs(untagged.at.x - 5) < 0.01 && Math.abs(untagged.at.y - 7) < 0.01,
      'CONTROL: a tag the map lacks arrives at its start')
check(untagged.log.includes("has no spawn tagged 'nowhere'"), 'and the server says so')

console.log(`\nmaptravel: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
