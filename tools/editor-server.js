//
// The level editor's backend.
//
//   node tools/editor-server.js        then open http://localhost:8081/
//
// The browser cannot write files or spawn processes, and the editor has to do
// both -- save a .map, run mapc, bring the stack up. So there is a small Node
// process, modelled on tools/devserver.js and deliberately kept the same shape.
//
// The difference is that this one WRITES and EXECS, which is why its mounts and
// its routes are explicit rather than convenient. That process has the whole
// source tree under it, including the edge's certificates and the Lua the
// server runs; devserver.js refuses to serve any of it and this refuses to
// write any of it. Every path that arrives from the page goes through
// underMaps() before it is opened, and there is exactly one of those functions
// so there is one place to be wrong.
//
// It does not replace tools/devserver.js. Test mode runs scripts/run_stack.sh,
// which brings up its own devserver on 8080 for the GAME; this serves the
// EDITOR on 8081 and the two are meant to be open at once.
//
import { createServer } from 'node:http'
import { readFile, writeFile, stat, readdir, mkdir, unlink } from 'node:fs/promises'
import { spawn, execFile } from 'node:child_process'
import { dirname, join, normalize, extname, relative, sep } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { arch } from 'node:os'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..')
const PORT = Number(process.env.DNP_EDITOR_PORT ?? 8081)
const ARCH = arch() === 'arm64' ? 'arm64' : 'x86_64'
const MAPC = join(ROOT, 'build', ARCH, 'mapc')
const BUILD = join(ROOT, 'build', ARCH)

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.png': 'image/png',
  '.txt': 'text/plain; charset=utf-8',
  '.map': 'text/plain; charset=utf-8',
  '.lua': 'text/plain; charset=utf-8'
}

// Prefixes served, and what they resolve to. An explicit list, for the reason
// devserver.js gives: this process has the whole source tree under it and none
// of it has any business being fetchable.
//
// /client/ is mounted whole rather than /client/src/ alone because core.js
// imports '../wasm/dist/dnp_core.mjs' -- that is the layout on disk, and a
// mount that skipped a level would make the source's own relative import wrong
// only when served, which is the kind of breakage that looks like a wasm bug.
const MOUNTS = [
  ['/vendor/three/', join(ROOT, 'client', 'node_modules', 'three', 'build')],
  ['/src/', join(ROOT, 'editor', 'src')],
  ['/client/', join(ROOT, 'client')],
  // The engine's own, which after the assets moved into modules is very
  // little -- kept so a map outside any module still finds whatever remains.
  // The module-scoped form is /asset/<module>/textures/NAME.png, resolved per
  // file below.
  ['/textures/', join(ROOT, 'assets', 'textures')],
  ['/sprites/', join(ROOT, 'assets', 'sprites')],
  ['/', join(ROOT, 'editor')]
]

// Stricter than tools/devserver.js's, in two places, and both were found by
// editor/test/server.test.js rather than by reading it:
//
//   1. `..` is refused BEFORE normalize(), not after. normalize('/../x') is
//      '/x' -- a leading climb above root is silently dropped -- so the check
//      devserver.js makes sees a clean path and lets it through. It stayed safe
//      there because of the containment test below, but "resolves to a file
//      that does not exist inside the mount" is a much weaker answer than "no",
//      and it is the kind of weaker answer that stops being safe when somebody
//      adds a mount.
//
//   2. containment requires a separator. `startsWith(dir)` alone would let a
//      mount on .../client also serve a sibling directory called clientele.
//      Not reachable with today's mounts; free to rule out.
function resolveStatic (urlPath) {
  let decoded
  try { decoded = decodeURIComponent(urlPath.split('?')[0]) } catch { return null }
  if (decoded.includes('..') || decoded.includes('\0')) return null
  const clean = normalize(decoded)
  if (clean.includes('..')) return null
  for (const [prefix, dir] of MOUNTS) {
    if (!clean.startsWith(prefix)) continue
    const rest = clean.slice(prefix.length) || 'index.html'
    const full = join(dir, rest)
    if (full !== dir && !full.startsWith(dir + sep)) return null
    return full
  }
  return null
}

// --- modules ---------------------------------------------------------------
//
// A module is one project on this engine -- its maps, its scripts, its assets,
// in one directory (docs/MODULES.md). For the editor it is a ROOT: which maps
// the list shows, which props.txt is edited, and which --module Test passes.
//
// The engine's own maps/ and assets/ are not a module and are not pretending to
// be one. They are what the engine did before modules existed, they are what
// every test uses, and `null` means them throughout.

const MODULES = join(ROOT, 'modules')

/**
 * The directory a module id names, or null.
 *
 * The id is an ALLOWLIST and not a sanitisation: it may only be letters,
 * digits, dash and underscore, so there is no separator, no leading dot and no
 * `..` to reason about. Everything else about module paths is built from this
 * one answer, which is the same argument underMaps() makes -- one place to be
 * wrong rather than one per route.
 *
 * `null` is the engine itself, and is a legitimate answer rather than a
 * failure: a bare map with its scripts is not a lesser case.
 */
function moduleRoot (id) {
  if (id === null || id === undefined || id === '') return ROOT
  if (typeof id !== 'string' || !/^[A-Za-z0-9][A-Za-z0-9_-]*$/.test(id)) return null
  if (id.length > 64) return null
  return join(MODULES, id)
}

/** module.txt, which is read for display and for nothing else. */
async function readModuleInfo (id) {
  const root = moduleRoot(id)
  if (!root || root === ROOT) return null
  const text = await readFile(join(root, 'module.txt'), 'utf8').catch(() => null)
  const info = { id, name: id, about: '' }
  if (text === null) return info
  for (const line of text.split('\n')) {
    const m = /^\s*(\w+)\s+(.*)$/.exec(line)
    if (!m) continue
    const [, key, value] = m
    // `about` repeats, one line per line. Joined rather than last-wins, because
    // the file uses it as a paragraph.
    if (key === 'about') info.about = info.about ? `${info.about} ${value.trim()}` : value.trim()
    else if (key === 'name') info.name = value.trim()
    else if (key === 'id' && value.trim() !== id) {
      // Reported, not honoured. The directory is what --module names, so an id
      // field that disagrees with it is a file describing a module nobody can
      // load by that name.
      info.warning = `module.txt says id '${value.trim()}' but the directory is '${id}'`
    }
  }
  return info
}

/**
 * Where a module's assets live, falling back per FILE to the engine's.
 *
 * The same rule the server and the web server use: a module that adds three
 * sprites does not have to copy the other two thousand. For the editor it
 * decides which props.txt is shown -- the module's if it has one, the engine's
 * if it does not -- and `writable` is where a change would go, which is always
 * the module's own copy.
 */
function moduleAsset (id, rel) {
  const root = moduleRoot(id)
  if (!root) return null
  const own = root === ROOT ? join(ROOT, 'assets', rel) : join(root, 'assets', rel)
  return { own, fallback: join(ROOT, 'assets', rel) }
}

// --- the one gate on every path that arrives from the page -----------------

/**
 * Resolve a repository-relative path and refuse it unless it names a file
 * inside maps/ with an extension the editor owns.
 *
 * Written as one function with one caller-visible answer because the
 * alternative -- a check at each of the six routes that touch the disk -- is
 * six chances to write the fifth one differently. It refuses rather than
 * sanitises: a path that needed rewriting to be safe is one the page should not
 * have sent, and quietly repairing it hides whichever bug produced it.
 *
 * The extension list is a whitelist and not a blacklist for the same reason.
 * `.map`, `.lua` and `.editor.json` are what an editor session is made of;
 * anything else under maps/ (a .dnpm, a stray shell script) it has no business
 * writing.
 */
function underMaps (p, { extensions = ['.map', '.lua', '.json'], module = null } = {}) {
  if (typeof p !== 'string' || p.length === 0 || p.length > 512) return null
  if (p.includes('\0')) return null
  const root = moduleRoot(module)
  if (!root) return null
  const maps = join(root, 'maps')
  const full = normalize(join(maps, p.replace(/^\/+/, '')))
  const rel = relative(maps, full)
  if (rel === '' || rel.startsWith('..') || rel.startsWith(sep) || rel.includes('..')) return null
  if (!extensions.includes(extname(full))) return null
  return full
}

/**
 * A Lua file inside a module, in maps/ or scripts/.
 *
 * A second gate rather than a widened underMaps(): that one answers "a map and
 * the files that travel with it", and this answers "a module's own source". A
 * gate that answered both would have to be read twice to see what it allows.
 */
function underModuleLua (module, rel) {
  const root = moduleRoot(module)
  if (!root || typeof rel !== 'string' || rel.length === 0 || rel.length > 512) return null
  if (rel.includes('\0')) return null
  const dir = rel.split('/')[0]
  if (dir !== 'maps' && dir !== 'scripts') return null
  const full = normalize(join(root, rel))
  const back = relative(root, full)
  if (back.startsWith('..') || back.includes('..') || back.startsWith(sep)) return null
  if (extname(full) !== '.lua') return null
  return full
}

/** Where a module's maps are, for listing. */
const mapsRootOf = (module) => {
  const root = moduleRoot(module)
  return root ? join(root, 'maps') : null
}

// --- the map list ----------------------------------------------------------

async function listMaps (module = null) {
  const MAPS = mapsRootOf(module)
  if (!MAPS) return []
  const out = []
  const scan = async (dir, prefix) => {
    let names = []
    try { names = await readdir(dir) } catch { return }
    for (const n of names.sort()) {
      if (n.startsWith('.')) continue
      const full = join(dir, n)
      const st = await stat(full).catch(() => null)
      if (!st) continue
      if (st.isDirectory()) { await scan(full, prefix ? `${prefix}/${n}` : n); continue }
      if (!n.endsWith('.map')) continue
      const rel = prefix ? `${prefix}/${n}` : n
      const base = rel.slice(0, -4)
      out.push({
        path: rel,
        name: base,
        bytes: st.size,
        modified: st.mtimeMs,
        lua: await exists(join(dir, `${n.slice(0, -4)}.lua`)),
        sidecar: await exists(join(dir, `${n.slice(0, -4)}.editor.json`))
      })
    }
  }
  await scan(MAPS, '')
  return out
}

const exists = (p) => stat(p).then(() => true, () => false)

/**
 * The Lua files under a module's maps/ or scripts/ that define mobs.
 *
 * Found by looking for the call rather than by a naming convention: a module
 * may call its file anything, and `doom_monsters.lua` and `mobs.lua` are two
 * names for the same job already. The mob NAMES are the keys of the table
 * handed to mobs.load, extracted for a list.
 *
 * A regex over Lua, and it is display-only for that reason -- see the /api/mobs
 * route. Enough to answer "what does this module define"; not enough, and not
 * meant, to edit one.
 */
async function mobFilesOf (module) {
  const root = moduleRoot(module)
  if (!root) return []
  const out = []
  for (const dir of ['maps', 'scripts']) {
    let names = []
    try { names = await readdir(join(root, dir)) } catch { continue }
    for (const n of names.sort()) {
      if (!n.endsWith('.lua')) continue
      const full = join(root, dir, n)
      const text = await readFile(full, 'utf8').catch(() => null)
      if (text === null || !/\bmobs\.load\s*\{/.test(text)) continue

      const body = text.slice(text.search(/\bmobs\.load\s*\{/))
      // Two spaces of indent is the shape every one of these files uses. A
      // deeper match would pick up the fields inside a mob.
      const mobs = [...body.matchAll(/^\s{4}([A-Za-z_]\w*)\s*=/gm)].map((m) => m[1])
      out.push({
        path: `${dir}/${n}`,
        bytes: text.length,
        mobs: [...new Set(mobs)]
      })
    }
  }
  return out
}

// --- compiling and running -------------------------------------------------

function run (cmd, args, opts = {}) {
  return new Promise((resolve) => {
    execFile(cmd, args, { cwd: ROOT, timeout: 60_000, ...opts }, (err, stdout, stderr) => {
      resolve({ ok: !err, code: err?.code ?? 0, stdout: String(stdout), stderr: String(stderr) })
    })
  })
}

/**
 * The stack Test mode brings up, and the reason there is only ever one.
 *
 * run_stack.sh binds three fixed ports. A second one started on top of the
 * first does not fail cleanly -- the server binds, the edge does not, and what
 * the author sees is a client that connects to the PREVIOUS map. So starting a
 * new one stops the old one first, and the page is told which map is running.
 */
const stack = { child: null, map: null, url: null, log: [] }

function stopStack () {
  if (!stack.child) return false
  // The whole process group: run_stack.sh spawns three children and traps to
  // clean them up, but a kill delivered to the script alone races that trap.
  try { process.kill(-stack.child.pid, 'SIGTERM') } catch { try { stack.child.kill('SIGTERM') } catch {} }
  stack.child = null
  stack.map = null
  stack.url = null
  return true
}

function startStack (mapRelPath, module = null) {
  stopStack()
  stack.log = []
  const args = module ? ['--module', module, '--map', mapRelPath] : ['--map', mapRelPath]
  const child = spawn(join(ROOT, 'scripts', 'run_stack.sh'), args, {
    cwd: ROOT,
    detached: true,          // its own process group, so stopStack can take it all
    stdio: ['ignore', 'pipe', 'pipe']
  })
  stack.child = child
  stack.map = mapRelPath

  const note = (buf) => {
    const s = String(buf)
    stack.log.push(s)
    if (stack.log.length > 200) stack.log.shift()
    // The URL run_stack.sh prints, taken from its output rather than
    // reconstructed here. It already knows which ports it used, including when
    // they came from the environment, and a second copy of that arithmetic
    // would be right until somebody set DNP_WT_PORT.
    const m = /(http:\/\/localhost:\d+\/\?server=\S+)/.exec(s)
    if (m) stack.url = m[1]
    process.stdout.write(`[stack] ${s}`)
  }
  child.stdout.on('data', note)
  child.stderr.on('data', note)
  child.on('exit', (code) => {
    stack.log.push(`run_stack.sh exited with ${code}\n`)
    if (stack.child === child) { stack.child = null; stack.url = null }
  })
  return child
}

process.on('exit', stopStack)
process.on('SIGINT', () => { stopStack(); process.exit(0) })
process.on('SIGTERM', () => { stopStack(); process.exit(0) })

// --- request plumbing ------------------------------------------------------

const log = (code, url) => console.log(`[editor] ${code} ${url}`)

function sendJson (res, code, body) {
  const text = JSON.stringify(body)
  res.writeHead(code, {
    'content-type': 'application/json; charset=utf-8',
    'cache-control': 'no-store'
  }).end(text)
}

async function readBody (req, limit = 8 << 20) {
  const chunks = []
  let total = 0
  for await (const c of req) {
    total += c.length
    if (total > limit) throw new Error('request body too large')
    chunks.push(c)
  }
  return Buffer.concat(chunks).toString('utf8')
}

const routes = {
  // --- reading ---

  /**
   * Every module, for the picker.
   *
   * Discovered from the filesystem rather than from a list: a manifest that has
   * to be kept in step with the directory is a second source of truth that goes
   * stale the first time somebody adds one. docs/MODULES.md makes the same
   * argument about module.txt not listing its own contents.
   */
  'GET /api/modules': async () => {
    let names = []
    try { names = await readdir(MODULES) } catch { return { code: 200, body: { modules: [] } } }
    const modules = []
    for (const n of names.sort()) {
      if (n.startsWith('.')) continue
      const st = await stat(join(MODULES, n)).catch(() => null)
      if (!st?.isDirectory()) continue
      const info = await readModuleInfo(n)
      if (!info) continue
      const maps = await listMaps(n)
      const assets = moduleAsset(n, 'props.txt')
      modules.push({
        ...info,
        maps: maps.length,
        // Whether the module carries its OWN catalogue. When it does not, the
        // editor shows the engine's and says that editing creates one.
        ownProps: await exists(assets.own)
      })
      // NOT the mob files. Finding them means reading every .lua in the module
      // looking for a mobs.load call -- thirty files of twenty kilobytes for
      // doom2 alone -- and this route is on the page's critical path. The mobs
      // panel asks for them when it is opened, which is the only time they are
      // wanted.
    }
    return { code: 200, body: { modules } }
  },

  'GET /api/maps': async (url) => {
    const module = url.searchParams.get('module') || null
    if (!moduleRoot(module)) return { code: 400, body: { error: 'no such module' } }
    return { code: 200, body: { module, maps: await listMaps(module) } }
  },

  'GET /api/map': async (url) => {
    const module = url.searchParams.get('module') || null
    const p = underMaps(url.searchParams.get('path'), { extensions: ['.map'], module })
    if (!p) return { code: 400, body: { error: 'not a map path' } }
    const text = await readFile(p, 'utf8').catch(() => null)
    if (text === null) return { code: 404, body: { error: 'no such map' } }

    // The sidecar, and the disagreement it exists to report.
    //
    // .map is the source of truth (nothing the editor does destroys a file a
    // person wrote), so the sidecar carries only what the format has no place
    // for. The two CAN diverge -- somebody edits the .map by hand -- and the
    // honest answer is to say so and let the author decide, not to reconcile
    // silently. Hence the stored hash and the `stale` flag.
    const side = p.replace(/\.map$/, '.editor.json')
    const raw = await readFile(side, 'utf8').catch(() => null)
    let sidecar = null
    let stale = false
    if (raw !== null) {
      try {
        sidecar = JSON.parse(raw)
        stale = typeof sidecar.mapHash === 'string' && sidecar.mapHash !== textHash(text)
      } catch {
        // A corrupt sidecar is reported, never discarded on the author's
        // behalf: it is the only copy of their trigger graph.
        sidecar = { error: 'the sidecar is not valid JSON and was left alone' }
      }
    }
    const luaPath = p.replace(/\.map$/, '.lua')
    const lua = await readFile(luaPath, 'utf8').catch(() => null)

    // Which declared scripts actually resolve, from where the .map is.
    //
    // Worth answering here because mapc does NOT stop for one that does not:
    // it prints "cannot open script ..." and then writes the output and exits
    // 0, despite its own comment calling that an error. So a map can bake
    // cleanly, load, and have none of its doors work -- and the one place that
    // can be caught before Test is the editor, which knows the paths and can
    // stat them.
    const scripts = []
    for (const m of text.matchAll(/^\s*script\s+(\S+)/gm)) {
      const rel = m[1]
      const full = normalize(join(dirname(p), rel))
      scripts.push({ path: rel, name: rel.split(/[/\\]/).pop(), found: await exists(full) })
    }

    return {
      code: 200,
      body: {
        path: url.searchParams.get('path'),
        text, sidecar, sidecarStale: stale, lua, scripts, hash: textHash(text)
      }
    }
  },

  /**
   * The texture names the client can actually draw.
   *
   * From assets/textures/index.json -- the same file loadRealTextures() reads
   * to decide which names resolve to an image and how big it is. Listing the
   * directory instead would offer names the renderer has no size for, which
   * draw as the missing-texture wireframe: an editor that lets you pick
   * something that cannot be drawn is worse than one that offers less.
   */
  /**
   * The trigger vocabulary, for the panel that is generated from it.
   *
   * A FIXED path rather than a new mount under maps/. The vocabulary is one
   * file the page needs and mounting the directory it lives in would open every
   * map script to the browser to deliver it -- and the comment above MOUNTS is
   * specifically about the containment check getting weaker the moment somebody
   * adds one. There is no user input on this path at all.
   *
   * It is GENERATED from maps/scripts/trigger_vocab.lua by
   * build/lua/trigger_vocab_json, so a missing file means the build step has
   * not been run, and saying so beats a panel that is mysteriously empty.
   */
  'GET /api/triggervocab': async () => {
    const raw = await readFile(join(ROOT, 'maps', 'scripts', 'trigger_vocab.json'), 'utf8')
      .catch(() => null)
    if (raw === null) {
      return {
        code: 200,
        body: {
          error: 'maps/scripts/trigger_vocab.json is missing -- run ' +
                 './scripts/build_lua_tests.sh && ./build/lua/trigger_vocab_json . ' +
                 '> maps/scripts/trigger_vocab.json'
        }
      }
    }
    try {
      return { code: 200, body: JSON.parse(raw) }
    } catch (e) {
      return { code: 200, body: { error: `trigger_vocab.json is not valid JSON: ${e.message}` } }
    }
  },

  'GET /api/textures': async (url) => {
    // The module's index first, the engine's second -- the same per-file
    // fallback everything else uses. It matters more than it did: the engine's
    // assets/ now carries only props.txt, so a module IS where the textures
    // are, and a route that only looked at assets/textures/index.json would
    // offer an empty dropdown to everybody.
    const module = url.searchParams.get('module') || null
    const where = moduleAsset(module, join('textures', 'index.json'))
    if (!where) return { code: 400, body: { error: 'no such module' } }

    const from = (await exists(where.own)) ? where.own : where.fallback
    const raw = await readFile(from, 'utf8').catch(() => null)
    if (raw === null) {
      return {
        code: 200,
        body: {
          textures: [],
          note: `no ${relative(ROOT, where.own)}` +
                (module ? ` and no ${relative(ROOT, where.fallback)}` : '')
        }
      }
    }
    try {
      const index = JSON.parse(raw)
      const sizes = index.sizes ?? {}
      return {
        code: 200,
        body: {
          module,
          path: relative(ROOT, from),
          textures: Object.keys(sizes).sort().map((name) => ({ name, size: sizes[name] }))
        }
      }
    } catch (e) {
      return { code: 200, body: { textures: [], note: `${relative(ROOT, from)} is not valid JSON: ${e.message}` } }
    }
  },

  /**
   * The monster names that have behaviour, from maps/scripts/enemies.lua.
   *
   * Served as the FILE, so the editor extracts them with one function that is
   * tested against the real Lua. Sending a parsed list from here would put the
   * extraction in the process that cannot be unit-tested against it.
   */
  'GET /api/enemies': async () => {
    const text = await readFile(join(ROOT, 'maps', 'scripts', 'enemies.lua'), 'utf8')
      .catch(() => null)
    if (text === null) {
      return { code: 200, body: { text: '', note: 'no maps/scripts/enemies.lua' } }
    }
    return { code: 200, body: { text } }
  },

  /**
   * The Lua files a module carries that define mobs, and which mobs each names.
   *
   * DISPLAY ONLY, and the reason is worth stating because it looks like a
   * missing feature. A mob data file is Lua WITH FUNCTIONS -- `humanoid{...}`,
   * `du(2048)`, `tics(16)` -- evaluated at load time, so its values do not
   * exist until the real interpreter has run it. Reading that from JavaScript
   * would be a second implementation of a grammar whose only definition is the
   * Lua runtime, which is the trap the .map parser already pays a hash control
   * for and which no equivalent oracle exists for here.
   *
   * So the names are extracted for a LIST and nothing is offered for editing.
   * What a real mob editor needs is written down in the panel and in
   * docs/EDITOR.md.
   */
  'GET /api/mobs': async (url) => {
    const module = url.searchParams.get('module') || null
    if (!moduleRoot(module)) return { code: 400, body: { error: 'no such module' } }
    return { code: 200, body: { module, files: await mobFilesOf(module) } }
  },

  /** One mob file's source, for the mob window. */
  'GET /api/mobfile': async (url) => {
    const module = url.searchParams.get('module') || null
    const p = underModuleLua(module, url.searchParams.get('path'))
    if (!p) return { code: 400, body: { error: 'not a Lua file in that module' } }
    const text = await readFile(p, 'utf8').catch(() => null)
    if (text === null) return { code: 404, body: { error: 'no such file' } }
    return { code: 200, body: { module, path: relative(ROOT, p), text, hash: textHash(text) } }
  },

  /**
   * Write one back.
   *
   * Guarded on the text that was opened, like every other write here. This file
   * is loaded by the SERVER at map load, and a mob whose numbers changed under
   * somebody is a level that plays differently with nothing to point at.
   */
  'POST /api/mobfile': async (url, body) => {
    const req = JSON.parse(body)
    const p = underModuleLua(req.module ?? null, req.path)
    if (!p) return { code: 400, body: { error: 'not a Lua file in that module' } }
    if (typeof req.text !== 'string') return { code: 400, body: { error: 'no text' } }
    if (req.text.length > (4 << 20)) return { code: 400, body: { error: 'implausibly large' } }
    if (!req.force && typeof req.baseHash === 'string') {
      const current = await readFile(p, 'utf8').catch(() => null)
      if (current !== null && textHash(current) !== req.baseHash) {
        return { code: 409, body: { error: 'that file has changed since it was opened', current } }
      }
    }
    await writeFile(p, req.text)
    return { code: 200, body: { hash: textHash(req.text), path: relative(ROOT, p) } }
  },

  'GET /api/props': async (url) => {
    const module = url.searchParams.get('module') || null
    const where = moduleAsset(module, 'props.txt')
    if (!where) return { code: 400, body: { error: 'no such module' } }

    // The module's own if it has one, the engine's otherwise -- the same
    // per-file fallback the server and the web server use. `own` says whether
    // what is being shown belongs to this module, because editing the engine's
    // catalogue from inside a module is a change to every module at once and
    // the author should be told which they are doing.
    const own = await exists(where.own)
    const from = own ? where.own : where.fallback
    const text = await readFile(from, 'utf8').catch(() => null)
    if (text === null) return { code: 404, body: { error: `no ${relative(ROOT, from)}` } }
    return {
      code: 200,
      body: {
        text, hash: textHash(text), module,
        own, path: relative(ROOT, from), writesTo: relative(ROOT, where.own)
      }
    }
  },

  'GET /api/stack': async () => ({
    code: 200,
    body: { running: !!stack.child, map: stack.map, url: stack.url, log: stack.log.join('') }
  }),

  // --- writing ---

  'POST /api/map': async (url, body) => {
    const req = JSON.parse(body)
    const p = underMaps(req.path, { extensions: ['.map'], module: req.module ?? null })
    if (!p) return { code: 400, body: { error: 'not a map path' } }
    if (typeof req.text !== 'string') return { code: 400, body: { error: 'no text' } }

    // A stale write is refused, not merged. The page sends the hash of the
    // text it opened; if the file has changed since -- another editor tab,
    // another agent, a hand edit -- overwriting it would destroy work that was
    // never seen. `force` is how the author says they looked and meant it.
    if (!req.force && typeof req.baseHash === 'string') {
      const current = await readFile(p, 'utf8').catch(() => null)
      if (current !== null && textHash(current) !== req.baseHash) {
        return {
          code: 409,
          body: { error: 'the file on disk has changed since it was opened', current }
        }
      }
    }

    await mkdir(dirname(p), { recursive: true })
    await writeFile(p, req.text)

    const written = { map: req.path, hash: textHash(req.text) }
    if (req.sidecar !== undefined) {
      const side = p.replace(/\.map$/, '.editor.json')
      if (req.sidecar === null) await unlink(side).catch(() => {})
      else {
        await writeFile(side, JSON.stringify({ ...req.sidecar, mapHash: written.hash }, null, 2) + '\n')
        written.sidecar = true
      }
    }
    if (typeof req.lua === 'string') {
      await writeFile(p.replace(/\.map$/, '.lua'), req.lua)
      written.lua = true
    }
    return { code: 200, body: written }
  },

  /**
   * Write the prop catalogue.
   *
   * A named route for ONE file, not a path from the page. assets/props.txt is
   * outside maps/, so underMaps() refuses it -- correctly, and the answer is a
   * route that can only ever mean this file rather than a second, looser gate.
   *
   * Guarded on the text the page opened, like a map: this file is read by both
   * binaries, so overwriting a change nobody saw is a change to every level at
   * once.
   */
  'POST /api/props': async (url, body) => {
    const req = JSON.parse(body)
    if (typeof req.text !== 'string') return { code: 400, body: { error: 'no text' } }
    if (req.text.length > (1 << 20)) return { code: 400, body: { error: 'implausibly large' } }

    // Written to the MODULE's own copy, always -- creating it if the module was
    // reading the engine's. A module that edits the engine's catalogue is
    // editing every other module's too, and the fallback exists so it does not
    // have to.
    const where = moduleAsset(req.module ?? null, 'props.txt')
    if (!where) return { code: 400, body: { error: 'no such module' } }
    const p = where.own
    if (!req.force && typeof req.baseHash === 'string') {
      const current = await readFile(p, 'utf8').catch(() => null)
      if (current !== null && textHash(current) !== req.baseHash) {
        return {
          code: 409,
          body: { error: 'assets/props.txt has changed since it was opened', current }
        }
      }
    }
    await mkdir(dirname(p), { recursive: true })
    await writeFile(p, req.text)
    return { code: 200, body: { hash: textHash(req.text), path: relative(ROOT, p) } }
  },

  'POST /api/compile': async (url, body) => {
    const req = JSON.parse(body)
    const p = underMaps(req.path, { extensions: ['.map'], module: req.module ?? null })
    if (!p) return { code: 400, body: { error: 'not a map path' } }
    await mkdir(BUILD, { recursive: true })
    const out = join(BUILD, `${req.path.replace(/[/\\]/g, '_').replace(/\.map$/, '')}.dnpm`)
    const r = await run(MAPC, [p, out])
    // mapc's own message, verbatim. It is written to be read by a person and
    // it names the sector and the reason; paraphrasing it in the page would
    // give the author a worse version of a diagnostic somebody already wrote
    // carefully, and would drift from it.
    return {
      code: 200,
      body: {
        ok: r.ok,
        output: out,
        stdout: r.stdout,
        stderr: r.stderr,
        hash: /hash ([0-9A-F]{16})/.exec(r.stdout)?.[1] ?? null
      }
    }
  },

  'POST /api/test': async (url, body) => {
    const req = JSON.parse(body)
    const dnpm = req.dnpm
    if (typeof dnpm !== 'string' || !dnpm.startsWith(BUILD + sep) || !dnpm.endsWith('.dnpm')) {
      return { code: 400, body: { error: 'not a compiled map in this build directory' } }
    }
    if (!await exists(dnpm)) return { code: 400, body: { error: 'that map has not been compiled' } }
    // --module, so the stack loads the module's assets: its props.txt, its
    // sprites, its textures. Without it a module's map runs against the
    // engine's catalogue and every prop the module added is unknown.
    const module = req.module ?? null
    if (!moduleRoot(module)) return { code: 400, body: { error: 'no such module' } }
    startStack(relative(ROOT, dnpm), module)
    return { code: 200, body: { started: true, map: relative(ROOT, dnpm), module } }
  },

  'POST /api/stop': async () => ({ code: 200, body: { stopped: stopStack() } })
}

/**
 * A content hash for change detection, and NOT a security property.
 *
 * FNV-1a over the bytes: cheap, stable, and the only thing it has to do is
 * notice that a file is not the one that was opened. Naming that plainly here
 * so nobody later reaches for it expecting collision resistance.
 */
function textHash (s) {
  let h = 0x811c9dc5
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i) & 0xff
    h = Math.imul(h, 0x01000193) >>> 0
    h ^= s.charCodeAt(i) >>> 8
    h = Math.imul(h, 0x01000193) >>> 0
  }
  return h.toString(16).padStart(8, '0')
}

// Listening is guarded on being run as a program rather than imported, so
// editor/test/server.test.js can exercise underMaps() -- the one gate every
// path from the page passes through -- without a port being bound by the act
// of importing the module that defines it.
const IS_MAIN = process.argv[1] &&
  import.meta.url === pathToFileURL(process.argv[1]).href

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost')
  const key = `${req.method} ${url.pathname}`
  try {
    const route = routes[key]
    if (route) {
      const body = req.method === 'POST' ? await readBody(req) : null
      const r = await route(url, body)
      log(r.code, key)
      return sendJson(res, r.code, r.body)
    }

    if (url.pathname.startsWith('/api/')) {
      log(404, key)
      return sendJson(res, 404, { error: 'no such route' })
    }

    // An asset in a module, falling back to the engine's PER FILE.
    //
    // The module is in the URL rather than in the server, because the server
    // has no session: two tabs on two modules would otherwise fight over one
    // variable. `-` is the engine, so the shape is the same either way and the
    // page builds one kind of URL.
    if (url.pathname.startsWith('/asset/')) {
      const parts = url.pathname.slice('/asset/'.length).split('/')
      const id = parts.shift()
      const rel = parts.join('/')
      const module = id === '-' ? null : id
      if (!moduleRoot(module) || rel.includes('..') || rel.includes('\0') || !rel) {
        log(404, req.url)
        res.writeHead(404).end('not found')
        return
      }
      const where = moduleAsset(module, rel)
      const from = (await exists(where.own)) ? where.own : where.fallback
      const data = await readFile(from).catch(() => null)
      log(data ? 200 : 404, req.url)
      if (!data) { res.writeHead(404).end('not found'); return }
      res.writeHead(200, {
        'content-type': TYPES[extname(from)] ?? 'application/octet-stream',
        'cache-control': 'no-store',
        'cross-origin-opener-policy': 'same-origin',
        'cross-origin-embedder-policy': 'require-corp'
      }).end(data)
      return
    }

    const file = resolveStatic(url.pathname)
    if (!file) { log(404, req.url); res.writeHead(404).end('not found'); return }
    const data = await readFile(file)
    log(200, req.url)
    res.writeHead(200, {
      'content-type': TYPES[extname(file)] ?? 'application/octet-stream',
      'cache-control': 'no-store',
      'cross-origin-opener-policy': 'same-origin',
      'cross-origin-embedder-policy': 'require-corp'
    }).end(data)
  } catch (e) {
    const code = e.code === 'ENOENT' ? 404 : 500
    log(code, req.url)
    if (url.pathname.startsWith('/api/')) sendJson(res, code, { error: String(e.message) })
    else res.writeHead(code).end(String(e.message))
  }
})

if (IS_MAIN) {
  server.listen(PORT, () => {
    console.log(`[editor] http://localhost:${PORT}/`)
    console.log(`[editor] engine maps under ${join(ROOT, 'maps')}, modules under ${MODULES}`)
    console.log(`[editor] mapc      ${MAPC}`)
    console.log('[editor] Test mode runs scripts/run_stack.sh, which serves the GAME on 8080')
  })
}

export {
  underMaps, underModuleLua, textHash, listMaps, resolveStatic, MOUNTS,
  moduleRoot, moduleAsset
}
