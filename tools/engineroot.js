//
// Where the engine is.
//
// The editor's tools -- mapc, luajit, run_stack.sh, the mob checker, the client
// -- all live at the engine's root, and a MODULE does not: a module is a folder
// of assets, maps and scripts that can sit anywhere. Open modules/doom2 as a
// workspace and none of what the editor needs is under it. So the editor has
// to find the engine, and this is the one rule for how.
//
// ONE RULE, IN ONE PLACE, because it is about to be needed twice. The editor
// server needs it today (and its tests needed it before that: with the root
// fixed to the script's own parent, the only way to test the server was
// against the real repo, which is why the browser tests build a throwaway
// module inside modules/). A VS Code extension will need it next, reading the
// same answer from a `dnp.engineRoot` setting. Two copies of "walk up looking
// for run_stack.sh" would be two that disagree the first time one is fixed.
//
// The precedence is explicit, then discovered, then assumed:
//
//   1. an explicit path       --engine-root, DNP_ENGINE_ROOT, or the setting.
//                              It is CHECKED, not trusted: a path that is not an
//                              engine is refused with what was looked for,
//                              rather than producing an editor that lists no
//                              maps and says nothing.
//   2. walk up from `from`     the working directory, or the workspace folder.
//                              Zero configuration when the module lives in
//                              <engine>/modules/, which is the repo's layout.
//   3. the fallback            beside the script, which is what the server has
//                              always assumed and is right whenever it is run
//                              from its own checkout.
//
import { existsSync } from 'node:fs'
import { dirname, join, resolve, relative, sep } from 'node:path'

/**
 * What makes a directory the engine's root.
 *
 * Two markers, and both are SOURCE files present in any checkout. Not
 * build/<arch>/mapc: a fresh clone has not built anything yet, and an editor
 * that refused to start until it had would be refusing over a step it could
 * have named. Whether mapc exists is reported separately, by /api/engine.
 */
export const MARKERS = ['scripts/run_stack.sh', 'core']

export const isEngineRoot = (dir) => MARKERS.every((m) => existsSync(join(dir, m)))

/**
 * What a module id may look like.
 *
 * ONE definition. The editor server had this inline in moduleRoot() and the
 * extension needs the same answer when it reads an id off a path: a map under
 * modules/_scratch/ must be refused HERE, with a reason, rather than located
 * and then refused by the server with "no such module". Two copies of a regex
 * are two that disagree the first time one is loosened.
 */
export const MODULE_ID = /^[A-Za-z0-9][A-Za-z0-9_-]*$/

/** Walk up from `from` to the filesystem root, or return null. */
export function findEngineRoot (from) {
  let dir = resolve(from)
  for (;;) {
    if (isEngineRoot(dir)) return dir
    const up = dirname(dir)
    if (up === dir) return null
    dir = up
  }
}

/**
 * Resolve the engine root, and say how.
 *
 * Returns { root, how } where `how` is one of 'explicit', 'found', 'fallback'
 * -- kept so the server can print it and the page can show it. "Which engine
 * is this editor pointed at" is a question that only ever gets asked once
 * something has gone wrong, and the answer should already be on screen.
 *
 * Throws on an explicit path that is not an engine. Not a warning: an explicit
 * setting is a person saying "here", and silently using somewhere else instead
 * is the kind of helpfulness that costs an afternoon.
 */
export function resolveEngineRoot ({ explicit = null, from = null, fallback = null } = {}) {
  if (explicit) {
    const root = resolve(explicit)
    if (!isEngineRoot(root)) {
      throw new Error(
        `${root} is not an engine root: expected ${MARKERS.join(' and ')} under it. ` +
        'Point --engine-root / DNP_ENGINE_ROOT at the DoomNetPlatformer checkout.')
    }
    return { root, how: 'explicit' }
  }
  if (from) {
    const found = findEngineRoot(from)
    if (found) return { root: found, how: 'found' }
  }
  if (fallback) return { root: resolve(fallback), how: 'fallback' }
  throw new Error(
    `no engine root: none given, none found above ${from ?? '(nowhere)'}, and no fallback`)
}

/**
 * From a map file's absolute path to what the editor needs to open it: the
 * engine, the module (or null for an engine map), and the map's path relative
 * to that module's maps/.
 *
 * THIS IS WHAT "FINDS THE MODULE AUTOMATICALLY". Double-click
 * <engine>/modules/doom2/maps/map01.map and the answer is
 * { root: <engine>, module: 'doom2', map: 'map01.map' } -- read off the path,
 * not asked for. An engine map, <engine>/maps/test.map, is
 * { module: null, map: 'test.map' }.
 *
 * `explicit` is a configured engine root (the dnp.engineRoot setting). It
 * wins over the walk-up, and it is checked: a file that is not under it is
 * reported rather than silently relocated to whatever the walk-up finds, since
 * a person who set the root meant it.
 *
 * Returns { ok: true, root, module, map } or { ok: false, reason, root? }.
 * Never throws: this runs on every click, and a click on the wrong file is
 * not an error condition -- it is a click that needs one sentence back.
 */
export function locateMap (absPath, { explicit = null } = {}) {
  const file = resolve(absPath)
  if (!file.endsWith('.map')) return { ok: false, reason: `${file} is not a .map file` }

  let root
  if (explicit) {
    root = resolve(explicit)
    if (!isEngineRoot(root)) {
      return { ok: false, reason: `dnp.engineRoot is ${root}, which is not an engine (no ${MARKERS.join(' / ')})` }
    }
  } else {
    root = findEngineRoot(dirname(file))
    if (!root) {
      return {
        ok: false,
        reason: `no engine above ${file}: nothing up the tree has ${MARKERS.join(' and ')}. ` +
                'Set dnp.engineRoot to the DoomNetPlatformer checkout.'
      }
    }
  }

  const rel = relative(root, file)
  if (rel.startsWith('..')) {
    return { ok: false, root, reason: `${file} is not under the engine at ${root}` }
  }
  const parts = rel.split(sep)

  // <root>/modules/<id>/maps/<...>
  if (parts[0] === 'modules' && parts[2] === 'maps' && parts.length > 3) {
    const id = parts[1]
    if (!MODULE_ID.test(id)) {
      return { ok: false, root, reason: `'${id}' is not a usable module id (${MODULE_ID})` }
    }
    return { ok: true, root, module: id, map: parts.slice(3).join('/') }
  }
  // <root>/maps/<...>
  if (parts[0] === 'maps' && parts.length > 1) {
    return { ok: true, root, module: null, map: parts.slice(1).join('/') }
  }
  return {
    ok: false,
    root,
    reason: `${rel} is under the engine but not under maps/ or modules/<id>/maps/, ` +
            'which is where the editor looks for maps'
  }
}
