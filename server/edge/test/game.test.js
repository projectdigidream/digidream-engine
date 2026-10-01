//
// GAME SCRIPTS, against a real server (docs/MODULES.md, "Game scripts").
//
// A module's game scripts run once for the whole game rather than once per
// level, so what they hold about a player -- and the panels they open -- goes
// with the player into the next level. The fixtures say what they are doing on
// the server's output: maps/scripts/game_test.lua (the game script) counts the
// ticks it has seen each player for and opens one panel; maps/scripts/
// game_level_test.lua (a level script) opens a panel of its own, asks the game
// every second what it has counted, and sends the player from level A to B.
//
//   game     the player changes level once. The game's panel was opened once
//            and never closed; the level's panel closed with its level; the
//            second level, asking the game, hears a count that kept going.
//   level    CONTROL: the same script loaded per level instead. Each level
//            opens a Game panel of its own, the first one closes with its
//            level, and the second level's copy counts from nothing -- the
//            bug game scripts exist to fix, reproduced.
//   saves    the game's own state and the player's game state go into a save
//            game and come back from it.
//   server   and the game's own state survives a server restart.
//   region   three levels on a metamap's grid, one world: walking from the
//            first into the second is a HOME, not a level change, and the
//            panel the first level opened closes with it while the game's
//            stays.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import {
  encodeHello, decodeControlMessage, encodeGameSave, encodeGameLoad, encodeInputBundle,
  decodeSnapshot, axisFromNumber, MSG
} from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34496)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34570)

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

function baked (name) {
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = join(ROOT, 'build', arch, name)
    if (existsSync(p)) return p
  }
  return null
}
const A = baked('level_a.dnpm')
const B = baked('level_b.dnpm')
if (!A || !B) { console.error('no baked level_a/level_b'); process.exit(2) }
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const SEAM = ['seam_a.dnpm', 'seam_b.dnpm', 'seam_c.dnpm'].map(baked)
if (SEAM.includes(null)) { console.error('no baked seam_a/seam_b/seam_c'); process.exit(2) }
const GAME = join(ROOT, 'maps', 'scripts', 'game_test.lua')
const LEVEL = join(ROOT, 'maps', 'scripts', 'game_level_test.lua')

let current = null
async function start (extra, maps = [A, B]) {
  const args = ['--port', String(LINK_PORT), ...maps.flatMap((m) => ['--map', m]),
                '--script', LEVEL, ...extra]
  const server = spawn(serverBin, args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
  const run = { server, log: '' }
  current = run
  server.stdout.on('data', (d) => { run.log += d.toString() })
  server.stderr.on('data', (d) => { run.log += d.toString() })
  await waitFor(() => run.log.includes('listening for edge'), 8000, 'server')
  run.edge = new Edge({
    port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
    cert: readFileSync(join(CERTS, 'cert.pem')),
    privKey: readFileSync(join(CERTS, 'key.pem')),
    log: () => {}
  })
  await run.edge.start()
  await waitFor(() => run.log.includes('edge connected'), 8000, 'edge')
  run.wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
    serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
  })
  await run.wt.ready
  const control = await run.wt.createBidirectionalStream()
  run.cw = control.writable.getWriter()
  run.welcomes = 0
  run.homes = []           // levels walked into
  run.snapshots = 0
  run.lastTick = 0
  run.opened = []          // { id, title }
  run.closed = []          // ids
  run.results = []
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
          if (m?.type === MSG.WELCOME) run.welcomes++
          else if (m?.type === MSG.HOME) run.homes.push(m.level)
          else if (m?.type === MSG.UI_OPEN) run.opened.push({ id: m.uiId, title: m.title })
          else if (m?.type === MSG.UI_CLOSE) run.closed.push(m.uiId)
          else if (m?.type === MSG.GAME_RESULT) run.results.push(m)
        }
      }
    } catch {}
  })()
  ;(async () => {
    const r = run.wt.datagrams.readable.getReader()
    try {
      for (;;) {
        const { value, done } = await r.read()
        if (done) break
        const s = decodeSnapshot(Buffer.from(value))
        if (s) { run.snapshots++; run.lastTick = s.serverTick }
      }
    } catch {}
  })()
  run.dw = run.wt.datagrams.createWritable().getWriter()
  run.tick = 0
  run.history = []
  await run.cw.write(encodeHello())
  await waitFor(() => run.welcomes >= 1, 8000, 'welcome')
  return run
}

// One input frame, walking east, kept leading the server's clock.
async function walkEast (run) {
  run.tick = Math.max(run.tick + 1, run.lastTick + 8)
  const f = { buttons: 0, moveX: axisFromNumber(1), moveY: 0, aim: 0 }
  run.history = [f, ...run.history].slice(0, 12)
  await run.dw.write(encodeInputBundle({ clientTick: run.tick, ackSnapshotTick: run.lastTick, frames: run.history }))
}

async function stop (run, signal = 'SIGKILL') {
  try { await run.cw.close() } catch {}
  try { run.wt.close() } catch {}
  await run.edge.stop()
  const exited = new Promise((r) => run.server.on('exit', r))
  run.server.kill(signal)
  await Promise.race([exited, sleep(5000)])
}

// What each level heard from the game: [level, ticks, from the game VM].
const answers = (log) => [...log.matchAll(/level (\d) asked the game: (\S+) ticks \(from the game VM: (\S+)\)/g)]
  .map((m) => ({ level: Number(m[1]), ticks: m[2] === 'nil' ? null : Number(m[2]), game: m[3] === 'true' }))
const titled = (run, t) => run.opened.filter((p) => p.title === t)

// Join, wait for the move to level B and a couple of answers there.
async function playThrough (run) {
  await waitFor(() => run.welcomes >= 2, 10000, 'the move to level B')
  await waitFor(() => run.log.includes('level 1 opened panel'), 4000, 'level B\'s panel')
  await waitFor(() => answers(run.log).filter((a) => a.level === 1).length >= 2, 6000, 'level B asking')
}

const state = mkdtempSync(join(tmpdir(), 'dnp-game-'))
try {
  // --- game ----------------------------------------------------------------
  console.log('game')
  {
    const run = await start(['--game-script', GAME])
    check(run.log.includes('game_test.lua (runs once for the whole game)'), 'the game script is loaded once, game-wide')
    await playThrough(run)
    const game = titled(run, 'Game')
    check(game.length === 1, `the game's panel was opened once, for both levels (${game.length})`)
    check(game.length === 1 && !run.closed.includes(game[0].id), 'and the level change did not close it')
    const l0 = titled(run, 'Level 0')
    check(l0.length === 1 && run.closed.includes(l0[0].id), 'the first level\'s own panel closed when the player left it')
    const l1 = titled(run, 'Level 1')
    check(l1.length === 1 && !run.closed.includes(l1[0].id), 'and the second level opened its own')
    const as = answers(run.log)
    const inA = as.filter((a) => a.level === 0)
    const inB = as.filter((a) => a.level === 1)
    check(inA.length > 0 && inB.length > 0, `both levels asked the game (${inA.length}, ${inB.length})`)
    check(as.every((a) => a.game), 'and every answer came from the game VM')
    const lastA = inA.at(-1)?.ticks ?? 0
    const firstB = inB[0]?.ticks ?? 0
    check(firstB > lastA, `the count went on across the level change (${lastA} in A, then ${firstB} in B)`)
    check(/player \S+ entered level 1 \(visit 2\)/.test(run.log), 'the game was told the player arrived in level B')
    await stop(run)
  }

  // --- level (the CONTROL) -------------------------------------------------
  console.log('level')
  {
    const run = await start(['--script', GAME])
    await playThrough(run)
    const game = titled(run, 'Game')
    check(game.length === 2, `CONTROL: loaded per level, each level opens a Game panel of its own (${game.length})`)
    check(game.length === 2 && run.closed.includes(game[0].id), 'CONTROL: and the first closes with its level')
    const inA = answers(run.log).filter((a) => a.level === 0)
    const inB = answers(run.log).filter((a) => a.level === 1)
    check(inB.every((a) => a.ticks === null) && inA.every((a) => a.ticks === null),
          'CONTROL: with no game VM, a level asking the game hears nothing')
    await stop(run)
  }

  // --- saves ---------------------------------------------------------------
  console.log('saves')
  {
    const run = await start(['--game-script', GAME, '--persist', 'saves', '--state-dir', state])
    await playThrough(run)
    await run.cw.write(encodeGameSave(0))
    await waitFor(() => run.results.length >= 1, 4000, 'save result')
    check(run.results[0].ok, `the game is saved (${run.results[0].text})`)
    const saved = Number(run.log.match(/saving (\d+) visits/)?.[1])
    check(saved === 2, `with the game's own state: two arrivals (${saved})`)
    const before = run.log.length
    await run.cw.write(encodeGameLoad(0))
    await waitFor(() => run.results.length >= 2, 6000, 'load result')
    check(run.results[1].ok, `the save is loaded (${run.results[1].text})`)
    const after = run.log.slice(before)
    check(/restored 2 visits/.test(after), 'the game\'s own state came back from it')
    check(/restored player \S+ at \d+ ticks/.test(after), 'and so did what the game keeps about the player')
    // A loaded game is the game being replaced: its panel goes and the new
    // game VM opens its own.
    const game = titled(run, 'Game')
    check(game.length === 2 && run.closed.includes(game[0].id), `the old game's panel closed and the loaded one opened (${game.length})`)
    await stop(run)
  }

  // --- server --------------------------------------------------------------
  console.log('server')
  {
    const dir = join(state, 'server')
    const first = await start(['--game-script', GAME, '--persist', 'server', '--state-dir', dir])
    await playThrough(first)
    await stop(first, 'SIGTERM')
    check(existsSync(join(dir, 'game.save')), 'stopping writes the game\'s state')
    const second = await start(['--game-script', GAME, '--persist', 'server', '--state-dir', dir])
    check(/restored 2 visits/.test(second.log), 'a restart brings it back: two arrivals')
    check(/saved game state read/.test(second.log), 'and says where it read it from')
    await waitFor(() => /visit 3\)/.test(second.log), 4000, 'the third arrival')
    check(true, 'and the next arrival counts on from there (visit 3)')
    await stop(second)
  }

  // --- region --------------------------------------------------------------
  console.log('region')
  {
    const run = await start(['--game-script', GAME, '--metamap', join(ROOT, 'maps', 'seam_metamap.txt')], SEAM)
    await waitFor(() => titled(run, 'Game').length === 1 && titled(run, 'Level 0').length === 1, 6000, 'the panels')
    for (let i = 0; i < 900 && !run.homes.includes(1); i++) { await walkEast(run); await sleep(6) }
    check(run.homes.includes(1), `the player walked into the second level (homes: ${run.homes.join(' ') || 'none'})`)
    await sleep(300)
    check(run.welcomes === 1, `one world: no second WELCOME (${run.welcomes})`)
    const game = titled(run, 'Game')
    check(game.length === 1 && !run.closed.includes(game[0].id), 'the game\'s panel stayed open')
    const l0 = titled(run, 'Level 0')
    check(l0.length === 1 && run.closed.includes(l0[0].id), 'the first level\'s panel closed when the player walked out of it')
    // CONTROL: a panel is not closed by walking for its own sake -- the second
    // level's (opened while the player was next door) is still open.
    const l1 = titled(run, 'Level 1')
    check(l1.length === 1 && !run.closed.includes(l1[0].id), 'CONTROL: and the level walked into kept its own')
    await stop(run)
  }
} catch (e) {
  check(false, `the run finished (${e.message})`)
  if (current) console.log(current.log.split('\n').slice(-30).join('\n'))
} finally {
  rmSync(state, { recursive: true, force: true })
}

console.log(`\ngame: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
