//
// Generates a self-signed certificate for local WebTransport development.
//
// WebTransport requires TLS even on localhost, but it also offers a way out of
// needing a real CA: a browser will accept a self-signed certificate if the
// page passes its SHA-256 digest via `serverCertificateHashes`. That path has
// three hard constraints, all of which this script satisfies and none of which
// produce a useful error message when violated:
//
//   * the key MUST be ECDSA P-256 (RSA is rejected outright)
//   * the validity period MUST be at most 14 days (we use 13)
//   * the digest MUST be of the DER encoding, not the PEM
//
// Regenerate every couple of weeks. The expiry is short by design -- it is what
// stops a development certificate from quietly becoming a production one.
//
import { execFileSync } from 'node:child_process'
import { mkdirSync, writeFileSync, readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const CERT_DIR = join(HERE, '..', 'certs')

mkdirSync(CERT_DIR, { recursive: true })

const keyPath = join(CERT_DIR, 'key.pem')
const certPath = join(CERT_DIR, 'cert.pem')

execFileSync('openssl', [
  'req', '-x509',
  '-newkey', 'ec',
  '-pkeyopt', 'ec_paramgen_curve:prime256v1',
  '-nodes',
  '-keyout', keyPath,
  '-out', certPath,
  '-days', '13',
  '-subj', '/CN=localhost',
  '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'
], { stdio: ['ignore', 'ignore', 'inherit'] })

// Digest the DER form. Hashing the PEM instead yields a plausible-looking value
// that simply never matches, with no diagnostic beyond a failed handshake.
const der = execFileSync('openssl', ['x509', '-in', certPath, '-outform', 'der'])
const { createHash } = await import('node:crypto')
const digest = createHash('sha256').update(der).digest()

const meta = {
  generated: new Date().toISOString(),
  expires: new Date(Date.now() + 13 * 864e5).toISOString(),
  sha256_base64: digest.toString('base64'),
  sha256_hex: digest.toString('hex')
}
writeFileSync(join(CERT_DIR, 'cert-hash.json'), JSON.stringify(meta, null, 2) + '\n')

console.log(`cert:    ${certPath}`)
console.log(`key:     ${keyPath}`)
console.log(`sha-256: ${meta.sha256_base64}`)
console.log(`expires: ${meta.expires}`)
