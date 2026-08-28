//
// What a client is told when the game server is not running.
//
// The edge is its own process. It stays up, listens, and accepts WebTransport
// sessions perfectly happily when the C++ server behind it is down -- so a
// browser genuinely connects, and then waits forever for a WELCOME nobody is
// going to send. The client's own message for that state is "connected;
// waiting for WELCOME...", which is accurate about the transport and says
// nothing at all about which of the two processes to go and look at.
//
// That cost a debugging session: a server refusing to start over a missing
// script, with no observer anywhere downstream. sessionOpen is fire-and-forget
// and returns false when there is no link, so nothing had to notice.
//
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = Number(process.env.DNP_WT_PORT ?? 34644)
// Deliberately nothing listening here. That IS the fixture.
const DEAD_UPSTREAM = Number(process.env.DNP_LINK_PORT ?? 34645)

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

const edge = new Edge({
  port: WT_PORT, host: '127.0.0.1', path: '/dnp', upstreamPort: DEAD_UPSTREAM,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  log: () => {}
})
await edge.start()

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const wt = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`,
  { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })

await wt.ready
// The transport really does come up. Without this the assertions below could
// pass because the connection failed outright, which is a different problem
// with a different fix.
check(true, 'the transport connects -- the edge is up and accepting')

const outcome = await Promise.race([
  wt.closed.then((i) => ({ closed: true, info: i })).catch((e) => ({ closed: true, info: e })),
  new Promise((r) => setTimeout(() => r({ closed: false }), 4000))
])

check(outcome.closed, 'and the session is closed rather than left hanging')
const reason = outcome.info?.reason ?? outcome.info?.message ?? ''
console.log(`  close reason: ${JSON.stringify(reason)}`)
check(/upstream|server/i.test(reason),
      'with a reason naming the missing game server, not a bare code')

await edge.stop()
console.log(`\nnoupstream: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
