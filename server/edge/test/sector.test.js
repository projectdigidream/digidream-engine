//
// SECTOR LOOKS ON A REAL SERVER (SECTOR_SET; docs/GRAPHS.md "sectors").
//
// maps/scripts/sector_test.lua changes a floor and starts fog at load, then
// dims a sector with a fade. A client there when it dims is told the fade; one
// that joins after is told the state -- dimmed, at once -- with the floor and
// the fog it missed. CONTROL: the late one is not told to fade.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { encodeHello, decodeControlMessage, MSG } from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT = Number(process.env.DNP_WT_PORT ?? 34484)
const LINK = Number(process.env.DNP_LINK_PORT ?? 34557)
let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, l) => { const end = Date.now() + ms; while (Date.now() < end) { if (fn()) return; await sleep(20) } throw new Error(`timeout: ${l}`) }

let MAP = null
for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
  const p = join(ROOT, 'build', arch, 'test.dnpm')
  if (!MAP && existsSync(p)) MAP = p
}
if (!MAP) { console.error('no baked test map'); process.exit(2) }
const certHash = Buffer.from(JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

let log = ''
const server = spawn(serverBin, ['--port', String(LINK), '--map', MAP, '--script', join(ROOT, 'maps', 'scripts', 'sector_test.lua')],
                     { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
server.stdout.on('data', (d) => { log += d }); server.stderr.on('data', (d) => { log += d })
const edge = new Edge({ port: WT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK,
  cert: readFileSync(join(CERTS, 'cert.pem')), privKey: readFileSync(join(CERTS, 'key.pem')), log: () => {} })

async function join_ () {
  const wt = new WebTransport(`https://127.0.0.1:${WT}/dnp`, { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
  await wt.ready
  const control = await wt.createBidirectionalStream()
  const c = { wt, sets: [], welcome: false }
  ;(async () => {
    const r = control.readable.getReader(); let buf = Buffer.alloc(0)
    try {
      for (;;) {
        const { value, done } = await r.read(); if (done) break
        buf = Buffer.concat([buf, Buffer.from(value)])
        while (buf.length >= 4) {
          const len = buf.readUInt32LE(0); if (buf.length < 4 + len) break
          const m = decodeControlMessage(buf.subarray(0, 4 + len)); buf = buf.subarray(4 + len)
          if (m?.type === MSG.WELCOME) c.welcome = true
          else if (m?.type === MSG.SECTOR_SET) c.sets.push(m)
        }
      }
    } catch {}
  })()
  await control.writable.getWriter().write(encodeHello())
  await waitFor(() => c.welcome, 8000, 'welcome')
  return c
}

try {
  await waitFor(() => log.includes('listening for edge'), 8000, 'server')
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge')
  const early = await join_()
  await waitFor(() => early.sets.some((m) => m.what === 'light'), 8000, 'the dimming')
  const late = await join_()
  await waitFor(() => late.sets.length >= 3, 8000, 'the late join')
  console.log('sector: looks changed by a script, kept for whoever arrives')
  const floor = early.sets.find((m) => m.what === 'floor')
  check(floor?.sector === 2 && floor.texture === 'LAVA1', `a floor's picture (${floor?.texture})`)
  const fog = early.sets.find((m) => m.what === 'effect')
  check(fog?.sector === 0 && fog.kind === 5 && fog.on && fog.params.some((p) => p.key === 8 && Number(p.raw) === 0x203040),
        'fog in a sector, with its colour')
  const dim = early.sets.find((m) => m.what === 'light')
  check(dim?.sector === 0 && dim.light === 30 && dim.fadeMs === 2000, `a light fading, told to whoever is there (${dim?.light}, ${dim?.fadeMs} ms)`)
  const lateDim = late.sets.find((m) => m.what === 'light')
  check(lateDim?.light === 30 && lateDim.fadeMs === 0, `CONTROL: a late arrival is told the state, at once (${lateDim?.fadeMs} ms)`)
  check(late.sets.some((m) => m.what === 'floor' && m.texture === 'LAVA1') && late.sets.some((m) => m.what === 'effect' && m.on),
        'and the floor and the fog it missed')
  early.wt.close(); late.wt.close()
} catch (e) {
  check(false, `the run finished (${e.message})`)
  console.log(log.split('\n').slice(-15).join('\n'))
} finally {
  try { await edge.stop() } catch {}
  server.kill('SIGKILL')
}
console.log(`\nsector: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
