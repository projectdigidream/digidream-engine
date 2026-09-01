//
// The packaged app's static file server.
//
// WHY A SERVER AT ALL, INSIDE A DESKTOP APP. The client is plain ES modules
// with an import map and a wasm binary (client/index.html says why there is no
// bundler). Loaded over file:// a browser refuses module imports cross-origin,
// will not stream wasm, and treats the page as an opaque origin -- so the
// options are "bundle the client for the desktop build" or "serve it". Bundling
// would put a compiled artefact between the source and what runs, and it would
// mean the desktop build runs different bytes from the browser build. Serving
// it on loopback means both run the same files.
//
// It binds to 127.0.0.1 on an EPHEMERAL port. Not a fixed one: two copies of
// the game should not fight over a number, and a fixed port is also a thing
// other software on the machine can reach. Loopback-only is what keeps this off
// the network and, on Windows, out of the firewall prompt.
//
// The route table is deliberately the same shape as tools/devserver.js's, and
// for the same reason: an explicit list, because the alternative is serving a
// directory that has more in it than the client should ever be able to fetch.
//
import { createServer } from 'node:http'
import { readFile } from 'node:fs/promises'
import { join, normalize, extname } from 'node:path'

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.dnpm': 'application/octet-stream',
  '.png': 'image/png',
  '.txt': 'text/plain; charset=utf-8',
  // A baked track is opaque bytes; only the synth ever reads one.
  '.mstk': 'application/octet-stream'
}

/**
 * @param root  the staged game directory (see scripts/build_app.sh)
 * @param modules  module directory names, in load order
 */
export function startStatic ({ root, modules = [], log = () => {} }) {
  const MOUNTS = [
    ['/vendor/three/', join(root, 'client', 'vendor', 'three')],
    ['/src/', join(root, 'client', 'src')],
    ['/wasm/', join(root, 'client', 'wasm')],
    // The synth and its instruments, staged by scripts/build_app.sh.
    //
    // /synth/ is the worklet AND its dist/ sibling: synth-worklet.js imports
    // './dist/midisynth.mjs' relatively, so the two cannot be separated.
    ['/synth/', join(root, 'synth')],
    ['/synth-content/', join(root, 'synth-content')],
    ['/music/', join(root, 'assets', 'music')]
  ]

  // The first module that has this file, or the engine's own copy. Per FILE and
  // not per directory, exactly as docs/MODULES.md specifies: a module that adds
  // three sprites does not have to carry the other two thousand.
  const moduleFile = async (rel) => {
    for (const m of modules) {
      const p = join(root, 'modules', m, rel)
      if (await readFile(p).then(() => true).catch(() => false)) return p
    }
    return join(root, rel)
  }

  const resolve = (url) => {
    const clean = normalize(decodeURIComponent(url.split('?')[0]))
    if (clean.includes('..')) return null
    if (clean === '/' || clean === '/index.html') return join(root, 'client', 'index.html')
    for (const [route, dir] of MOUNTS) {
      if (clean.startsWith(route)) return join(dir, clean.slice(route.length))
    }
    return null
  }

  const server = createServer(async (req, res) => {
    try {
      const path = req.url.split('?')[0]

      // The two catalogues are CONCATENATED across modules, never chosen
      // between -- several modules load at once and each contributes entries.
      // Ids are absolute and are never renumbered here; the server refuses to
      // start on a collision, which is the one place that check can be made.
      for (const [route, rel] of [['/animations.txt', 'assets/animations.txt'],
                                  ['/props.txt', 'assets/props.txt']]) {
        if (path !== route) continue
        const parts = []
        const base = await readFile(join(root, rel)).catch(() => null)
        if (base) parts.push(base.toString())
        for (const m of modules) {
          const extra = await readFile(join(root, 'modules', m, rel)).catch(() => null)
          if (extra) parts.push(`\n# --- ${m} ---\n` + extra.toString())
        }
        if (!parts.length) { res.writeHead(404).end('no ' + rel); return }
        res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8' })
           .end(parts.join('\n'))
        return
      }

      let file
      if (path.startsWith('/sprites/') || path.startsWith('/textures/') ||
          path.startsWith('/music/')) {
        const clean = normalize(decodeURIComponent(path))
        file = clean.includes('..') ? null : await moduleFile(join('assets', clean))
      } else {
        file = resolve(req.url)
      }
      if (!file) { res.writeHead(404).end('not found'); return }

      const body = await readFile(file)
      res.writeHead(200, {
        'content-type': TYPES[extname(file)] ?? 'application/octet-stream',
        'cross-origin-opener-policy': 'same-origin',
        'cross-origin-embedder-policy': 'require-corp'
      }).end(body)
    } catch (e) {
      res.writeHead(e.code === 'ENOENT' ? 404 : 500).end(String(e.message))
    }
  })

  return new Promise((resolve_, reject) => {
    server.on('error', reject)
    // Port 0: the OS picks a free one and tells us which. Two copies of the
    // game can then run side by side, which a fixed port makes impossible.
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address()
      log(`static http://127.0.0.1:${port}/`)
      resolve_({ server, port })
    })
  })
}
