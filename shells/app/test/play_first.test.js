//
// A PLAY-TEST SERVES ITS OWN MODULE FIRST (shells/app/static.js setFirst).
//
// Two modules in the writable tree, each with a module-wide HUD named
// hud.json, its index's default. Every lookup is first-wins and the tree's
// modules come in name order, so a play-test of `zzz` used to be drawn with
// `aaa`'s HUD -- the user's "the map does not react to the module's HUD,
// unless it has its own". CONTROL: with no play-test, the first by name wins,
// as before.
//
// House style: standalone, prints ok/FAIL, ends with a count.
//
import { mkdirSync, writeFileSync, mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { startStatic } from '../static.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

const ROOT = mkdtempSync(join(tmpdir(), 'dnp-play-root-'))
const RW = mkdtempSync(join(tmpdir(), 'dnp-play-rw-'))
const put = (base, rel, body) => { mkdirSync(dirname(join(base, rel)), { recursive: true }); writeFileSync(join(base, rel), typeof body === 'string' ? body : JSON.stringify(body)) }
for (const m of ['aaa', 'zzz']) {
  put(RW, `modules/${m}/assets/ui/index.json`, { version: 1, default: 'hud.json', maps: { first: `map_${m}.json` } })
  put(RW, `modules/${m}/assets/ui/hud.json`, { _about: m })
}
const statics = await startStatic({ root: ROOT, modules: [], rwRoot: RW })
const get = async (p) => { const r = await fetch(`http://127.0.0.1:${statics.port}${p}`); return r.ok ? r.json() : null }

check((await get('/ui/hud.json'))?._about === 'aaa', 'CONTROL: with no play-test, the module first by name serves hud.json')
statics.setFirst(join(RW, 'modules', 'zzz'))
check((await get('/ui/hud.json'))?._about === 'zzz', 'a play-test of zzz is served zzz\'s hud.json, not aaa\'s')
const index = await get('/ui/index.json')
check(index?.default === 'hud.json' && index?.maps?.first === 'map_zzz.json', `and the index's map entries are zzz's where both name a map (${JSON.stringify(index?.maps)})`)
statics.setFirst(null)
check((await get('/ui/hud.json'))?._about === 'aaa', 'and back to the name order when the play-test ends')

statics.server.close()
console.log(`\nplay_first: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
