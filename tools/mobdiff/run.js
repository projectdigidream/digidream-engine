//
// Do the two mob runtimes decide the same things?
//
// maps/scripts/mobs.lua and server/src/mobs/ both walk the same graph and both
// drive the same bodies, and the whole case for the C++ one is that it is
// indistinguishable from the Lua one. That is a strong claim and it deserves a
// strong test: the SAME scenario, from the same seed, through each runtime,
// with the world's own state hash compared TICK BY TICK.
//
// Not the final positions. Two runs that diverge and reconverge are still two
// simulations, and a monster that took a different route for a second and
// arrived anyway is exactly the kind of difference a player would notice and
// an end-state comparison would not.
//
// WHY A HASH AND NOT A FIELD-BY-FIELD DIFF: World::hash() is the number the
// determinism gate is built on -- it already folds every agent's position,
// velocity, state and the rest, and it is the same function three targets are
// required to agree on. Writing a second comparison here would be a second
// opinion about what "the same world" means.
//
// THE CONTROL IS BUILT IN. A comparison of two things that are equal by
// construction proves nothing, so this refuses to report agreement unless the
// C++ run actually took the mobs -- the census line mobs.lua prints -- and
// unless the world changed over the run at all.
//
// And the control for the control: --self runs ONE runtime twice. A divergence
// there is this harness rather than a difference between the runtimes, and
// finding that out afterwards costs a day in the wrong file. It found exactly
// that the first time: without --test-player the client's arrival tick is
// wall-clock dependent, mobs think on `tick % 8`, and two runs start with a
// different set of monsters deciding first -- a phase difference, which no
// re-basing can align.
//
// WHAT IT FOUND, when it first passed its own self-check: `hurt` read the
// current state from `m.st`, which only the Lua path maintains, so a mob the
// C++ loop owned could never flinch. Silently -- a monster that does not
// flinch looks like a monster with a low pain chance.
//
// MEASURED, at 1000 monsters on maps/mobperf.map, three alternating runs each,
// microseconds of Lua per awake mob per tick:
//
//     C++ loop   3.33  3.00  2.74
//     Lua        3.80  4.21  4.01
//
// A quarter off, and the two groups do not overlap -- which is the bar the JIT
// measurement failed and this one has to clear to mean anything. Not the 7x a
// naive reading of the profile suggested, because the actions, the missiles,
// the corpses and the player refresh are all still Lua and are a large part of
// that column.
//
import { spawn } from 'node:child_process'
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { tmpdir } from 'node:os'
import { arch } from 'node:os'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')
const ARCH = arch() === 'arm64' ? 'arm64' : 'x86_64'
const SERVER = process.env.DNP_SERVER_BIN ?? join(ROOT, 'build', 'server', 'dnp-server')

const arg = (name, dflt) => {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt
}
const MAP = arg('map', join(ROOT, 'build', ARCH, 'monsters.dnpm'))
const PROPS = arg('props', join(ROOT, 'modules', 'doom2', 'assets', 'props.txt'))
const TICKS = Number(arg('ticks', 900))
const EVERY = Number(arg('every', 1))

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// ONE RUN. The server plays the level with a client attached, because a level
// with nobody in it is not ticked at all -- and a mob does nothing without
// somebody to hunt, so an empty run would compare two rooms of statues.
async function run (label, env) {
  const port = 34600 + Math.floor(Math.random() * 300)
  // --test-player rather than a real client, and that is the whole reason this
  // comparison is possible at all. A level is not ticked until a session
  // arrives, and its clock keeps counting while it waits -- so the tick a
  // player lands on depends on wall clock, mobs think on `tick % 8`, and two
  // runs start with a different set of monsters deciding first. Not a shift
  // that could be re-based: a phase difference, which never realigns.
  const srv = spawn(SERVER, [
    '--port', String(port), '--map', MAP, '--spawn', 'start', '--props', PROPS,
    '--test-player', '--hash-trace', String(EVERY)
  ], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } })

  let log = ''
  srv.stdout.on('data', (d) => { log += d })
  srv.stderr.on('data', (d) => { log += d })
  const dead = new Promise((res) => srv.on('exit', (c) => res(c)))
  const stop = () => { try { srv.kill('SIGKILL') } catch {} }
  process.on('exit', stop)

  try {
    const deadline = Date.now() + 20000
    while (!/listening for edge/.test(log)) {
      if (Date.now() > deadline) throw new Error(`${label}: server never listened\n${log}`)
      await sleep(20)
    }

    // No client at all. Ticks at 60 Hz, so this is how long the run takes.
    await sleep((TICKS / 60) * 1000 + 500)
  } finally {
    stop()
    await dead
  }

  const hashes = new Map()
  for (const m of log.matchAll(/^\[hash\] (\d+) (\d+) ([0-9A-F]+)$/gm)) {
    hashes.set(`${m[1]}:${m[2]}`, m[3])
  }
  // Per-agent detail, keyed the same way, so a divergence can be read rather
  // than merely detected.
  const agents = new Map()
  for (const m of log.matchAll(/^\[agent\] (\d+) (\d+) (\d+) (-?\d+) (-?\d+) (-?\d+) (\d+) (\d+) (-?\d+)$/gm)) {
    agents.set(`${m[1]}:${m[2]}:${m[3]}`,
               { x: m[4], y: m[5], z: m[6], facing: m[7], anim: m[8], sector: m[9] })
  }
  const census = /mobs: placed (\d+) on the C\+\+ loop, (\d+) on the Lua path/.exec(log)
  return {
    hashes,
    agents,
    cpp: census ? Number(census[1]) : 0,
    lua: census ? Number(census[2]) : 0,
    log
  }
}

// The first tick at which two runs disagree, or null.
function firstDivergence (a, b) {
  const keys = [...a.hashes.keys()].filter((k) => b.hashes.has(k))
  keys.sort((x, y) => {
    const [lx, tx] = x.split(':').map(Number)
    const [ly, ty] = y.split(':').map(Number)
    return lx - ly || tx - ty
  })
  for (const k of keys) {
    if (a.hashes.get(k) !== b.hashes.get(k)) {
      return { at: k, a: a.hashes.get(k), b: b.hashes.get(k) }
    }
  }
  return null
}

console.log(`mobdiff: ${TICKS} ticks of ${MAP.replace(ROOT + '/', '')}, ` +
            `hashed every ${EVERY}`)

// SELF-CHECK FIRST: the same runtime twice.
//
// Before comparing two runtimes it has to be true that ONE of them compares
// equal to itself through this harness. A divergence here is the harness --
// a client that connected a tick later, a port, an ordering -- and reporting
// it as a difference between the runtimes would be a whole day spent in the
// wrong file.
const REPEAT = process.argv.includes('--self')
if (REPEAT) {
  const a = await run('cpp-a', {})
  const b = await run('cpp-b', {})
  const d0 = firstDivergence(a, b)
  console.log(`\n  self-check: ${a.hashes.size} vs ${b.hashes.size} hashes, ` +
              (d0 ? `DIVERGED at ${d0.at}: ${d0.a} vs ${d0.b}` : 'identical'))
  for (let t = 0; t < 8; t++) {
    console.log(`    tick ${t}: A ${a.hashes.get(`0:${t}`)}  B ${b.hashes.get(`0:${t}`)}`)
  }
  // IS IT A SHIFT? A run whose player arrived a tick later produces the same
  // sequence offset by one, which is a harness problem and not a difference
  // between two simulations. Worth asking before reading any physics.
  for (const off of [-6,-5,-4,-3,-2,-1,1,2,3,4,5,6]) {
    let same = 0, n = 0
    for (const [k, v] of a.hashes) {
      const [lv, t] = k.split(':').map(Number)
      const other = b.hashes.get(`${lv}:${t + off}`)
      if (other !== undefined) { n++; if (other === v) same++ }
    }
    if (n > 0 && same > 0) console.log(`    offset ${off}: ${same}/${n} match`)
  }
  process.exit(d0 ? 1 : 0)
}

const cpp = await run('cpp', {})
const lua = await run('lua', { DNP_MOBS_LUA: '1' })

console.log(`\n  C++ run: ${cpp.cpp} mobs on the loop, ${cpp.lua} on the Lua path, ` +
            `${cpp.hashes.size} hashes`)
console.log(`  Lua run: ${lua.cpp} mobs on the loop, ${lua.lua} on the Lua path, ` +
            `${lua.hashes.size} hashes\n`)

// THE CONTROLS FIRST, because the comparison below is worthless without them.
//
// The two runs have to be genuinely different runtimes: one with the mobs on
// the loop and one with them off it. If DNP_MOBS_LUA did nothing, or if the
// loop refused every mob anyway, the diff would compare a run against itself
// and pass forever.
check(cpp.cpp > 0, `the C++ run actually took mobs (${cpp.cpp})`)
check(lua.cpp === 0, `the Lua run took none (${lua.cpp})`)
check(lua.lua > 0, `and ran them itself instead (${lua.lua})`)
check(cpp.hashes.size > 10, `there are hashes to compare (${cpp.hashes.size})`)

// AND THE WORLD HAS TO HAVE MOVED. Two runs of a room where nothing happens
// agree perfectly and mean nothing.
const distinct = new Set([...cpp.hashes.values()]).size
check(distinct > 5, `the world changed over the run (${distinct} distinct hashes)`)

const d = firstDivergence(cpp, lua)
if (d) {
  const [level, tick] = d.at.split(':')
  console.log(`\n  DIVERGED at level ${level} tick ${tick}: ${d.a} vs ${d.b}`)
  // WHICH agent, and which field. A hash says two runs differ; this says what
  // to go and read.
  for (let a = 0; a < 16; a++) {
    const k = `${level}:${tick}:${a}`
    const x = cpp.agents.get(k)
    const y = lua.agents.get(k)
    if (!x && !y) continue
    if (!x || !y) { console.log(`    agent ${a}: present in only one run`); continue }
    const diff = Object.keys(x).filter((f) => x[f] !== y[f])
    if (diff.length === 0) continue
    console.log(`    agent ${a}: ` +
                diff.map((f) => `${f} ${x[f]} vs ${y[f]}`).join(', '))
  }
  // A window, so the first difference can be read as a story rather than a
  // snapshot: what each agent was doing for a few ticks either side.
  const t0 = Math.max(0, Number(tick) - 6 * EVERY)
  console.log(`    window ${t0}..${Number(tick) + 2 * EVERY}  (anim | facing)`)
  for (let t = t0; t <= Number(tick) + 2 * EVERY; t += EVERY) {
    const row = []
    for (let a = 0; a < 8; a++) {
      const x = cpp.agents.get(`${level}:${t}:${a}`)
      const y = lua.agents.get(`${level}:${t}:${a}`)
      if (!x && !y) continue
      const mark = (x && y && x.anim === y.anim && x.facing === y.facing) ? ' ' : '*'
      row.push(`${mark}a${a} ${x ? x.anim : '-'}/${y ? y.anim : '-'}`)
    }
    console.log(`      t${String(t).padStart(4)}  ${row.join('  ')}`)
  }

  // And the tick before, so the first difference can be read against the state
  // it came out of.
  const prev = Number(tick) - EVERY
  for (let a = 0; a < 16; a++) {
    const x = cpp.agents.get(`${level}:${prev}:${a}`)
    const y = lua.agents.get(`${level}:${prev}:${a}`)
    if (!x || !y) continue
    const diff = Object.keys(x).filter((f) => x[f] !== y[f])
    if (diff.length) console.log(`    (tick ${prev}) agent ${a}: ` +
                                 diff.map((f) => `${f} ${x[f]} vs ${y[f]}`).join(', '))
  }
}
check(d === null, 'the two runtimes produce the same world, tick by tick')

console.log(`\nmobdiff: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
