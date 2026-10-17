//
// THE TIME SCALE (server --time-scale, DNP_TIME_SCALE): the game runs slower or
// faster in wall time -- for automated testing, a tester who looks at the game
// every few seconds -- and the client is told so in WELCOME.
//
// Over the link, as tools/mobperf does: the server's own tick pacing is the
// question, and an edge would only add its jitter to it. Snapshots carry the
// server's tick, so ticks per wall second is read straight off them.
//
import { spawn } from 'node:child_process'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Upstream } from '../src/upstream.js'
import { encodeHello, decodeControlMessage, decodeSnapshot, MSG } from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const arch = process.arch === 'x64' ? 'x86_64' : 'arm64'
const MAP = join(ROOT, 'build', arch, 'test.dnpm')

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// One run: the server at `args`/`env`, a session, and how fast its tick goes.
async function run (port, args, env = {}) {
  const srv = spawn(serverBin, ['--port', String(port), '--map', MAP, '--spawn', 'start', ...args],
                    { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, DNP_TIME_SCALE: '', ...env } })
  let log = ''
  srv.stdout.on('data', (d) => { log += d })
  srv.stderr.on('data', (d) => { log += d })
  const dead = new Promise((res) => srv.on('exit', res))
  try {
    for (let t = 0; !/listening for edge/.test(log); t++) {
      if (t > 400) throw new Error(`server never listened\n${log}`)
      await sleep(25)
    }
    const up = new Upstream({ host: '127.0.0.1', port, reconnectMs: 100 })
    let welcome = null
    const ticks = []
    up.on('frame', (f) => {
      if (f.type === 0x03 && !welcome) {
        const m = decodeControlMessage(f.payload)
        if (m && m.type === MSG.WELCOME) welcome = m
      } else if (f.type === 0x04) {
        const s = decodeSnapshot(f.payload)
        if (s) ticks.push({ at: performance.now(), tick: s.serverTick })
      }
    })
    const opened = new Promise((res) => up.once('connected', res))
    up.connect()
    await opened
    up.sessionOpen(1, { path: '/dnp' })
    up.send(0x03, 1, encodeHello())
    await sleep(3000)
    up.stop()
    // Ticks a second over the last two seconds (the first is the session arriving).
    const late = ticks.filter((t) => t.at >= ticks[ticks.length - 1].at - 2000)
    const rate = late.length >= 2
      ? (late[late.length - 1].tick - late[0].tick) / ((late[late.length - 1].at - late[0].at) / 1000)
      : 0
    return { welcome, rate, log }
  } finally {
    try { srv.kill('SIGKILL') } catch {}
    await dead
  }
}

console.log('time scale: the game at a quarter speed, and the client told')
{
  const quarter = await run(34691, ['--time-scale', '0.25'])
  console.log(`  --time-scale 0.25: ${quarter.rate.toFixed(1)} ticks a second; WELCOME says ${quarter.welcome?.timeScale}`)
  check(quarter.welcome !== null && quarter.welcome.timeScale === 0.25, 'WELCOME carries the scale (0.25)')
  check(quarter.rate > 12 && quarter.rate < 18, `the server ticks at a quarter of 60 (${quarter.rate.toFixed(1)})`)
  check(/time scale 0\.25/.test(quarter.log), 'and says so as it starts')
  // The environment, for an app or a stack launched with it set.
  const env = await run(34692, [], { DNP_TIME_SCALE: '2' })
  console.log(`  DNP_TIME_SCALE=2: ${env.rate.toFixed(1)} ticks a second; WELCOME says ${env.welcome?.timeScale}`)
  check(env.welcome?.timeScale === 2 && env.rate > 105 && env.rate < 135, 'DNP_TIME_SCALE=2: twice as fast, and said')
  // CONTROL: without it, real time -- so the quarter above is the flag's doing.
  const real = await run(34693, [])
  console.log(`  no scale: ${real.rate.toFixed(1)} ticks a second; WELCOME says ${real.welcome?.timeScale}`)
  check(real.welcome?.timeScale === 1 && real.rate > 52 && real.rate < 68, 'CONTROL: no scale, 60 ticks a second and 1 said')
}

console.log(`\ntimescale: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
