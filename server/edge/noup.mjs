import { readFileSync } from 'node:fs'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '~/DoomNetPlatformer/server/edge/src/edge.js'
const CERTS = '~/DoomNetPlatformer/server/edge/certs'
const edge = new Edge({
  port: 34640, host: '127.0.0.1', path: '/dnp',
  upstreamPort: 34641,          // nothing is listening there
  cert: readFileSync(`${CERTS}/cert.pem`), privKey: readFileSync(`${CERTS}/key.pem`),
  log: (...a) => console.log('[edge]', ...a)
})
await edge.start()
const hash = Buffer.from(JSON.parse(readFileSync(`${CERTS}/cert-hash.json`,'utf8')).sha256_base64,'base64')
const wt = new WebTransport('https://127.0.0.1:34640/dnp',
  { serverCertificateHashes: [{ algorithm: 'sha-256', value: hash }] })
await wt.ready
console.log('client: transport up')
const closed = await Promise.race([
  wt.closed.then((i) => `closed: ${JSON.stringify(i)}`).catch((e) => `closed(err): ${e.message}`),
  new Promise((r) => setTimeout(() => r('STILL WAITING after 3s'), 3000))
])
console.log('client:', closed)
await edge.stop(); process.exit(0)
