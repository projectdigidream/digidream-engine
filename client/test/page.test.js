//
// The client, loaded in a browser.
//
// THE FILE NOTHING ELSE COVERS. client/test/tdz.test.js says it plainly:
// main.js "wants a DOM, a WebGL context, three.js and a live WebTransport
// session, so it is the one file in the client with no automated coverage at
// all -- and it is the file that changes most". A static check over its
// declaration order was the cheap half. This is the other half: load it, and
// see whether it ran.
//
// It answers exactly two questions, and they are the two that matter after a
// change to the client's start-up:
//
//   did the module get to the end without throwing
//   are the touch controls there when they should be, and gone when they
//   should not
//
// Not a rendering test. There is no GPU here worth the name -- SwiftShader
// draws a differently-shaped frame and would name culprits real hardware does
// not have -- so nothing is asserted about pixels. What IS asserted is the
// DOM, which is where the controls live and where a thrown error lands:
// index.html writes anything uncaught into #status, so a bad import or a
// missing export shows up as text.
//
// Opt-in, because it launches a headless browser. The pure tests cover the
// arithmetic; this covers "any of it works in a browser at all", which they
// structurally cannot.
//
import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')
const PORT = Number(process.env.DNP_CLIENT_TEST_PORT ?? 8191)

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

if (!process.env.DNP_CLIENT_PAGE_TEST) {
  console.log('  skip (set DNP_CLIENT_PAGE_TEST=1 -- it launches a headless browser)')
  console.log('\nclientpage: 0 checks, 0 failures')
  process.exit(0)
}

const CHROMES = [
  process.env.DNP_CHROME,
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/Applications/Chromium.app/Contents/MacOS/Chromium',
  '/usr/bin/google-chrome',
  '/usr/bin/chromium',
  '/usr/bin/chromium-browser'
].filter(Boolean)
const chrome = CHROMES.find((p) => existsSync(p))
if (!chrome) {
  // Skipped rather than failed: a headless browser is not a build dependency
  // of this project, and making it one so the suite can run would be a worse
  // trade than a check that says it did not run.
  console.log('  skip (no Chrome; set DNP_CHROME to run the client page test)')
  console.log('\nclientpage: 0 checks, 0 failures')
  process.exit(0)
}

const server = spawn(process.execPath, [join(ROOT, 'tools', 'devserver.js'), '--module', 'doom2'],
                     { env: { ...process.env, DNP_HTTP_PORT: String(PORT) }, stdio: 'pipe' })
let serverErr = ''
server.stderr.on('data', (d) => { serverErr += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })

/** Load a URL and give back the DOM the page ended up with. */
function dump (url) {
  const profile = mkdtempSync(join(tmpdir(), 'dnp-client-'))
  try {
    const r = spawnSync(chrome, [
      '--headless=new', '--mute-audio', '--no-sandbox', '--disable-dev-shm-usage',
      `--user-data-dir=${profile}`,
      // SwiftShader, because there is no GPU here and the client makes a WebGL
      // context before it does anything else. Without this the page throws at
      // the renderer and every check below fails for the wrong reason.
      '--enable-unsafe-swiftshader',
      // Enough virtual time for the wasm core, the map and the textures. The
      // page is deterministic under virtual time, so this is a budget and not
      // a race.
      '--virtual-time-budget=10000',
      '--dump-dom', url
    ], { encoding: 'utf8', timeout: 120_000, maxBuffer: 32 << 20 })
    return r.stdout ?? ''
  } finally {
    rmSync(profile, { recursive: true, force: true })
  }
}

const statusOf = (dom) => /<div id="status"[^>]*>([\s\S]*?)<\/div>/.exec(dom)?.[1] ?? ''
const url = (q) => `http://localhost:${PORT}/?map=/maps/map01.dnpm&${q}`

// The server needs a moment before Chrome asks it for anything.
await new Promise((r) => setTimeout(r, 1500))

try {
  console.log('--- the client loads, with the touch controls asked for ---')
  const dom = dump(url('touch=1'))
  check(dom.length > 0, 'chrome returned a DOM')

  // WHERE A THROWN ERROR LANDS. index.html installs an `error` and an
  // `unhandledrejection` handler that write into #status, so a bad import
  // path, a missing export or a use-before-declaration shows up here as text
  // rather than as a silently blank page.
  const status = statusOf(dom).trim()
  check(!/^error:/.test(status), `nothing threw (${JSON.stringify(status.slice(0, 160))})`)

  // The module got all the way past its awaits: the core loaded, the map was
  // fetched and parsed, and the status line was cleared.
  check(status === '', 'and the module reached the end of its start-up')

  console.log('\n--- the controls are there ---')
  {
    const btns = [...dom.matchAll(/class="dnp-touch-btn ([a-z]+)[ "]/g)].map((m) => m[1])
    check(new Set(btns).size === 4,
          `all four buttons are mounted (${[...new Set(btns)].sort().join(', ')})`)
    check(/dnp-touch-pad/.test(dom) && /dnp-touch-knob/.test(dom),
          'the pad and its knob are drawn')
    check(/dnp-touch-zone/.test(dom),
          'and the zone a thumb has to land in exists to be measured')
    // The class the page's own CSS keys off to move the HUD out of the way.
    check(/<html[^>]*class="[^"]*dnp-touch-on/.test(dom),
          'the page knows it is being played with a thumb')
    check(!/id="touchhelp"/.test(dom),
          'and no "drag to turn" tip is shown (the user asked for it gone)')
  }

  let off = ''
  console.log('\n--- CONTROL: and gone when they are not wanted ---')
  {
    // Without this, every check above passes for a client that mounts a
    // thumbstick over a desktop game that has a keyboard. `?touch=0` is the
    // same override a touchscreen laptop would use.
    off = dump(url('touch=0'))
    check(!/^error:/.test(statusOf(off).trim()),
          `it still loads (${JSON.stringify(statusOf(off).trim().slice(0, 120))})`)
    check(!/class="dnp-touch-btn/.test(off), 'no buttons')
    check(!/class="dnp-touch-pad"/.test(off), 'no pad')
    check(!/<html[^>]*class="[^"]*dnp-touch-on/.test(off),
          'and the page does not think it is a phone')
    // The keyboard legend is the thing that takes the corner back.
    check(/WASD move/.test(off), 'the keyboard legend is the one shown')
  }

  console.log('\n--- launching without the debug overlay (?debug=0) ---')
  {
    // The numbers block, written every frame, and the legend under it. The
    // control is the default launch (`off`, above): the same block, there.
    const hudOf = (d) => /<div id="hud"([^>]*)>([\s\S]*?)<\/div>/.exec(d)
    const helpOf = (d) => /<div id="help"([^>]*)>/.exec(d)?.[1] ?? ''
    const shown = hudOf(off)
    check(shown && /tick \d+/.test(shown[2]) && !/display: none/.test(shown[1]),
          'CONTROL: by default the debug block is drawn and written')
    check(/display: block/.test(helpOf(off)), 'CONTROL: and so is the legend')
    const clean = dump(url('touch=0&debug=0'))
    check(!/^error:/.test(statusOf(clean).trim()) && statusOf(clean).trim() === '',
          `it loads to the end (${JSON.stringify(statusOf(clean).trim().slice(0, 120))})`)
    const h = hudOf(clean)
    check(h && /display: none/.test(h[1]), 'the debug block is hidden')
    check(h && h[2] === '', 'and never written, not merely hidden')
    check(/display: none/.test(helpOf(clean)), 'the legend is hidden too')
  }

  console.log('\n--- CONTROL: a headless desktop is not asked to guess ---')
  {
    // With no override at all, the decision is matchMedia's. Headless Chrome
    // on a desktop reports a fine pointer, so the controls must NOT appear --
    // which is what says wantsTouch is reading the browser rather than
    // defaulting to yes.
    const auto = dump(url('zoom=44'))
    check(!/class="dnp-touch-btn/.test(auto),
          'with no ?touch, a fine-pointer browser gets no thumbstick')
  }
} finally {
  server.kill('SIGTERM')
}

if (serverErr.trim()) console.log(`  ..   devserver said: ${serverErr.trim().split('\n')[0]}`)
console.log(`\nclientpage: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
