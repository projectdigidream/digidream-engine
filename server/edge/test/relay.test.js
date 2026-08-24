//
// End-to-end relay test: WebTransport client -> edge -> link socket -> mock
// upstream -> back again, on both channels.
//
// The mock upstream stands in for the C++ server, which does not exist yet.
// That substitution is the point: if the edge is a dumb pipe as intended, it
// cannot tell the difference, and this test stays valid when the real server
// arrives.
//
import net from 'node:net'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import {
  encodeControlMessage, encodeLinkFrame, createLinkReader, createControlReader,
  decodeLinkFrame, decodeControlMessage, LINK
} from '../src/framing.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const CERTS = join(HERE, '..', 'certs')
const WT_PORT = 34435
const LINK_PORT = 34505

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

const withTimeout = (p, ms, label) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`timeout: ${label}`)), ms))])

const waitFor = async (fn, ms, label) => {
  const deadline = Date.now() + ms
  while (Date.now() < deadline) {
    if (fn()) return true
    await new Promise((r) => setTimeout(r, 20))
  }
  throw new Error(`timeout: ${label}`)
}

// ---------------------------------------------------- mock authoritative server

const seen = { open: [], close: [], control: [], datagram: [] }

const upstreamServer = net.createServer((sock) => {
  sock.setNoDelay(true)
  const reader = createLinkReader()
  sock.on('data', (chunk) => {
    for (const raw of reader.push(chunk)) {
      const f = decodeLinkFrame(raw)
      if (f.type === LINK.SESSION_OPEN) {
        seen.open.push(f.sessionId)
      } else if (f.type === LINK.SESSION_CLOSE) {
        seen.close.push(f.sessionId)
      } else if (f.type === LINK.CONTROL) {
        seen.control.push(decodeControlMessage(f.payload))
        // Echo the whole framed control message straight back down.
        sock.write(encodeLinkFrame(LINK.CONTROL, f.sessionId, f.payload))
      } else if (f.type === LINK.DATAGRAM) {
        seen.datagram.push(Buffer.from(f.payload))
        sock.write(encodeLinkFrame(LINK.DATAGRAM, f.sessionId, f.payload))
      }
    }
  })
  sock.on('error', () => {})
})

await new Promise((res) => upstreamServer.listen(LINK_PORT, '127.0.0.1', res))

// ------------------------------------------------------------------ edge + client

const edge = new Edge({
  port: WT_PORT,
  host: '127.0.0.1',
  path: '/dnp',
  upstreamPort: LINK_PORT,
  cert: readFileSync(join(CERTS, 'cert.pem')),
  privKey: readFileSync(join(CERTS, 'key.pem')),
  log: () => {}
})
await edge.start()

const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64'
)

const client = new WebTransport(`https://127.0.0.1:${WT_PORT}/dnp`, {
  serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
})
await withTimeout(client.ready, 8000, 'client ready')
check(true, 'client connected through edge')

await waitFor(() => seen.open.length === 1, 3000, 'SESSION_OPEN upstream')
check(seen.open.length === 1, 'upstream saw SESSION_OPEN')

// --- control channel round trip ---------------------------------------------
{
  const stream = await withTimeout(client.createBidirectionalStream(), 5000, 'open bidi')
  const w = stream.writable.getWriter()
  const r = stream.readable.getReader()

  const HELLO = 0x01
  const body = Buffer.from(JSON.stringify({ protocol_version: 0, client_id: 'abc' }))
  const framed = encodeControlMessage(HELLO, body)

  // Split the message across two writes, straddling the length prefix, so the
  // edge's reassembly is actually exercised rather than incidentally skipped.
  await w.write(framed.subarray(0, 2))
  await new Promise((r2) => setTimeout(r2, 50))
  await w.write(framed.subarray(2))

  await waitFor(() => seen.control.length === 1, 3000, 'control upstream')
  check(seen.control.length === 1, 'upstream received exactly one control message')
  check(seen.control[0]?.type === HELLO, 'control message type survived the split write')
  check(
    seen.control[0]?.body.toString() === body.toString(),
    'control message body survived the split write'
  )

  const framer = createControlReader()
  const echoed = []
  while (echoed.length === 0) {
    const { value, done } = await withTimeout(r.read(), 5000, 'control echo')
    if (done) break
    echoed.push(...framer.push(Buffer.from(value)))
  }
  check(echoed.length === 1, 'control echo arrived back at the client')
  check(decodeControlMessage(echoed[0]).type === HELLO, 'control echo type intact')
}

// --- state channel round trip ------------------------------------------------
{
  const w = client.datagrams.createWritable().getWriter()
  const r = client.datagrams.readable.getReader()

  const dg = Buffer.alloc(16)
  dg.writeUInt8(0x80, 0)        // INPUT_BUNDLE
  dg.writeUInt32LE(12345, 1)    // client_tick
  await w.write(dg)

  await waitFor(() => seen.datagram.length >= 1, 3000, 'datagram upstream')
  check(seen.datagram.length >= 1, 'upstream received the datagram')
  check(seen.datagram[0]?.readUInt8(0) === 0x80, 'datagram type byte intact')
  check(seen.datagram[0]?.readUInt32LE(1) === 12345, 'datagram payload intact')

  const { value } = await withTimeout(r.read(), 5000, 'datagram echo')
  check(value && Buffer.from(value).readUInt32LE(1) === 12345, 'datagram echo arrived back')
}

// --- session teardown --------------------------------------------------------
{
  client.close()
  await waitFor(() => seen.close.length === 1, 5000, 'SESSION_CLOSE upstream')
  check(seen.close.length === 1, 'upstream saw SESSION_CLOSE on client disconnect')
}

await edge.stop()
upstreamServer.close()

console.log(`\nrelay: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
