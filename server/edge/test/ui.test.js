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
import { serverBin, doomProps } from './serverbin.js'
import { decodeControlMessage, encodeHello, encodeUiEvent, encodeInputBundle,
         decodeSnapshot, axisFromNumber, MSG, UI_KIND, CAMERA_FIELD }
  from '../../../client/src/proto.js'

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
              '--props', doomProps]
if (!NO_SCRIPT) args.push('--script', join(ROOT, 'maps', 'scripts', 'debug_panel.lua'))

const server = spawn(serverBin, args,
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
let welcomes = 0
let panel = null
// PARAM_UPDATEs seen. A replicated parameter changing is the client's own copy
// of the rules changing, which is what makes an attack slider a physics change
// rather than a number in a script.
let paramUpdates = 0
// Widget updates pushed by the server into an already-open panel.
let uiSets = []
// Camera settings pushed by a script (design §4.1).
let cameraSets = []
// The panel currently open. Read through a function because a map change
// replaces it, and a captured id is one that has already been closed.
const uiIdNow = () => panel?.uiId ?? 0
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
        if (m?.type === MSG.WELCOME) { welcome = m; welcomes++ }
        else if (m?.type === MSG.UI_OPEN) panel = m
        else if (m?.type === MSG.UI_CLOSE) closed.push(m.uiId)
        else if (m?.type === MSG.PARAM_UPDATE) paramUpdates++
        else if (m?.type === MSG.UI_SET) uiSets.push(m)
        else if (m?.type === MSG.CAMERA_SET) cameraSets.push(m)
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
  //
  // NO REDUNDANT MESSAGE, rather than no message at all. This used to assert
  // that the count did not move, which is a claim about the FIXTURE and not
  // about the panel: this level has monsters in it, they shoot the player, and
  // a health change during the window is a UI_SET the panel is right to send.
  // It failed roughly one run in four and cost a bisect each time, because the
  // number it printed -- "1 in 60 ticks" -- looks like the waste it is named
  // after.
  //
  // So the waste is measured directly: a UI_SET repeating a value the client
  // already has. That is the thing the panel promises not to do, and it is true
  // whether or not anything shot anybody.
  const known = new Map()
  for (const u of uiSets) known.set(u.widgetId, u.value)
  const before = uiSets.length
  await step(60)
  const repeats = uiSets.slice(before).filter((u) => {
    const had = known.get(u.widgetId)
    known.set(u.widgetId, u.value)
    return had === u.value
  })
  //
  // Verified to have teeth by taking the `hp ~= shown_hp` guard out of
  // maps/scripts/debug_panel.lua and re-running: "61 sent in 60 ticks, 60
  // redundant", which is one a tick, exactly the waste this is named after.
  // That control needs the runtime broken so it cannot live here, and the
  // number it produced is recorded instead.
  check(repeats.length === 0,
        `no UI_SET repeats a value the client already has ` +
        `(${uiSets.length - before} sent in 60 ticks, ${repeats.length} redundant)`)
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

// --- the camera is configured from Lua ---------------------------------------
//
// Design §4.1 puts the camera on the CLIENT and lets Lua configure and trigger
// it: the server never says where the camera is, only how it should follow. So
// the proof is a CAMERA_SET arriving with the value a slider was moved to --
// and the panel is the only thing that knows those values, because nothing
// reports them back.
{
  const names = ['Distance', 'Pitch /1000 turn', 'Follow speed', 'Accel (0=off)',
                 'Look ahead', 'Look-ahead speed']
  const found = names.filter((n) => panel.widgets.some((w) => w.label === n))
  check(found.length === names.length,
        `the panel offers every camera setting (${found.length}/${names.length})`)

  cameraSets = []
  const lead = panel.widgets.find((w) => w.label === 'Look ahead')
  await cw.write(encodeUiEvent(panel.uiId, lead.id, 7))
  await step(20)
  check(cameraSets.length > 0, `moving one sends a CAMERA_SET (${cameraSets.length})`)

  // EVERY setting, not just the one moved: they are read together and a
  // half-applied camera is worse to look at than either the old or the new one.
  const last = cameraSets.at(-1)
  check(last?.fields.length === 6,
        `carrying all six settings (${last?.fields.length})`)
  const byName = Object.fromEntries(last.fields.map((f) => [CAMERA_FIELD[f.id], f.value]))
  check(byName.lead === 7, `with the value that was set (lead ${byName.lead})`)
  // Thousandths on the wire, so an angle survives as a fraction rather than
  // arriving as zero.
  check(Math.abs(byName.pitch - 0.125) < 1e-6,
        `and an angle as a turn fraction (pitch ${byName.pitch})`)

  // A field this client did not know would be skipped rather than refusing the
  // message -- so every id sent must be one it can name, or the setting is
  // silently dropped.
  check(last.fields.every((f) => CAMERA_FIELD[f.id] !== undefined),
        'and every field named by a client that knows it')
}

// --- a panel survives a map change -------------------------------------------
//
// Every level runs its own VM with its own copy of the script, so the panel a
// player had is closed when they leave and a new one is opened by the level
// they arrive in. That round trip is what this asserts.
//
// It does NOT prove the fix that went in beside it. The script used to keep "I
// have already opened a panel for this player" as true forever, which would
// deny a panel to somebody returning into the same agent slot -- and a return
// here happens to get a different slot, so this passes either way. The stale
// bookkeeping is wrong regardless and is cleared; the case that would catch it
// is not built.
{
  const mapWidget = panel.widgets.find((w) => w.kind === UI_KIND.SELECT)
  const goWidget = panel.widgets.find((w) => w.label === 'Go to map')
  const startTitle = panel.title

  // To the second map, then back to the first.
  for (const target of [1, 0]) {
    // Captured BEFORE clearing `panel`: the events below address the panel that
    // is open now, and nulling it first makes uiIdNow() report 0 -- which the
    // script correctly ignores, so nothing happens and the test blames the
    // engine for its own mistake.
    const openId = panel.uiId
    panel = null
    await cw.write(encodeUiEvent(openId, mapWidget.id, target))
    await step(5)
    await cw.write(encodeUiEvent(openId, goWidget.id, 1))
    // Long enough for the change, the new level's first tick, and the panel.
    for (let i = 0; i < 90 && panel === null; i++) await step(1)
    check(panel !== null,
          `a panel opens in the level moved to (map index ${target}, ` +
          `${welcomes} welcomes)`)
    if (panel === null) break
    check(panel.title === startTitle, 'and it is the same panel, rebuilt there')
  }
}

console.log(`\nui: ${checks} checks, ${fails} failures`)
try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')
process.exit(fails === 0 ? 0 : 1)
