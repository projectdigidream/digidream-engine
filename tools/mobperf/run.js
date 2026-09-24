//
// Where a tick goes, against the number of monsters in it.
//
//   node tools/mobperf/run.js                    # 50, 200, 500, 1000
//   node tools/mobperf/run.js --n 1000 --secs 12
//
// WHY THIS EXISTS. "Some ticks are taking way too long" arrived as a number
// measured on an Android emulator, with the native server answering `0 stalls`
// on the same level. Neither figure answers the question that matters -- what
// does a monster cost, and what happens at a thousand of them -- and this
// repository has a named trap for optimising a cost nobody measured.
//
// WHAT IT DOES. Spawns the real server on maps/mobperf.map, connects to its
// LINK socket as the edge would, opens one session, and drives it with input
// bundles at 60 Hz. The server reports `[perf]` lines split into Lua, physics
// and snapshot; this parses them and prints the curve.
//
// NO EDGE, NO WEBTRANSPORT, NO BROWSER. The link is plain TCP carrying the
// frames server/edge/src/framing.js defines, and server/edge/src/upstream.js is
// already a client for it -- so this is the edge minus the part that needs a
// certificate. That is not a shortcut: WebTransport in the loop would add a
// second process and its own jitter to a measurement about the server.
//
// WHAT IT DELIBERATELY DOES NOT MEASURE. The client. Rendering, prediction and
// the 46-entity snapshot budget are real limits and are not this file's
// question; see the plan. This measures the authoritative tick.
//
import { spawn } from 'node:child_process'
import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Upstream } from '../../server/edge/src/upstream.js'
import { encodeHello, encodeInputBundle, axisFromNumber } from '../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')

const args = process.argv.slice(2)
const arg = (name, fallback) => {
  const i = args.indexOf(`--${name}`)
  return i >= 0 && args[i + 1] !== undefined ? args[i + 1] : fallback
}
const COUNTS = arg("n", null) ? [Number(arg("n"))] : [50, 150, 250, 350, 500]
const SECS = Number(arg('secs', 8))
const PORT = Number(arg('port', 34700))

const arch = process.arch === 'x64' ? 'x86_64' : 'arm64'
const MAP = join(ROOT, 'build', arch, 'mobperf.dnpm')
const LUA = join(ROOT, 'build', arch, 'mobperf.lua')
const SERVER = process.env.DNP_SERVER_BIN ?? join(ROOT, 'build', 'server', 'dnp-server')
const PROPS = join(ROOT, 'modules', 'doom2', 'assets', 'props.txt')

for (const [what, p] of [['map', MAP], ['script', LUA], ['server', SERVER], ['props', PROPS]]) {
  if (!existsSync(p)) {
    console.error(`no ${what} at ${p}`)
    console.error('  build it:  ./scripts/build_server.sh')
    console.error(`             ./build/${arch}/mapc maps/mobperf.map build/${arch}/mobperf.dnpm`)
    process.exit(2)
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

/**
 * Set the monster count in the script BESIDE THE BAKED MAP, not in maps/.
 *
 * The server loads a level's scripts from its own directory -- that is what
 * mapc's copies are for -- so rewriting the copy changes the next run without
 * re-baking, and leaves the source in maps/ alone. Writing to maps/mobperf.lua
 * instead would make every run a modification of a tracked file.
 */
function setCount (n, dormant) {
  const src = readFileSync(LUA, 'utf8')
  let out = src.replace(/^local N = \d+$/m, `local N = ${n}`)
  out = out.replace(/^local DORMANT = (?:true|false)$/m, `local DORMANT = ${dormant}`)
  // CHECKED, because a silent no-op here is a whole sweep measuring the
  // previous sweep's numbers -- and it would look entirely plausible.
  if (!new RegExp(`^local N = ${n}$`, 'm').test(out) ||
      !new RegExp(`^local DORMANT = ${dormant}$`, 'm').test(out)) {
    throw new Error(`could not set N/DORMANT in ${LUA}`)
  }
  writeFileSync(LUA, out)
}

/** One run: N monsters, SECS seconds, and the numbers the server reported. */
async function measure (n, dormant = false) {
  setCount(n, dormant)

  const srv = spawn(SERVER, [
    '--port', String(PORT), '--map', MAP, '--spawn', 'start', '--props', PROPS,
    // Every 120 ticks: two seconds of averaging, so one slow tick does not
    // become the whole answer, and enough reports per run to see it settle.
    '--perf', '120'
  ], { stdio: ['ignore', 'pipe', 'pipe'] })

  let log = ''
  srv.stdout.on('data', (d) => { log += d })
  srv.stderr.on('data', (d) => { log += d })
  const dead = new Promise((res) => srv.on('exit', (c) => res(c)))

  const stop = () => { try { srv.kill('SIGKILL') } catch {} }
  process.on('exit', stop)

  try {
    await Promise.race([
      (async () => {
        // The server binds after it has loaded the map, built the nav graph and
        // run the level's Lua -- which for 1000 monsters is not instant.
        const deadline = Date.now() + 30000
        while (!/listening for edge/.test(log)) {
          if (Date.now() > deadline) throw new Error('server never listened')
          await sleep(25)
        }
      })(),
      dead.then((c) => { throw new Error(`server exited ${c}\n${log}`) })
    ])

    const up = new Upstream({ host: '127.0.0.1', port: PORT, reconnectMs: 100 })
    const opened = new Promise((res) => up.once('connected', res))
    up.connect()
    await opened
    up.sessionOpen(1, { path: '/dnp' })
    up.send(0x03 /* LINK.CONTROL */, 1, encodeHello())

    // A HELD INPUT, not a still one. A player standing still is a player the
    // monsters walk to and then stop against, which measures the pile-up and
    // not the chase. Walking keeps them steering, pathing and re-facing, which
    // is the expensive state and the one the question is about.
    const zero = axisFromNumber(0)
    let clientTick = 0
    const timer = setInterval(() => {
      clientTick++
      const t = clientTick / 60
      const frame = {
        buttons: 0,
        moveX: axisFromNumber(Math.cos(t * 0.7)),
        moveY: axisFromNumber(Math.sin(t * 0.7)),
        aim: 0
      }
      try {
        up.send(0x04 /* LINK.DATAGRAM */, 1,
                encodeInputBundle({ clientTick, ackSnapshotTick: 0, frames: [frame] }))
      } catch { /* the run is ending */ }
    }, 1000 / 60)

    await sleep(SECS * 1000)
    clearInterval(timer)
    up.stop()
    void zero
  } finally {
    stop()
    await dead
  }

  const asked = /\[mobperf\] asked for (\d+), spawned (\d+), refused (\d+), dormant (\w+)/.exec(log)
  if (asked && (asked[4] === 'true') !== dormant) {
    throw new Error(`asked for dormant=${dormant} and the level reported ${asked[4]}`)
  }
  const rows = [...log.matchAll(
    /\[perf\] (\d+) ticks {2}agents (\d+) {2}lua (\d+) us {2}step (\d+) us {2}snap (\d+) us {2}total (\d+) us of (\d+) {2}worst (\d+) us/g)]

  return {
    n,
    spawned: asked ? Number(asked[2]) : 0,
    refused: asked ? Number(asked[3]) : 0,
    // The FIRST report is dropped: it covers the ticks where the session was
    // still arriving and the monsters had not yet acquired a target, which is
    // not the state being measured.
    rows: rows.slice(1).map((m) => ({
      agents: Number(m[2]), lua: Number(m[3]), step: Number(m[4]),
      snap: Number(m[5]), total: Number(m[6]), budget: Number(m[7]), worst: Number(m[8])
    })),
    log
  }
}

const med = (xs) => {
  if (xs.length === 0) return 0
  const s = [...xs].sort((a, b) => a - b)
  return s[Math.floor(s.length / 2)]
}

const sweep = async (dormant) => {
  const out = []
  console.log(`\n  ${dormant ? 'DORMANT -- physics only, population stable'
                             : 'AWAKE   -- physics + the mob runtime'}`)
  console.log('     N  spawned   agents      lua      step      snap     total   of budget')
  console.log('  ' + '-'.repeat(72))
  for (const n of COUNTS) {
    const r = await measure(n, dormant)
    if (r.rows.length === 0) {
      console.log(`  ${String(n).padStart(4)}  no [perf] reports -- the run produced nothing`)
      console.log(r.log.split('\n').slice(-12).join('\n'))
      process.exit(1)
    }
    const pick = (k) => med(r.rows.map((x) => x[k]))
    const lua = pick('lua'); const step = pick('step'); const snap = pick('snap')
    const total = pick('total'); const agents = pick('agents')

    // THE PLAYER GOT LOCKED OUT, and without this the run reports a number for
    // a state nobody asked about.
    //
    // MAX_AGENTS is 384 and it is shared between players and monsters -- a
    // monster is an agent because it runs the player's mover. So 384 monsters
    // take every slot, the arriving session gets none, mobs.lua sees
    // `#players == 0` and puts the whole level to sleep, and the tick gets
    // CHEAPER. Printing that beside the others would be reporting the cost of
    // an empty room as the cost of a full one.
    if (snap === 0 && r.spawned > 0) {
      console.log(`  ${String(n).padStart(4)}  ${String(r.spawned).padStart(7)}  ` +
                  `${String(agents).padStart(6)}   NOT MEASURED -- the agent table filled ` +
                  `and the player never got a slot`)
      console.log(`                        MAX_AGENTS is ${384}, shared with players; ` +
                  `${r.refused} refused, then the level slept`)
      continue
    }
    out.push({ n, spawned: r.spawned, agents, lua, step, snap, total })
    console.log(
      `  ${String(n).padStart(4)}  ${String(r.spawned).padStart(7)}  ${String(agents).padStart(6)}  ` +
      `${String(lua).padStart(6)}us  ${String(step).padStart(6)}us  ${String(snap).padStart(6)}us  ` +
      `${String(total).padStart(6)}us  ${(total / r.rows[0].budget * 100).toFixed(1).padStart(7)}%` +
      (r.refused ? `   (${r.refused} refused)` : ''))
  }
  return out
}

console.log(`mobperf: ${COUNTS.join(', ')} monsters, ${SECS}s each, on maps/mobperf.map`)

const dormant = await sweep(true)
const awake = await sweep(false)

// THE SHAPE OF THE PHYSICS, from the dormant sweep, where the population holds
// still and nothing is deciding anything.
//
// FITTED, NOT ASSUMED. The first version of this projected quadratically
// because `World::resolve_agent_collisions` is a flat double loop with no
// broadphase -- and the measurement said per-agent cost is very nearly FLAT,
// so the projection was 2x too pessimistic. The pair loop is O(n^2) in
// iterations, but each iteration is a cheap distance reject, and at these
// counts the per-agent fixed work dominates it: `physics::resolve_slabs` scans
// EVERY sector for EVERY agent, and this fixture has 144.
//
// So the model is  per-agent = A + B*n  -- A the fixed per-agent work, B the
// pair loop -- fitted by least squares, and both terms reported. Assuming the
// shape is what made the first answer wrong, and assuming it the other way
// would be the same mistake with a friendlier number.
if (dormant.length >= 3) {
  console.log('\n  physics per agent, from the DORMANT sweep:\n')
  console.log('    agents    step/agent')
  for (const r of dormant) {
    console.log(`    ${String(r.agents).padStart(6)}    ${(r.step / r.agents).toFixed(2).padStart(6)} us`)
  }

  const xs = dormant.map((r) => r.agents)
  const ys = dormant.map((r) => r.step / r.agents)
  const n = xs.length
  const mx = xs.reduce((a, b) => a + b, 0) / n
  const my = ys.reduce((a, b) => a + b, 0) / n
  let num = 0; let den = 0
  for (let k = 0; k < n; k++) { num += (xs[k] - mx) * (ys[k] - my); den += (xs[k] - mx) ** 2 }
  const B = den === 0 ? 0 : num / den        // us per agent, per other agent
  const A = my - B * mx                      // us per agent, fixed

  console.log(`\n    fitted:  per-agent = ${A.toFixed(2)} us + ${B.toFixed(4)} us x agents`)
  console.log(`      ${A.toFixed(2)} us is the FIXED per-agent work -- step_agent, the`)
  console.log(`      query_surface calls, and resolve_slabs scanning all 144 sectors.`)
  console.log(`      ${B.toFixed(4)} us x agents is the pair loop in resolve_agent_collisions.`)
  const at = (N) => (A + B * N) * N / 1000
  const share = (N) => (B * N) / (A + B * N) * 100
  console.log(`\n    the pair loop is ${share(dormant[dormant.length - 1].agents).toFixed(0)}% of ` +
              `per-agent cost at ${dormant[dormant.length - 1].agents} agents, ` +
              `${share(1000).toFixed(0)}% at 1000.`)

  console.log(`\n  projected -- A PROJECTION FROM A FIT, NOT A MEASUREMENT:`)
  console.log(`    MAX_AGENTS is 384, so 1000 cannot be run today at all.`)
  for (const N of [384, 600, 1000]) {
    console.log(`    ${String(N).padStart(4)} agents:  step ~${at(N).toFixed(1)} ms` +
                (at(N) > 16.67 ? '   OVER a 16.7 ms tick on its own' : ''))
  }
}

// What the mob runtime itself costs: the difference between the two sweeps at
// the same N. Taking Lua's figure alone would miss that an awake monster is
// also more expensive to SIMULATE -- it moves, so it collides differently.
if (dormant.length && awake.length) {
  console.log('\n  what being awake costs, per agent per tick:\n')
  console.log('     N     lua     step    total')
  for (const w of awake) {
    const d = dormant.find((x) => x.n === w.n)
    if (!d || !w.agents) continue
    console.log(`  ${String(w.n).padStart(4)}  ${((w.lua - d.lua) / w.agents).toFixed(2).padStart(6)}us  ` +
                `${((w.step - d.step) / w.agents).toFixed(2).padStart(6)}us  ` +
                `${((w.total - d.total) / w.agents).toFixed(2).padStart(6)}us`)
  }
}
