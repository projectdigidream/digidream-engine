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
// WITH THE STUDIO (docs/STUDIO.md) there is a second module tree: the writable
// one under the user's data directory, where the studio writes and the server
// reads. Its modules come FIRST in every per-file lookup and are concatenated
// into the catalogues like any other, so a texture the author just imported is
// the one the game draws.
//
import { createServer } from 'node:http'
import { readFile, readdir } from 'node:fs/promises'
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
  '.lua': 'text/plain; charset=utf-8',
  // A baked track is opaque bytes; only the synth ever reads one.
  '.mstk': 'application/octet-stream',
  // A sound bank: its source (JSON) or its bake.
  '.sfxproj': 'application/json',
  '.sfxb': 'application/octet-stream'
}

/**
 * @param root     the staged game directory (see scripts/build_app.sh)
 * @param modules  module directory names, in load order
 * @param rwRoot   the writable game tree, or null without the studio
 */
export function startStatic ({ root, modules = [], rwRoot = null, log = () => {} }) {
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
    ['/music/', join(root, 'assets', 'music')],
    // The sound-effects engine's single-file module (sound/tool/dist), and the
    // modules' sound banks. Staging the engine is scripts/build_app.sh's; until
    // it does, /sfx/ is a 404 and the game is silent but for its music.
    ['/sfx/', join(root, 'sfx')],
    ['/sounds/', join(root, 'assets', 'sounds')],
    ['/ui/', join(root, 'assets', 'ui')],
    // The studio and the runtime Lua it bakes against; both staged only with
    // --studio, and a 404 otherwise.
    ['/studio/runtime/', join(root, 'studio', 'runtime')],
    ['/studio/', join(root, 'studio')]
  ]

  // Every module root, the writable tree's first. Read on each request: the
  // studio creates modules while the app runs, and a directory listing on a
  // desktop costs nothing anybody can measure.
  const moduleRoots = async () => {
    const roots = []
    if (rwRoot) {
      try {
        for (const e of await readdir(join(rwRoot, 'modules'), { withFileTypes: true })) {
          if (e.isDirectory()) roots.push(join(rwRoot, 'modules', e.name))
        }
      } catch {}
    }
    for (const m of modules) roots.push(join(root, 'modules', m))
    return roots
  }

  // The first module that has this file, or the engine's own copy. Per FILE and
  // not per directory, exactly as docs/MODULES.md specifies: a module that adds
  // three sprites does not have to carry the other two thousand.
  const moduleFile = async (rel) => {
    for (const m of await moduleRoots()) {
      const p = join(m, rel)
      if (await readFile(p).then(() => true).catch(() => false)) return p
    }
    return join(root, rel)
  }

  const resolve = (url) => {
    const clean = normalize(decodeURIComponent(url.split('?')[0]))
    if (clean.includes('..')) return null
    if (clean === '/' || clean === '/index.html') return join(root, 'client', 'index.html')
    for (const [route, dir] of MOUNTS) {
      if (clean.startsWith(route)) return join(dir, clean.slice(route.length) || 'index.html')
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
        for (const m of await moduleRoots()) {
          const extra = await readFile(join(m, rel)).catch(() => null)
          if (extra) parts.push(`\n# --- ${m} ---\n` + extra.toString())
        }
        if (!parts.length) { res.writeHead(404).end('no ' + rel); return }
        res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8' })
           .end(parts.join('\n'))
        return
      }

      // The texture index is MERGED: it is per module on disk, and the
      // renderer asks for one. First module to name a texture wins, as it
      // does for the picture itself.
      if (path === '/textures/index.json') {
        const index = { sizes: {}, cutout: {} }
        const sources = [...(await moduleRoots()), root]
        for (const m of sources) {
          const raw = await readFile(join(m, 'assets', 'textures', 'index.json'), 'utf8').catch(() => null)
          if (!raw) continue
          let j
          try { j = JSON.parse(raw) } catch { continue }
          for (const [k, v] of Object.entries(j.sizes ?? {})) index.sizes[k] ??= v
          for (const [k, v] of Object.entries(j.cutout ?? {})) index.cutout[k] ??= v
        }
        res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(index))
        return
      }

      // The UI layout index: the first module that says wins, then the engine,
      // as for every asset file (tools/devserver.js does the same).
      if (path === '/ui/index.json') {
        const out = { version: 1, default: null, maps: {} }
        for (const m of [...(await moduleRoots()), root]) {
          const raw = await readFile(join(m, 'assets', 'ui', 'index.json'), 'utf8').catch(() => null)
          if (!raw) continue
          let j
          try { j = JSON.parse(raw) } catch { continue }
          if (j.default) out.default ??= j.default
          for (const [k, v] of Object.entries(j.maps ?? {})) out.maps[k] ??= v
        }
        res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(out))
        return
      }
      // The music index likewise: per module on disk, one table for the
      // client, keyed by map hash -- a union, since two modules' maps have
      // different hashes.
      if (path === '/music/index.json') {
        const index = {}
        for (const m of [...(await moduleRoots()), root]) {
          const raw = await readFile(join(m, 'assets', 'music', 'index.json'), 'utf8').catch(() => null)
          if (!raw) continue
          let j
          try { j = JSON.parse(raw) } catch { continue }
          for (const [k, v] of Object.entries(j)) index[k] ??= v
        }
        res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(index))
        return
      }

      let file
      if (path.startsWith('/sprites/') || path.startsWith('/textures/') ||
          path.startsWith('/music/') || path.startsWith('/sounds/') || path.startsWith('/ui/')) {
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
      res.writeHead(e.code === 'ENOENT' || e.code === 'EISDIR' ? 404 : 500).end(String(e.message))
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
