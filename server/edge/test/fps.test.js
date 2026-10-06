//
// FIRST-PERSON CONTROLS, against a real server (client/src/controls.js).
//
// The module decides the scheme (module.txt `controls fps`; --controls here).
// What this asserts, each against the same run on a platformer server:
//
//   1. WELCOME says which scheme it is;
//   2. the push is relative to the view: aiming EAST and pushing forward walks
//      east -- where a platformer's same frame walks north, the world's y;
//   3. the pitch a client sends reaches the server, where a script reads it
//      (maps/scripts/fps_test.lua prints e:pitch());
//   4. and the body faces the aim at once, which is what lets the client's
//      view and the simulated body never disagree.
//
import { spawn } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import {
  encodeHello, decodeControlMessage, decodeSnapshot, encodeInputBundle, axisFromNumber, fxToNumber, MSG
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

// One server, one client sending `frame(n)` every 16 ms for `ms`; what came back.
async function session (args, frame, ms) {
  const server = spawn(serverBin, ['--port', String(LINK_PORT), '--map', MAP,
                                   '--script', join(ROOT, 'maps', 'scripts', 'fps_test.lua'), ...args],
                       { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
  const run = { log: '', welcome: null, path: [], facing: [] }
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
          if (!s?.self?.pos) continue
          lastTick = s.serverTick
          run.path.push({ x: fxToNumber(s.self.pos.x), y: fxToNumber(s.self.pos.y) })
          run.facing.push(s.self.facing >>> 0)
        }
      } catch {}
    })()
    await cw.write(encodeHello())
    await waitFor(() => run.welcome !== null, 8000, 'welcome')
    const dw = wt.datagrams.createWritable().getWriter()
    let tick = 0
    let n = 0
    const frames = []
    const end = Date.now() + ms
    while (Date.now() < end) {
      tick = Math.max(tick + 1, lastTick + 8)
      frames.unshift(frame(n++))
      frames.length = Math.min(frames.length, 12)
      await dw.write(encodeInputBundle({ clientTick: tick, ackSnapshotTick: lastTick, frames }))
      await sleep(16)
    }
    await sleep(200)
  } catch (e) {
    check(false, `the run finished (${e.message})`)
    console.log(run.log.split('\n').slice(-15).join('\n'))
  } finally {
    try { wt?.close() } catch {}
    await edge?.stop()
    const exited = new Promise((r) => server.on('exit', r))
    server.kill('SIGKILL')
    await Promise.race([exited, sleep(3000)])
  }
  return run
}

// Aim EAST (brad 0), push FORWARD (the stick's y), look up a little: the frame
// whose meaning differs between the two schemes.
const EAST_FORWARD = () => ({ buttons: 0, moveX: 0, moveY: axisFromNumber(1), aim: 0, pitch: 3000 })
const moved = (run) => {
  const a = run.path[Math.min(5, run.path.length - 1)]
  const b = run.path.at(-1)
  return a && b ? { dx: b.x - a.x, dy: b.y - a.y } : { dx: 0, dy: 0 }
}

console.log('fps')
{
  const run = await session(['--controls', 'fps'], EAST_FORWARD, 2500)
  check(run.welcome?.controls === 1, `WELCOME says the scheme is fps (${run.welcome?.controls})`)
  check(run.welcome?.debug === 1, `and, with no module saying otherwise, to show the debug overlay (${run.welcome?.debug})`)
  const m = moved(run)
  console.log(`  aim east, push forward: moved (${m.dx.toFixed(2)}, ${m.dy.toFixed(2)})`)
  check(m.dx > 2 && Math.abs(m.dy) < 0.2, 'the push is relative to the view: it walked east')
  check(run.facing.length > 0 && run.facing.at(-1) === 0, `and the body faces the aim exactly (${run.facing.at(-1)})`)
  const pitches = [...run.log.matchAll(/controls fps pitch (-?\d+)\/65536/g)].map((x) => Number(x[1]))
  check(pitches.length > 0 && pitches.every((p) => p === 3000),
        `the pitch it sent is the pitch the server reads (${pitches.join(', ')})`)
}

console.log('platformer')
{
  const run = await session([], EAST_FORWARD, 2500)
  check(run.welcome?.controls === 0, `CONTROL: a platformer server says so (${run.welcome?.controls})`)
  const m = moved(run)
  console.log(`  aim east, push forward: moved (${m.dx.toFixed(2)}, ${m.dy.toFixed(2)})`)
  check(m.dy > 2 && Math.abs(m.dx) < 0.2, 'CONTROL: the same frame walks the world\'s y -- north')
  check(/controls platformer pitch/.test(run.log), 'and a script there is told it is a platformer')
}

console.log(`\nfps: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
