//
// Which entities a snapshot spends its budget on.
//
// A datagram is 1200 bytes and a moving monster costs about 23 of them, so
// roughly forty-six entities fit. On a level with more than that, WHICH
// forty-six is the whole question -- and `docs/protocol.md` section 5.4 has
// always answered it: "sorts candidates by priority -- distance from the
// observer, then time since that entity was last sent".
//
// IT DID NOT DO THAT. Entities were ordered near-then-far against a radius of
// 64 engine units -- 3072 Doom map units, most of a level -- and within each
// half by entity INDEX. Entity slots are handed out in spawn order, so the
// forty-six a player was told about were the forty-six the map script happened
// to place first, for ever, however far away they had wandered. And once the
// near half alone overflowed the budget the far half was never reached at all,
// so the round-robin cursor that was supposed to stop anything starving
// advanced every tick and was read by nothing.
//
// WHY THE FIXTURE SPAWNS OUTSIDE IN. maps/mobperf.lua places monster 1 furthest
// from the player and monster N nearest, which costs its own measurement
// nothing and is the only way this test can exist: spawning nearest-first would
// make index order and distance order agree, and "the lowest forty-six indices"
// and "the nearest forty-six" would be the same set. The bug would pass.
//
// No edge and no WebTransport. This talks link frames straight to the server
// with server/edge/src/upstream.js, which is what the edge does minus the part
// that needs a certificate.
//
import { spawn } from 'node:child_process'
import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Upstream } from '../src/upstream.js'
import { LINK } from '../src/framing.js'
import { encodeHello, encodeInputBundle, decodeSnapshot, axisFromNumber, handleIndex }
  from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
// The repository: the engine is ROOT, and modules, docs, the studio and the
// synth are beside it.
const REPO = join(ROOT, '..')
const PORT = Number(process.env.DNP_ORDER_PORT ?? 34690)

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

const arch = process.arch === 'x64' ? 'x86_64' : 'arm64'
const MAP = join(ROOT, 'build', arch, 'mobperf.dnpm')
const LUA = join(ROOT, 'build', arch, 'mobperf.lua')
const SERVER = process.env.DNP_SERVER_BIN ?? join(ROOT, 'build', 'server', 'dnp-server')
const PROPS = join(REPO, 'modules', 'doom2', 'assets', 'props.txt')

if (!existsSync(MAP) || !existsSync(LUA)) {
  // Skipped loudly. The fixture is baked by scripts/test.sh alongside the other
  // maps; a silent pass here would be a claim about ordering that nothing checked.
  console.log(`  skip: no ${MAP} -- bake maps/mobperf.map first`)
  console.log('\nsnapshot order: 0 checks, 0 failures')
  process.exit(0)
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// Many more monsters than a datagram can carry, so the budget really does have
// to choose. 250 against a ceiling of ~46 is a clear five-to-one.
const N = 250
writeFileSync(LUA, readFileSync(LUA, 'utf8')
  .replace(/^local N = \d+$/m, `local N = ${N}`)
  .replace(/^local DORMANT = (?:true|false)$/m, 'local DORMANT = true'))

const srv = spawn(SERVER, ['--port', String(PORT), '--map', MAP, '--spawn', 'start',
                           '--props', PROPS],
                  { stdio: ['ignore', 'pipe', 'pipe'] })
let log = ''
srv.stdout.on('data', (d) => { log += d })
srv.stderr.on('data', (d) => { log += d })
process.on('exit', () => { try { srv.kill('SIGKILL') } catch {} })

const seen = []          // entity indices, per snapshot
let spawned = 0

try {
  const deadline = Date.now() + 30000
  while (!/listening for edge/.test(log)) {
    if (Date.now() > deadline) throw new Error(`server never listened\n${log}`)
    await sleep(25)
  }
  spawned = Number(/\[mobperf\] asked for \d+, spawned (\d+)/.exec(log)?.[1] ?? 0)

  const up = new Upstream({ host: '127.0.0.1', port: PORT, reconnectMs: 100 })
  const opened = new Promise((res) => up.once('connected', res))
  up.on('frame', ({ type, payload }) => {
    if (type !== LINK.DATAGRAM) return
    const snap = decodeSnapshot(payload)
    if (snap && snap.entities.length > 0) {
      seen.push(snap.entities.map((e) => handleIndex(e.handle)))
    }
  })
  up.connect()
  await opened
  up.sessionOpen(1, { path: '/dnp' })
  up.send(LINK.CONTROL, 1, encodeHello())

  let clientTick = 0
  const timer = setInterval(() => {
    clientTick++
    up.send(LINK.DATAGRAM, 1, encodeInputBundle({
      clientTick, ackSnapshotTick: 0,
      frames: [{ buttons: 0, moveX: axisFromNumber(0), moveY: axisFromNumber(0), aim: 0 }]
    }))
  }, 1000 / 60)

  await sleep(4000)
  clearInterval(timer)
  up.stop()
} finally {
  try { srv.kill('SIGKILL') } catch {}
}

console.log('--- the budget has to choose ---')
check(spawned >= 200, `${spawned} monsters spawned, against a datagram that holds ~46`)
check(seen.length > 5, `${seen.length} snapshots carried entities`)

if (seen.length === 0) {
  console.log(log.split('\n').slice(-15).join('\n'))
  console.log(`\nsnapshot order: ${checks} checks, ${fails + 1} failures`)
  process.exit(1)
}

// The fixture spawns OUTSIDE IN, so a high index is a near monster. The player
// holds a low agent slot, and the monsters fill the rest in spawn order.
const later = seen.slice(Math.floor(seen.length / 2))     // after it has settled
const flat = later.flat()
const median = (xs) => { const s = [...xs].sort((a, b) => a - b); return s[Math.floor(s.length / 2)] }

console.log('\n--- it spends the budget on the NEAREST, not the lowest-numbered ---')
{
  const mid = median(flat)
  const half = spawned / 2
  check(mid > half,
        `the median entity sent is index ${mid}, in the near half (> ${half.toFixed(0)})`)

  // THE CONTROL, and it is the whole test. Before this change the answer was
  // the lowest indices -- the FURTHEST monsters -- because the order within the
  // near set was the entity index. If the median came out below half, that is
  // precisely the old behaviour, and this assertion is what tells them apart.
  check(mid > spawned * 0.6,
        `and well into it (${mid} of ${spawned}); the old order put this near 1`)

  const lowest = flat.filter((i) => i <= 20).length
  check(lowest < flat.length * 0.2,
        `the twenty furthest monsters are not hogging it (${lowest} of ${flat.length} sends)`)
}

console.log('\n--- and nothing is starved outright ---')
{
  // The ageing term: an entity past the budget has its effective distance
  // halved every 8 unsent snapshots, so the far field gets in eventually. A
  // strict distance sort would never send monster 1 at all.
  const distinct = new Set(flat)
  check(distinct.size > 46,
        `more distinct entities were sent over time (${distinct.size}) than fit in one snapshot`)

  const perSnap = median(later.map((s) => s.length))
  check(perSnap > 10 && perSnap < 200,
        `each snapshot carries a budget's worth (${perSnap} entities)`)
}

console.log(`\nsnapshot order: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
