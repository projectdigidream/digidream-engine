//
// A GAME'S FILES, AS THE PAGE AND ITS SERVER SEE THEM: the rules the dev server
// serves by (tools/devserver.js) and a static web build copies by
// (tools/webgame/build.mjs), in one place, so the two cannot drift.
//
//   * art (textures, sprites, music, sounds, ui) per FILE: the first module
//     that has it, else the engine's assets/;
//   * the catalogues (props.txt, animations.txt) CONCATENATED, the engine's
//     first, then each module's, as the server reads them;
//   * ui/index.json MERGED, the first module to name a key winning;
//   * the server's own files for the server in the page (docs/WEB.md): of each
//     module its module.txt, all of maps/ and assets/props.txt; with no module,
//     the engine's test level and props, as run_stack.sh would start it.
//
import { readFile, readdir, stat } from 'node:fs/promises'
import { existsSync } from 'node:fs'
import { basename, dirname, join, isAbsolute } from 'node:path'
import { fileURLToPath } from 'node:url'

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..')
export const REPO = join(ROOT, '..')
export const ART = ['textures', 'sprites', 'music', 'sounds', 'ui']

/**
 * A module named on a command line, as the server takes it: `doom2` or
 * `modules/doom2`, beside the engine first, then the engine's own modules/.
 * Null when there is none, with where it looked.
 */
export function resolveModule (given) {
  const candidates = isAbsolute(given)
    ? [given]
    : [join(REPO, 'modules', given), join(REPO, given), join(ROOT, given), join(ROOT, 'modules', given)]
  const root = candidates.find((p) => existsSync(p))
  return root ? { root } : { root: null, candidates }
}

/** The first module with `rel` (a path under a module root), else the engine's. */
export async function artFile (modules, rel) {
  for (const m of modules) {
    const p = join(m, rel)
    if (await stat(p).then(() => true, () => false)) return p
  }
  return join(ROOT, rel)
}

/** props.txt or animations.txt as the page reads it, or null if nobody has one. */
export async function catalogue (modules, name) {
  const rel = join('assets', name)
  const parts = []
  const base = await readFile(join(ROOT, rel)).catch(() => null)
  if (base) parts.push(base.toString())
  for (const m of modules) {
    const extra = await readFile(join(m, rel)).catch(() => null)
    if (extra) parts.push('\n# --- ' + m + ' ---\n' + extra.toString())
  }
  return parts.length ? parts.join('\n') : null
}

/** ui/index.json, merged. */
export async function uiIndex (modules) {
  const out = { version: 1, default: null, maps: {} }
  for (const dir of [...modules, ROOT]) {
    const raw = await readFile(join(dir, 'assets', 'ui', 'index.json'), 'utf8').catch(() => null)
    if (!raw) continue
    let j
    try { j = JSON.parse(raw) } catch { continue }
    if (j.default) out.default ??= j.default
    for (const [k, v] of Object.entries(j.maps ?? {})) out.maps[k] ??= v
  }
  return out
}

/** What the server in the page reads, as [published path, file on disk]. */
export async function gameFiles (modules) {
  const out = []
  const walk = async (dir, rel) => {
    for (const e of await readdir(dir, { withFileTypes: true })) {
      if (e.name.startsWith('.')) continue
      if (e.isDirectory()) await walk(join(dir, e.name), `${rel}/${e.name}`)
      else out.push([`${rel}/${e.name}`, join(dir, e.name)])
    }
  }
  if (modules.length) {
    for (const m of modules) {
      const name = basename(m)
      out.push([`${name}/module.txt`, join(m, 'module.txt')])
      if (existsSync(join(m, 'maps'))) await walk(join(m, 'maps'), `${name}/maps`)
      if (existsSync(join(m, 'assets', 'props.txt'))) out.push([`${name}/assets/props.txt`, join(m, 'assets', 'props.txt')])
    }
  } else {
    const built = join(ROOT, 'build', process.arch === 'arm64' ? 'arm64' : 'x86_64')
    for (const f of await readdir(built)) if (/\.(dnpm|lua|json)$/.test(f)) out.push([`maps/${f}`, join(built, f)])
    out.push(['assets/props.txt', join(ROOT, 'assets', 'props.txt')])
  }
  return out
}

/** The server's command line for those files, under /game. */
export function gameArgs (modules) {
  // --single-player: the page's own server, for its one player.
  return [...(modules.length
    ? modules.flatMap((m) => ['--module', `/game/${basename(m)}`])
    : ['--map', '/game/maps/test.dnpm']), '--single-player']
}
