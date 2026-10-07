//
// THE GRAPHS' SAVED VARIABLES, ON A REAL SERVER (docs/GRAPHS.md "Saved on disk").
//
// Three server runs over one --state-dir, a level graph each:
//
//   A  load_vars before anything was saved: `none`        (the CONTROL)
//   B  save_vars n = 7: the file appears under store/
//   C  a fresh server, n starting at 0, load_vars: 7 comes back
//
// What test_lua_graphs cannot show: that the server hands its state
// directory to the level VMs at all, and that the value outlives the process.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs'
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
const WT = Number(process.env.DNP_WT_PORT ?? 34484)
const LINK = Number(process.env.DNP_LINK_PORT ?? 34555)
let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, l) => { const end = Date.now() + ms; while (Date.now() < end) { if (fn()) return; await sleep(20) } throw new Error(`timeout: ${l}`) }

const mod = mkdtempSync(join(tmpdir(), 'dnp-store-'))
const state = join(mod, 'state')
mkdirSync(join(mod, 'maps'))
for (const f of ['trigger_vocab.lua', 'triggers.lua', 'graph_vocab.lua', 'graphs.lua']) {
  writeFileSync(join(mod, 'maps', f), readFileSync(join(ROOT, 'maps', 'scripts', f)))
}
writeFileSync(join(mod, 'module.txt'), 'name Store\nid store\n')
const mapc = ['arm64', 'x86_64', 'native'].map((a) => join(ROOT, 'build', a, 'mapc')).find((p) => existsSync(p))
const mapText = readFileSync(join(ROOT, 'maps', 'test.map'), 'utf8').replace(/^sector /m,
  'script trigger_vocab.lua\nscript triggers.lua\nscript graph_vocab.lua\nscript graphs.lua\nscript store.graph.json\n\nsector ')
writeFileSync(join(mod, 'maps', 'store.map'), mapText)
// mapc opens every declared script, so the graph has to exist to bake; each run writes its own.
writeFileSync(join(mod, 'maps', 'store.graph.json'), JSON.stringify({ graph: 1, name: 'store', nodes: [], links: [] }))
const built = spawn(mapc, [join(mod, 'maps', 'store.map'), join(mod, 'maps', 'store.dnpm')])
await new Promise((r) => built.on('exit', r))

// Every half second: load `n` from the store and show it, -1 when there is none.
const LOADER = {
  graph: 1, name: 'store', vars: { n: 0 },
  nodes: [{ id: 'e', type: 'every', seconds: 0.5 }, { id: 'l', type: 'load_vars', store: 'edge' },
          { id: 'g', type: 'get_var', var: 'n' }, { id: 'h', type: 'set_hud', to: 'everyone', name: 'n' },
          { id: 'z', type: 'set_hud', to: 'everyone', name: 'n', value: -1 }],
  links: [['e.then', 'l.in'], ['l.then', 'h.in'], ['g.value', 'h.value'], ['l.none', 'z.in']]
}
// Every half second: save n = 7 and say so.
const SAVER = {
  graph: 1, name: 'store', vars: { n: 7 },
  nodes: [{ id: 'e', type: 'every', seconds: 0.5 }, { id: 's', type: 'save_vars', store: 'edge', vars: 'n' },
          { id: 'h', type: 'set_hud', to: 'everyone', name: 'saved', value: 1 }, { id: 'x', type: 'set_hud', to: 'everyone', name: 'saved', value: -1 }],
  links: [['e.then', 's.in'], ['s.then', 'h.in'], ['s.failed', 'x.in']]
}

const certHash = Buffer.from(JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

/** One server run with this graph; resolves to the HUD values once `until(hud)` holds. */
async function session (graph, until) {
  writeFileSync(join(mod, 'maps', 'store.graph.json'), JSON.stringify(graph))
  let log = ''
  const server = spawn(serverBin, ['--port', String(LINK), '--map', join(mod, 'maps', 'store.dnpm'), '--module', mod,
                                   '--state-dir', state], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
  server.stdout.on('data', (d) => { log += d }); server.stderr.on('data', (d) => { log += d })
  const edge = new Edge({ port: WT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK,
    cert: readFileSync(join(CERTS, 'cert.pem')), privKey: readFileSync(join(CERTS, 'key.pem')), log: () => {} })
  let wt = null
  const hud = {}
  try {
    await waitFor(() => log.includes('listening for edge'), 8000, 'server')
    await edge.start()
    await waitFor(() => log.includes('edge connected'), 8000, 'edge')
    wt = new WebTransport(`https://127.0.0.1:${WT}/dnp`, { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
    await wt.ready
    const control = await wt.createBidirectionalStream()
    ;(async () => {
      const r = control.readable.getReader(); let buf = Buffer.alloc(0)
      try {
        for (;;) {
          const { value, done } = await r.read(); if (done) break
          buf = Buffer.concat([buf, Buffer.from(value)])
          while (buf.length >= 4) {
            const len = buf.readUInt32LE(0); if (buf.length < 4 + len) break
            const m = decodeControlMessage(buf.subarray(0, 4 + len)); buf = buf.subarray(4 + len)
            if (m?.type === MSG.HUD_SET) for (const f of m.fields) hud[f.name] = f.value
          }
        }
      } catch {}
    })()
    await control.writable.getWriter().write(encodeHello())
    // Played, not just joined: snapshots read and empty input frames sent, as a client does.
    const snaps = []
    ;(async () => {
      const r = wt.datagrams.readable.getReader()
      try { for (;;) { const { value, done } = await r.read(); if (done) break; const sn = decodeSnapshot(Buffer.from(value)); if (sn) snaps.push(sn) } } catch {}
    })()
    const dw = wt.datagrams.createWritable().getWriter()
    let tick = 0
    const end = Date.now() + 8000
    while (!until(hud) && Date.now() < end) {
      tick = Math.max(tick + 1, (snaps.at(-1)?.serverTick ?? 0) + 6)
      await dw.write(encodeInputBundle({ clientTick: tick, ackSnapshotTick: snaps.at(-1)?.serverTick ?? 0,
        frames: [{ buttons: 0, moveX: 0, moveY: 0, aim: 0, actions: 0 }] }))
      await sleep(16)
    }
    if (!until(hud)) throw new Error(`timeout: the graph answered (${JSON.stringify(hud)}, ${snaps.length} snapshots)`)
  } catch (e) {
    check(false, `the run finished (${e.message})`)
    console.log(log.split('\n').slice(-12).join('\n'))
  } finally {
    try { wt?.close() } catch {}
    try { await edge.stop() } catch {}
    server.kill('SIGKILL')
    await sleep(300)
  }
  return hud
}

const file = join(state, 'store', 'edge.store')
try {
  console.log('store: a graph\'s variables saved to the disk, on a real server')
  const a = await session(LOADER, (h) => h.n !== undefined)
  check(Number(a.n) === -1 && !existsSync(file), `CONTROL: before anything was saved, load_vars says none (n ${a.n})`)
  const b = await session(SAVER, (h) => h.saved !== undefined)
  check(Number(b.saved) === 1, `save_vars reports it saved (${b.saved})`)
  check(existsSync(file), `and the file is in the state directory (${file.replace(mod, '<module>')})`)
  const c = await session(LOADER, (h) => h.n !== undefined)
  check(Number(c.n) === 7, `a NEW server, n starting at 0, loads 7 back (${c.n})`)
} finally {
  rmSync(mod, { recursive: true, force: true })
}
console.log(`\nstore: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
