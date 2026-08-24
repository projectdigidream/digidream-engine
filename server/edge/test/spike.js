//
// WebTransport viability spike.
//
// The plan named "WebTransport in C++" as the project's longest-lead unknown.
// This answers the part of it that actually needed answering: can we terminate
// WebTransport, in this stack, with BOTH channels the protocol requires --
// unreliable datagrams for the state channel and a reliable ordered stream for
// the control channel (docs/protocol.md section 1) -- and does the self-signed
// certificate path work without a CA.
//
// Runs server and client in one process. Exits non-zero on any failure.
//
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Http3Server, WebTransport } from '@fails-components/webtransport'

const HERE = dirname(fileURLToPath(import.meta.url))
const CERTS = join(HERE, '..', 'certs')
const PORT = 34433
const PATH = '/dnp'

let failures = 0
const check = (ok, label) => {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) failures++
}

const cert = readFileSync(join(CERTS, 'cert.pem'))
const privKey = readFileSync(join(CERTS, 'key.pem'))
const certHash = Buffer.from(
  JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64,
  'base64'
)

const withTimeout = (promise, ms, label) =>
  Promise.race([
    promise,
    new Promise((_, rej) => setTimeout(() => rej(new Error(`timeout: ${label}`)), ms))
  ])

// --------------------------------------------------------------- server side

const server = new Http3Server({
  port: PORT,
  host: '127.0.0.1',
  secret: 'dnp-dev-spike',
  cert,
  privKey
})

server.startServer()
await withTimeout(server.ready, 5000, 'server ready')
console.log(`server listening on https://127.0.0.1:${PORT}${PATH}`)

// Echo whatever arrives, on both channels.
const serveOne = async () => {
  const reader = server.sessionStream(PATH).getReader()
  const { value: session } = await reader.read()
  await session.ready

  // Control channel: reliable, ordered bidirectional stream.
  ;(async () => {
    const bidiReader = session.incomingBidirectionalStreams.getReader()
    const { value: stream } = await bidiReader.read()
    const r = stream.readable.getReader()
    const w = stream.writable.getWriter()
    for (;;) {
      const { value, done } = await r.read()
      if (done) break
      await w.write(value) // echo
    }
  })().catch(() => {})

  // State channel: unreliable datagrams.
  ;(async () => {
    const r = session.datagrams.readable.getReader()
    const w = session.datagrams.createWritable().getWriter()
    for (;;) {
      const { value, done } = await r.read()
      if (done) break
      await w.write(value) // echo
    }
  })().catch(() => {})

  return session
}

const serverSessionPromise = serveOne()

// --------------------------------------------------------------- client side

console.log('\nconnecting client with serverCertificateHashes (no CA involved)')
const client = new WebTransport(`https://127.0.0.1:${PORT}${PATH}`, {
  serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }]
})

try {
  await withTimeout(client.ready, 8000, 'client ready')
  check(true, 'session established over self-signed cert')
} catch (err) {
  check(false, `session established over self-signed cert (${err.message})`)
  process.exit(1)
}

// --- reliable control channel ---
{
  const stream = await withTimeout(client.createBidirectionalStream(), 5000, 'open bidi')
  const w = stream.writable.getWriter()
  const r = stream.readable.getReader()

  // Three frames in order. The control channel's guarantee is ordering, so a
  // reordered echo here would invalidate the channel split in section 1 of the
  // protocol before a single line of it was implemented.
  const sent = ['control-1', 'control-2', 'control-3']
  for (const s of sent) await w.write(new TextEncoder().encode(s))

  const received = []
  const decoder = new TextDecoder()
  let buf = ''
  while (received.length < sent.length) {
    const { value, done } = await withTimeout(r.read(), 5000, 'bidi echo')
    if (done) break
    buf += decoder.decode(value, { stream: true })
    while (buf.length >= 9) {
      received.push(buf.slice(0, 9))
      buf = buf.slice(9)
    }
  }
  check(received.length === 3, `control channel: received ${received.length}/3 frames`)
  check(
    JSON.stringify(received) === JSON.stringify(sent),
    `control channel: order preserved (${received.join(',')})`
  )
}

// --- unreliable state channel ---
{
  // createWritable(), not the deprecated `.writable` accessor.
  const w = client.datagrams.createWritable().getWriter()
  const r = client.datagrams.readable.getReader()

  const COUNT = 20
  for (let i = 0; i < COUNT; i++) {
    const dg = new Uint8Array(8)
    new DataView(dg.buffer).setUint32(0, i, true)
    await w.write(dg)
  }

  // Datagrams may legitimately be lost or reordered -- that is the point of the
  // channel. Assert only that the path works at all, not that it is lossless.
  let got = 0
  const deadline = Date.now() + 3000
  try {
    while (got < COUNT && Date.now() < deadline) {
      const { value, done } = await withTimeout(r.read(), 1500, 'datagram echo')
      if (done) break
      if (value) got++
    }
  } catch { /* expected once the echoes stop arriving */ }

  check(got > 0, `state channel: ${got}/${COUNT} datagrams echoed (loss is acceptable here)`)
}

// --- max datagram size: drives the snapshot budget, protocol open question 1 ---
{
  const size = client.datagrams.maxDatagramSize
  console.log(`\n  maxDatagramSize = ${size} bytes`)
  check(
    typeof size === 'number' && size > 0,
    'maxDatagramSize reported (bounds the per-snapshot byte budget)'
  )
}

client.close()
await serverSessionPromise.catch(() => {})
server.stopServer()

console.log(`\n${failures === 0 ? 'SPIKE PASSED' : `SPIKE FAILED (${failures})`}`)
process.exit(failures === 0 ? 0 : 1)
