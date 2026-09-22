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
import { realpathSync, readFileSync } from 'node:fs'
import { X509Certificate } from 'node:crypto'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { arch } from 'node:os'
import { resolveEngineRoot, MODULE_ID, MAP_NAME } from './engineroot.js'
import { roomText } from '../editor/src/shapes.js'
import { parseMap, emitMap } from '../editor/src/mapfmt.js'

const HERE = dirname(fileURLToPath(import.meta.url))

/**
 * Where the ENGINE is -- maps, modules, build/, scripts/, the client. Not
 * necessarily beside this script any more.
 *
 * It was `join(HERE, '..')`, fixed, and that was right for as long as the
 * editor only ever ran from its own checkout. Two things need it to be a
 * parameter: a VS Code extension that opens a MODULE folder and has to find the
 * engine from there, and this server's own tests, which could only ever run
 * against the real repository and so had to build a throwaway module inside
 * modules/ to have anything to test with.
 *
 * Precedence and the reasons are in tools/engineroot.js. The value comes from
 * --engine-root, then DNP_ENGINE_ROOT, then walking up from the working
 * directory, then beside this script -- which is what it always was.
 */
function engineRootFromArgs () {
  const argv = process.argv.slice(2)
  const at = argv.findIndex((a) => a === '--engine-root' || a.startsWith('--engine-root='))
  if (at < 0) return process.env.DNP_ENGINE_ROOT || null
  const a = argv[at]
  return a.includes('=') ? a.slice(a.indexOf('=') + 1) : (argv[at + 1] ?? null)
}
const ENGINE = resolveEngineRoot({
  explicit: engineRootFromArgs(),
  from: process.cwd(),
  fallback: join(HERE, '..')
})
const ROOT = ENGINE.root
const PORT = Number(process.env.DNP_EDITOR_PORT ?? 8081)
const ARCH = arch() === 'arm64' ? 'arm64' : 'x86_64'
const MAPC = join(ROOT, 'build', ARCH, 'mapc')
const LUAJIT = join(ROOT, 'third_party', 'luajit', 'src', 'luajit')
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
//
// TWO KINDS OF MOUNT, now that the engine root can be elsewhere. The editor's
// own pages and source (/, /src/) travel with this script and are served from
// beside it; everything else is the ENGINE's -- its client, its assets -- and
// is served from ROOT. The two are the same directory when the editor runs
// from its own checkout, which is why nothing noticed the difference before.
const EDITOR = join(HERE, '..', 'editor')
const MOUNTS = [
  ['/vendor/three/', join(ROOT, 'client', 'node_modules', 'three', 'build')],
  ['/src/', join(EDITOR, 'src')],
  ['/client/', join(ROOT, 'client')],
  // The engine's own, which after the assets moved into modules is very
  // little -- kept so a map outside any module still finds whatever remains.
  // The module-scoped form is /asset/<module>/textures/NAME.png, resolved per
  // file below.
  ['/textures/', join(ROOT, 'assets', 'textures')],
  ['/sprites/', join(ROOT, 'assets', 'sprites')],
  ['/', EDITOR]
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
  if (typeof id !== 'string' || !MODULE_ID.test(id)) return null
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
 * The engine's own copy of a script name, if there is one.
 *
 * maps/mobs/ and maps/scripts/ are where the shared libraries live and what
 * every module's map declares by a relative path. A file of the same name
 * inside a module's maps/ is mapc's copy of it unless the module genuinely
 * carries its own.
 */
async function findSource (name) {
  for (const dir of ['mobs', 'scripts']) {
    const p = join(ROOT, 'maps', dir, name)
    if (await exists(p)) return p
  }
  return null
}

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
  // maps/mobs is in this list because the sources live there. Without it the
  // window listed only copies -- it would tell you to go and edit
  // maps/mobs/doom_monsters.lua and then offer no way to open it.
  for (const dir of ['maps', 'maps/mobs', 'scripts']) {
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
      // Is this the SOURCE, or the copy mapc left beside a baked map?
      //
      // mapc copies every script a map declares next to its .dnpm -- that is
      // what makes a baked map a complete deliverable -- so a module's maps/
      // is full of copies of files that live elsewhere. Editing one is editing
      // a build artefact: it works until the next bake and then silently does
      // not, which is the worst shape a bug can have.
      //
      // Detected by finding the same name under the engine's maps/, which is
      // where the shared libraries live and where every DOOM II map's `script`
      // line points.
      const source = await findSource(n)
      out.push({
        path: `${dir}/${n}`,
        bytes: text.length,
        mobs: [...new Set(mobs)],
        // Where to edit it instead, when this is a copy. Null when this IS the
        // source, which is the ordinary case for a module's own file.
        copyOf: source && source !== join(root, dir, n) ? relative(ROOT, source) : null
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

/**
 * What a new map contains: one sixteen-unit room at the origin, floor 0,
 * ceiling 32 (roomText's default), and a spawn in the middle of it.
 *
 * Counter-clockwise, which is the winding the renderer takes as fact -- see
 * editor/src/topology.js -- so the walls face in. Through parseMap and back
 * out, so the text on disk is the editor's own emitter's, not a string here
 * that agrees with it until it does not.
 */
function starterMap () {
  const h = 8
  const room = roomText(
    [{ x: -h, y: -h }, { x: h, y: -h }, { x: h, y: h }, { x: -h, y: h }], { floor: 0 })
  return emitMap(parseMap(`${room}spawn start 0 0 2\n`))
}

/**
 * The dev certificate's state: 'missing', 'expired' or 'ok', and when it ends.
 *
 * WebTransport over a self-signed certificate works only while the certificate
 * is currently valid and no more than fourteen days long -- which is why
 * server/edge regenerates one every thirteen. scripts/test.sh checks expiry
 * before it starts a stack; scripts/run_stack.sh, which Test uses, checks only
 * that the file exists. So the editor started a stack the browser then refused
 * with "WebTransport connection rejected", two days after the certificate had
 * quietly run out.
 *
 * `now` is a parameter so the decision can be tested at any point on the
 * calendar; the margin is an hour, because a certificate that expires
 * mid-session is one the browser rejects at the next reconnect with the same
 * screen and less to go on.
 */
const CERT = join(ROOT, 'server', 'edge', 'certs', 'cert.pem')
const GEN_CERT = join(ROOT, 'server', 'edge', 'scripts', 'gen_dev_cert.js')
const CERT_MARGIN_MS = 3600e3

function certState (pemPath = CERT, now = Date.now()) {
  let pem
  try { pem = readFileSync(pemPath) } catch { return { state: 'missing', validTo: null } }
  try {
    const validTo = new Date(new X509Certificate(pem).validTo)
    return { state: validTo.getTime() - now < CERT_MARGIN_MS ? 'expired' : 'ok', validTo }
  } catch {
    // Unreadable is missing: whatever is there, the edge cannot serve with it.
    return { state: 'missing', validTo: null }
  }
}

/**
 * Make sure a usable dev certificate exists, regenerating it the way test.sh
 * does -- through the edge's own script, so there is one way to make one.
 * Returns what was found and what was done, for the stack log.
 */
async function ensureDevCert ({ pemPath = CERT, gen = GEN_CERT, now = Date.now() } = {}) {
  const before = certState(pemPath, now)
  if (before.state === 'ok') return { ...before, regenerated: false }
  const why = before.state === 'missing' ? 'is missing' : `expired ${before.validTo.toISOString()}`
  const r = await new Promise((resolve) => {
    execFile(process.execPath, [gen], { cwd: dirname(dirname(gen)) }, (err, stdout, stderr) =>
      resolve({ ok: !err, out: String(stdout) + String(stderr) }))
  })
  const after = certState(pemPath, now)
  return { ...after, regenerated: r.ok && after.state === 'ok', why, output: r.out.trim() }
}

async function startStack (mapRelPath, module = null) {
  stopStack()
  stack.log = []

  // Before run_stack.sh, which would start a stack the browser refuses. Said
  // in the stack log, which the page shows, rather than only on the console.
  const cert = await ensureDevCert()
  if (cert.why) {
    stack.log.push(cert.regenerated
      ? `[editor] the dev certificate ${cert.why}; regenerated, valid to ${cert.validTo.toISOString()}\n`
      : `[editor] the dev certificate ${cert.why} and could NOT be regenerated:\n${cert.output}\n`)
    process.stdout.write(stack.log[stack.log.length - 1])
  }
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
   * The sprite frames that were actually extracted.
   *
   * The reason the editor can offer animation editing at all. props.js set
   * itself a limit -- it would not let you TYPE a frame name, because one that
   * does not exist draws as nothing with no way to tell why -- and this is what
   * replaces typing with choosing.
   *
   * The names are whole frame names, rotation digit included: BKEYA0 for a
   * thing with one view, POSSA1..POSSA8 for one with eight. A `rot8` sprite
   * line names the BASE (POSSA) and the renderer appends the digit, so the
   * bases are reported separately -- the editor needs the list a rot8 state can
   * choose from, and deriving it in the browser would be a second copy of the
   * naming rule that client/src/render/props.js owns.
   */
  'GET /api/sprites': async (url) => {
    const module = url.searchParams.get('module') || null
    const where = moduleAsset(module, join('sprites', 'index.json'))
    if (!where) return { code: 400, body: { error: 'no such module' } }

    const from = (await exists(where.own)) ? where.own : where.fallback
    const raw = await readFile(from, 'utf8').catch(() => null)
    if (raw === null) {
      return {
        code: 200,
        body: {
          frames: [],
          bases: [],
          note: `no ${relative(ROOT, where.own)}` +
                (module ? ` and no ${relative(ROOT, where.fallback)}` : '')
        }
      }
    }
    try {
      const sizes = JSON.parse(raw).sizes ?? {}
      const frames = Object.keys(sizes).sort()

      // A base is rot8-able when every one of its eight rotations is present.
      // All eight rather than just the first: a base missing rotation 5 is a
      // monster that vanishes when you walk round it, and offering it as a
      // choice would put that in the catalogue on purpose.
      const seen = new Map()
      for (const f of frames) {
        const base = f.slice(0, -1)
        const digit = f.slice(-1)
        if (!/^[1-8]$/.test(digit)) continue
        seen.set(base, (seen.get(base) ?? 0) | (1 << (Number(digit) - 1)))
      }
      const bases = [...seen].filter(([, mask]) => mask === 0xff).map(([b]) => b).sort()

      return { code: 200, body: { module, path: relative(ROOT, from), frames, bases } }
    } catch (e) {
      return {
        code: 200,
        body: { frames: [], bases: [], note: `${relative(ROOT, from)} is not valid JSON: ${e.message}` }
      }
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

  /**
   * The mob vocabulary, as JSON.
   *
   * Generated from maps/scripts/mob_vocab.lua by the real interpreter
   * (tools/vocabjson) and checked in, exactly as the trigger one is. The
   * editor's mob UI is generated from it -- which is what mob_vocab.lua's own
   * header asks for -- so a missing file means the build step has not been run
   * and the window says so rather than rendering an empty form.
   */
  'GET /api/mobvocab': async () => {
    const raw = await readFile(join(ROOT, 'maps', 'scripts', 'mob_vocab.json'), 'utf8')
      .catch(() => null)
    if (raw === null) {
      return {
        code: 200,
        body: {
          error: 'maps/scripts/mob_vocab.json is missing -- run ' +
                 './scripts/build_lua_tests.sh && ./build/lua/trigger_vocab_json . ' +
                 'maps/scripts/mob_vocab.lua mob_vocab > maps/scripts/mob_vocab.json'
        }
      }
    }
    try {
      return { code: 200, body: { vocab: JSON.parse(raw) } }
    } catch (e) {
      return { code: 200, body: { error: `mob_vocab.json is not valid JSON: ${e.message}` } }
    }
  },

  /**
   * The mobs the EDITOR owns, and the Lua generated from them.
   *
   * The sidecar is the source and the .lua is its output -- docs/TRIGGERS.md's
   * arrangement, for its reason: generated source cannot be round-tripped once
   * a person has touched it. Hand-authored mob files are somebody else's and
   * are not read here.
   */
  'GET /api/mobdoc': async (url) => {
    const module = url.searchParams.get('module') || null
    const root = moduleRoot(module)
    if (!root) return { code: 400, body: { error: 'no such module' } }
    const p = join(root, 'mobs.editor.json')
    const raw = await readFile(p, 'utf8').catch(() => null)
    if (raw === null) return { code: 200, body: { module, doc: null, path: relative(ROOT, p) } }
    try {
      return { code: 200, body: { module, doc: JSON.parse(raw), path: relative(ROOT, p), hash: textHash(raw) } }
    } catch (e) {
      // Reported, never discarded on the author's behalf: it is the only copy
      // of their mobs.
      return { code: 200, body: { module, doc: null, error: `${relative(ROOT, p)} is not valid JSON: ${e.message}` } }
    }
  },

  /**
   * Write the sidecar AND the Lua it generates, and check the Lua LOADS.
   *
   * The check is tools/mobcheck/check.lua under LuaJIT -- the real loader, the
   * one the server uses. Nothing is written unless it passes, because a mob
   * file the server refuses stops the whole map from loading, and finding that
   * out by pressing Test is a worse way to learn it than being told here.
   */
  'POST /api/mobdoc': async (url, body) => {
    const req = JSON.parse(body)
    const module = req.module ?? null
    const root = moduleRoot(module)
    if (!root) return { code: 400, body: { error: 'no such module' } }
    if (typeof req.lua !== 'string' || typeof req.doc !== 'object') {
      return { code: 400, body: { error: 'need both the document and its Lua' } }
    }

    const sidecar = join(root, 'mobs.editor.json')
    const luaPath = join(root, 'maps', 'editor_mobs.lua')

    if (!req.force && typeof req.baseHash === 'string') {
      const current = await readFile(sidecar, 'utf8').catch(() => null)
      if (current !== null && textHash(current) !== req.baseHash) {
        return { code: 409, body: { error: 'the mob sidecar has changed since it was opened', current } }
      }
    }

    // Checked BEFORE either file is written, against a temporary copy. Writing
    // first and checking after would leave a module whose maps do not load
    // while the editor reported the problem.
    const probe = join(BUILD, `mobcheck-${Date.now()}.lua`)
    await mkdir(BUILD, { recursive: true })
    await writeFile(probe, req.lua)
    const checked = await run(LUAJIT, [join(ROOT, 'tools', 'mobcheck', 'check.lua'), probe])
    await unlink(probe).catch(() => {})

    if (!checked.ok && await exists(LUAJIT)) {
      return {
        code: 200,
        body: {
          ok: false,
          problems: (checked.stdout + checked.stderr).trim().split('\n').filter(Boolean)
        }
      }
    }

    const text = JSON.stringify(req.doc, null, 2) + '\n'
    await mkdir(dirname(luaPath), { recursive: true })
    await writeFile(sidecar, text)
    await writeFile(luaPath, req.lua)
    return {
      code: 200,
      body: {
        ok: true,
        hash: textHash(text),
        sidecar: relative(ROOT, sidecar),
        lua: relative(ROOT, luaPath),
        // What the loader said it found, so "saved" is a fact rather than a
        // hope: 3 mobs, 0 problems.
        loaded: checked.stdout.trim().split('\n').pop() ?? ''
      }
    }
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

  /**
   * Which engine this editor is pointed at, and how it decided.
   *
   * The question only gets asked once something has gone wrong -- an empty map
   * list, a Test that will not start -- so the answer is a request away rather
   * than a log line in a terminal somebody closed. `mapc` and `luajit` are
   * reported as present or not, because "found the engine" and "the engine is
   * built" are different facts and the second is the one Test needs.
   */
  'GET /api/engine': async () => ({
    code: 200,
    body: {
      root: ROOT,
      how: ENGINE.how,
      mapc: await exists(MAPC),
      luajit: await exists(LUAJIT),
      editor: EDITOR
    }
  }),

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

  /**
   * A new map, made and written -- for the command palette.
   *
   * The page's own New makes NO file: it opens an empty document and leaves
   * the name to the author, on the reasoning that inventing untitled.map on
   * disk is a file nobody asked for. A palette command is the author asking,
   * name first, so this writes one. What it writes is the editor's own idea of
   * a room -- shapes.js's roomText, the thing the draw tool emits -- with
   * somewhere to stand, so Test works before a wall has been drawn; and it is
   * round-tripped through parseMap/emitMap, so the file is what the editor
   * would have written rather than a template that drifts from it.
   */
  'POST /api/newmap': async (url, body) => {
    const req = JSON.parse(body)
    const module = req.module ?? null
    const root = moduleRoot(module)
    // moduleRoot() answers for any WELL-FORMED id, whether or not the module
    // is there -- it is a path rule, not a directory check -- and this route
    // creates files, so it has to look. Found by the test: a map for a module
    // that did not exist got a 200 and a fresh modules/nosuch/maps/.
    if (!root || (module && !await exists(root))) return { code: 400, body: { error: `no such module: ${module}` } }
    if (typeof req.name !== 'string' || !MAP_NAME.test(req.name)) {
      return { code: 400, body: { error: `'${req.name}' is not a map name: letters, digits, - and _, starting with a letter or digit` } }
    }
    const rel = `${req.name}.map`
    const p = underMaps(rel, { extensions: ['.map'], module })
    if (!p) return { code: 400, body: { error: 'not a map path' } }
    if (await exists(p)) return { code: 409, body: { error: `${relative(ROOT, p)} already exists` } }

    const text = starterMap()
    await mkdir(dirname(p), { recursive: true })
    await writeFile(p, text)
    return { code: 200, body: { path: rel, module, file: p, hash: textHash(text) } }
  },

  /**
   * A new module, made -- for the command palette.
   *
   * What a module IS comes from docs/MODULES.md: modules/<id>/ with
   * module.txt, maps/, scripts/ and assets/, and "nothing in it is required" --
   * the engine's assets/ fills in whatever a module does not carry. So this
   * makes the four things and no more: no props.txt, no scripts, which would
   * be inventing content the doc says a module need not have. module.txt is
   * written in the shape the two existing ones have, columns and all, and
   * says `id` so the reader that warns about a mismatch has something to
   * agree with.
   */
  'POST /api/newmodule': async (url, body) => {
    const req = JSON.parse(body)
    if (typeof req.id !== 'string' || !MODULE_ID.test(req.id)) {
      return { code: 400, body: { error: `'${req.id}' is not a module id: letters, digits, - and _, starting with a letter or digit` } }
    }
    const root = join(MODULES, req.id)
    if (await exists(root)) return { code: 409, body: { error: `modules/${req.id} already exists` } }

    const name = typeof req.name === 'string' && req.name.trim() ? req.name.trim().replace(/\s+/g, ' ') : req.id
    const about = typeof req.about === 'string' ? req.about.trim() : ''
    const lines = [`name    ${name}`, `id      ${req.id}`]
    for (const l of about.split('\n').map((x) => x.trim()).filter(Boolean)) lines.push(`about   ${l}`)

    for (const d of ['maps', 'scripts', 'assets']) await mkdir(join(root, d), { recursive: true })
    await writeFile(join(root, 'module.txt'), lines.join('\n') + '\n')
    return { code: 200, body: { id: req.id, name, root, path: relative(ROOT, root) } }
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
    await startStack(relative(ROOT, dnpm), module)
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
//
// BY REAL PATH, not by URL. Node resolves symlinks when it loads an ES module,
// so import.meta.url is the file's real location while process.argv[1] is
// whatever was typed -- and if that was a symlink (a scratch engine whose
// tools/ points at the real one, which is how the extension is tested) the
// two differ, this is false, and the server imports itself, does nothing and
// exits 0. Found by exactly that: "the editor server did not come up (it
// exited with 0)", with no output, because nothing had gone wrong.
const IS_MAIN = (() => {
  if (!process.argv[1]) return false
  try { return realpathSync(process.argv[1]) === fileURLToPath(import.meta.url) } catch { return false }
})()

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
    console.log(`[editor] engine root ${ROOT} (${ENGINE.how})`)
    console.log(`[editor] engine maps under ${join(ROOT, 'maps')}, modules under ${MODULES}`)
    console.log(`[editor] mapc      ${MAPC}`)
    console.log('[editor] Test mode runs scripts/run_stack.sh, which serves the GAME on 8080')
  })
}

export {
  ROOT, ENGINE, certState, ensureDevCert,
  underMaps, underModuleLua, textHash, listMaps, resolveStatic, MOUNTS,
  moduleRoot, moduleAsset
}
