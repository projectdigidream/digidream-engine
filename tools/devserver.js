//
// A static file server for the browser client.
//
// Plain HTTP on localhost, which browsers treat as a secure context -- so
// WebTransport, wasm and modules all work without a certificate for the PAGE.
// The WebTransport connection to the edge needs its own cert, and has one.
//
// Deliberately small and dependency-free: this exists so the client can be
// looked at, and a dev server that needs its own toolchain is one more thing
// between a change and seeing it.
//
import { createServer } from 'node:http'
import { readFile, stat } from 'node:fs/promises'
import { dirname, join, normalize, extname } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..')
const PORT = Number(process.env.DNP_HTTP_PORT ?? 8080)

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.dnpm': 'application/octet-stream',
  '.map': 'application/json',
  '.png': 'image/png'
}

// Prefixes served, and what they resolve to. An explicit list rather than
// serving the repository root: this process has the whole source tree under it,
// including certificates and Lua the server runs, and none of that has any
// business being fetchable.
const MOUNTS = [
  ['/vendor/three/', join(ROOT, 'client', 'node_modules', 'three', 'build')],
  ['/src/', join(ROOT, 'client', 'src')],
  // Texture images, keyed by the NAME a map uses. The map carries names and the
  // client resolves them (§4) -- which is what lets one map serve clients with
  // different asset sets, and keeps megabytes of images off the control channel
  // that every join waits on.
  ['/textures/', join(ROOT, 'assets', 'textures')],
  // client/wasm, not client/wasm/dist: core.js imports '../wasm/dist/...',
  // which is the layout on disk, and a mount that skipped the dist level would
  // make the source's own relative import wrong only when served.
  ['/wasm/', join(ROOT, 'client', 'wasm')],
  // NOTE: /maps/ is NOT a plain mount -- see findMap below.
  ['/', join(ROOT, 'client')]
]

function resolve (urlPath) {
  const clean = normalize(decodeURIComponent(urlPath.split('?')[0]))
  // normalize() collapses ".." but a path that still contains one has escaped
  // its mount, and the honest response is to refuse rather than to keep
  // rewriting until it looks safe.
  if (clean.includes('..')) return null
  for (const [prefix, dir] of MOUNTS) {
    if (!clean.startsWith(prefix)) continue
    const rest = clean.slice(prefix.length) || 'index.html'
    const full = join(dir, rest)
    if (!full.startsWith(dir)) return null
    return full
  }
  return null
}

// Maps are served out of build/, where mapc writes, and the architecture
// subdirectory varies by machine -- so a bare name is searched for.
//
// The per-architecture directories are searched FIRST and build/ itself only as
// a fallback. That ordering is not cosmetic: build/ accumulates artefacts from
// older conventions, and an early version of this served a stale build/
// test.dnpm from a previous map format in preference to the one the test
// harness had just compiled. The map version check caught it -- correctly, and
// confusingly, because the client looked broken when it was reading a file
// nothing had written this month.
async function findMap (name) {
  if (name.includes('/') || name.includes('..')) return null
  for (const dir of [join(ROOT, 'build', 'arm64'), join(ROOT, 'build', 'x86_64'),
                     join(ROOT, 'build', 'native'), join(ROOT, 'build')]) {
    const p = join(dir, name)
    try {
      await stat(p)
      return p
    } catch {}
  }
  return null
}

// Every request, logged. A browser that stops halfway through an import chain
// says nothing useful on the page; the server's view of which file it asked for
// last says exactly where it stopped.
const log = (code, url) => console.log(`[dev] ${code} ${url}`)

createServer(async (req, res) => {
  try {
    // The development edge's certificate HASH, so a browser can accept exactly
    // that self-signed certificate via serverCertificateHashes.
    //
    // Public by construction -- the server presents the certificate itself on
    // every connection -- so this is not a secret being handed out. The private
    // key sits in the same directory and is deliberately not reachable through
    // any mount, which is why this is one named route rather than serving
    // certs/.
    if (req.url === '/certhash') {
      const p = join(ROOT, 'server', 'edge', 'certs', 'cert-hash.json')
      const body = await readFile(p).catch(() => null)
      log(body ? 200 : 404, req.url)
      if (!body) { res.writeHead(404).end('no dev certificate; run npm run certs in server/edge'); return }
      res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' }).end(body)
      return
    }

    let file
    if (req.url.startsWith('/maps/')) {
      file = await findMap(req.url.slice('/maps/'.length).split('?')[0])
    } else {
      file = resolve(req.url)
    }
    if (!file) { log(404, req.url); res.writeHead(404).end('not found'); return }
    const body = await readFile(file)
    log(200, req.url)
    res.writeHead(200, {
      'content-type': TYPES[extname(file)] ?? 'application/octet-stream',
      'cache-control': 'no-store',
      // wasm threads are not used, but these are what a browser wants before it
      // will hand out a SharedArrayBuffer, and setting them now costs nothing.
      'cross-origin-opener-policy': 'same-origin',
      'cross-origin-embedder-policy': 'require-corp'
    }).end(body)
  } catch (e) {
    log(e.code === 'ENOENT' ? 404 : 500, req.url)
    res.writeHead(e.code === 'ENOENT' ? 404 : 500).end(String(e.message))
  }
}).listen(PORT, () => {
  console.log(`[dev] http://localhost:${PORT}/`)
  console.log('[dev] maps are served from build/<arch>/ -- run mapc first')
})
