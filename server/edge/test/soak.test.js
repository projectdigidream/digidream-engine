//
// DOES THE SERVER LEAK? Asked the way the editor's leak test asks Chrome: the
// same stimulus, round after round, and the process's memory read after each.
// A leak grows every round; churn and caches plateau.
//
// Two stimuli, each its own phase: a player's whole life on a DOOM II server
// (join, walk, jump, fire, leave), and the churn of levels and games (a level
// loaded, left and dropped, the game replaced). Twelve rounds each. The first
// few are warm-up (a level's first load, the Lua heaps finding their size), so
// what is judged is the LATER half: its growth per round against a bound.
//
// DNP_LEAKS=1 also runs the server under macOS `leaks --atExit`, which reports
// memory nothing points at any more when the server stops (LeakSanitizer does
// not run on Apple silicon, which is why scripts/check_sanitizers.sh turns it
// off). Slower, so opt-in.
//
import { spawn, execFileSync } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import {
  encodeHello, decodeControlMessage, decodeSnapshot, encodeInputBundle, encodeGameNew, axisFromNumber, MSG, BUTTON
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
// The repository: the engine is ROOT, and modules, docs, the studio and the
// synth are beside it.
const REPO = join(ROOT, '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34496)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34570)
const ROUNDS = Number(process.env.DNP_SOAK_ROUNDS ?? 12)
const LEAKS = process.env.DNP_LEAKS === '1'
// THE CONTROL: the same run with maps/scripts/leak_test.lua, which keeps a
// megabyte for every player who arrives. It must be reported as growth.
const CONTROL = process.env.DNP_SOAK_CONTROL === '1'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms, l) => {
  const end = Date.now() + ms
  while (Date.now() < end) { if (fn()) return true; await sleep(20) }
  throw new Error(`timeout: ${l}`)
}

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

// The process's PHYSICAL FOOTPRINT, in KB: what macOS accounts to it, dirty
// memory included whether or not it is compressed. Not `ps`'s resident size,
// which moves with paging and memory compression: the same twelve rounds read
// 8 to 18 MB on one run and a flat 67 MB on the next. Elsewhere, resident size
// is what there is.
function rssKb (pid) {
  if (process.platform === 'darwin') {
    const out = execFileSync('vmmap', ['-summary', String(pid)], { stdio: ['ignore', 'pipe', 'ignore'] }).toString()
    const m = out.match(/Physical footprint:\s+([\d.]+)([KMG])/)
    if (m) return Number(m[1]) * { K: 1, M: 1024, G: 1024 * 1024 }[m[2]]
  }
  return Number(execFileSync('ps', ['-o', 'rss=', '-p', String(pid)]).toString().trim())
}

// One player's life: join, play for `ms`, leave. `newGame`: having changed
// level, start a new game before leaving -- the game VM torn down and built
// again, every level dropped.
async function life (ms, { newGame = false } = {}) {
  const wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
    serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
  })
  await wt.ready
  const control = await wt.createBidirectionalStream()
  const cw = control.writable.getWriter()
  const got = { welcome: false, welcomes: 0, panels: 0, results: 0, hud: {} }
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
          if (m?.type === MSG.WELCOME) { got.welcome = true; got.welcomes++ }
          else if (m?.type === MSG.UI_OPEN) got.panels++
          else if (m?.type === MSG.GAME_RESULT) got.results++
          else if (m?.type === MSG.HUD_SET) for (const f of m.fields) got.hud[f.name] = f.value
        }
      }
    } catch {}
  })()
  let lastTick = 0
  let sounds = 0
  ;(async () => {
    const r = wt.datagrams.readable.getReader()
    try {
      for (;;) {
        const { value, done } = await r.read()
        if (done) break
        const s = decodeSnapshot(Buffer.from(value))
        if (s) { lastTick = s.serverTick; sounds += s.sounds.length }
      }
    } catch {}
  })()
  await cw.write(encodeHello())
  await waitFor(() => got.welcome, 15000, 'welcome')
  const dw = wt.datagrams.createWritable().getWriter()
  let tick = 0
  let n = 0
  const frames = []
  const end = Date.now() + ms
  while (Date.now() < end) {
    tick = Math.max(tick + 1, lastTick + 8)
    n++
    // Walk in a slow circle, jump now and then, fire now and then.
    const a = n / 40
    const buttons = (n % 90 < 3 ? BUTTON.JUMP : 0) | (n % 30 < 2 ? BUTTON.ATTACK_PRIMARY : 0)
    frames.unshift({ buttons, moveX: axisFromNumber(Math.cos(a)), moveY: axisFromNumber(Math.sin(a)), aim: 0 })
    frames.length = Math.min(frames.length, 12)
    await dw.write(encodeInputBundle({ clientTick: tick, ackSnapshotTick: lastTick, frames }))
    await sleep(16)
  }
  if (newGame) {
    await cw.write(encodeGameNew())
    await waitFor(() => got.results > 0, 8000, 'the new game')
  }
  try { await cw.close() } catch {}
  try { wt.close() } catch {}
  return { panels: got.panels, sounds, welcomes: got.welcomes, hud: got.hud }
}


// One phase: a server, `rounds` lives, its memory after each, and the verdict
// on the later half. Returns nothing; checks as it goes.
async function phase (name, args, rounds, play, describe) {
  console.log(name)
  const server = LEAKS
    ? spawn('leaks', ['--atExit', '--', serverBin, ...args],
            { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, MallocStackLogging: '1' } })
    : spawn(serverBin, args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
  let log = ''
  server.stdout.on('data', (d) => { log += d.toString() })
  server.stderr.on('data', (d) => { log += d.toString() })
  // The SERVER's pid: under `leaks` it is the wrapper's child.
  const serverPid = () => LEAKS
    ? Number(execFileSync('pgrep', ['-P', String(server.pid)]).toString().trim().split('\n')[0])
    : server.pid
  let edge = null
  try {
    await waitFor(() => log.includes('listening for edge'), 30000, 'server')
    edge = new Edge({
      port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
      cert: readFileSync(join(CERTS, 'cert.pem')),
      privKey: readFileSync(join(CERTS, 'key.pem')),
      log: () => {}
    })
    await edge.start()
    await waitFor(() => log.includes('edge connected'), 8000, 'edge')
    const pid = serverPid()
    const samples = []
    const lives = []
    for (let round = 0; round < rounds; round++) {
      const r = await play()
      lives.push(r)
      // Let the session close and the server settle before reading.
      await sleep(800)
      samples.push(rssKb(pid))
      console.log(`  round ${String(round + 1).padStart(2)}: ${(samples.at(-1) / 1024).toFixed(1)} MB  ${describe(r)}`)
    }
    check(/session \d+ closed/.test(log), 'every session was closed by the server')
    // THE LATER HALF, judged. Least squares over it, so one noisy sample
    // neither hides a leak nor invents one.
    const half = samples.slice(Math.floor(samples.length / 2))
    const xs = half.map((_, i) => i)
    const mx = xs.reduce((a, b) => a + b, 0) / xs.length
    const my = half.reduce((a, b) => a + b, 0) / half.length
    const slope = xs.reduce((s, x, i) => s + (x - mx) * (half[i] - my), 0) /
                  xs.reduce((s, x) => s + (x - mx) * (x - mx), 0)
    // 256 KB a round: a player's whole life costing a quarter of a megabyte
    // for ever would be a leak worth finding; allocator noise is well under it.
    if (CONTROL) {
      check(slope >= 256, `CONTROL: a script keeping a megabyte a player is caught (${slope.toFixed(0)} KB per round)`)
    } else {
      check(slope < 256, `memory settles: ${slope.toFixed(0)} KB per round over the later half (bound 256)`)
    }
    return { lives, log }
  } catch (e) {
    check(false, `the run finished (${e.message})`)
    console.log(log.split('\n').slice(-20).join('\n'))
    return { lives: [], log }
  } finally {
    await edge?.stop()
    const exited = new Promise((r) => server.on('exit', r))
    if (LEAKS) {
      // Stopped the way an operator stops it, so it exits through main() and
      // `leaks` gets to look.
      try { process.kill(serverPid(), 'SIGTERM') } catch {}
      await Promise.race([exited, sleep(60000)])
      const m = log.match(/Process \d+: (\d+) leaks? for (\d+) total leaked bytes/)
      check(m !== null, '`leaks` reported on the server at exit')
      if (m) {
        check(Number(m[1]) === 0, `nothing the server allocated is left unreachable (${m[1]} leaks, ${m[2]} bytes)`)
        if (Number(m[1]) > 0) { const i = log.indexOf('Process '); console.log(log.slice(i, i + 4000)) }
      }
    } else {
      server.kill('SIGKILL')
      await Promise.race([exited, sleep(5000)])
    }
  }
}

// --- a DOOM II life --------------------------------------------------------
//
// Join, walk, jump, fire, leave: the monsters thinking, the weapons in the game
// VM keeping the player's inventory and forgetting it, the panel and the
// sounds that come with it.
{
  const maps = ['map01.dnpm', 'map02.dnpm'].map((m) => join(REPO, 'modules', 'doom2', 'maps', m))
  const args = ['--port', String(LINK_PORT), '--module', 'doom2', ...maps.flatMap((m) => ['--map', m])]
  if (CONTROL) args.push('--game-script', join(ROOT, 'maps', 'scripts', 'leak_test.lua'))
  const { lives } = await phase('doom', args, ROUNDS, () => life(3000),
                                (r) => `(${Object.keys(r.hud).length} HUD value(s), ${r.sounds} sound(s))`)
  // A REAL LIFE is one the game's weapons saw: they told this player's HUD
  // what is in hand. This counted weapon PANELS, and the panel has been opt-in
  // since 2026-10-06 (weapons.load{ panel = true }), which DOOM II does not ask
  // for -- so it counted none, every life, and failed whenever the suite got
  // this far.
  const told = lives.filter((r) => r.hud.weapon !== undefined).length
  const sounds = lives.reduce((a, r) => a + r.sounds, 0)
  check(told === lives.length && sounds > 0, `every life was a real one: the HUD told the weapon, sounds heard (${told} of ${lives.length}, ${sounds})`)
  // THE HUD'S VALUES (HUD_SET, docs/UI_LAYOUT.md 4.3), published by the
  // weapons runtime with no script of the module's own: the weapon in hand and
  // the pool it draws from.
  const hud = lives[0]?.hud ?? {}
  check(hud.weapon === 'pistol' && hud.ammo_type === 'bullets' && hud.ammo_max === 200 && typeof hud.ammo === 'number',
        `the HUD is told the weapon in hand and its ammunition (${JSON.stringify(hud).slice(0, 120)})`)
}

// --- levels and games ------------------------------------------------------
//
// The churn a long session has and a join does not: a level loaded, left and
// dropped, and the game itself replaced. Each life starts in level A, is sent
// on to B by maps/scripts/game_level_test.lua, starts a new game -- every level
// dropped and the game VM built again -- and leaves.
if (!CONTROL) {
  const baked = (name) => {
    for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
      const p = join(ROOT, 'build', arch, name)
      if (existsSync(p)) return p
    }
    return null
  }
  const A = baked('level_a.dnpm')
  const B = baked('level_b.dnpm')
  if (A && B) {
    const args = ['--port', String(LINK_PORT), '--map', A, '--map', B,
                  '--script', join(ROOT, 'maps', 'scripts', 'game_level_test.lua'),
                  '--game-script', join(ROOT, 'maps', 'scripts', 'game_test.lua')]
    const { lives, log } = await phase('levels', args, ROUNDS, () => life(5500, { newGame: true }),
                                       (r) => `(${r.welcomes} welcome(s))`)
    check(lives.length > 0 && lives.every((r) => r.welcomes >= 3),
          'every life changed level and started a new game (three welcomes each)')
    check((log.match(/runs once for the whole game/g) ?? []).length > lives.length,
          'and the game VM was built again every time')
  } else {
    console.log('  (no baked level_a/level_b; the levels phase is skipped)')
  }
}

console.log(`\nsoak: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
