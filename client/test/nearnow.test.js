//
// The bodies near us, drawn at the present (render/nearnow.js): carried
// forward from the newest sample to now, a correction that settles rather than
// snaps, never further ahead than MAX_AHEAD, and a shove counted once.
//
import { NearPresent, presentWeight, NEAR, FAR, MAX_AHEAD } from '../src/render/nearnow.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; if (!ok) { fails++; console.log(`  FAIL ${label}`) } }
const near = (a, b, e = 1e-6) => Math.abs(a - b) <= e

// The weight: whole inside NEAR, none beyond FAR, between them a fade.
check(presentWeight(NEAR / 2) === 1 && presentWeight(FAR + 1) === 0, 'whole up close, none far away')
check(near(presentWeight((NEAR + FAR) / 2), 0.5), 'half way between')

// A monster walking +x at 0.1 a tick, sampled every other tick.
const walk = [{ tick: 100, x: 0, y: 0, z: 0 }, { tick: 102, x: 0.2, y: 0, z: 0 }]
const np = new NearPresent()
const p = np.present('m', walk, 108, 1 / 60)
check(near(p.x, 0.2 + 0.1 * 6), `carried to the present at its own speed (${p.x.toFixed(3)})`)
// CONTROL: a body not moving stays where it is.
const still = np.present('s', [{ tick: 100, x: 1, y: 0, z: 0 }, { tick: 102, x: 1, y: 0, z: 0 }], 108, 1 / 60)
check(near(still.x, 1), 'CONTROL: a still body is not moved')
// Never further than MAX_AHEAD.
const far = new NearPresent().present('f', walk, 102 + 100, 1 / 60)
check(near(far.x, 0.2 + 0.1 * MAX_AHEAD), `and never more than ${MAX_AHEAD} ticks ahead`)
// A teleport between samples is not a speed.
const tp = new NearPresent().present('t', [{ tick: 100, x: 0, y: 0, z: 0 }, { tick: 102, x: 40, y: 0, z: 0 }], 108, 1 / 60)
check(near(tp.x, 40), 'a teleport is not carried forward')

// IT TURNED: the next sample says it stopped. The new guess is 0.6 back from
// the old one; the jump is not drawn, it settles over a few frames.
const t = new NearPresent()
let at = 108
let q = t.present('m', walk, at, 1 / 60)
const stopped = [...walk, { tick: 104, x: 0.2, y: 0, z: 0 }]
at += 1
q = t.present('m', stopped, at, 1 / 60)
const truth = 0.2
check(q.x > truth + 0.4, `no snap: the frame after the correction is still near the old guess (${q.x.toFixed(3)})`)
for (let i = 0; i < 30; i++) { at += 1 / 4; q = t.present('m', stopped, at, 1 / 60) }
check(near(q.x, truth, 0.05), `and it settles onto the truth within half a second (${q.x.toFixed(3)})`)

// SHOVED: the knock layer carries it, and the samples' velocity (the same
// shove) is not added on top.
const shoveSamples = [{ tick: 100, x: 0, y: 0, z: 0 }, { tick: 102, x: 0.4, y: 0, z: 0 }]
const s = new NearPresent().present('k', shoveSamples, 108, 1 / 60, (from) => ({ x: 0.5, y: 0, z: 0 }))
check(near(s.x, 0.4 + 0.5), `a shoved body is where the shove takes it, counted once (${s.x.toFixed(3)})`)

// A TELEPORT the server says is one (ent_field::TELEPORTED): drawn there at
// once, with nothing settling -- however short the hop.
{
  const j = new NearPresent()
  const base = [{ tick: 100, x: 0, y: 0, z: 0 }, { tick: 102, x: 0, y: 0, z: 0 }]
  j.present('j', base, 104, 1 / 60)
  const hop = [...base, { tick: 104, x: 3, y: 0, z: 0, jump: true }]
  const r = j.present('j', hop, 105, 1 / 60)
  check(near(r.x, 3), `a flagged jump is drawn where it landed (${r.x.toFixed(3)})`)
  // CONTROL: the same hop unflagged is a correction, and settles.
  const c = new NearPresent()
  c.present('c', base, 104, 1 / 60)
  const r2 = c.present('c', [...base, { tick: 104, x: 3, y: 0, z: 0 }], 105, 1 / 60)
  check(r2.x < 2, `CONTROL: unflagged, it is still on its way (${r2.x.toFixed(3)})`)
}

console.log(`nearnow: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
