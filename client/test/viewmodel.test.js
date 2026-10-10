//
// THE FIRST-PERSON WEAPON, the pure half (client/src/render/viewmodel.js), and
// the module's data for it (modules/doom2/assets/sprites/weapons.json).
//
import { readFileSync, existsSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { frameAt, place, TIC, ViewModel, parseAnims } from '../src/render/viewmodel.js'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
// The repository: the engine is ROOT, and modules, docs, the studio and the
// synth are beside it.
const REPO = join(ROOT, '..')
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
const W = join(REPO, 'modules', 'doom2', 'assets', 'sprites', 'weapons.json')
const I = join(REPO, 'modules', 'doom2', 'assets', 'sprites', 'index.json')
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
  const runtime = readFileSync(join(REPO, 'modules', 'doom2', 'maps', 'weapons_doom2.lua'), 'utf8')
  const unknown = Object.keys(defs.weapons).filter((n) => n !== 'bfg' && !new RegExp(`^\\s*${n} = \\{`, 'm').test(runtime))
  check(unknown.length === 0, `every view model names a weapon the runtime declares${unknown.length ? ': ' + unknown.join(', ') : ''}`)
}

// --- per-phase animations (docs/VIEWMODEL_PHASES.md) ------------------------
console.log('viewmodel: a weapon animated per phase')
{
  // Just enough DOM for the view model: elements with a style and a dataset.
  const el = () => ({ style: {}, dataset: {}, children: [], appendChild (c) { this.children.push(c); return c }, remove () {} })
  const doc = { createElement: el }
  const names = ['SWDA', 'SWDB', 'SWDC', 'SWDD', 'SWDE', 'SWDF', 'SWDR', 'SWDS']
  const index = { sizes: Object.fromEntries(names.map((n) => [n, [64, 64]])), offsets: Object.fromEntries(names.map((n) => [n, [0, 0]])) }
  const said = []
  const defs = {
    weapons: {
      sword: {
        ready: [['SWDA', 1]], fire: [['SWDB', 3], ['SWDC', 4]],
        anims: {
          0: [['SWDA', 1]],
          3: [['SWDD', 3], ['SWDE', 3], ['SWDF', 5]],
          5: { frames: [['SWDR', 4], ['SWDS', 4]], loop: true },
          x: [['SWDA', 1]],
          7: [['SWDA']].concat([[42, 1]])
        }
      },
      plain: { ready: [['SWDA', 1]], fire: [['SWDB', 3], ['SWDC', 4]] }
    }
  }
  const vm = new ViewModel({ host: el(), doc, log: (m) => said.push(m) })
  vm.setDefs(defs, index)
  const shown = () => vm.gun.dataset.src?.replace(/^\/sprites\/|\.png$/g, '')
  const size = { width: 640, height: 400 }
  const run = (state, key, seconds) => { vm.setAnim(state, key); vm.update(seconds, 0, true, size); return shown() }
  vm.setWeapon('sword')
  check(said.length === 2 && said.some((m) => /'x'/.test(m)) && said.some((m) => /"7"/.test(m)),
        `a bad key and a bad frame list are said (${said.length})`)
  check(vm.anims.get('sword').size === 3, 'and left out; the rest is used')
  check(run(3, '3:100', 0) === 'SWDD', `state 3 plays anims["3"] from its first frame (${shown()})`)
  check(run(3, '3:100', 4 * TIC) === 'SWDE', 'and on through it')
  check(run(3, '3:100', 20 * TIC) === 'SWDF', 'a list that does not loop holds its last frame')
  check(run(3, '3:200', 0) === 'SWDD', 'entering the same state again (a new swing) starts it over')
  check(run(5, '5:300', 0) === 'SWDR' && run(5, '5:300', 9 * TIC) === 'SWDR', 'a looping list wraps')
  check(run(0, '0:400', 0) === 'SWDA', 'state 0 is its own list')
  // A state the weapon has no animation for: ready, and fire on a shot, as before.
  check(run(9, '9:500', 0) === 'SWDA', 'a state with no animation falls back to ready')
  vm.fire()
  check(run(9, '9:500', TIC) === 'SWDB', 'and to fire on a shot')
  // CONTROL: a weapon with no anims at all behaves exactly as before.
  const old = new ViewModel({ host: el(), doc })
  old.setDefs(defs, index)
  old.setWeapon('plain')
  old.setAnim(3, '3:1'); old.update(0, 0, true, size)
  check(old.gun.dataset.src === '/sprites/SWDA.png', 'CONTROL: a weapon without anims shows ready')
  old.fire(); old.setAnim(3, '3:1'); old.update(TIC, 0, true, size)
  check(old.gun.dataset.src === '/sprites/SWDB.png', 'CONTROL: and fire on a shot')
  const errs = []
  check(parseAnims({ 2: { frames: [['A', 2]] } }, 'w', errs).get(2).loop === false && parseAnims({ 0: [['A', 1]] }, 'w', errs).get(0).loop === true,
        'loop defaults: true for state 0, false otherwise')
}

console.log(`\nviewmodel: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
