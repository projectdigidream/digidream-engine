//
// The desktop transport, against a real server -- and against the STAGED app,
// not the source tree.
//
// WHAT THIS IS FOR. The packaged game replaces WebTransport with an IPC bridge
// (client/src/net_local.js, app/relay.js). That is a new path carrying the same
// protocol, and "the same protocol" is a claim, not a fact: the edge does
// session bookkeeping, control-message framing and per-channel routing, and a
// relay that got any of it subtly wrong would still connect and still draw a
// world -- it would just be missing messages nobody thought to look for.
//
// WHY IT DOES NOT NEED ELECTRON. What Electron adds is ipcRenderer/ipcMain: a
// message pipe. Everything that can actually be wrong -- the link framing, the
// session id, whether a control message arrives whole, whether datagrams reach
// the right decoder -- is on this side of that pipe, so the pipe is stubbed and
// the rest is real. Same reasoning as server/tests/test_lua_shootrange.cpp,
// which stands up the real bindings and the real World and fakes only the map.
//
// AND WHY IT RUNS THE STAGED TREE. scripts/build_app.sh copies framing.js and
// upstream.js next to relay.js, the way mapc copies a map's scripts next to the
// baked map. Testing the sources would leave the copy -- the thing that
// actually ships -- unexercised, and a staging script that forgot a file would
// pass.
//
import { execFileSync, spawn } from 'node:child_process'
import { createServer } from 'node:net'
import { existsSync, rmSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
const OUT_REL = 'build/app-transport-test'
const APP = join(ROOT, OUT_REL, 'doom2')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, what) => {
  const end = Date.now() + ms
  while (Date.now() < end) { if (fn()) return true; await sleep(20) }
  throw new Error(`timeout: ${what}`)
}

const freePort = () => new Promise((resolve, reject) => {
  const s = createServer()
  s.on('error', reject)
  s.listen(0, '127.0.0.1', () => { const { port } = s.address(); s.close(() => resolve(port)) })
})

console.log('local transport: the desktop path, against a real server')

// --- stage the app ------------------------------------------------------------
rmSync(join(ROOT, OUT_REL), { recursive: true, force: true })
// doom2 rather than the smaller shootrange, and the reason is worth writing
// down: shootrange spawns Doom monsters but ships no catalogue of its own, so a
// standalone app built from it REFUSES TO START --
//
//   cannot spawn 'zombieman': the prop catalogue is empty or missing
//
// which is correct and is new information. It was always true; it was hidden
// while the engine's assets/ carried Doom's catalogue for everybody. A module
// that names props it does not carry is not self-contained, and an app is
// exactly where that stops being survivable. doom2 IS self-contained, so it is
// what this runs.
execFileSync(join(ROOT, 'scripts', 'build_app.sh'),
             ['--module', 'doom2', '--out', OUT_REL],
             { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
check(existsSync(join(APP, 'app', 'relay.js')) &&
      existsSync(join(APP, 'app', 'framing.js')) &&
      existsSync(join(APP, 'app', 'upstream.js')),
      'the staged shell carries its own copy of the link framing')

// The staged relay, imported from where it will actually run.
const { Relay } = await import(join(APP, 'app', 'relay.js'))

// --- a real server ------------------------------------------------------------
const LINK_PORT = await freePort()
const server = spawn(join(APP, 'bin', 'dnp-server'),
                     ['--port', String(LINK_PORT), '--module', join(APP, 'modules', 'doom2')],
                     { cwd: APP, stdio: ['ignore', 'pipe', 'pipe'] })
let log = ''
server.stdout.on('data', (d) => { log += d.toString() })
server.stderr.on('data', (d) => { log += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })

try {
  await waitFor(() => log.includes('listening for edge'), 8000, 'server to start')
} catch (e) {
  console.error(log)
  throw e
}
check(true, 'the staged server binary starts and listens')

// --- the bridge Electron would provide ----------------------------------------
//
// The ONLY stub in this test. ipcRenderer.send/on is a message pipe; this is a
// message pipe. Everything either side of it is the shipping code.
const listeners = []
let readyResolve
const readyPromise = new Promise((r) => { readyResolve = r })
const relay = new Relay({ port: LINK_PORT, log: () => {} })

globalThis.window = {
  dnp: {
    ready: () => readyPromise,
    send: (kind, bytes) => relay.send(kind, bytes),
    onMessage: (cb) => listeners.push(cb),
    close: () => relay.stop()
  }
}

relay.start((msg) => { for (const l of listeners) l(msg) })
await waitFor(() => relay.open, 8000, 'the relay to reach the server')
readyResolve()
// waitFor, not a bare check: relay.open goes true when OUR connect() returns,
// which can happen before the server has written its line. Asserting straight
// after was a race that failed about half the time.
await waitFor(() => log.includes('edge connected'), 5000, 'the server to log the edge')
check(log.includes('edge connected'), 'and the server accepts the relay as its edge')

// --- the real client modules --------------------------------------------------
const { LocalConnection } = await import(join(ROOT, 'client', 'src', 'net_local.js'))
const { MapAccumulator } = await import(join(ROOT, 'client', 'src', 'maploader.js'))
const { MSG } = await import(join(ROOT, 'client', 'src', 'proto.js'))

let welcome = null
let closed = null
const acc = new MapAccumulator()
const snapshots = []

const conn = new LocalConnection({
  onControl (m) {
    if (m.type === MSG.WELCOME) welcome ??= m
    else if (m.type === MSG.MAP_DATA) acc.push(m)
  },
  onSnapshot (s) { snapshots.push(s) },
  onClose (e) { closed = e?.reason ?? 'closed' }
})

await conn.open()

// --- the handshake ------------------------------------------------------------
await waitFor(() => welcome !== null, 8000, 'WELCOME')
check(welcome !== null, 'WELCOME arrives over the bridge')
check(typeof welcome.serverTick === 'number',
      `carrying the server's tick and our own handle (${welcome.serverTick}, handle ${welcome.selfHandle})`)

await waitFor(() => acc.complete, 8000, 'the map')
check(acc.complete, 'and the map arrives whole, in pieces, reassembled by the real loader')
check(!acc.failed, `with no transfer failure (${acc.failed ?? 'none'})`)

// A map is thousands of bytes across many control messages. That it reassembles
// is the strongest single check here: it means every control message arrived,
// in order, complete and exactly once. A relay that split or merged frames
// would fail precisely here.
check(acc.total > 0 && acc.received === acc.total,
      `every byte of the map arrived (${acc.received}/${acc.total})`)

// --- datagrams ----------------------------------------------------------------
await waitFor(() => snapshots.length > 0, 8000, 'snapshots')
check(snapshots.length > 0, `snapshots arrive as datagrams (${snapshots.length})`)

// --- input reaches the simulation ---------------------------------------------
//
// The half that has to work in the other direction. Everything above would pass
// with a relay that never sent a byte upstream after the handshake.

const before = snapshots.length
let tick = welcome.tick ?? 0
for (let i = 0; i < 120; i++) {
  tick++
  // Full forward on one axis. 32767 is full deflection (16-bit axes).
  conn.send(tick, snapshots.length ? snapshots[snapshots.length - 1].serverTick : 0,
            [{ tick, moveX: 0, moveY: 32767, aim: 0, buttons: 0 }])
  await sleep(16)
}
check(snapshots.length > before,
      `the server kept streaming while input flowed (${snapshots.length - before} more)`)

// The server acknowledges progress by advancing its tick. If input never
// arrived the world would still tick -- so this is a liveness check, not a
// movement one; movement under prediction is server/edge/test/prediction.test.js's
// job and it needs no second implementation here.
const firstTick = snapshots[0].serverTick
const lastTick = snapshots[snapshots.length - 1].serverTick
check(lastTick > firstTick, `and the world advanced (${firstTick} -> ${lastTick})`)

// --- the server going away is REPORTED ----------------------------------------
//
// The edge learned this the hard way: a client that has genuinely connected
// sits on "waiting for WELCOME" forever when there is nothing behind the relay,
// which is an accurate message about the wrong thing.
server.kill('SIGKILL')
await waitFor(() => closed !== null, 8000, 'a close to be reported')
check(closed !== null, `losing the server is reported to the client (${closed})`)

conn.close()
relay.stop()
rmSync(join(ROOT, OUT_REL), { recursive: true, force: true })

console.log(`\nlocal transport: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
