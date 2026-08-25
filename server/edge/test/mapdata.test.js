//
// MAP_DATA transfer, end to end.
//
// This is what retires the compiled-in test map. Up to now client and server
// agreed on geometry because both had a copy of the same source file; after
// this they agree because they parsed the same BYTES, which is a much stronger
// property and the one the design actually requires.
//
// The assertion that matters is not "a map arrived" but "the map the client
// parsed is bit-identical to the server's" -- checked through the content hash,
// which is computed independently on each side from its own parsed structures.
//
import { spawn } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { Core } from '../../../client/src/core.js'
import { MapAccumulator } from '../../../client/src/maploader.js'
import { decodeControlMessage, encodeHello, MSG } from '../../../client/src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34446)
const LINK_PORT = Number(process.env.DNP_LINK_PORT ?? 34520)
const MAP_PATH = process.env.DNP_MAP ?? join(ROOT, 'build', 'arm64', 'test.dnpm')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const withTimeout = (p, ms, l) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`timeout: ${l}`)), ms))])
const waitFor = async (fn, ms, l) => {
  const end = Date.now() + ms
  while (Date.now() < end) { if (fn()) return true; await sleep(20) }
  throw new Error(`timeout: ${l}`)
}

// --- unit: the accumulator refuses what the ordered channel cannot produce ---

{
  const a = new MapAccumulator()
  const data = new Uint8Array(100).map((_, i) => i & 0xff)
  check(a.push({ totalBytes: 100, offset: 0, chunk: data.subarray(0, 60) }).ok, 'accumulator: first chunk')
  check(!a.complete, 'accumulator: incomplete until the last chunk')
  check(a.push({ totalBytes: 100, offset: 60, chunk: data.subarray(60) }).ok, 'accumulator: second chunk')
  check(a.complete, 'accumulator: complete')
  check(Buffer.compare(Buffer.from(a.bytes()), Buffer.from(data)) === 0, 'accumulator: reassembled exactly')

  // The control channel is ordered and reliable, so a gap is not a lost packet.
  const b = new MapAccumulator()
  b.push({ totalBytes: 100, offset: 0, chunk: data.subarray(0, 60) })
  check(!b.push({ totalBytes: 100, offset: 80, chunk: data.subarray(80) }).ok,
        'accumulator: an out-of-order chunk is fatal, not tolerated')

  const c = new MapAccumulator()
  check(!c.push({ totalBytes: 1 << 30, offset: 0, chunk: data }).ok,
        'accumulator: an implausible size is refused before allocating')

  const d = new MapAccumulator()
  d.push({ totalBytes: 100, offset: 0, chunk: data.subarray(0, 60) })
  check(!d.push({ totalBytes: 100, offset: 60, chunk: new Uint8Array(80) }).ok,
        'accumulator: a chunk running past the declared size is refused')
}

// --- end to end --------------------------------------------------------------

const server = spawn(join(ROOT, 'build', 'server', 'dnp-server'),
                     ['--port', String(LINK_PORT), '--map', MAP_PATH],
                     { stdio: ['ignore', 'inherit', 'pipe'] })
let serverLog = ''
server.stderr.on('data', (d) => { serverLog += d.toString() })
process.on('exit', () => { try { server.kill('SIGKILL') } catch {} })

await waitFor(() => serverLog.includes('listening for edge'), 5000, 'server')
check(serverLog.includes('sectors,'), `server loaded the baked map (${MAP_PATH})`)

const edge = new Edge({
  port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK_PORT,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  log: () => {}
})
await edge.start()
await waitFor(() => serverLog.includes('edge connected'), 8000, 'edge')

const core = await Core.load()
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')

const wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
  serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
})
await withTimeout(wt.ready, 8000, 'client ready')
const control = await wt.createBidirectionalStream()
const cw = control.writable.getWriter()

let welcome = null
const acc = new MapAccumulator()
let accError = null

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
        if (m?.type === MSG.WELCOME) welcome = m
        else if (m?.type === MSG.MAP_DATA) {
          const res = acc.push(m)
          if (!res.ok) accError = res.reason
        }
      }
    }
  } catch {}
})()

await cw.write(encodeHello())
await waitFor(() => welcome, 8000, 'welcome')
check(welcome.mapHash !== 0n, `WELCOME carries a map hash (${welcome.mapHash.toString(16)})`)

await waitFor(() => acc.complete || accError, 8000, 'map transfer')
check(accError === null, `map transferred without error (${accError ?? 'clean'})`)
check(acc.complete, `map fully received (${acc.received}/${acc.total} bytes)`)

// The bytes the client received must be byte-identical to the file on disk.
{
  const onDisk = readFileSync(MAP_PATH)
  check(Buffer.compare(Buffer.from(acc.bytes()), onDisk) === 0,
        'received bytes are identical to the baked file')
}

// ...and parsing them must produce the same geometry the server has. The hash
// is computed independently on each side from its own parsed structures, so
// equality here means the two really did end up with the same world -- not
// merely that the same bytes moved.
{
  const { ok, status, map } = core.loadMap(acc.bytes())
  check(ok, `client parsed the map (status ${status})`)
  if (ok) {
    check(map.hash === welcome.mapHash,
          `client hash matches WELCOME.map_hash (${map.hash.toString(16)})`)
    check(map.sectorCount === 3, `client sees ${map.sectorCount} sectors`)

    // Spawn tags survive the round trip, which is what portals will use to
    // place arrivals (design 3.3).
    const start = map.spawnForTag('start')
    check(start.x === 10n * 4294967296n && start.y === 10n * 4294967296n,
          'named spawn point resolves correctly')
    const missing = map.spawnForTag('does-not-exist')
    check(missing.x === start.x,
          'an unknown tag falls back rather than failing the join')

    // And a world driven by the received map runs.
    const world = core.createWorld({ map })
    const agent = world.spawn(start)
    for (let i = 0; i < 60; i++) { world.setInput(agent, {}); world.step() }
    const st = world.readAgent(agent)
    check(st.grounded, 'an agent spawned into the received map falls and lands')
    check(st.sector >= 0, `and is inside a sector (${st.sector})`)
    world.destroy()
    map.destroy()
  }
}

try { wt.close() } catch {}
await edge.stop()
server.kill('SIGTERM')

console.log(`\nmapdata: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
