//
// WALKING FROM ONE LEVEL INTO THE NEXT, IN A BROWSER, AND LOOKING AT THE VIEW.
//
// The seam fixture's three maps are one world on the server (a region) and on
// this client too: the client holds its neighbours as chunks of its own
// simulation, in world coordinates. Walking east out of level 0 is then walking
// between two rooms -- the server says HOME, and nothing is re-placed,
// re-predicted or re-based. So what is on screen must not change at all: the
// camera moves by what the body moves, which is one frame of walking.
//
// This used to measure a jump of one pitch and ask that the camera jump with
// it. Four separate bugs lived in that jump (a spawn-point arrival, a velocity
// reset, neighbours a frame late, a stray snapTo). There is no jump now.
//
// The seam FIXTURE rather than a converted level, because a bot cannot walk
// through MAP01 to reach a boundary: seam_a is a room and a corridor pointing
// due east, and ?autowalk=east pushes east in world terms whatever the camera
// is doing.
//
//   DNP_CLIENT_PAGE_TEST=1 node client/test/seamcam.test.js
//
// DNP_SEAMCAM_SHUTTLE=1 is the harder run: maps sent in 128-byte chunks, and
// the browser turns round at every level change -- east, back west, east again.
//
import { spawn } from 'node:child_process'
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Edge } from '../../server/edge/src/edge.js'
import { serverBin } from '../../server/edge/test/serverbin.js'

const HERE = dirname(dirname(fileURLToPath(import.meta.url)))
const ROOT = join(HERE, '..')
const CERTS = join(ROOT, 'server', 'edge', 'certs')
const HTTP = Number(process.env.DNP_SEAMCAM_HTTP ?? 8193)
const WT = Number(process.env.DNP_SEAMCAM_WT ?? 34693)
const LINK = Number(process.env.DNP_SEAMCAM_LINK ?? 34694)
const SHUTTLE = process.env.DNP_SEAMCAM_SHUTTLE === '1'
const CROSSINGS = SHUTTLE ? 4 : 1

let checks = 0, fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const done = () => {
  console.log(`\nseamcam: ${checks} checks, ${fails} failures`)
  process.exit(fails === 0 ? 0 : 1)
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// Opt-in and skipped cleanly, exactly as client/test/page.test.js is: a headless
// browser is not a build dependency of this project, and a check that says it
// did not run beats making one.
if (!process.env.DNP_CLIENT_PAGE_TEST) {
  console.log('  skip (set DNP_CLIENT_PAGE_TEST=1 -- it launches a headless browser)')
  done()
}
const CHROMES = [
  process.env.DNP_CHROME,
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/Applications/Chromium.app/Contents/MacOS/Chromium',
  '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser'
].filter(Boolean)
const chrome = CHROMES.find((p) => existsSync(p))
if (!chrome) { console.log('  skip (no Chrome; set DNP_CHROME)'); done() }

const built = (n) => ['arm64', 'x86_64', 'native']
  .map((a) => join(ROOT, 'build', a, n)).find(existsSync)
const A = built('seam_a.dnpm'), B = built('seam_b.dnpm'), C = built('seam_c.dnpm')
if (!A || !B || !C) { console.log('  skip (no baked seam fixture)'); done() }
if (!existsSync(join(CERTS, 'cert-hash.json'))) { console.log('  skip (no dev certificate)'); done() }

const kill = []
process.on('exit', () => { for (const p of kill) { try { p.kill('SIGKILL') } catch {} } })

const server = spawn(serverBin,
  ['--port', String(LINK), '--map', A, '--map', B, '--map', C,
   '--metamap', join(ROOT, 'maps', 'seam_metamap.txt'),
   ...(SHUTTLE ? ['--map-chunk', '128'] : [])],
  { cwd: ROOT, stdio: ['ignore', 'ignore', 'pipe'] })
kill.push(server)
let srvLog = ''
server.stderr.on('data', (d) => { srvLog += d })
for (let i = 0; i < 200 && !srvLog.includes('listening for edge'); i++) await sleep(50)

const edge = new Edge({
  port: WT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  log: () => {}
})
await edge.start()
for (let i = 0; i < 200 && !srvLog.includes('edge connected'); i++) await sleep(50)

// The dev server, for the page AND for its /dev-log route: the page reports
// what the crossing did to the view, and a browser console is the one place a
// test cannot read.
const web = spawn(process.execPath, [join(ROOT, 'tools', 'devserver.js')],
                  { cwd: ROOT, env: { ...process.env, DNP_HTTP_PORT: String(HTTP) },
                    stdio: ['ignore', 'pipe', 'pipe'] })
kill.push(web)
let pageLog = ''
web.stdout.on('data', (d) => { pageLog += d })
await sleep(500)

const profile = mkdtempSync(join(tmpdir(), 'dnp-seamcam-'))
const browser = spawn(chrome, [
  '--headless=new', '--mute-audio', '--no-sandbox', '--disable-dev-shm-usage',
  `--user-data-dir=${profile}`,
  '--enable-unsafe-swiftshader', '--window-size=1200,800',
  // KEEPS IT ALIVE. --dump-dom and --screenshot both exit as soon as the page
  // loads, and --virtual-time-budget runs the clock forward faster than the
  // connection can answer -- the session opened and closed having received no
  // snapshots at all. A debugging port makes Chrome wait to be told to stop.
  '--remote-debugging-port=0',
  `http://localhost:${HTTP}/?server=https://127.0.0.1:${WT}/dnp&autowalk=${SHUTTLE ? 'shuttle' : 'east'}&diag`
], { stdio: 'ignore' })
kill.push(browser)

// Long enough to load the core, join, and walk the length of seam_a's corridor.
const crossed = () => (pageLog.match(/\[seam\] crossing:/g) ?? []).length
for (let i = 0; i < 90 * CROSSINGS && crossed() < CROSSINGS; i++) await sleep(400)
const gone = new Promise((r) => browser.once('exit', r))
try { browser.kill('SIGKILL') } catch {}
// Chrome's helpers outlive the main process by a moment and are still writing
// the profile, so a delete straight after the kill raced them (ENOTEMPTY) and
// failed a run whose checks had all passed. Wait for the exit, retry the delete,
// and never fail on what is only a temporary directory.
await Promise.race([gone, sleep(3000)])
try { rmSync(profile, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }) } catch {}

if (process.env.DNP_SEAMCAM_VERBOSE) console.log(pageLog)
const seam = pageLog.split('\n').filter((l) => l.includes('[seam]'))
for (const l of seam) console.log(`  ${l.trim()}`)

// Nothing the client considered an error. Cheap, general, and it says what it
// covers: the fixture's map is one chunk, so a fault that needs a PART-
// DELIVERED transfer -- which is every converted Doom map -- will not reproduce
// here. "chunk out of order (expected 0, got 65536)" was exactly that, and this
// check would have stayed quiet through it.
check(!seam.some((l) => /failed|STUCK|VIOLATION|NOT MOVING/.test(l)),
      `nothing went wrong (${seam.filter((l) => /failed|STUCK|VIOLATION|NOT MOVING/.test(l)).length})`)
const walks = seam.filter((l) => l.includes('walked into level'))
check(walks.length >= CROSSINGS, `the browser walked into the next level (${walks.length})`)
// NO RE-PLACEMENT: one world, so no level change goes through a WELCOME.
check(!seam.some((l) => l.includes('entered level')), 'and nothing was re-placed on the way')
// IN WORLD COORDINATES: past the boundary at x = 40 is x > 40, not a local x
// near zero. CONTROL for the continuity claims below -- a client still in its
// old level's frame would report the same small slip and be somewhere else.
const xs = walks.map((l) => Number(l.match(/\((-?[\d.]+),/)?.[1]))
check(xs.length > 0 && xs.every((x) => Number.isFinite(x) && x > 36 && x < 44),
      `at the boundary, in world coordinates (x ${xs.map((x) => x.toFixed(2)).join(', ')})`)

// [body, camera, slip, slip on the frame before, arrival correction]
const slips = [...pageLog.matchAll(/body moved ([\d.]+), camera moved ([\d.]+), slip ([\d.]+) \(the frame before: ([\d.]+), correction ([\d.]+)\)/g)]
  .map((m) => m.slice(1).map(Number))
check(slips.length >= CROSSINGS, `and reported what each did to the view (${slips.length})`)
if (slips.length) {
  // THE CLAIM. Nothing jumped: the body moved one frame of walking, and the
  // camera with it.
  check(slips.every(([body]) => body < 0.5),
        `the body did not jump (${slips.map(([b]) => b.toFixed(3)).join(', ')})`)
  const worst = Math.max(...slips.map(([, , slip, before]) => slip - before))
  check(worst < 0.08, `the camera moved with it (worst added slip ${worst.toFixed(3)})`)
}
if (SHUTTLE) {
  const into = walks.map((l) => l.match(/walked into level (\d+)/)[1])
  check(new Set(into).size >= 2, `it went back and forth (${into.join(' -> ')})`)
  check(walks.every((l) => l.includes('view promoted')), 'every level walked into was already drawn')
  // CONTROL: the maps really did arrive in pieces.
  const chunks = [...pageLog.matchAll(/bytes in (\d+) chunk/g)].map((m) => Number(m[1]))
  check(chunks.length > 0 && chunks.every((n) => n > 1),
        `CONTROL: every neighbour map came in several chunks (${chunks.join(', ')})`)
}

done()
