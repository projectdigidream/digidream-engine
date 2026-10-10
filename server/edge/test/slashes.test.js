//
// ANOTHER BODY'S SWING, ON A REAL SERVER (docs/SWINGS.md).
//
// A client draws its own swing from what it predicts. To draw a monster's it
// needs two things the server never used to send:
//
//   its SHAPE      the monster's swing parameters (reach, arc, sweep, ticks),
//                  set by its script before this client arrived -- sent now as
//                  a PARAM_UPDATE about that body when a client joins
//   its PROGRESS   how far into a swing it is -- the entity block's ATTACK
//                  field, a countdown, sent while it changes
//
// One awake demon two units from the spawn, so it closes and swings at once.
// CONTROLS: the updates name the monster, never us; and the countdown counts
// DOWN, which a field that merely repeated a number would not.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync, mkdirSync, writeFileSync, rmSync, copyFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot, MSG, ENT_FIELD } from '../../../client/src/proto.js'
import { serverBin, doomProps } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT = Number(process.env.DNP_WT_PORT ?? 34486)
const LINK = Number(process.env.DNP_LINK_PORT ?? 34557)
let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, l) => { const end = Date.now() + ms; while (Date.now() < end) { if (fn()) return; await sleep(20) } throw new Error(`timeout: ${l}`) }

// The monsters map, with its own placement: one demon, awake, beside the spawn.
const dir = mkdtempSync(join(tmpdir(), 'dnp-slashes-'))
mkdirSync(join(dir, 'scripts'))
mkdirSync(join(dir, 'mobs'))
copyFileSync(join(ROOT, 'maps', 'scripts', 'mob_vocab.lua'), join(dir, 'scripts', 'mob_vocab.lua'))
copyFileSync(join(ROOT, 'maps', 'scripts', 'mobs.lua'), join(dir, 'scripts', 'mobs.lua'))
copyFileSync(join(ROOT, 'maps', 'mobs', 'doom_monsters.lua'), join(dir, 'mobs', 'doom_monsters.lua'))
writeFileSync(join(dir, 'place.lua'), 'mobs.place{ scale = 48, {kind="demon", x=336, y=480, z=48} }\n')
writeFileSync(join(dir, 'slashes.map'),
  readFileSync(join(ROOT, 'maps', 'monsters.map'), 'utf8').replace('script monsters_lines.lua', 'script place.lua'))
const mapc = ['arm64', 'x86_64', 'native'].map((a) => join(ROOT, 'build', a, 'mapc')).find((p) => existsSync(p))
const built = spawn(mapc, [join(dir, 'slashes.map'), join(dir, 'slashes.dnpm')])
await new Promise((r) => built.on('exit', r))

const certHash = Buffer.from(JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
let log = ''
const server = spawn(serverBin, ['--port', String(LINK), '--map', join(dir, 'slashes.dnpm'), '--spawn', 'start', '--props', doomProps],
                     { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
server.stdout.on('data', (d) => { log += d }); server.stderr.on('data', (d) => { log += d })
const edge = new Edge({ port: WT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK,
  cert: readFileSync(join(CERTS, 'cert.pem')), privKey: readFileSync(join(CERTS, 'key.pem')), log: () => {} })
let wt = null
try {
  await waitFor(() => log.includes('listening for edge'), 8000, 'server')
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge')
  wt = new WebTransport(`https://127.0.0.1:${WT}/dnp`, { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
  await wt.ready
  const control = await wt.createBidirectionalStream()
  let welcome = null
  const params = []
  const snaps = []
  ;(async () => {
    const r = control.readable.getReader(); let buf = Buffer.alloc(0)
    try {
      for (;;) {
        const { value, done } = await r.read(); if (done) break
        buf = Buffer.concat([buf, Buffer.from(value)])
        while (buf.length >= 4) {
          const len = buf.readUInt32LE(0); if (buf.length < 4 + len) break
          const m = decodeControlMessage(buf.subarray(0, 4 + len)); buf = buf.subarray(4 + len)
          if (m?.type === MSG.WELCOME) welcome = m
          else if (m?.type === MSG.PARAM_UPDATE) params.push(m)
        }
      }
    } catch {}
  })()
  ;(async () => {
    const r = wt.datagrams.readable.getReader()
    try { for (;;) { const { value, done } = await r.read(); if (done) break; const s = decodeSnapshot(Buffer.from(value)); if (s) snaps.push(s) } } catch {}
  })()
  await control.writable.getWriter().write(encodeHello())
  const dw = wt.datagrams.createWritable().getWriter()
  // Played, not just joined: input frames, standing still.
  let tick = 0
  const attacks = []   // [handle, left] in arrival order
  let seen = 0
  const end = Date.now() + 8000
  while (Date.now() < end) {
    tick = Math.max(tick + 1, (snaps.at(-1)?.serverTick ?? 0) + 6)
    await dw.write(encodeInputBundle({ clientTick: tick, ackSnapshotTick: snaps.at(-1)?.serverTick ?? 0,
      frames: [{ buttons: 0, moveX: 0, moveY: 0, aim: 0, actions: 0 }] }))
    for (; seen < snaps.length; seen++) {
      for (const e of snaps[seen].entities ?? []) if (e.mask & ENT_FIELD.ATTACK) attacks.push([e.handle >>> 0, e.attackLeft])
    }
    if (attacks.filter(([, l]) => l > 0).length >= 4) break
    await sleep(16)
  }
  const self = (welcome?.selfHandle ?? 0) >>> 0
  console.log('slashes: a monster\'s swing reaches the client -- its shape and its progress')
  const others = params.filter((m) => ((m.handle & 0x00ffffff) - 1) !== ((self & 0x00ffffff) - 1))
  check(others.length > 0, `on joining, the client is told the other bodies' parameters (${others.length} update(s))`)
  check(params.some((m) => (m.handle >>> 0) === self && (m.flags & 1)), 'CONTROL: and its own, in full, as before')
  const swinger = attacks.find(([, l]) => l > 0)?.[0]
  check(swinger !== undefined && swinger !== self, `a monster's swing arrives as ATTACK in the snapshots (${attacks.slice(0, 8).map(([, l]) => l).join(' ')})`)
  check(others.some((m) => (m.handle >>> 0) === swinger && m.fields.length >= 3),
        'and the client was told that very body\'s parameters -- its swing\'s shape')
  const run = attacks.filter(([h]) => h === swinger).map(([, l]) => l)
  const firstSwing = run.slice(0, run.findIndex((l, i) => i > 0 && l > run[i - 1]) >>> 0 || run.length)
  const falling = firstSwing.length >= 2 && firstSwing.every((l, i) => i === 0 || l < firstSwing[i - 1])
  check(falling, `CONTROL: it counts DOWN through the swing (${firstSwing.join(' ')})`)
} catch (e) {
  check(false, `the run finished (${e.message})`)
  console.log(log.split('\n').slice(-15).join('\n'))
} finally {
  try { wt?.close() } catch {}
  try { await edge.stop() } catch {}
  server.kill('SIGKILL')
  rmSync(dir, { recursive: true, force: true })
}
console.log(`\nslashes: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
