//
// THE UI LAYOUT ROUTES (docs/UI_LAYOUT.md 3.1), in the app's static server and
// the devserver: /ui/index.json merged across two modules and the engine, and
// /ui/<file> looked up per file -- by the SAME precedence, which is the point.
// An index naming hud.json must be served the hud.json of the module that named
// it, or a module's HUD is drawn from another module's file.
//
// House style: standalone, prints ok/FAIL, ends with a count.
//
import { spawn } from 'node:child_process'
import { mkdirSync, writeFileSync, rmSync, mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, relative } from 'node:path'
import { fileURLToPath } from 'node:url'
import { startStatic } from '../static.js'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// A tree with the engine and two modules, `first` loaded before `second`.
const T = mkdtempSync(join(tmpdir(), 'dnp-ui-'))
const put = (rel, body) => {
  mkdirSync(dirname(join(T, rel)), { recursive: true })
  writeFileSync(join(T, rel), typeof body === 'string' ? body : JSON.stringify(body))
}
put('assets/ui/index.json', { version: 1, default: 'engine.json', maps: { MAP01: 'engine1.json', E1M1: 'e1.json' } })
put('modules/first/assets/ui/index.json', { version: 1, default: 'hud.json', maps: { MAP07: 'boss.json' } })
put('modules/first/assets/ui/hud.json', { _about: 'first' })
put('modules/second/assets/ui/index.json', { version: 1, default: 'second.json', maps: { MAP07: 'other.json', MAP01: 'two.json' } })
put('modules/second/assets/ui/hud.json', { _about: 'second' })
put('modules/second/assets/ui/other.json', { _about: 'second other' })
put('modules/broken/assets/ui/index.json', '{ not json')

function expectations (label, get) {
  return (async () => {
    const index = await (await get('/ui/index.json')).json()
    check(index.default === 'hud.json', `${label}: the first module's default wins (${index.default})`)
    check(index.maps.MAP07 === 'boss.json', `${label}: and its map entry over the second's (${index.maps.MAP07})`)
    check(index.maps.MAP01 === 'two.json', `${label}: a module's entry wins over the engine's (${index.maps.MAP01})`)
    check(index.maps.E1M1 === 'e1.json', `${label}: entries nobody overrode are kept -- a union`)
    const hud = await (await get('/ui/hud.json')).json()
    check(hud._about === 'first', `${label}: the default it names is served from the SAME module (${hud._about})`)
    const other = await get('/ui/other.json')
    check(other.ok && (await other.json())._about === 'second other', `${label}: a file only a later module has is still found`)
    check((await get('/ui/nothing.json')).status === 404, `${label}: CONTROL: a file nobody has is a 404`)
  })()
}

// --- the app ------------------------------------------------------------------
{
  const { server, port } = await startStatic({ root: T, modules: ['broken', 'first', 'second'] })
  try {
    await expectations('app', (p) => fetch(`http://127.0.0.1:${port}${p}`))
  } finally { server.close() }
  const solo = await startStatic({ root: T, modules: [] })
  try {
    const index = await (await fetch(`http://127.0.0.1:${solo.port}/ui/index.json`)).json()
    check(index.default === 'engine.json', 'app: CONTROL: with no modules the engine\'s own index is served')
  } finally { solo.server.close() }
}

// --- the devserver --------------------------------------------------------------
//
// It serves THIS repository's root, so the tree is reached as modules by path.
{
  const port = 18000 + Math.floor(Math.random() * 1000)
  // Module paths are taken relative to the repository root.
  const mod = (n) => relative(ROOT, join(T, 'modules', n))
  const dev = spawn(process.execPath, [join(ROOT, 'tools', 'devserver.js'),
    '--module', mod('broken'), '--module', mod('first'), '--module', mod('second')],
  { cwd: ROOT, env: { ...process.env, DNP_HTTP_PORT: String(port) }, stdio: ['ignore', 'pipe', 'pipe'] })
  let log = ''
  dev.stdout.on('data', (d) => { log += d })
  dev.stderr.on('data', (d) => { log += d })
  try {
    let up = false
    for (let i = 0; i < 100 && !up; i++) {
      await sleep(50)
      up = await fetch(`http://127.0.0.1:${port}/ui/index.json`).then((r) => r.ok).catch(() => false)
    }
    check(up, `devserver: up on ${port}${up ? '' : ' -- ' + log.trim().split('\n').slice(-3).join(' | ')}`)
    if (up) {
      // The engine's own assets/ui is the repository's, not the tree's.
      const index = await (await fetch(`http://127.0.0.1:${port}/ui/index.json`)).json()
      check(index.default === 'hud.json' && index.maps.MAP07 === 'boss.json' && index.maps.MAP01 === 'two.json',
            'devserver: the same merge, the same precedence')
      const hud = await (await fetch(`http://127.0.0.1:${port}/ui/hud.json`)).json()
      check(hud._about === 'first', 'devserver: and the file the default names is the same module\'s')
    }
  } finally {
    dev.kill('SIGKILL')
  }
}

rmSync(T, { recursive: true, force: true })
console.log(`\nui routes: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
