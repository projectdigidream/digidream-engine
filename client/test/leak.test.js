//
// Does the GAME CLIENT leak?
//
// Asked of Chrome the way editor/test/leak.test.js asks it: one page, a
// repeated stimulus, a forced GC and a metrics read after each round. A leak
// grows every round; churn plateaus.
//
// The stimulus is a round of real play against a real stack: twelve seconds on
// DOOM II's MAP01 -- walking, firing, monsters replicated, snapshots and sounds
// arriving, the music and the sound engine running -- and then a NEW GAME,
// which hands the client a fresh world: the map view torn down and rebuilt,
// every entity dropped, the panels closed and opened again. That rebuild is the
// path where the editor once stranded a texture set per open, so the GPU's own
// count of live geometries and textures is measured alongside the JS heap, the
// retained DOM nodes and the listeners.
//
// DNP_LEAK_CONTROL=1 is the CONTROL: each round also keeps a hundred thousand
// small objects (several megabytes of JS heap) the page never lets go of, and
// the heap check must catch it -- a test that cannot fail measures nothing.
//
// OPT-IN, like the other browser tests (DNP_CLIENT_PAGE_TEST=1): one Chrome
// and a whole stack, for a couple of minutes.
//
import { spawn } from 'node:child_process'
import { mkdtempSync, rmSync, existsSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
const HTTP = Number(process.env.DNP_LEAK_HTTP_PORT ?? 8191)
const WT = Number(process.env.DNP_LEAK_WT_PORT ?? 34733)
const LINK = Number(process.env.DNP_LEAK_LINK_PORT ?? 34700)
const CDP = 9700 + Math.floor(Math.random() * 200)
const ROUNDS = Number(process.env.DNP_LEAK_ROUNDS ?? 10)
const CONTROL = process.env.DNP_LEAK_CONTROL === '1'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

if (!process.env.DNP_CLIENT_PAGE_TEST) {
  console.log('  ..   leak test skipped: a browser and a stack, minutes long. DNP_CLIENT_PAGE_TEST=1 runs it.')
  console.log('\nclientleak: 0 checks, 0 failures (skipped)')
  process.exit(0)
}
const CHROMES = [
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser'
]
const chromeBin = CHROMES.find((p) => existsSync(p))
if (!chromeBin) {
  console.log('  ..   no Chrome found; the leak test needs one and cannot run')
  console.log('\nclientleak: 0 checks, 0 failures (skipped)')
  process.exit(0)
}

// --- the stack ----------------------------------------------------------------
const maps = ['map01.dnpm', 'map02.dnpm'].map((m) => join('modules', 'doom2', 'maps', m))
const stack = spawn('bash', [join(ROOT, 'scripts', 'run_stack.sh'), '--module', 'doom2',
                             ...maps.flatMap((m) => ['--map', m]), '--persist', 'none'], {
  cwd: ROOT,
  env: { ...process.env, DNP_HTTP_PORT: String(HTTP), DNP_WT_PORT: String(WT), DNP_LINK_PORT: String(LINK) },
  stdio: ['ignore', 'pipe', 'pipe'],
  detached: true
})
let stackLog = ''
stack.stdout.on('data', (d) => { stackLog += d.toString() })
stack.stderr.on('data', (d) => { stackLog += d.toString() })
const stopStack = () => { try { process.kill(-stack.pid, 'SIGTERM') } catch {} }
process.on('exit', stopStack)
for (let i = 0; i < 300 && !stackLog.includes('edge connected'); i++) await sleep(200)
for (let i = 0; i < 100; i++) {
  try { if ((await fetch(`http://127.0.0.1:${HTTP}/`)).ok) break } catch { await sleep(150) }
}

// --- one Chrome, driven over its debugging protocol ---------------------------
const profile = mkdtempSync(join(tmpdir(), 'dnp-clientleak-'))
const chrome = spawn(chromeBin, [
  '--headless=new', '--no-sandbox', '--enable-unsafe-swiftshader',
  '--autoplay-policy=no-user-gesture-required',
  `--remote-debugging-port=${CDP}`, `--user-data-dir=${profile}`, '--window-size=1100,800', 'about:blank'
], { stdio: 'ignore' })

let target = null
for (let i = 0; i < 100 && !target; i++) {
  try { target = (await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).find((t) => t.type === 'page') } catch { await sleep(200) }
}
const ws = target && new WebSocket(target.webSocketDebuggerUrl)
if (ws) await new Promise((r, j) => { ws.onopen = r; ws.onerror = j })
let seq = 0
const pending = new Map()
const consoleLines = []
if (ws) {
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data)
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return }
    if (m.method === 'Runtime.consoleAPICalled') consoleLines.push(m.params.args.map((a) => a.value ?? '').join(' '))
  }
}
const send = (method, params = {}) => new Promise((resolve) => {
  const id = ++seq
  pending.set(id, resolve)
  ws.send(JSON.stringify({ id, method, params }))
})
const evaluate = async (expression) => {
  const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true })
  if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description ?? 'evaluate failed')
  return r.result?.result?.value
}
const key = async (code, keyName) => {
  await send('Input.dispatchKeyEvent', { type: 'keyDown', code, key: keyName })
  await sleep(40)
  await send('Input.dispatchKeyEvent', { type: 'keyUp', code, key: keyName })
}
const metrics = async () => {
  await send('HeapProfiler.collectGarbage')
  await sleep(300)
  const r = await send('Performance.getMetrics')
  const m = Object.fromEntries(r.result.metrics.map((x) => [x.name, x.value]))
  const gpu = await evaluate('window.__dnpStats?.gpu?.()') ?? {}
  const wasm = await evaluate('window.__dnpStats?.wasmBytes?.() ?? 0')
  return {
    nodes: m.Nodes, listeners: m.JSEventListeners, heapMB: m.JSHeapUsedSize / 1048576,
    geometries: gpu.geometries ?? 0, textures: gpu.textures ?? 0, wasmMB: wasm / 1048576
  }
}
// Least squares over the later rows, per round.
const slope = (rows, f) => {
  const ys = rows.map(f)
  const xs = ys.map((_, i) => i)
  const mx = xs.reduce((a, b) => a + b, 0) / xs.length
  const my = ys.reduce((a, b) => a + b, 0) / ys.length
  return xs.reduce((s, x, i) => s + (x - mx) * (ys[i] - my), 0) /
         Math.max(1e-9, xs.reduce((s, x) => s + (x - mx) * (x - mx), 0))
}

try {
  if (!ws) {
    check(false, 'chrome came up with a debuggable page')
  } else {
    await send('Runtime.enable'); await send('Performance.enable'); await send('HeapProfiler.enable')
    await send('Page.navigate', { url: `http://localhost:${HTTP}/?server=https://127.0.0.1:${WT}/dnp&autowalk=shuttle&touch=0` })
    for (let i = 0; i < 400; i++) {
      try { if (await evaluate('(window.__dnpStats?.snapshots ?? 0) > 30')) break } catch { /* not yet */ }
      await sleep(250)
    }
    const snaps0 = await evaluate('window.__dnpStats?.snapshots ?? 0')
    check(snaps0 > 30, `the page joined the server and is receiving snapshots (${snaps0})`)

    const rows = []
    let census = await evaluate('window.__dnpStats?.scene?.()') ?? {}
    for (let r = 0; r < ROUNDS; r++) {
      // Twelve seconds of play: walking (autowalk) and firing, monsters
      // replicated, sounds arriving.
      const end = Date.now() + 12000
      while (Date.now() < end) {
        await send('Input.dispatchKeyEvent', { type: 'keyDown', code: 'KeyF', key: 'f' })
        await sleep(120)
        await send('Input.dispatchKeyEvent', { type: 'keyUp', code: 'KeyF', key: 'f' })
        await sleep(600)
      }
      // Objects, not a typed array: a typed array's bytes are EXTERNAL memory,
      // which the JS heap figure never counts -- the first version of this
      // control kept 2 MB a round of it and was not caught.
      if (CONTROL) await evaluate('(window.__kept = window.__kept || []).push(Array.from({ length: 100000 }, (_, i) => ({ i, s: "x" + i })))')
      // A NEW GAME: F2, then F2 again (client/src/main.js). A fresh world.
      const w0 = await evaluate('window.__dnpStats.welcomes')
      await key('F2', 'F2'); await sleep(300); await key('F2', 'F2')
      for (let i = 0; i < 80; i++) {
        if ((await evaluate('window.__dnpStats.welcomes')) > w0) break
        await sleep(100)
      }
      await sleep(2500)
      rows.push(await metrics())
      // What changed in the scene since the last round, by type and name: the
      // first place to look when the GPU counts climb.
      const now = await evaluate('window.__dnpStats?.scene?.()') ?? {}
      const diff = Object.keys({ ...census, ...now })
        .filter((k) => (now[k] ?? 0) !== (census[k] ?? 0))
        .map((k) => `${k} ${census[k] ?? 0}->${now[k] ?? 0}`)
      if (diff.length) console.log(`         scene: ${diff.join(', ')}`)
      census = now
      const m = rows.at(-1)
      console.log(`  round ${r + 1}: heap ${m.heapMB.toFixed(1)} MB, wasm ${m.wasmMB.toFixed(0)} MB, nodes ${m.nodes}, ` +
                  `listeners ${m.listeners}, geometries ${m.geometries}, textures ${m.textures}`)
    }
    const welcomes = await evaluate('window.__dnpStats.welcomes')
    check(welcomes >= ROUNDS + 1, `every round started a new game: a fresh world each time (${welcomes} welcomes)`)
    const sfx = await evaluate('JSON.stringify({ ready: window.__sfx?.ready, played: window.__sfx?.played })')
    console.log(`  sound engine: ${sfx}`)

    // THE LATER HALF is judged. The early rounds pay for what is loaded on
    // demand -- a monster's sprite frames the first time one comes into view --
    // which climbs and then stops; a leak keeps climbing.
    const later = rows.slice(Math.floor(rows.length / 2))
    const g = {
      heap: slope(later, (m) => m.heapMB), nodes: slope(later, (m) => m.nodes),
      listeners: slope(later, (m) => m.listeners),
      geometries: slope(later, (m) => m.geometries), textures: slope(later, (m) => m.textures),
      wasm: slope(later, (m) => m.wasmMB)
    }
    console.log(`       per round: heap ${g.heap.toFixed(2)} MB, wasm ${g.wasm.toFixed(2)} MB, nodes ${g.nodes.toFixed(1)}, ` +
                `listeners ${g.listeners.toFixed(1)}, geometries ${g.geometries.toFixed(1)}, textures ${g.textures.toFixed(1)}`)
    if (CONTROL) {
      check(g.heap >= 1.5, `CONTROL: a hundred thousand objects kept per round are caught (${g.heap.toFixed(2)} MB per round)`)
    } else {
      // THE WASM HEAP: a world and its map left behind at every new game were
      // 14 MB a round, until enterLevel destroyed them.
      check(g.wasm < 1, `the wasm heap does not grow across new games (${g.wasm.toFixed(2)} MB per round)`)
      // EXACT for geometry: the body's capsule left on the GPU at every new
      // world was precisely one a round, which any looser bound waves through.
      check(g.geometries < 0.5, `GPU geometries do not grow across new games (${g.geometries.toFixed(1)} per round)`)
      check(g.textures < 1, `GPU textures do not grow once loaded (${g.textures.toFixed(1)} per round)`)
      check(g.nodes < 50, `retained DOM nodes do not grow (${g.nodes.toFixed(1)} per round)`)
      check(g.listeners < 5, `retained listeners do not grow (${g.listeners.toFixed(1)} per round)`)
      check(g.heap < 1.0, `the JS heap settles (${g.heap.toFixed(2)} MB per round, bound 1.0)`)
    }
  }
} catch (e) {
  check(false, `the run finished (${e.message})`)
  console.log(consoleLines.slice(-15).join('\n'))
} finally {
  try { ws?.close() } catch {}
  try { chrome.kill('SIGKILL') } catch {}
  stopStack()
  await sleep(500)
  rmSync(profile, { recursive: true, force: true })
}

console.log(`\nclientleak: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
