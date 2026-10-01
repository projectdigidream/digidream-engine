//
// THE FIRST-PERSON WEAPON, the pure half (client/src/render/viewmodel.js), and
// the module's data for it (modules/doom2/assets/sprites/weapons.json).
//
import { readFileSync, existsSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { frameAt, place, TIC } from '../src/render/viewmodel.js'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// --- frame timing, in Doom's tics ------------------------------------------
const fire = [['A', 4], ['B', 6, 'FLASH'], ['C', 4]]
check(frameAt(fire, 0)[0] === 'A', 'the first frame shows at once')
check(frameAt(fire, 3.9 * TIC)[0] === 'A' && frameAt(fire, 4.1 * TIC)[0] === 'B',
      'and gives way after its four tics')
check(frameAt(fire, 5 * TIC)[2] === 'FLASH', 'a frame carries its flash')
check(frameAt(fire, 14.1 * TIC) === null, 'a sequence ends: the gun returns to ready')
check(frameAt(fire, 14.1 * TIC, true)[0] === 'A', 'CONTROL: looped, it starts again instead')
check(frameAt([], 0) === null, 'no frames, nothing drawn')

// --- placement on Doom's 320x200 screen -------------------------------------
// The pistol: 57x62, offsets (-126, -106), at rest at (1, 32).
const r = place({ width: 1280, height: 720 }, [57, 62], [-126, -106], 1, 32)
const s = 720 / 200
check(Math.abs(r.top - 138 * s) < 1e-6, `its top is where Doom draws it, 138 of 200 (${(r.top / s).toFixed(1)})`)
check(Math.abs(r.left - ((1280 - 320 * s) / 2 + 127 * s)) < 1e-6, 'and centred on the real screen')
check(Math.abs(r.width - 57 * s) < 1e-6, 'scaled with the screen')
const lowered = place({ width: 1280, height: 720 }, [57, 62], [-126, -106], 1, 128)
check(lowered.top > 720, 'CONTROL: lowered to Doom\'s 128 it is off the bottom of the screen')

// --- the module's data -------------------------------------------------------
const W = join(ROOT, 'modules', 'doom2', 'assets', 'sprites', 'weapons.json')
const I = join(ROOT, 'modules', 'doom2', 'assets', 'sprites', 'index.json')
if (existsSync(W) && existsSync(I)) {
  const defs = JSON.parse(readFileSync(W, 'utf8'))
  const index = JSON.parse(readFileSync(I, 'utf8'))
  const missing = []
  for (const [name, w] of Object.entries(defs.weapons)) {
    for (const f of [...w.ready, ...w.fire]) {
      for (const pic of [f[0], f[2]].filter(Boolean)) {
        if (!index.sizes?.[pic] || !index.offsets?.[pic]) missing.push(`${name}:${pic}`)
      }
    }
  }
  check(missing.length === 0, `every frame of every weapon has a picture and its offsets${missing.length ? ': missing ' + missing.slice(0, 5).join(', ') : ''}`)
  // The names must be the weapons runtime's own, or hud.weapon never matches.
  const runtime = readFileSync(join(ROOT, 'modules', 'doom2', 'maps', 'weapons_doom2.lua'), 'utf8')
  const unknown = Object.keys(defs.weapons).filter((n) => n !== 'bfg' && !new RegExp(`^\\s*${n} = \\{`, 'm').test(runtime))
  check(unknown.length === 0, `every view model names a weapon the runtime declares${unknown.length ? ': ' + unknown.join(', ') : ''}`)
}

console.log(`\nviewmodel: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
