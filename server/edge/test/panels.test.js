//
// PANELS BY NAME, ON A REAL SERVER (docs/PANELS.md).
//
// A throwaway module whose GAME graph opens a shop panel when a player
// arrives, answers its Buy button by name, and answers the player's × --
// all through the real protocol: UI_OPEN out, UI_EVENT back (widget 0 for the
// ×), the graph's set_hud as HUD_SET. CONTROL: a press on the panel after the
// × is dropped, because the server forgot it.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { encodeHello, decodeControlMessage, encodeUiEvent, MSG } from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT = Number(process.env.DNP_WT_PORT ?? 34483)
const LINK = Number(process.env.DNP_LINK_PORT ?? 34554)
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

const mod = mkdtempSync(join(tmpdir(), 'dnp-panels-'))
mkdirSync(join(mod, 'maps'))
// A game graph runs on the graph runtime, which the module lists first --
// copies of the engine's own, as a module's maps directory carries them.
for (const f of ['trigger_vocab.lua', 'triggers.lua', 'graph_vocab.lua', 'graphs.lua']) {
  writeFileSync(join(mod, 'maps', f), readFileSync(join(ROOT, 'maps', 'scripts', f)))
}
writeFileSync(join(mod, 'module.txt'),
  'name Panels\nid panels\ngame trigger_vocab.lua\ngame triggers.lua\ngame graph_vocab.lua\ngame graphs.lua\ngame shop.graph.json\n')
writeFileSync(join(mod, 'maps', 'shop.graph.json'), JSON.stringify({
  graph: 1, name: 'shop',
  panels: { shop: { title: 'Shop', widgets: [
    { kind: 'label', id: 1, name: 'gold', label: 'Gold' },
    { kind: 'button', id: 2, name: 'buy', label: 'Buy' },
    { kind: 'button', id: 3, name: 'sell', label: 'Sell', hidden: true }] } },
  nodes: [
    { id: 'arrive', type: 'when_player_arrives' }, { id: 'open', type: 'open_panel', panel: 'shop' },
    { id: 'used', type: 'when_panel_used', panel: 'shop', widget: 'buy' }, { id: 'bought', type: 'set_hud', name: 'bought', value: 1 },
    { id: 'nobuy', type: 'hide_element', panel: 'shop', widget: 'buy' }, { id: 'cansell', type: 'show_element', panel: 'shop', widget: 'sell' },
    { id: 'nosign', type: 'hide_element', panel: 'shop', decor: 'sign' },
    { id: 'shut', type: 'when_panel_closed', panel: 'shop' }, { id: 'closed', type: 'set_hud', name: 'closed', value: 1 }],
  links: [['arrive.then', 'open.in'], ['used.then', 'bought.in'], ['bought.then', 'nobuy.in'], ['nobuy.then', 'cansell.in'],
          ['cansell.then', 'nosign.in'], ['shut.then', 'closed.in']]
}))

let log = ''
const server = spawn(serverBin, ['--port', String(LINK), '--map', MAP, '--module', mod], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
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
  const opens = []
  const uiSets = []
  const hud = {}
  ;(async () => {
    const r = control.readable.getReader(); let buf = Buffer.alloc(0)
    try {
      for (;;) {
        const { value, done } = await r.read(); if (done) break
        buf = Buffer.concat([buf, Buffer.from(value)])
        while (buf.length >= 4) {
          const len = buf.readUInt32LE(0); if (buf.length < 4 + len) break
          const m = decodeControlMessage(buf.subarray(0, 4 + len)); buf = buf.subarray(4 + len)
          if (m?.type === MSG.UI_OPEN) opens.push(m)
          else if (m?.type === MSG.UI_SET) uiSets.push(m)
          else if (m?.type === MSG.HUD_SET) for (const f of m.fields) hud[f.name] = f.value
        }
      }
    } catch {}
  })()
  await cw.write(encodeHello())
  await waitFor(() => opens.length >= 1, 8000, 'the shop panel')
  console.log('panels: a game graph opens, answers and is told of the ×')
  const shop = opens[0]
  check(shop.title === 'Shop' && shop.widgets.length === 3, `arriving opens the graph's panel (${shop.title}, ${shop.widgets.length} widgets)`)
  await waitFor(() => uiSets.length >= 1, 3000, 'the hidden widget')
  const said = (m) => `${m.what} ${m.what === 'decor' ? m.decor : m.widgetId}=${m.value}`
  check(uiSets.length === 1 && said(uiSets[0]) === 'shown 3=0' && uiSets[0].uiId === shop.uiId,
        `Sell, declared hidden, is hidden as the panel opens (${uiSets.map(said).join(', ')})`)
  await cw.write(encodeUiEvent(shop.uiId, 2, 1))
  await waitFor(() => hud.bought !== undefined, 5000, 'the press answered')
  check(hud.bought === 1, 'pressing Buy reaches when_panel_used by its name, over the wire')
  await waitFor(() => uiSets.length >= 4, 3000, 'the shows and hides')
  check(uiSets.slice(1).map(said).join(', ') === 'shown 2=0, shown 3=1, decor sign=0',
        `and the graph hides Buy, shows Sell and hides the sign decor, over the wire (${uiSets.slice(1).map(said).join(', ')})`)
  await cw.write(encodeUiEvent(shop.uiId, 0, 0))
  await waitFor(() => hud.closed !== undefined, 5000, 'the × answered')
  check(hud.closed === 1, 'the × (widget 0) reaches when_panel_closed')
  delete hud.bought
  await cw.write(encodeUiEvent(shop.uiId, 2, 1))
  await sleep(600)
  check(hud.bought === undefined, 'CONTROL: a press after the × is dropped -- the server forgot the panel')
} catch (e) {
  check(false, `the run finished (${e.message})`)
  console.log(log.split('\n').slice(-15).join('\n'))
} finally {
  try { wt?.close() } catch {}
  try { await edge.stop() } catch {}
  server.kill('SIGKILL')
  rmSync(mod, { recursive: true, force: true })
}
console.log(`\npanels: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
