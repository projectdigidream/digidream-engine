// Where the game server binary is, for the tests that spawn one.
//
// ONE ANSWER, not six. Every end-to-end test used to spell out
// join(ROOT, 'build', 'server', 'dnp-server') for itself, which is the same
// path repeated in six files -- and the moment the suite needed to build
// somewhere else, six files had to agree to move.
//
// AND IT NEEDED TO. scripts/build_server.sh writing into build/server relinks
// the very file a running stack is executing, which replaces the image under a
// live process and kills it: an edge logging "upstream lost" every time
// somebody runs the tests. The suite now builds to its own directory and
// points DNP_SERVER_BIN here, so running the tests and playing the game at the
// same time is no longer a way to lose your session.
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..', '..')

export const serverBin = process.env.DNP_SERVER_BIN ??
                         join(ROOT, 'build', 'server', 'dnp-server')

// The prop catalogue these tests need, for the same "one answer" reason.
//
// It is the MODULE's, not the engine's. Every prop with a picture -- the keys,
// the barrel, the lever, the monsters -- is DOOM II art, so the whole catalogue
// moved to modules/doom2/ alongside the sprites it names, and the engine's own
// assets/props.txt is now almost empty by design (docs/MODULES.md).
//
// These tests spawn barrels and keys and zombiemen, so they want the module's
// file. They pass it with --props rather than --module because the MAPS they
// run are the engine's own; only the catalogue comes from Doom.
export const doomProps = join(ROOT, 'modules', 'doom2', 'assets', 'props.txt')

/**
 * The catalogue AS THE CLIENT SEES IT: the engine's, then each module's.
 *
 * Concatenated, because that is what actually reaches a client --
 * tools/devserver.js and app/static.js both serve /props.txt by joining the
 * engine's file with every loaded module's, and the server does the same when
 * it loads them (docs/MODULES.md). Reading one file in a test and the join at
 * runtime is how a test comes to disagree with the thing it is testing: the
 * engine keeps the vfx records and the module keeps the props, so either file
 * alone is a catalogue no running client ever has.
 */
export function catalogueText (modules = ['doom2']) {
  const parts = [readFileSync(join(ROOT, 'assets', 'props.txt'), 'utf8')]
  for (const m of modules) {
    parts.push(readFileSync(join(ROOT, 'modules', m, 'assets', 'props.txt'), 'utf8'))
  }
  return parts.join('\n')
}
