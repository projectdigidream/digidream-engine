//
// THE PLAYER'S RULES FROM A MODULE (docs/PLAYER.md), against a real server.
//
// A throwaway module whose module.txt says how its player moves, and the FULL
// PARAM_UPDATE a joining client is sent: that is what the client predicts
// with, so it is the thing to read -- not the server's own idea of it.
//
//   module-wide lines land; a per-level line lands on its level (and on
//   pl_arena through the <prefix>_<name> rule); a line for another level does
//   not (CONTROL); a bad line is said and changes nothing; a later module's
//   line beats an earlier one's; and a body size lands on a map that names none.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync, mkdirSync, writeFileSync, copyFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { Core } from '../../../client/src/core.js'
import { encodeHello, decodeControlMessage, MSG } from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34489)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34563)

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, l) => {
  const end = Date.now() + ms
  while (Date.now() < end) { if (fn()) return true; await sleep(20) }
  throw new Error(`timeout: ${l}`)
}

let MAP = null
for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
  const p = join(ROOT, 'build', arch, 'test.dnpm')
  if (!MAP && existsSync(p)) MAP = p
}
if (!MAP) { console.error('no baked test map'); process.exit(2) }
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const core = await Core.load()
const world = core.createWorld()
const ONE = 2 ** 32

const tmp = mkdtempSync(join(tmpdir(), 'dnp-player-'))
// The level is pl_arena: a per-level line for `arena` reaches it by the
// <prefix>_<name> rule, as dw_map12 gets map12's.
const map = join(tmp, 'pl_arena.dnpm')
copyFileSync(MAP, map)
const mod = (name, text) => {
  const dir = join(tmp, name)
  mkdirSync(join(dir, 'maps'), { recursive: true })
  writeFileSync(join(dir, 'module.txt'), text)
  return dir
}
const first = mod('first', [
  'name First', 'id first',
  'player jump_impulse 12.5',
  'player max_jumps 3',
  'player hover_enabled on',
  'player hover_fuel_max 4',
  'player dash_speed 20',
  'player climb_enabled off',
  'player radius 0.4',
  'player arena turn_speed 2',
  'player elsewhere move_speed 1',
  'player jump_height 3',
  'player max_jumps lots',
  ''].join('\n'))
// Loaded second: its line for the same key wins, as a later `controls` does.
const second = mod('second', ['name Second', 'id second', 'player dash_speed 25', ''].join('\n'))

let log = ''
const server = spawn(serverBin, ['--port', String(LINK_PORT), '--map', map, '--no-scripts',
                                 '--module', first, '--module', second],
                     { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
server.stdout.on('data', (d) => { log += d.toString() })
server.stderr.on('data', (d) => { log += d.toString() })
let edge = null
let wt = null
try {
  await waitFor(() => log.includes('listening for edge'), 8000, 'server')
  edge = new Edge({
    port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
    cert: readFileSync(join(CERTS, 'cert.pem')), privKey: readFileSync(join(CERTS, 'key.pem')), log: () => {}
  })
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge')
  wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
  await wt.ready
  const control = await wt.createBidirectionalStream()
  const cw = control.writable.getWriter()
  const updates = []
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
          if (m?.type === MSG.PARAM_UPDATE) updates.push(m)
        }
      }
    } catch {}
  })()
  await cw.write(encodeHello())
  await waitFor(() => updates.length >= 1, 8000, 'the full parameter set')
  const full = updates[0]
  const val = (name) => {
    const f = full.fields.find((x) => x.id === world.paramId(name))
    return f === undefined ? undefined : Number(f.raw) / ONE
  }
  const raw = (name) => Number(full.fields.find((x) => x.id === world.paramId(name))?.raw)
  console.log('player: a module says how its player moves')
  check(val('jump_impulse') === 12.5, `a decimal lands exactly (jump_impulse ${val('jump_impulse')})`)
  check(raw('max_jumps') === 3 && raw('hover_enabled') === 1 && raw('climb_enabled') === 0,
        'a count and two switches land')
  check(val('hover_fuel_max') === 4, 'the hover tank\'s size lands')
  check(val('turn_speed') === 2, 'a line for `arena` lands on pl_arena')
  check(val('move_speed') === 8, `CONTROL: a line for another level does not (move_speed ${val('move_speed')}, the engine's 8)`)
  check(val('dash_speed') === 25, `a later module's line wins (dash_speed ${val('dash_speed')})`)
  check(/player jump_height 3 -- no movement parameter/.test(log) && /player max_jumps lots -- a count/.test(log),
        'bad lines are said, naming what was wrong')
  check(Math.abs(val('radius') - 0.4) < 1e-9, `a body size lands on a map that names none (radius ${val('radius')})`)
  check(val('friction') === 60 && val('height') === 2, 'and what nobody named is the engine\'s')
} catch (e) {
  check(false, `the run finished (${e.message})`)
  console.log(log.split('\n').slice(-20).join('\n'))
} finally {
  try { wt?.close() } catch {}
  try { await edge?.stop() } catch {}
  server.kill('SIGKILL')
  rmSync(tmp, { recursive: true, force: true })
}

console.log(`\nplayer: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
