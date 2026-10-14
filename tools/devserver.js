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
import { existsSync } from 'node:fs'
import { dirname, join, normalize, extname, isAbsolute } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..')
// The repository: the engine is ROOT, and modules, docs, the studio and the
// synth are beside it.
const REPO = join(ROOT, '..')
const PORT = Number(process.env.DNP_HTTP_PORT ?? 8080)

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  // A baked track is opaque bytes; the browser only ever hands it to the synth.
  '.mstk': 'application/octet-stream',
  // A sound bank: its source (a .sfxproj, JSON) or its bake (.sfxb).
  '.sfxproj': 'application/json',
  '.sfxb': 'application/octet-stream',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.dnpm': 'application/octet-stream',
  '.map': 'application/json',
  '.png': 'image/png',
  '.txt': 'text/plain; charset=utf-8',
  // What the studio's plugin editors are built of (docs/PLUGINS.md): a stylesheet
  // served as bytes is refused by the browser, and the editor comes up bare.
  '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.jpg': 'image/jpeg',
  '.webp': 'image/webp',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
  '.webmanifest': 'application/manifest+json',
  '.tsprite': 'application/zip',
  '.zip': 'application/zip'
}

// Prefixes served, and what they resolve to. An explicit list rather than
// serving the repository root: this process has the whole source tree under it,
// including certificates and Lua the server runs, and none of that has any
// business being fetchable.
// Modules named on the command line, in order (docs/MODULES.md). Repeatable:
// several load together, and every asset lookup tries them in turn before the
// engine's own assets/. Per FILE, so a module that adds three sprites does not
// have to copy the other two thousand.
const MODULES = []
for (let i = 2; i < process.argv.length; i++) {
  if (process.argv[i] === '--module' && process.argv[i + 1]) {
    const given = process.argv[++i]
    // BOTH `doom2` AND `modules/doom2`, because the SERVER accepts both --
    // server/src/main.cpp says so in as many words -- and these two processes
    // are handed the same flag by the same run_stack.sh invocation. One
    // argument meaning two different things to two programs in one stack is the
    // "two programs, one number" trap this project keeps paying for.
    //
    // The path form is not a mistake anybody has to make deliberately: shell
    // completion produces it.
    // The engine's own modules/ last (the starter lives there), as the build
    // scripts look: a module beside the engine first, then the engine's.
    const candidates = isAbsolute(given) ? [given] : [join(REPO, 'modules', given), join(REPO, given), join(ROOT, given), join(ROOT, 'modules', given)]
    const root = candidates.find((p) => existsSync(p))
    if (!root) {
      // LOUDLY, AND FATAL. `join(REPO, 'modules', 'modules/doom2')` is a
      // perfectly good path that simply does not exist, and serving from it
      // fails one file at a time: every texture and every track 404s, the game
      // still runs, and it looks like the renderer lost its textures. That cost
      // a debugging session. A module root that is not there is a startup
      // error, not two hundred quiet 404s.
      console.error(`[web] no such module: ${given}`)
      console.error(`[web]   looked in ${candidates.join(' and ')}`)
      process.exit(2)
    }
    MODULES.push(root)
  }
}
if (MODULES.length) console.error('[web] modules: ' + MODULES.join(', '))

/**
 * The first module that has this file, or the engine's own copy.
 *
 * `rel` is a path under a module root, like 'assets/sprites/POSSA1.png'. The
 * engine's fallback drops the leading 'assets/' because that IS its assets
 * root -- modules/x/assets/sprites/a.png and assets/sprites/a.png are the same
 * file in two places.
 */
async function moduleFile (rel) {
  for (const m of MODULES) {
    const p = join(m, rel)
    if (await readFile(p).then(() => true).catch(() => false)) return p
  }
  return join(ROOT, rel)
}

const MOUNTS = [
  ['/vendor/three/', join(ROOT, 'client', 'node_modules', 'three', 'build')],
  ['/src/', join(ROOT, 'client', 'src')],
  // Texture images, keyed by the NAME a map uses. The map carries names and the
  // client resolves them (§4) -- which is what lets one map serve clients with
  // different asset sets, and keeps megabytes of images off the control channel
  // that every join waits on.
  ['/textures/', join(ROOT, 'assets', 'textures')],
  // Sprite frames, indexed the same way textures are: from the renderer's side
  // a sprite frame is a texture that happens to have holes in it.
  ['/sprites/', join(ROOT, 'assets', 'sprites')],
  // client/wasm, not client/wasm/dist: core.js imports '../wasm/dist/...',
  // which is the layout on disk, and a mount that skipped the dist level would
  // make the source's own relative import wrong only when served.
  ['/wasm/', join(ROOT, 'client', 'wasm')],
  // The synth, and its instruments.
  //
  // TWO MOUNTS AND NOT ONE ROOT: midisynth/ also holds sources, a build tree
  // and vendored third-party code, none of which has any business being
  // fetchable -- the same reason this server lists prefixes instead of serving
  // the repository.
  //
  // tool/ rather than a copy of the two files in it, because synth-worklet.js
  // imports './dist/midisynth.mjs' RELATIVELY: the worklet and the wasm module
  // have to stay siblings or the import resolves to nothing.
  ['/synth/', join(REPO, 'midisynth', 'tool')],
  ['/synth-content/', join(REPO, 'midisynth', 'content')],
  // Baked .mstk tracks. A FALLBACK, not the only place: a module ships its own
  // soundtrack (modules/doom2/assets/music) and the per-file rule above finds
  // it first, exactly as it does for sprites.
  ['/music/', join(ROOT, 'assets', 'music')],
  // The sound-effects engine (sound/, DigidreamSound): its single-file wasm
  // module, which client/src/audio/sfx-worklet.js imports. Only dist/ -- the
  // rest of tool/ is the sound editor's.
  ['/sfx/', join(REPO, 'sound', 'tool', 'dist')],
  // Sound banks, by module like music (modules/doom2/assets/sounds).
  ['/sounds/', join(ROOT, 'assets', 'sounds')],
  // UI layouts (docs/UI_LAYOUT.md), by module; /ui/index.json is merged above.
  ['/ui/', join(ROOT, 'assets', 'ui')],
  // The studio (docs/STUDIO.md), and the runtime Lua plus vocabularies it
  // bakes and validates against. The runtime mount comes first because the
  // studio mount would otherwise claim the prefix.
  ['/studio/runtime/', join(ROOT, 'maps', 'scripts')],
  // The packager plugin's engine kits, when this checkout has built them
  // (packager/scripts/build_kits.sh): hundreds of MB, never inside the studio.
  ['/studio/packager-kits/', join(REPO, 'packager', 'build', 'kits')],
  ['/studio/', join(REPO, 'studio')],
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
  // Modules first, then the build directories. A module's maps live beside its
  // scripts, which is how a baked map stays self-contained.
  const dirs = MODULES.map((m) => join(m, 'maps'))
  for (const dir of [...dirs, join(ROOT, 'build', 'arm64'), join(ROOT, 'build', 'x86_64'),
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

    // A LINE THE PAGE WANTED SOMEBODY TO SEE.
    //
    // The browser's console is where the client says things and it is exactly
    // where nobody debugging the server can read them -- three rounds of "the
    // camera twitches at a crossing" were three different causes, and telling
    // them apart meant asking a person to copy a line out of devtools every
    // time. This puts it in the stack log beside everything else.
    //
    // Dev server only, which is the one that is never deployed. Nothing reads
    // it back and nothing is stored: it is printed and dropped.
    if (req.url.startsWith('/dev-log')) {
      // A POST carries a whole trace; a query string carries one line. Both end
      // up in the same log, one `[page]` line each, so a multi-line body stays
      // greppable rather than becoming one enormous line.
      let text
      if (req.method === 'POST') {
        const chunks = []
        for await (const c of req) chunks.push(c)
        text = Buffer.concat(chunks).toString('utf8').slice(0, 64 << 10)
      } else {
        text = decodeURIComponent(req.url.slice(req.url.indexOf('?') + 1))
      }
      for (const line of text.split('\n')) console.log(`[page] ${line}`)
      res.writeHead(204, { 'cache-control': 'no-store' }).end()
      return
    }

    // The prop catalogue, whole. A named route rather than a mount, because a
    // mount is a directory prefix and MOUNTS entries are matched with
    // startsWith -- '/props.txt' would have resolved to 'props.txt/index.html'.
    //
    // The client reads the renderer's half of the same file the server reads
    // the simulation's half of. One file, so the two cannot end up describing
    // different worlds.
    // The animation catalogue, alongside the prop one and for the same reason:
    // a named route, because MOUNTS are directory prefixes matched with
    // startsWith and '/animations.txt' would resolve to a directory.
    // The two catalogues are CONCATENATED across modules, not chosen between:
    // several modules load at once and each contributes its own entries. Ids
    // are absolute and are never renumbered here -- the server refuses to start
    // on a collision, which is the only place that check can be made once.
    // THE UI LAYOUT INDEX (docs/UI_LAYOUT.md 3.1), merged across modules with
    // the rule every asset has: the FIRST module that says wins, then the
    // engine's. It has to be the same rule, or an index naming hud.json would be
    // one module's while the hud.json served for it was another's.
    if (req.url.split('?')[0] === '/ui/index.json') {
      const out = { version: 1, default: null, maps: {} }
      for (const dir of [...MODULES, ROOT]) {
        const raw = await readFile(join(dir, 'assets', 'ui', 'index.json'), 'utf8').catch(() => null)
        if (!raw) continue
        let j
        try { j = JSON.parse(raw) } catch { continue }
        if (j.default) out.default ??= j.default
        for (const [k, v] of Object.entries(j.maps ?? {})) out.maps[k] ??= v
      }
      log(200, req.url)
      res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' }).end(JSON.stringify(out))
      return
    }

    for (const [route, rel] of [['/animations.txt', 'assets/animations.txt'],
                                ['/props.txt', 'assets/props.txt']]) {
      if (req.url.split('?')[0] !== route) continue
      const parts = []
      const base = await readFile(join(ROOT, rel)).catch(() => null)
      if (base) parts.push(base.toString())
      for (const m of MODULES) {
        const extra = await readFile(join(m, rel)).catch(() => null)
        if (extra) parts.push('\n# --- ' + m + ' ---\n' + extra.toString())
      }
      log(parts.length ? 200 : 404, req.url)
      if (!parts.length) { res.writeHead(404).end('no ' + rel); return }
      res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' })
         .end(parts.join('\n'))
      return
    }

    let file
    if (req.url.startsWith('/maps/')) {
      file = await findMap(req.url.slice('/maps/'.length).split('?')[0])
    } else if (MODULES.length &&
               (req.url.startsWith('/sprites/') || req.url.startsWith('/textures/') ||
                req.url.startsWith('/music/') || req.url.startsWith('/sounds/') ||
                req.url.startsWith('/ui/'))) {
      // PER FILE, not per directory. A module that adds three sprites does not
      // have to carry the other two thousand, and one that ships a whole set
      // overrides all of them -- without either having to say which it is.
      const clean = normalize(decodeURIComponent(req.url.split('?')[0]))
      file = clean.includes('..') ? null : await moduleFile(join('assets', clean))
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
