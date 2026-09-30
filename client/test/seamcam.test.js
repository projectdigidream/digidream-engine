//
// WALKING ACROSS A SEAM, IN A BROWSER, AND LOOKING AT THE VIEW.
//
// A seam is a change of frame and nothing else: the body is at the same world
// point said in the next level's words. So crossing one must not change what is
// on screen -- the camera and the body have to move by exactly the same amount,
// and the difference between those two numbers is the whole of what a player
// means by "it twitches".
//
// THIS EXISTS BECAUSE FOUR SEPARATE THINGS BROKE IT and every one of them
// looked identical from the outside:
//
//   the body arrived at the map's spawn instead of the boundary;
//   its facing and velocity were reset, so the view swung and the motion stalled;
//   the neighbouring cells stayed at their old offsets for a frame, so the whole
//     world jumped rather than the camera;
//   and adoptMap ended with an unconditional camera.snapTo(spawn) that silently
//     overwrote every one of the fixes above it.
//
// Four rounds of "the camera twitches" and four different causes. None of them
// was findable by looking at the screen, and all of them are one number here.
//
// The seam FIXTURE rather than a converted level, because a bot cannot walk
// through MAP01 to reach a boundary: seam_a is a room and a corridor pointing
// due east, and ?autowalk=east pushes east in world terms whatever the camera
// is doing. Three cells, because a crossing has to have a neighbour to RETAIN
// -- see maps/seam_c.map.
//
//   DNP_CLIENT_PAGE_TEST=1 node client/test/seamcam.test.js
//
// DNP_SEAMCAM_SHUTTLE=1 is the harder run: maps sent in 128-byte chunks, so
// every transfer is part-delivered for a while, and the browser turns round at
// every crossing -- east, back west, east again -- which is what walking into
// the level just left looks like, and the commonest thing anybody does.
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
  '--headless=new', '--no-sandbox', '--disable-dev-shm-usage',
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
try { browser.kill('SIGKILL') } catch {}
rmSync(profile, { recursive: true, force: true })

if (process.env.DNP_SEAMCAM_VERBOSE) console.log(pageLog)
const seam = pageLog.split('\n').filter((l) => l.includes('[seam]'))
for (const l of seam) console.log(`  ${l.trim()}`)

// Nothing the client considered an error. Cheap, general, and it says what it
// covers: the fixture's map is one chunk, so a fault that needs a PART-
// DELIVERED transfer -- which is every converted Doom map -- will not reproduce
// here. "chunk out of order (expected 0, got 65536)" was exactly that, and this
// check would have stayed quiet through it.
check(!seam.some((l) => /failed|STUCK|VIOLATION|NOT MOVING/.test(l)),
      `the crossing reported no fault (${seam.filter((l) => /failed|STUCK|VIOLATION|NOT MOVING/.test(l)).length})`)
check(seam.some((l) => l.includes('crossing:')), 'the browser walked across a seam')
check(seam.some((l) => l.includes('arrived at the seam')),
      'and its body arrived at the boundary, not at the spawn point')

// [body, camera, slip, slip on the frame before, arrival correction]
const slips = [...pageLog.matchAll(/body moved ([\d.]+), camera moved ([\d.]+), slip ([\d.]+) \(the frame before: ([\d.]+), correction ([\d.]+)\)/g)]
  .map((m) => m.slice(1).map(Number))
check(slips.length >= CROSSINGS, `and reported what each crossing did to the view (${slips.length})`)
if (slips.length) {
  // THE CLAIM. The camera moved with the body, so the view did not change.
  // What the CROSSING adds. A walking camera trails the body a little every
  // frame, and just after a turn-round by a tenth of a unit or so; that is the
  // camera's follow, the same on the frame before as on the crossing. A
  // crossing that re-placed the camera or the body shows as a jump here --
  // beyond the correction, when the client's prediction fell short of the
  // boundary and the server's arrival was used: the body really moves by that
  // much, and the camera follows it rather than jumping with it.
  const worst = Math.max(...slips.map(([, , slip, before, corr]) => slip - before - corr))
  check(worst < 0.08, `the camera moved with the body (worst added slip ${worst.toFixed(3)})`)
  // CONTROL: they both really moved, by about the pitch the metamap declares.
  // Two numbers that were both zero would agree perfectly and prove nothing --
  // and a client that never crossed would produce exactly that.
  check(slips.every(([body, cam]) => body > 30 && cam > 30),
        `CONTROL: and each really moved a pitch (${slips.map(([b]) => b.toFixed(1)).join(', ')})`)
}
if (SHUTTLE) {
  // EVERY CROSSING WAS INTO A LEVEL ALREADY HELD, the way back included: no
  // map was streamed for any of them, and each view was promoted.
  const entered = seam.filter((l) => l.includes('entered level'))
  check(entered.length >= CROSSINGS && entered.every((l) => l.includes(': held') && l.includes('view promoted')),
        `every crossing entered a held, built level (${entered.map((l) => l.match(/level (\d+)/)[1]).join(' -> ')})`)
  check(new Set(entered.map((l) => l.match(/level (\d+)/)[1])).size >= 2,
        'and it went back and forth rather than onward')
  // CONTROL: the maps really did arrive in pieces, or this run is the
  // single-chunk one under another name.
  const chunks = [...pageLog.matchAll(/bytes in (\d+) chunk/g)].map((m) => Number(m[1]))
  check(chunks.length > 0 && chunks.every((n) => n > 1),
        `CONTROL: and every neighbour map came in several chunks (${chunks.join(', ')})`)
}

done()
