//
// MODULES DO NOT SHARE (shells/app/static.js setFirst, with a writable tree).
//
// Two modules in the writable tree, each with a module-wide HUD named
// hud.json and a prop catalogue. The page is served the engine's files and
// ONE module's -- the one played, or the one the studio has open -- as a game
// packaged from it serves them. It used to be served every module at once:
// a play-test of `zzz` drawn with `aaa`'s HUD, the catalogues of both
// concatenated, so Play and the studio used what a build would not have.
// CONTROL: with no module open, neither module's files.
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
put(ROOT, 'assets/props.txt', 'prop 1 engine_thing\n')
for (const m of ['aaa', 'zzz']) {
  put(RW, `modules/${m}/assets/ui/index.json`, { version: 1, default: 'hud.json', maps: { first: `map_${m}.json` } })
  put(RW, `modules/${m}/assets/ui/hud.json`, { _about: m })
  put(RW, `modules/${m}/assets/props.txt`, `prop 9 ${m}_thing\n`)
}
const statics = await startStatic({ root: ROOT, modules: [], rwRoot: RW })
const get = async (p) => { const r = await fetch(`http://127.0.0.1:${statics.port}${p}`); return r.ok ? r.json() : null }
const text = async (p) => (await fetch(`http://127.0.0.1:${statics.port}${p}`)).text()

check((await get('/ui/hud.json')) === null, 'CONTROL: with no module open, no module\'s hud.json is served')
check(!/aaa_thing|zzz_thing/.test(await text('/props.txt')) && /engine_thing/.test(await text('/props.txt')), 'and the prop catalogue is the engine\'s alone')
statics.setFirst(join(RW, 'modules', 'zzz'))
check((await get('/ui/hud.json'))?._about === 'zzz', 'with zzz open (or played), zzz\'s hud.json')
const index = await get('/ui/index.json')
check(index?.maps?.first === 'map_zzz.json', `and zzz's layout index (${JSON.stringify(index?.maps)})`)
const props = await text('/props.txt')
check(/engine_thing/.test(props) && /zzz_thing/.test(props) && !/aaa_thing/.test(props), 'the catalogue: the engine\'s and zzz\'s, never aaa\'s')
statics.setFirst(join(RW, 'modules', 'aaa'))
check((await get('/ui/hud.json'))?._about === 'aaa' && !/zzz_thing/.test(await text('/props.txt')), 'aaa open: aaa\'s files, nothing of zzz')
await text('/studio/')
check((await get('/ui/hud.json')) === null, 'the studio\'s page loaded again: no module open until it says')

statics.server.close()
console.log(`\nplay_first: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
