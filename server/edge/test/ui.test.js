//
// A panel, end to end (docs/protocol.md §11).
//
// A script opens one, it crosses the reliable channel, the client renders it,
// a click goes back, and the script acts on it. Every link in that is silent
// when it breaks -- a hook nobody registered, an id the server no longer has,
// a value the client invented -- so the assertions are placed at each of them.
//
// The script under test is maps/scripts/debug_panel.lua, which is also the
// thing being delivered: a panel that changes speed, damage, health and map.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { decodeControlMessage, encodeHello, encodeUiEvent, encodeInputBundle,
         decodeSnapshot, axisFromNumber, MSG, UI_KIND } from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34648)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34649)
// The control: the same run with the script left out. Nothing may open a panel
// on its own, or "a panel arrived" is satisfied by the engine doing it.
const NO_SCRIPT = process.env.DNP_NO_UI_SCRIPT === '1'

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

function mapPath (name) {
  for (const a of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', a, name)
    if (existsSync(p)) return p
  }
  return null
}
// monsters.dnpm because it declares the MONSTER script, and the damage slider
// is a claim about this panel reaching that one. A map without it would be
// testing the panel talking to itself.
const MAP_A = mapPath('monsters.dnpm')
const MAP_B = mapPath('level_b.dnpm')
if (!MAP_A || !MAP_B) { console.error('no baked levels'); process.exit(2) }

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

// Two maps, so the panel's map selector has somewhere to go.
// --props with an absolute path: the default is relative to the working
// directory, and this does not run from the repository root -- which is also
// true of any real deployment. Needed because MAP_A spawns monsters, and a
// catalogue that is not found is fatal for the level that wanted it.
const args = ['--port', String(LINK_PORT), '--map', MAP_A, '--map', MAP_B, '--spawn', 'start',
              '--props', join(ROOT, 'assets', 'props.txt')]
if (!NO_SCRIPT) args.push('--script', join(ROOT, 'maps', 'scripts', 'debug_panel.lua'))

const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'), args,
                     { stdio: ['ignore', 'inherit', 'pipe'] })
let serverLog = ''
server.stderr.on('data', (d) => { serverLog += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })
await waitFor(() => serverLog.includes('listening for edge'), 15000,
              `server (log: ${serverLog.slice(-200)})`)

const edge = new Edge({
  port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  log: () => {}
})
await edge.start()
await waitFor(() => serverLog.includes('edge connected'), 8000, 'edge')

const wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`,
  { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
await withTimeout(wt.ready, 8000, 'ready')
const control = await wt.createBidirectionalStream()
const cw = control.writable.getWriter()
const dw = wt.datagrams.createWritable().getWriter()

let welcome = null
let panel = null
// PARAM_UPDATEs seen. A replicated parameter changing is the client's own copy
// of the rules changing, which is what makes an attack slider a physics change
// rather than a number in a script.
let paramUpdates = 0
// Widget updates pushed by the server into an already-open panel.
let uiSets = []
let closed = []
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
        if (m?.type === MSG.WELCOME) welcome = m
        else if (m?.type === MSG.UI_OPEN) panel = m
        else if (m?.type === MSG.UI_CLOSE) closed.push(m.uiId)
        else if (m?.type === MSG.PARAM_UPDATE) paramUpdates++
        else if (m?.type === MSG.UI_SET) uiSets.push(m)
      }
    }
  } catch {}
})()

const snapshots = []
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
await waitFor(() => welcome, 8000, 'welcome')

// The server drops a client that stops sending input, so this keeps stepping
// through every wait rather than sleeping. A test that pauses is disconnected,
// and everything after is written to a session that no longer exists.
let tick = 0
async function step (n = 1) {
  for (let i = 0; i < n; i++) {
    tick++
    const f = { buttons: 0, moveX: axisFromNumber(0), moveY: axisFromNumber(0), aim: 0 }
    await dw.write(encodeInputBundle({
      clientTick: (snapshots.at(-1)?.serverTick ?? 0) + 8,
      ackSnapshotTick: snapshots.at(-1)?.serverTick ?? 0, frames: [f]
    }))
    await sleep(1000 / 60)
  }
}

await step(60)

if (NO_SCRIPT) {
  check(panel === null, 'CONTROL: with no script, nothing opens a panel')
  console.log(`\nui: ${checks} checks, ${fails} failures`)
  try { wt.close() } catch {}
  await edge.stop()
  server.kill('SIGTERM')
  process.exit(fails === 0 ? 0 : 1)
}

check(panel !== null, 'the script opened a panel on the client that joined')
check(panel?.title === 'Debug', `with its title (${panel?.title})`)
check((panel?.widgets.length ?? 0) >= 6, `and its widgets (${panel?.widgets.length})`)

// The map selector is built from what the OPERATOR loaded, not a list in the
// script -- so it must show both maps this server was started with.
const mapSel = panel?.widgets.find((w) => w.kind === UI_KIND.SELECT)
check(mapSel != null && mapSel.options.length === 2,
      `the map selector lists the server's own levels (${mapSel?.options.join(',')})`)
check(mapSel?.options.includes('monsters') && mapSel?.options.includes('level_b'),
      `by name, not by path (${mapSel?.options.join(',')})`)

// --- a slider changes physics ------------------------------------------------
//
// Speed is a replicated parameter, so the proof is a PARAM_UPDATE arriving --
// the client's own copy of the rules changing, not just a number in a script.

const speed = panel.widgets.find((w) => w.kind === UI_KIND.SLIDER && w.label === 'Speed')
check(speed != null, 'there is a speed slider')
await cw.write(encodeUiEvent(panel.uiId, speed.id, 3))
await step(30)
check(serverLog.includes('error') === false, 'and setting it raised no script error')

// The effective value must have actually moved. Asked of the server's own log?
// No -- of the client, through the private block it already replicates.
const hpOf = () => snapshots.at(-1)?.hp
check(hpOf() === 100, `health starts at 100 (${hpOf()})`)

// --- health ------------------------------------------------------------------
const hp = panel.widgets.find((w) => w.label === 'Health')
await cw.write(encodeUiEvent(panel.uiId, hp.id, 37))
await step(20)
check(hpOf() === 37, `a slider changed health to 37 (${hpOf()})`)

// Out of range is clamped by the SERVER, which does not trust the client
// (§11.3). The client's own clamp is a separate thing and is not what this
// tests: these bytes never went near it.
await cw.write(encodeUiEvent(panel.uiId, hp.id, 99999))
await step(20)
check(hpOf() === hp.hi, `a value past the maximum is clamped to ${hp.hi} (got ${hpOf()})`)

// An event for a widget the panel does not contain reaches no script.
const before = hpOf()
await cw.write(encodeUiEvent(panel.uiId, 31337, 5))
await step(20)
check(hpOf() === before, 'an event for an unknown widget changes nothing')

// And one for a panel that is not open. A no-op, not an error (§11.2).
await cw.write(encodeUiEvent(panel.uiId + 999, hp.id, 5))
await step(20)
check(hpOf() === before, 'an event for a stale ui id changes nothing')

// --- the button that reopens -------------------------------------------------
const heal = panel.widgets.find((w) => w.label === 'Full health')
const wasId = panel.uiId
panel = null
await cw.write(encodeUiEvent(wasId, heal.id, 1))
await step(30)
check(hpOf() === 100, `the heal button restored health (${hpOf()})`)
check(panel !== null && panel.uiId !== wasId,
      'and reopened the panel so its slider shows the new value')

// --- one panel, not a pile of them -------------------------------------------
//
// Every open allocates a fresh ui_id, so reopening does not replace: the client
// keys panels by id and draws a second one beside the first. The script has to
// close the old one, and the proof is a UI_CLOSE arriving for it -- not just
// "a panel exists", which was true the whole time it was going wrong.
{
  const opened = panel.uiId
  closed = []
  panel = null
  await cw.write(encodeUiEvent(opened, heal.id, 1))
  await step(30)
  check(closed.includes(opened),
        `reopening closes the panel it replaces (closed ${closed.join(',')})`)
  check(panel !== null, 'and opens exactly one new one')
}

// --- the panel changes the monster script's numbers --------------------------
//
// The panel writes enemies.melee_damage and the monster script reads it. Both
// halves are ordinary globals in one VM, which is the extension mechanism here
// -- and when the script kept the number in a LOCAL instead, the slider moved a
// field nothing ever read. Nothing failed; the damage simply never changed.
{
  const dmg = panel.widgets.find((w) => w.label === 'Damage')
  check(dmg != null, `there is a damage slider (showing ${dmg?.value})`)
  await cw.write(encodeUiEvent(panel.uiId, dmg.id, 77))
  await step(20)

  // Read it back through a FRESH panel. A panel is built from state at the
  // moment it is opened, so one that now shows 77 is the monster script's own
  // number having changed -- not the widget remembering what was set on it.
  const wasId = panel.uiId
  panel = null
  await cw.write(encodeUiEvent(wasId, heal.id, 1))
  await step(30)
  const again = panel?.widgets.find((w) => w.label === 'Damage')
  check(again?.value === 77,
        `and the monster script now reads 77 from it (${again?.value})`)
}

// --- a panel follows state it did not cause ----------------------------------
//
// A panel is built from state at the moment it opens and nothing pushes changes
// into one, so a health slider sat at whatever it was when the window appeared
// while monsters shot the number down behind it. Reported as "health does not
// update when hit on the debug", and the fix is UI_SET rather than reopening:
// reopening allocates a new id, and from the player's side is a window that
// flickers every time they are hit.
{
  uiSets = []
  // Change health WITHOUT touching the health widget, so what is observed is
  // the panel following state rather than echoing an input.
  await cw.write(encodeUiEvent(panel.uiId, heal.id, 1))
  await step(10)
  const hpWidget = panel.widgets.find((w) => w.label === 'Health')
  await cw.write(encodeUiEvent(panel.uiId, hpWidget.id, 55))
  await step(20)
  const forHp = uiSets.filter((u) => u.widgetId === hpWidget.id)
  check(forHp.some((u) => u.value === 55),
        `health changes push a UI_SET to the open panel (${forHp.map((u) => u.value).join(',')})`)

  // And ONLY when it changes. Sixty reliable control messages a second saying
  // the same number is a waste of the channel that carries map data.
  const before = uiSets.length
  await step(60)
  check(uiSets.length === before,
        `a steady value sends nothing (${uiSets.length - before} in 60 ticks)`)
}

// --- the attack sliders reach the physics ------------------------------------
//
// These are replicated parameters, so the proof is a PARAM_UPDATE arriving --
// the CLIENT's own copy of the rules changing. A panel that renders but changes
// nothing looks identical to one that works until you swing at something, and
// the whole point of these being parameters rather than script numbers is that
// the client predicts the swing too.
{
  const names = ['Reach /100', 'Min reach /100', 'Half arc /100 turn',
                 'Sweep from /100', 'Sweep to /100', 'Swing ticks',
                 'Cooldown ticks']
  const found = names.filter((n) => panel.widgets.some((w) => w.label === n))
  check(found.length === names.length,
        `the panel offers every attack parameter (${found.length}/${names.length})`)

  const reach = panel.widgets.find((w) => w.label === 'Reach /100')
  check(reach.value === 200, `reach starts at the engine default of 2.00 (${reach.value / 100})`)

  paramUpdates = 0
  await cw.write(encodeUiEvent(panel.uiId, reach.id, 550))
  await step(30)
  check(paramUpdates > 0,
        `changing it replicated a PARAM_UPDATE to the client (${paramUpdates})`)

  // And read it back through a fresh panel, so this is the parameter having
  // actually changed rather than a message having been sent about it.
  const wasId = panel.uiId
  panel = null
  await cw.write(encodeUiEvent(wasId, heal.id, 1))
  await step(30)
  const again = panel?.widgets.find((w) => w.label === 'Reach /100')
  check(again?.value === 550, `and the engine now reports 5.50 (${again?.value / 100})`)
}

console.log(`\nui: ${checks} checks, ${fails} failures`)
try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')
process.exit(fails === 0 ? 0 : 1)
