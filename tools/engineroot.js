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
import { dirname, join, resolve } from 'node:path'

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
