//
// THE MODULE'S OWN KEYS, ON A REAL SERVER (docs/KEYS.md).
//
// A throwaway module declares `key reload KeyR touch RELOAD` and a level graph
// answering it. The client is told the key (KEYS), holds it in its input
// frames (the `actions` byte), and the graph's when_button_pressed fires --
// answered with set_hud. CONTROL: frames without the bit fire nothing.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync, mkdirSync, writeFileSync, rmSync, copyFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot, MSG } from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT = Number(process.env.DNP_WT_PORT ?? 34482)
const LINK = Number(process.env.DNP_LINK_PORT ?? 34553)
let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, l) => { const end = Date.now() + ms; while (Date.now() < end) { if (fn()) return; await sleep(20) } throw new Error(`timeout: ${l}`) }

const mod = mkdtempSync(join(tmpdir(), 'dnp-keys-'))
mkdirSync(join(mod, 'maps'))
for (const f of ['trigger_vocab.lua', 'triggers.lua', 'graph_vocab.lua', 'graphs.lua']) {
  writeFileSync(join(mod, 'maps', f), readFileSync(join(ROOT, 'maps', 'scripts', f)))
}
writeFileSync(join(mod, 'module.txt'), 'name Keys\nid keys\nkey reload KeyR touch RELOAD\nkey map KeyM\ndebug off\n')
writeFileSync(join(mod, 'maps', 'keys.graph.json'), JSON.stringify({
  graph: 1, name: 'keys', vars: { n: 0 },
  nodes: [{ id: 'p', type: 'when_button_pressed', button: 'reload' }, { id: 'a', type: 'add_var', var: 'n', value: 1 },
          { id: 'g', type: 'get_var', var: 'n' }, { id: 'h', type: 'set_hud', name: 'reloads' }],
  links: [['p.then', 'a.in'], ['a.then', 'h.in'], ['g.value', 'h.value']]
}))
// A map that runs the graph: the engine's test map, with the four scripts and the graph declared.
let SRC = null
for (const p of [join(ROOT, 'maps', 'test.map')]) if (existsSync(p)) SRC = p
const mapc = ['arm64', 'x86_64', 'native'].map((a) => join(ROOT, 'build', a, 'mapc')).find((p) => existsSync(p))
const mapText = readFileSync(SRC, 'utf8').replace(/^sector /m,
  'script trigger_vocab.lua\nscript triggers.lua\nscript graph_vocab.lua\nscript graphs.lua\nscript keys.graph.json\n\nsector ')
writeFileSync(join(mod, 'maps', 'keys.map'), mapText)
const built = spawn(mapc, [join(mod, 'maps', 'keys.map'), join(mod, 'maps', 'keys.dnpm')])
await new Promise((r) => built.on('exit', r))

const certHash = Buffer.from(JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
let log = ''
const server = spawn(serverBin, ['--port', String(LINK), '--map', join(mod, 'maps', 'keys.dnpm'), '--module', mod],
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
  const cw = control.writable.getWriter()
  const dw = wt.datagrams.createWritable().getWriter()
  let keys = null
  let welcome = null
  const hud = {}
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
          else if (m?.type === MSG.KEYS) keys = m.keys
          else if (m?.type === MSG.HUD_SET) for (const f of m.fields) hud[f.name] = f.value
        }
      }
    } catch {}
  })()
  ;(async () => {
    const r = wt.datagrams.readable.getReader()
    try { for (;;) { const { value, done } = await r.read(); if (done) break; const s = decodeSnapshot(Buffer.from(value)); if (s) snaps.push(s) } } catch {}
  })()
  await cw.write(encodeHello())
  await waitFor(() => keys !== null && snaps.length > 3, 8000, 'KEYS and snapshots')
  console.log('keys: a module key, told to the client and answered by a graph')
  check(keys.length === 2 && keys[0].name === 'reload' && keys[0].code === 'KeyR' && keys[0].label === 'RELOAD' &&
        keys[1].name === 'map' && keys[1].label === '', `the client is told the module's keys (${JSON.stringify(keys)})`)
  // The module launches without the debug overlay (`debug off`); fps.test.js
  // is the control, a module that says nothing and gets it.
  check(welcome?.debug === 0, `WELCOME tells the client to hide its debug overlay (${welcome?.debug})`)
  let tick = 0
  let hist = []
  const send = async (actions, ms) => {
    const end = Date.now() + ms
    while (Date.now() < end) {
      tick = Math.max(tick + 1, (snaps.at(-1)?.serverTick ?? 0) + 6)
      hist = [{ buttons: 0, moveX: 0, moveY: 0, aim: 0, actions }, ...hist].slice(0, 8)
      await dw.write(encodeInputBundle({ clientTick: tick, ackSnapshotTick: snaps.at(-1)?.serverTick ?? 0, frames: hist }))
      await sleep(16)
    }
  }
  await send(0, 500)
  check(hud.reloads === undefined, 'CONTROL: frames without the key fire nothing')
  await send(1, 300)
  await send(0, 300)
  await waitFor(() => hud.reloads !== undefined, 3000, 'the graph answered')
  check(hud.reloads === 1, `holding the key presses it once (${hud.reloads})`)
  await send(1, 300)
  await send(0, 300)
  await waitFor(() => hud.reloads === 2, 3000, 'a second press')
  check(hud.reloads === 2, 'and pressing again, again')
} catch (e) {
  check(false, `the run finished (${e.message})`)
  console.log(log.split('\n').slice(-15).join('\n'))
} finally {
  try { wt?.close() } catch {}
  try { await edge.stop() } catch {}
  server.kill('SIGKILL')
  rmSync(mod, { recursive: true, force: true })
}
console.log(`\nkeys: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
