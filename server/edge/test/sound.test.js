//
// SOUNDS on the wire, against a real server (docs/protocol.md §5.7).
//
// The fixture (maps/scripts/sound_test.lua) makes three sounds every half
// second: one beside the player, one far outside earshot, and one from the
// player themself marked as predicted. What this asserts:
//
//   1. a sound arrives, under the hash of its NAME -- the number the client's
//      own soundHash computes, so neither side keeps a table of ids;
//   2. one out of earshot does not arrive at all (CONTROL: it was made, at the
//      same moment as the near one, by the same script);
//   3. the player's own predicted sound carries their handle and both flags,
//      which is what lets their client drop the copy it already played.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import {
  encodeHello, decodeControlMessage, decodeSnapshot, encodeInputBundle, MSG,
  soundHash, SOUND_FOLLOW, SOUND_PREDICTED, PREDICT, BUTTON
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
const MAP = baked('test.dnpm')
if (!MAP) { console.error('no baked test map'); process.exit(2) }
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

// One server, an edge in front of it, one client sending idle input (or the
// buttons `press(tick)` returns).
async function session (args, press = () => 0, ms = 2500) {
  const server = spawn(serverBin, ['--port', String(LINK_PORT), '--map', MAP, ...args],
                       { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
  const run = { log: '', sounds: [], predicts: [], welcome: null }
  server.stdout.on('data', (d) => { run.log += d.toString() })
  server.stderr.on('data', (d) => { run.log += d.toString() })
  let edge = null
  let wt = null
  try {
    await waitFor(() => run.log.includes('listening for edge'), 8000, 'server')
    edge = new Edge({
      port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
      cert: readFileSync(join(CERTS, 'cert.pem')),
      privKey: readFileSync(join(CERTS, 'key.pem')),
      log: () => {}
    })
    await edge.start()
    await waitFor(() => run.log.includes('edge connected'), 8000, 'edge')
    wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
      serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
    })
    await wt.ready
    const control = await wt.createBidirectionalStream()
    const cw = control.writable.getWriter()
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
            if (m?.type === MSG.WELCOME) run.welcome = m
            else if (m?.type === MSG.SOUND_PREDICT) run.predicts.push(m)
          }
        }
      } catch {}
    })()
    let lastTick = 0
    ;(async () => {
      const r = wt.datagrams.readable.getReader()
      try {
        for (;;) {
          const { value, done } = await r.read()
          if (done) break
          const s = decodeSnapshot(Buffer.from(value))
          if (!s) continue
          lastTick = s.serverTick
          for (const so of s.sounds) run.sounds.push(so)
        }
      } catch {}
    })()
    await cw.write(encodeHello())
    await waitFor(() => run.welcome !== null, 8000, 'welcome')
    // Inputs, so the session is a player the world is stepping for.
    const dw = wt.datagrams.createWritable().getWriter()
    let tick = 0
    let n = 0
    const frames = []
    const end = Date.now() + ms
    while (Date.now() < end) {
      tick = Math.max(tick + 1, lastTick + 8)
      frames.unshift({ buttons: press(n++), moveX: 0, moveY: 0, aim: 0 })
      frames.length = Math.min(frames.length, 12)
      await dw.write(encodeInputBundle({ clientTick: tick, ackSnapshotTick: lastTick, frames }))
      await sleep(16)
    }
  } catch (e) {
    check(false, `the run finished (${e.message})`)
    console.log(run.log.split('\n').slice(-20).join('\n'))
  } finally {
    try { wt?.close() } catch {}
    await edge?.stop()
    const exited = new Promise((r) => server.on('exit', r))
    server.kill('SIGKILL')
    await Promise.race([exited, sleep(3000)])
  }
  return run
}

// --- the wire --------------------------------------------------------------
console.log('wire')
{
  const run = await session(['--script', join(ROOT, 'maps', 'scripts', 'sound_test.lua')])
  const { sounds, welcome } = run
  const near = sounds.filter((s) => s.sound === soundHash('test.near'))
  const far = sounds.filter((s) => s.sound === soundHash('test.far'))
  const self = sounds.filter((s) => s.sound === soundHash('test.self'))
  console.log(`  ${sounds.length} sounds received: ${near.length} near, ${far.length} far, ${self.length} own`)
  check(near.length >= 3, `a sound arrives, under the hash of its name (${near.length})`)
  check(far.length === 0, 'one out of earshot does not arrive')
  check(near.length > 0 && near.every((s) => s.source === 0 && s.flags === 0),
        'a sound at a place comes from nobody and follows nothing')
  check(self.length >= 3, `the player's own sound arrives too (${self.length})`)
  check(self.length > 0 && self.every((s) => (s.source >>> 0) === (welcome.selfHandle >>> 0)),
        'from the player: their handle')
  check(self.length > 0 && self.every((s) => (s.flags & SOUND_FOLLOW) && (s.flags & SOUND_PREDICTED)),
        'following them, and marked as predicted -- so their client drops it')
  // CONTROL for the hash: a name the server never sent matches nothing.
  check(sounds.every((s) => s.sound !== soundHash('test.never')), 'CONTROL: no sound carries a name nobody made')
}

// --- prediction ------------------------------------------------------------
//
// maps/scripts/player_sounds.lua as a game script: the client is told what to
// play on its own jump, and the server's copy of that jump comes marked.
console.log('prediction')
{
  // Jump once a second.
  const run = await session(['--game-script', join(ROOT, 'maps', 'scripts', 'player_sounds.lua')],
                            (n) => (n % 60 < 4 ? BUTTON.JUMP : 0), 3500)
  const move = run.predicts.find((m) => m.group === soundHash('move'))
  check(!!move, `the client is told what its own movement sounds like (${run.predicts.length} message(s))`)
  const jump = move?.entries.find((e) => e.trigger === PREDICT.JUMP)
  const land = move?.entries.find((e) => e.trigger === PREDICT.LAND)
  check(jump?.sound === soundHash('player.jump'), 'its jump')
  check(land?.sound === soundHash('player.oof') && land.param === 12 * 16,
        `and a hard landing, at 12 units a second (${land?.param})`)
  const jumps = run.sounds.filter((s) => s.sound === soundHash('player.jump'))
  check(jumps.length >= 2, `jumping makes the server's copy (${jumps.length})`)
  check(jumps.length > 0 && jumps.every((s) => (s.flags & SOUND_PREDICTED) &&
                                       (s.source >>> 0) === (run.welcome.selfHandle >>> 0)),
        'marked predicted and from the jumper, so their own client drops it')
  // CONTROL: standing still, nobody jumps.
  const still = await session(['--game-script', join(ROOT, 'maps', 'scripts', 'player_sounds.lua')],
                              () => 0, 2000)
  check(still.sounds.filter((s) => s.sound === soundHash('player.jump')).length === 0,
        'CONTROL: standing still makes no jump')
}

console.log(`\nsound: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
