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
const MAPS = join(ROOT, 'maps')
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
function underMaps (p, { extensions = ['.map', '.lua', '.json'] } = {}) {
  if (typeof p !== 'string' || p.length === 0 || p.length > 512) return null
  if (p.includes('\0')) return null
  const full = normalize(join(MAPS, p.replace(/^\/+/, '')))
  const rel = relative(MAPS, full)
  if (rel === '' || rel.startsWith('..') || rel.startsWith(sep) || rel.includes('..')) return null
  if (!extensions.includes(extname(full))) return null
  return full
}

// --- the map list ----------------------------------------------------------

async function listMaps () {
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

function startStack (mapRelPath) {
  stopStack()
  stack.log = []
  const child = spawn(join(ROOT, 'scripts', 'run_stack.sh'), ['--map', mapRelPath], {
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

  'GET /api/maps': async () => ({ code: 200, body: { maps: await listMaps() } }),

  'GET /api/map': async (url) => {
    const p = underMaps(url.searchParams.get('path'), { extensions: ['.map'] })
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
    return {
      code: 200,
      body: { path: url.searchParams.get('path'), text, sidecar, sidecarStale: stale, lua, hash: textHash(text) }
    }
  },

  'GET /api/props': async () => {
    const text = await readFile(join(ROOT, 'assets', 'props.txt'), 'utf8').catch(() => null)
    if (text === null) return { code: 404, body: { error: 'no assets/props.txt' } }
    return { code: 200, body: { text } }
  },

  'GET /api/stack': async () => ({
    code: 200,
    body: { running: !!stack.child, map: stack.map, url: stack.url, log: stack.log.join('') }
  }),

  // --- writing ---

  'POST /api/map': async (url, body) => {
    const req = JSON.parse(body)
    const p = underMaps(req.path, { extensions: ['.map'] })
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

  'POST /api/compile': async (url, body) => {
    const req = JSON.parse(body)
    const p = underMaps(req.path, { extensions: ['.map'] })
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
    startStack(relative(ROOT, dnpm))
    return { code: 200, body: { started: true, map: relative(ROOT, dnpm) } }
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
    console.log(`[editor] maps under ${MAPS}`)
    console.log(`[editor] mapc      ${MAPC}`)
    console.log('[editor] Test mode runs scripts/run_stack.sh, which serves the GAME on 8080')
  })
}

export { underMaps, textHash, listMaps, resolveStatic, MOUNTS }
