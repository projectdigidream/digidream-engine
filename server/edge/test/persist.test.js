//
// PERSISTENCE, against a real server (docs/SAVING.md).
//
// The fixture (maps/scripts/persist_test.lua) puts four keys down and takes one
// away every two seconds, and saves which are left through dnp.persist. It says
// what it is doing on the server's output, which is what this reads: whether a
// key is drawn is a renderer's question, and how many a script believes are
// there is the state being saved.
//
// Three runs, one per switch:
//
//   saves   save, wait for the level to change, load: the level is back as it
//           was saved, and the player is in it again. CONTROL: it really had
//           changed in between.
//   none    the same save, refused, with the reason -- a module that does not
//           ask for saves does not get them.
//   server  stop the server with keys missing and start it again on the same
//           state directory: the level comes back as it was left, and the
//           script did not put down a fresh four. CONTROL: the first start did.
//
// And PLACED PROPS (maps/scripts/placed_test.lua): three put down the way the
// studio's placement does it, knowing nothing of saving. A restored level has
// the ones still there, where they were left, and not the one taken -- the
// engine's doing (docs/SAVING.md, "Placed props"). CONTROL: the fresh placement
// had all three where they were authored.
//
// And a NEW GAME in two of them: the world from scratch, and nothing kept --
// not in memory, and not on disk.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync, mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { encodeHello, decodeControlMessage, encodeGameSave, encodeGameLoad, encodeGameNew, MSG } from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
// The repository: the engine is ROOT, and modules, docs, the studio and the
// synth are beside it.
const REPO = join(ROOT, '..')
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
const MAP = baked('test.dnpm')
if (!MAP) { console.error('no baked test map'); process.exit(2) }
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

// One server, an edge in front of it, one client.
// `hosted`: started as a hosted server, not a game's own for its one player
// (--single-player, which the desktop app, the Android app and the web build pass).
async function start (persist, stateDir, { hosted = false } = {}) {
  const args = ['--port', String(LINK_PORT), '--map', MAP,
                '--script', join(ROOT, 'maps', 'scripts', 'persist_test.lua'),
                '--script', join(ROOT, 'maps', 'scripts', 'placed_test.lua'),
                '--props', join(REPO, 'modules', 'doom2', 'assets', 'props.txt'),
                '--persist', persist, '--state-dir', stateDir]
  if (!hosted) args.push('--single-player')
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
  run.results = []
  run.skies = []
  run.sectors = []
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
          else if (m?.type === MSG.GAME_RESULT) run.results.push(m)
          else if (m?.type === MSG.SKY_SET) run.skies.push(m)
          else if (m?.type === MSG.SECTOR_SET) run.sectors.push(m)
        }
      }
    } catch {}
  })()
  await run.cw.write(encodeHello())
  await waitFor(() => run.welcomes >= 1, 8000, 'welcome')
  return run
}

async function stop (run, signal = 'SIGKILL') {
  try { await run.cw.close() } catch {}
  try { run.wt.close() } catch {}
  await run.edge.stop()
  const exited = new Promise((r) => run.server.on('exit', r))
  run.server.kill(signal)
  await Promise.race([exited, sleep(5000)])
}

const lastLeft = (log) => {
  const m = [...log.matchAll(/keys left: (\d)/g)]
  return m.length ? Number(m.at(-1)[1]) : null
}
const leftNow = (run) => lastLeft(run.log)

let current = null
const state = mkdtempSync(join(tmpdir(), 'dnp-persist-'))
try {
  // --- saves ---------------------------------------------------------------
  console.log('saves')
  {
    const run = await start('saves', state)
    check(run.log.includes('[persist_test] placed 4 keys'), 'a fresh level puts down its four keys')
    check(run.log.includes('[placed_test] placed 3 at 14,18,22'), 'CONTROL: and its placed props, all three where authored')
    await waitFor(() => run.log.includes('[placed_test] changed: 2 left at -,30,22'), 6000, 'a placed prop taken, one moved')
    await waitFor(() => leftNow(run) === 3, 6000, 'one key taken')
    await run.cw.write(encodeGameSave(0))
    await waitFor(() => run.results.length >= 1, 4000, 'save result')
    const saved = run.results[0]
    check(saved.ok && saved.op === 'save', `the game is saved (${saved.text})`)
    const savedWith = Number(run.log.match(/saving (\d) keys/)?.[1])
    check(savedWith === 3, `with the three keys still there (${savedWith})`)
    // The level goes on changing after the save.
    await waitFor(() => leftNow(run) === 1, 8000, 'two more taken')
    // CONTROL: by now it really is different from the save.
    check(leftNow(run) === 1, `CONTROL: and the level went on changing after it (${leftNow(run)} left)`)
    const before = run.welcomes
    await run.cw.write(encodeGameLoad(0))
    await waitFor(() => run.results.length >= 2, 6000, 'load result')
    const loaded = run.results[1]
    check(loaded.ok && loaded.op === 'load', `the save is loaded (${loaded.text})`)
    check(run.welcomes === before + 1, 'and the player is put back in the level')
    const restored = [...run.log.matchAll(/restored (\d) keys/g)].map((m) => Number(m[1]))
    check(restored.at(-1) === 3, `the level is back as it was saved: three keys (${restored.at(-1)})`)
    check(run.log.includes('level 0 restored'), 'and the server says it restored it')
    const placedLines = [...run.log.matchAll(/\[placed_test\] placed (.*)/g)].map((m) => m[1])
    check(placedLines.at(-1) === '2 at -,30,22',
          `the placed props are back where they were left, the taken one not (${placedLines.at(-1)})`)

    // A NEW GAME: the world from scratch. The level is placed afresh -- four
    // keys again -- and nothing kept comes back.
    const placedBefore = (run.log.match(/placed 4 keys/g) ?? []).length
    const w0 = run.welcomes
    await run.cw.write(encodeGameNew())
    await waitFor(() => run.results.length >= 3, 6000, 'new game result')
    const fresh = run.results[2]
    check(fresh.ok && fresh.op === 'new', `a new game starts (${fresh.text})`)
    check(run.welcomes === w0 + 1, 'and the player is put in it')
    check((run.log.match(/placed 4 keys/g) ?? []).length === placedBefore + 1,
          'the level is placed afresh: four keys')
    check([...run.log.matchAll(/\[placed_test\] placed (.*)/g)].at(-1)?.[1] === '3 at 14,18,22',
          'and all three placed props, where authored')
    await stop(run)
  }

  // --- none ----------------------------------------------------------------
  console.log('none')
  {
    const run = await start('none', state)
    await run.cw.write(encodeGameSave(0))
    await waitFor(() => run.results.length >= 1, 4000, 'save result')
    const r = run.results[0]
    check(!r.ok, 'a module that does not ask for saves does not get them')
    check(/not enabled/.test(r.text), `and is told why (${r.text})`)
    await stop(run)
  }

  // --- server --------------------------------------------------------------
  console.log('server')
  {
    const dir = join(state, 'server')
    const first = await start('server', dir)
    // CONTROL: a first start has nothing to come back to.
    check(first.log.includes('placed 4 keys'), 'CONTROL: the first start puts down four keys')
    const skyOf = (run) => { const m = run.skies.at(-1); return m ? `${m.kind}:${m.images.join()}:${m.fields.map((f) => f.milli).join()}` : 'none sent' }
    check(skyOf(first) === '1:SKY2:2000', `CONTROL: the fresh start's script gives the level a sky (${skyOf(first)})`)
    await waitFor(() => leftNow(first) === 2, 8000, 'two taken')
    await stop(first, 'SIGTERM')
    check(/level state\(s\) written/.test(first.log), 'stopping writes the level\'s state')

    const second = await start('server', dir)
    check(!second.log.includes('placed 4 keys'), 'the restart does not put down a fresh four')
    const restored = Number(second.log.match(/restored (\d) keys/)?.[1])
    check(restored === 2, `it comes back as it was left: two keys (${restored})`)
    const placedBack = second.log.match(/\[placed_test\] placed (.*)/)?.[1]
    check(placedBack === '2 at -,30,22', `and its placed props as they were left (${placedBack})`)
    check(skyOf(second) === '1:SKY2:2000', `and with the sky its script gave it, which nothing set again (${skyOf(second)})`)
    const looks = (run) => run.sectors.map((m) => m.what === 'light' ? `light ${m.light}` : `${m.what} ${m.texture ?? ''}`).sort().join(', ')
    check(looks(first) === 'ceiling F_SKY1, light 77', `CONTROL: the fresh start's script changed a sector (${looks(first)})`)
    check(looks(second) === 'ceiling F_SKY1, light 77', `and the restart keeps it, from the saved state (${looks(second)})`)
    check(/saved level state\(s\) read/.test(second.log), 'and says where it read them from')

    // A new game forgets the kept state ON DISK too, or the next restart would
    // bring the old game back -- on a game's own server. On a HOSTED one that
    // keeps its world, the lone player on it may not: the world is the host's.
    const file = join(dir, 'levels', 'test.save')
    check(existsSync(file), 'CONTROL: the kept state is on disk before a new game')
    await stop(second)
    const hosted = await start('server', dir, { hosted: true })
    await hosted.cw.write(encodeGameNew())
    await waitFor(() => hosted.results.length >= 1, 6000, 'hosted new game result')
    check(!hosted.results[0].ok && /host/.test(hosted.results[0].text),
          `a hosted world that persists refuses the new game (${hosted.results[0].text})`)
    check(existsSync(file), 'and keeps its state on disk')
    // A SECOND HELLO is ignored: it used to place the player again, at the
    // spawn with full health, the level's on_player_enter run again.
    const w0 = hosted.welcomes
    await hosted.cw.write(encodeHello())
    await sleep(600)
    check(hosted.welcomes === w0, `a second HELLO places nobody again (${hosted.welcomes - w0} welcomes)`)
    await stop(hosted)
    const third = await start('server', dir)
    await third.cw.write(encodeGameNew())
    await waitFor(() => third.results.length >= 1, 6000, 'new game result')
    check(third.results[0].ok && /deleted/.test(third.results[0].text),
          `CONTROL: on the game's own server, a new game deletes it (${third.results[0].text})`)
    check(!existsSync(file), 'and it is gone')
    await stop(third)
  }
} catch (e) {
  check(false, `the run finished (${e.message})`)
  if (current) console.log(current.log.split('\n').slice(-25).join('\n'))
} finally {
  rmSync(state, { recursive: true, force: true })
}

console.log(`\npersist: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
