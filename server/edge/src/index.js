#!/usr/bin/env node
//
// Edge entry point.
//
//   node src/index.js [--port 4433] [--upstream-port 34500] [--path /dnp]
//                     [--cert cert.pem --key key.pem]   (a real certificate; the dev one otherwise)
//
import { existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Edge } from './edge.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const CERTS = join(HERE, '..', 'certs')

const argv = process.argv.slice(2)
const arg = (name, fallback) => {
  const i = argv.indexOf(`--${name}`)
  return i >= 0 && argv[i + 1] ? argv[i + 1] : fallback
}

if (!arg('cert') && !existsSync(join(CERTS, 'cert.pem'))) {
  console.error('no certificate found -- run: npm run certs')
  process.exit(1)
}

const edge = new Edge({
  port: Number(arg('port', 4433)),
  host: arg('host', '0.0.0.0'),
  path: arg('path', '/dnp'),
  upstreamHost: arg('upstream-host', '127.0.0.1'),
  upstreamPort: Number(arg('upstream-port', 34500)),
  certPath: arg('cert', join(CERTS, 'cert.pem')),
  keyPath: arg('key', join(CERTS, 'key.pem'))
})

await edge.start()

const shutdown = async () => {
  console.log('\n[edge] shutting down')
  await edge.stop()
  process.exit(0)
}
process.on('SIGINT', shutdown)
process.on('SIGTERM', shutdown)
