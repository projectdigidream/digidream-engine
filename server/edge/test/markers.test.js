//
// MARKERS ON A REAL SERVER (MARK_SET / MARK_CLEAR; docs/MARKERS.md).
//
// maps/scripts/markers_test.lua hangs a sign at a place, a bar on the first
// player and a pointer only they see; when a second player arrives it changes
// the bar and takes the sign down. The first is told all three; the second is
// told the level's markers on arrival but never the first player's own; both
// are told the change and the removal.
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
const WT = Number(process.env.DNP_WT_PORT ?? 34656)
const LINK = Number(process.env.DNP_LINK_PORT ?? 34657)
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
const server = spawn(serverBin, ['--port', String(LINK), '--map', MAP, '--script', join(ROOT, 'maps', 'scripts', 'markers_test.lua')],
                     { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
server.stdout.on('data', (d) => { log += d }); server.stderr.on('data', (d) => { log += d })
const edge = new Edge({ port: WT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK,
  cert: readFileSync(join(CERTS, 'cert.pem')), privKey: readFileSync(join(CERTS, 'key.pem')), log: () => {} })

async function join_ () {
  const wt = new WebTransport(`https://127.0.0.1:${WT}/dnp`, { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
  await wt.ready
  const control = await wt.createBidirectionalStream()
  const c = { wt, marks: new Map(), cleared: [], sets: 0, welcome: null }
  ;(async () => {
    const r = control.readable.getReader(); let buf = Buffer.alloc(0)
    try {
      for (;;) {
        const { value, done } = await r.read(); if (done) break
        buf = Buffer.concat([buf, Buffer.from(value)])
        while (buf.length >= 4) {
          const len = buf.readUInt32LE(0); if (buf.length < 4 + len) break
          const m = decodeControlMessage(buf.subarray(0, 4 + len)); buf = buf.subarray(4 + len)
          if (m?.type === MSG.WELCOME) c.welcome = m
          else if (m?.type === MSG.MARK_SET) { c.marks.set(m.id, m); c.sets++ }
          else if (m?.type === MSG.MARK_CLEAR) { c.marks.delete(m.id); c.cleared.push(m.id) }
        }
      }
    } catch {}
  })()
  await control.writable.getWriter().write(encodeHello())
  await waitFor(() => c.welcome, 8000, 'welcome')
  return c
}
const byTemplate = (c, t) => [...c.marks.values()].filter((m) => m.template === t)

try {
  await waitFor(() => log.includes('listening for edge'), 8000, 'server')
  await edge.start()
  await waitFor(() => log.includes('edge connected'), 8000, 'edge')
  const first = await join_()
  await waitFor(() => first.marks.size >= 3, 8000, 'the first player\'s markers')
  console.log('markers: hung by a script, told to who sees them, kept for whoever arrives')
  const sign = byTemplate(first, 'label')[0]
  check(sign?.anchor === 1 && sign.pos.x === 1n << 32n && sign.pos.y === 2n << 32n && sign.values.text === 'Hello',
        `a sign at a place, with its text (${sign?.values?.text})`)
  const bar = byTemplate(first, 'health')[0]
  const self = first.welcome.selfHandle
  check(bar?.anchor === 0 && (bar.handle & 0xffffff) === (self & 0xffffff) && bar.values.value === 3 && bar.values.max === 4 &&
        bar.height === 1n << 31n, `a bar on the player, by the handle the snapshots use (${bar?.values?.value}/${bar?.values?.max})`)
  check(byTemplate(first, 'marker')[0]?.values?.text === 'yours', 'and one only they see')
  const second = await join_()
  await waitFor(() => first.cleared.length >= 1 && second.cleared.length >= 1, 8000, 'the change')
  await sleep(200)
  check(second.sets >= 2 && byTemplate(second, 'marker').length === 0,
        `CONTROL: a later arrival was told the level's markers (${second.sets} sent), not the first player's own`)
  check(byTemplate(first, 'health')[0]?.values?.value === 1 && byTemplate(second, 'health')[0]?.values?.value === 1,
        'a change is told to both')
  check(byTemplate(first, 'label').length === 0 && byTemplate(second, 'label').length === 0 &&
        first.cleared[0] === sign.id, 'and a marker taken down is gone for both')
  first.wt.close(); second.wt.close()
} catch (e) {
  check(false, `the run finished (${e.message})`)
  console.log(log.split('\n').slice(-15).join('\n'))
} finally {
  try { await edge.stop() } catch {}
  server.kill('SIGKILL')
}
console.log(`\nmarkers: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
