//
// A SINGLE-PLAYER GAME AS A STATIC FOLDER (docs/WEB.md), for any static host:
// GitHub Pages, a project site under a path included.
//
//   node tools/webgame/build.mjs <out> --module doom2 [--module doom2fps ...]
//
// The page with its server inside it: the client, both wasm modules (the
// core and the server, scripts/build_wasm.sh and build_server_web.sh), the
// link framing, the game's art and catalogues laid out as the dev server
// serves them (tools/gamefiles.js, the rules they share), and the server's own
// files under game/ with the manifest the server's Worker reads. index.html
// says DNP_LOCAL, and DNP_BASE is its own directory, so the folder works at a
// site's root and under any path.
//
import { cp, mkdir, readFile, rm, writeFile, readdir, copyFile } from 'node:fs/promises'
import { existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { ROOT, REPO, ART, resolveModule, catalogue, uiIndex, gameFiles, gameArgs } from '../gamefiles.js'

const argv = process.argv.slice(2)
const out = argv.find((a, i) => !a.startsWith('--') && argv[i - 1] !== '--module')
const modules = []
for (let i = 0; i < argv.length; i++) {
  if (argv[i] !== '--module') continue
  const { root, candidates } = resolveModule(argv[++i])
  if (!root) { console.error(`[webgame] no such module: ${argv[i]} (looked in ${candidates.join(', ')})`); process.exit(2) }
  modules.push(root)
}
if (!out) { console.error('usage: node tools/webgame/build.mjs <out> --module <name> ...'); process.exit(2) }
const need = (p, how) => { if (!existsSync(p)) { console.error(`[webgame] missing ${p} -- ${how}`); process.exit(2) } }
need(join(ROOT, 'client', 'wasm', 'dist', 'dnp_core.mjs'), 'run scripts/build_wasm.sh')
need(join(ROOT, 'client', 'wasm', 'dist', 'dnp-server.mjs'), 'run scripts/build_server_web.sh')
need(join(ROOT, 'client', 'node_modules', 'three', 'build'), 'npm install in client/')

await rm(out, { recursive: true, force: true })
await mkdir(out, { recursive: true })

// The client, as every shell serves it.
await cp(join(ROOT, 'client', 'src'), join(out, 'src'), { recursive: true })
await cp(join(ROOT, 'client', 'wasm', 'dist'), join(out, 'wasm', 'dist'), { recursive: true })
await cp(join(ROOT, 'client', 'node_modules', 'three', 'build'), join(out, 'vendor', 'three'), { recursive: true })
await copyFile(join(ROOT, 'client', 'shell.js'), join(out, 'shell.js'))
await copyFile(join(ROOT, 'server', 'edge', 'src', 'framing.js'), join(out, 'framing.js'))
// Its page, with every root path made relative, the server in the page on,
// and the base its own directory.
let html = await readFile(join(ROOT, 'client', 'index.html'), 'utf8')
html = html.replaceAll('"/vendor/three/', '"./vendor/three/')
           .replace('<script src="/shell.js"></script>', '<script src="./shell.js"></script>')
           .replace('<script type="module" src="/src/main.js"></script>',
                    '<script>globalThis.DNP_LOCAL = true; globalThis.DNP_BASE = new URL(\'./\', location.href).pathname</script>\n' +
                    '  <script type="module" src="./src/main.js"></script>')
if (!html.includes('DNP_LOCAL') || html.includes('src="/src/')) { console.error('[webgame] client/index.html changed shape; update this script'); process.exit(2) }
await writeFile(join(out, 'index.html'), html)

// Art, per file, the first module winning: the engine's first, then the
// modules from last to first, each copy over the one before.
for (const kind of ART) {
  for (const dir of [join(ROOT, 'assets', kind), ...modules.slice().reverse().map((m) => join(m, 'assets', kind))]) {
    if (existsSync(dir)) await cp(dir, join(out, kind), { recursive: true, force: true })
  }
}
for (const name of ['props.txt', 'animations.txt']) {
  const text = await catalogue(modules, name)
  if (text) await writeFile(join(out, name), text)
}
await mkdir(join(out, 'ui'), { recursive: true })
await writeFile(join(out, 'ui', 'index.json'), JSON.stringify(await uiIndex(modules)))

// The synth and the sound engine, when built (as the app and the APK).
if (existsSync(join(REPO, 'midisynth', 'tool', 'dist', 'midisynth.mjs'))) {
  await cp(join(REPO, 'midisynth', 'tool'), join(out, 'synth'), { recursive: true })
  await cp(join(REPO, 'midisynth', 'content'), join(out, 'synth-content'), { recursive: true })
} else console.warn('[webgame] no synth built (midisynth/scripts/build_wasm.sh): no music')
if (existsSync(join(REPO, 'sound', 'tool', 'dist', 'dsound.mjs'))) {
  await mkdir(join(out, 'sfx'), { recursive: true })
  await copyFile(join(REPO, 'sound', 'tool', 'dist', 'dsound.mjs'), join(out, 'sfx', 'dsound.mjs'))
} else console.warn('[webgame] no sound engine built (sound/scripts/build_wasm.sh): no sound effects')

// The server's files, and the manifest its Worker reads.
const files = await gameFiles(modules)
for (const [rel, abs] of files) {
  await mkdir(dirname(join(out, 'game', rel)), { recursive: true })
  await copyFile(abs, join(out, 'game', rel))
}
await writeFile(join(out, 'game', 'manifest.json'),
  JSON.stringify({ root: '/game', args: gameArgs(modules), files: files.map(([r]) => r) }))
// GitHub Pages runs Jekyll unless told not to, which drops files and folders
// whose names start with an underscore.
await writeFile(join(out, '.nojekyll'), '')

const count = async (d) => { let n = 0; for (const e of await readdir(d, { withFileTypes: true, recursive: true })) if (e.isFile()) n++; return n }
console.log(`[webgame] ${out}: ${await count(out)} files; ${files.length} for the server; modules ${modules.map((m) => m.split('/').pop()).join(', ') || '(the engine test level)'}`)
