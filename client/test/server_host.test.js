//
// The web build's server (client/wasm/dist/dnp-server.mjs) under Node, as the
// page's Worker runs it (client/src/server_host.js): the engine's test level in its file
// system, a session opened and a HELLO sent as the edge would, input at 60 Hz.
// It must answer with WELCOME and the map, and send snapshots as it ticks.
//
//   source emsdk/emsdk_env.sh && scripts/build_server_web.sh && node client/test/server_host.test.js
//
// CONTROL: a second server given no HELLO says nothing at all, so the WELCOME
// above is an answer, not something it sends by itself.
//
import { readFileSync, readdirSync, existsSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { startServer } from '../src/server_host.js'
import { LINK, encodeLinkFrame, createLinkReader, decodeLinkFrame, createControlReader } from '../../server/edge/src/framing.js'
import { encodeHello, encodeInputBundle, MSG } from '../src/proto.js'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
const ARCH = process.arch === 'arm64' ? 'arm64' : 'x86_64'
const BUILT = join(ROOT, 'client', 'wasm', 'dist', 'dnp-server.mjs')
let checks = 0
let fails = 0
const check = (ok, what) => { checks++; if (!ok) fails++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}`) }

if (!existsSync(BUILT)) {
  console.log('web server: skip (run scripts/build_server_web.sh)')
  process.exit(0)
}
const createModule = (await import(pathToFileURL(BUILT).href)).default

// The level and everything beside it (the scripts mapc copied), and the props.
const files = []
for (const f of readdirSync(join(ROOT, 'build', ARCH))) {
  if (/\.(dnpm|lua|json)$/.test(f)) files.push([`/work/maps/${f}`, readFileSync(join(ROOT, 'build', ARCH, f))])
}
files.push(['/work/assets/props.txt', readFileSync(join(ROOT, 'assets', 'props.txt'))])

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

async function session (hello) {
  const log = []
  const host = await startServer({
    createModule, files, cwd: '/work', args: ['--map', '/work/maps/test.dnpm'], onLog: (l) => log.push(l)
  })
  const links = createLinkReader()
  const controls = createControlReader()
  const seen = { control: [], datagrams: 0 }
  host.run((bytes) => {
    for (const f of links.push(bytes)) {
      const { type, payload } = decodeLinkFrame(f)
      if (type === LINK.CONTROL) for (const m of controls.push(payload)) seen.control.push(m[4])
      else if (type === LINK.DATAGRAM) seen.datagrams++
    }
  })
  host.feed(encodeLinkFrame(LINK.SESSION_OPEN, 1, new TextEncoder().encode('{"path":"/dnp"}')))
  if (hello) host.feed(encodeLinkFrame(LINK.CONTROL, 1, encodeHello()))
  let clientTick = 0
  const input = setInterval(() => {
    host.feed(encodeLinkFrame(LINK.DATAGRAM, 1, encodeInputBundle({
      clientTick: ++clientTick, ackSnapshotTick: 0, frames: [{ buttons: 0, moveX: 0, moveY: 0, aim: 0 }]
    })))
  }, 1000 / 60)
  const t0 = Date.now()
  await sleep(3000)
  clearInterval(input)
  host.stop()
  return { ...seen, secs: (Date.now() - t0) / 1000, log }
}

console.log('web server: the real server as wasm, through server/web/host.js')
const live = await session(true)
const welcome = live.control.includes(MSG.WELCOME)
console.log(`  ${live.control.length} control messages (${[...new Set(live.control)].map((t) => '0x' + t.toString(16)).join(' ')}), ` +
            `${live.datagrams} datagrams in ${live.secs.toFixed(1)} s`)
check(welcome, 'a HELLO is answered with WELCOME')
check(live.control.includes(MSG.MAP_DATA ?? 0x10), 'and the map is sent')
check(live.datagrams >= 45, `snapshots arrive as it ticks (${live.datagrams} in 3 s, ~30 a second)`)
if (!welcome) console.log(live.log.slice(-15).join('\n'))

const quiet = await session(false)
check(quiet.control.length === 0, `CONTROL: no HELLO, nothing said (${quiet.control.length} control messages)`)

console.log(`\nweb server: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
