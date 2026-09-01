//
// Does the packaged app actually start?
//
// WHY THIS EXISTS. app/test/local_transport.test.js deliberately needs no
// Electron: it stubs the IPC pipe and drives everything either side of it,
// which is the right seam for testing the transport. But it means nothing in
// the suite ever LOADED app/main.js under Electron -- and the first time
// anybody did, it failed on its very first line, three different ways in a row:
//
//   import { app } from 'electron'        SyntaxError: no export 'BrowserWindow'
//   import electron from 'electron'       TypeError: electron.app is undefined
//   require('electron')                   Error: Cannot find module 'electron'
//
// All three were one cause -- ELECTRON_RUN_AS_NODE was set in the environment,
// so the Electron binary was running as plain Node with no app, no
// BrowserWindow and no builtin module at all. The original import was correct
// the whole time. Two of those "fixes" made working code worse before the
// environment variable was found.
//
// So this test asserts the two things that failure would have caught:
// that main.js loads under a REAL Electron, and that the app reaches a joined
// session. It also fails loudly if it is asked to run as node, rather than
// reporting a mystery.
//
// OPT-IN, like editor/test/page.test.js. Launching Electron is a window, a GPU
// process and a ~150 MB dependency, which is not something a suite should
// require of everybody. DNP_APP_LAUNCH_TEST=1 includes it.
//
import { execFileSync, spawn } from 'node:child_process'
import { existsSync, rmSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
const OUT_REL = 'build/app-launch-test'
const APP = join(ROOT, OUT_REL, 'doom2')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

if (process.env.DNP_APP_LAUNCH_TEST !== '1') {
  console.log('launch: skipped (set DNP_APP_LAUNCH_TEST=1 to launch Electron)')
  process.exit(0)
}

console.log('launch: the packaged app, under a real Electron')

rmSync(join(ROOT, OUT_REL), { recursive: true, force: true })
execFileSync(join(ROOT, 'scripts', 'build_app.sh'),
             ['--module', 'doom2', '--out', OUT_REL],
             { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })

// The staged app declares electron as a devDependency; installing it here would
// be a 150 MB download inside a test. Reuse the one the developer already has.
let electronBin = null
for (const candidate of [
  join(ROOT, 'build', 'app', 'doom2', 'node_modules', 'electron'),
  join(APP, 'node_modules', 'electron')
]) {
  const pathFile = join(candidate, 'path.txt')
  if (!existsSync(pathFile)) continue
  const rel = (await import('node:fs')).readFileSync(pathFile, 'utf8').trim()
  const bin = join(candidate, 'dist', rel)
  if (existsSync(bin)) { electronBin = bin; break }
}

if (!electronBin) {
  console.log('launch: skipped (no Electron installed --')
  console.log('        run: cd build/app/doom2 && npm install)')
  process.exit(0)
}

// ELECTRON_RUN_AS_NODE makes the binary behave as plain Node: no app, no
// BrowserWindow, no builtin electron module. Everything below would fail in
// ways that all point at the import instead of at the environment. Stripped
// rather than tolerated, and named here so the next person does not spend the
// afternoon the last one did.
const env = { ...process.env }
delete env.ELECTRON_RUN_AS_NODE

const proc = spawn(electronBin, ['.'], { cwd: APP, env, stdio: ['ignore', 'pipe', 'pipe'] })
let out = ''
proc.stdout.on('data', (d) => { out += d.toString() })
proc.stderr.on('data', (d) => { out += d.toString() })

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const deadline = Date.now() + 45000
while (Date.now() < deadline && !out.includes('session 1 -> level')) {
  if (proc.exitCode !== null) break
  await sleep(200)
}
proc.kill()
await sleep(500)

// main.js loaded at all. Any of the three import failures dies here, before a
// line of our own code, so the absence of THIS line is the whole signal.
check(out.includes('[app] server:'),
      'app/main.js loads under Electron and starts the game server')
check(/\[app\] static http:\/\/127\.0\.0\.1:\d+\//.test(out),
      'the static server binds a loopback port')
check(out.includes('[app] upstream connected') && out.includes('edge connected'),
      'the relay attaches to the server as its edge')

// The one that means "this is a game": a window loaded the page, the page chose
// the local transport, said HELLO, and the server put it in a level.
check(/session 1 -> level \d+ agent \d+/.test(out),
      'and a window joins a level with an agent')

if (fails) {
  console.log('\n--- what the app printed ---')
  console.log(out.split('\n').filter((l) => l.startsWith('[app]') || l.includes('Error')).join('\n'))
}

rmSync(join(ROOT, OUT_REL), { recursive: true, force: true })

console.log(`\nlaunch: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
