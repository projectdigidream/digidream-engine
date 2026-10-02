//
// scripts/build_app.sh: does staging a module actually produce a runnable app,
// and does staging a module that is NOT Doom produce one with no Doom in it?
//
// THE SECOND QUESTION IS THE POINT. Every prop with a picture in this engine
// used to live in assets/, and every one of those pictures came out of
// DOOM2.WAD. An app built from somebody's own module would have carried all of
// it -- 2,563 sprites and 563 textures belonging to id Software -- without
// anybody choosing that, because it was simply what "the engine's assets" meant.
//
// So the claim is "a module is a ROOT, and an app contains the modules named and
// nothing else", and the control is an app staged from `shootrange`, which has
// no assets of its own: it must come out with ZERO images in it. If Doom art
// ever leaks back into the engine's assets/, that count stops being zero and
// this test fails, which is the only way that regression gets noticed at all --
// it is invisible in a running game, where everything looks right.
//
// House style: standalone, prints ok/FAIL, ends with a count.
//
import { execFileSync } from 'node:child_process'
import { readFileSync, existsSync, rmSync, mkdirSync, writeFileSync } from 'node:fs'
import { readdirSync, statSync } from 'node:fs'
import { dirname, join, extname } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..', '..')
// The repository: the engine is ROOT, and modules, docs, the studio and the
// synth are beside it.
const REPO = join(ROOT, '..')
const OUT = join(ROOT, 'build', 'app-test')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

/** Every file under `dir`, relative to it. */
function walk (dir, base = dir, out = []) {
  if (!existsSync(dir)) return out
  for (const e of readdirSync(dir)) {
    const p = join(dir, e)
    if (statSync(p).isDirectory()) walk(p, base, out)
    else out.push(p.slice(base.length + 1))
  }
  return out
}

const stage = (args) =>
  execFileSync(join(ROOT, 'scripts', 'build_app.sh'), args,
               { cwd: ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] })

rmSync(OUT, { recursive: true, force: true })

console.log('app: staging a module into a standalone application')

// --- a Doom app ---------------------------------------------------------------
console.log('\n--- doom2 ---')
stage(['--module', 'doom2', '--out', 'build/app-test'])
const doom = join(OUT, 'doom2')

// Everything app/main.js reaches for at startup. Listed explicitly rather than
// counted: "the tree is big" is not the same claim as "the tree is complete",
// and a missing preload fails at launch with a blank window.
const REQUIRED = [
  'game.json', 'package.json',
  'app/main.js', 'app/preload.cjs', 'app/static.js', 'app/relay.js', 'app/studio_files.js',
  'app/framing.js', 'app/upstream.js',
  'bin/dnp-server',
  'assets/props.txt',
  'client/index.html',
  'client/src/main.js', 'client/src/net_local.js', 'client/src/net.js',
  'client/wasm/dist/dnp_core.mjs',
  'client/vendor/three/three.module.js'
]
const missing = REQUIRED.filter((f) => !existsSync(join(doom, f)))
check(missing.length === 0, `every file the shell needs is staged${missing.length ? ': missing ' + missing.join(', ') : ''}`)

const manifest = JSON.parse(readFileSync(join(doom, 'game.json'), 'utf8'))
check(manifest.name === 'DOOM II', `game.json takes its name from module.txt (${manifest.name})`)
check(Array.isArray(manifest.modules) && manifest.modules.length === 1 &&
      manifest.modules[0] === 'doom2', 'and names exactly the module asked for')

const pkg = JSON.parse(readFileSync(join(doom, 'package.json'), 'utf8'))
check(pkg.main === 'app/main.js', 'package.json points Electron at the shell')
check(pkg.type === 'module', 'and declares ESM, which is why the preload is .cjs')

const doomMaps = walk(join(doom, 'modules', 'doom2', 'maps'))
check(doomMaps.filter((f) => extname(f) === '.dnpm').length === 32,
      `all 32 baked maps are there (${doomMaps.filter((f) => extname(f) === '.dnpm').length})`)
check(doomMaps.filter((f) => extname(f) === '.map').length === 0,
      'and the .map sources are not -- the server reads only the baked form')
check(doomMaps.some((f) => extname(f) === '.lua'),
      "but the maps' scripts are, because a baked map is nothing without them")

// SOUND (docs/SOUNDS.md): the engine, where the client's worklet imports it,
// and the module's bank, which travels with the module. Only when the engine
// has been built -- staging says so out loud when it has not.
if (existsSync(join(REPO, 'sound', 'tool', 'dist', 'dsound.mjs'))) {
  check(existsSync(join(doom, 'sfx', 'dsound.mjs')), 'the sound engine is staged at sfx/')
} else {
  console.log('  (sound/tool/dist/dsound.mjs not built; the sound engine check is skipped)')
}
check(existsSync(join(doom, 'modules', 'doom2', 'assets', 'sounds', 'bank.sfxproj')),
      "and the module's sound bank travels with it")
// And the game scripts module.txt names, beside the maps that also list them.
const gameLines = readFileSync(join(doom, 'modules', 'doom2', 'module.txt'), 'utf8')
  .split('\n').map((l) => l.match(/^\s*game\s+(\S+)/)?.[1]).filter(Boolean)
check(gameLines.length >= 4 && gameLines.every((g) => existsSync(join(doom, 'modules', 'doom2', 'maps', g))),
      `every game script the module declares is staged (${gameLines.join(', ')})`)

const doomFiles = walk(doom)
const doomImages = doomFiles.filter((f) => extname(f) === '.png')
check(doomImages.length > 2000, `and the art it needs (${doomImages.length} images)`)
check(doomImages.every((f) => f.startsWith(join('modules', 'doom2') + '/')),
      'all of it inside the module, none of it in the engine\'s assets/')

// --- THE CONTROL: an app that is not Doom -------------------------------------
//
// shootrange declares no assets at all. If staging it yields a single image,
// something is copying the engine's assets wholesale -- which is exactly the
// arrangement this was built to end.
console.log('\n--- CONTROL: a module that is not Doom ---')
stage(['--module', 'shootrange', '--out', 'build/app-test'])
const range = join(OUT, 'shootrange')

const rangeFiles = walk(range)
const rangeImages = rangeFiles.filter((f) => extname(f) === '.png')
check(rangeImages.length === 0,
      `an app built from shootrange contains no images at all (${rangeImages.length})`)
check(!rangeFiles.some((f) => f.includes('doom2')),
      'and nothing from the doom2 module')
check(existsSync(join(range, 'bin', 'dnp-server')) &&
      existsSync(join(range, 'client', 'src', 'main.js')),
      'while still being a complete app: server, client and shell')

// The engine's own catalogue, as shipped. This is the file a new project
// starts from, and it must carry no art anybody else owns.
const engineCat = readFileSync(join(range, 'assets', 'props.txt'), 'utf8')
const declared = [...engineCat.matchAll(/^(prop|sprite|mesh)\s/gm)]
check(declared.length === 0,
      `the engine catalogue declares no props or sprites (${declared.length})`)
check(/^vfx\s/m.test(engineCat),
      'but does still carry what the engine itself authored (the tracers)')

// --- the studio, only when asked for --------------------------------------------
//
// An app built without --studio must not carry the editor: the control comes
// first, on the shootrange app just staged, and then the same module is
// staged again with the studio.
console.log('\n--- the studio ---')
check(!existsSync(join(range, 'studio')), 'CONTROL: without --studio the app has no studio in it')
if (!existsSync(join(REPO, 'studio', 'wasm', 'dist', 'mapc.mjs'))) {
  console.log('  (studio wasm not built -- scripts/build_mapc_wasm.sh -- staging with --studio is not checked)')
} else {
  stage(['--module', 'shootrange', '--studio', '--out', 'build/app-test'])
  const NEEDED = ['studio/index.html', 'studio/src/main.js', 'studio/vendor/tsprite.js', 'studio/wasm/dist/mapc.wasm', 'studio/wasm/dist/mapc.mjs',
                  'studio/runtime/triggers.lua', 'studio/runtime/mobs.lua', 'studio/runtime/trigger_vocab.json', 'studio/runtime/mob_vocab.json']
  const gone = NEEDED.filter((f) => !existsSync(join(range, f)))
  check(gone.length === 0, `--studio stages the studio, its compiler and its runtime${gone.length ? ': missing ' + gone.join(', ') : ''}`)
  check(JSON.parse(readFileSync(join(range, 'game.json'), 'utf8')).studio === true, 'and game.json says so')
}

// --- refusals -----------------------------------------------------------------
//
// Both of these produce an app that starts and hosts nothing, which is a much
// worse failure than not building: it looks like a bug in the game.
console.log('\n--- what it refuses ---')
{
  let threw = false
  try { stage(['--module', 'nosuchmodule', '--out', 'build/app-test']) }
  catch { threw = true }
  check(threw, 'a module that does not exist is refused')

  const empty = join(REPO, 'modules', '__stagetest_empty')
  rmSync(empty, { recursive: true, force: true })
  mkdirSync(join(empty, 'maps'), { recursive: true })
  writeFileSync(join(empty, 'module.txt'), 'name Empty\nid __stagetest_empty\n')
  let threw2 = false
  try { stage(['--module', '__stagetest_empty', '--out', 'build/app-test']) }
  catch { threw2 = true }
  rmSync(empty, { recursive: true, force: true })
  check(threw2, 'and so is a module with no baked maps')
}

rmSync(OUT, { recursive: true, force: true })

console.log(`\napp: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
