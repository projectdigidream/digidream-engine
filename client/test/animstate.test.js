//
// Which animation a body should be playing (design §4.1).
//
// Two claims here are easy to satisfy by accident, so each is asserted in BOTH
// directions:
//
//   the derivation really discriminates -- a version that always answered
//   'idle' passes every individual check that only says what idle looks like;
//
//   declared beats derived AND zero means derived -- a version that always
//   preferred one of the two passes half the assertions perfectly.
//
import {
  CONDITIONS, defaultStates, derive, deriveRemote, stateFor, resolveState, easeToward
} from '../src/render/animstate.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-6) => Math.abs(a - b) <= eps

// Raw fx, the engine's own unit: one unit per second is 2^32.
const FX = (n) => BigInt(Math.round(n * 4294967296))
const still = { grounded: true, vel: { x: 0n, y: 0n, z: 0n } }

console.log('\n--- every condition resolves to a state that exists ---')
{
  let allNumbers = true
  for (const c of CONDITIONS) {
    const n = stateFor(null, c)
    if (typeof n !== 'number' || !Number.isInteger(n) || n < 0) allNumbers = false
  }
  check(allNumbers, `all ${CONDITIONS.length} conditions map to a whole state number`)

  // CONTROL. A condition nobody named must still be a number. A version
  // returning undefined would ask the registry for state `undefined`, which
  // silently resolves to state 0 one layer further from the mistake.
  check(stateFor(null, 'somersault') === 0, 'CONTROL: an unknown condition is 0, not undefined')

  // The five-value convention every catalogue in the repo already follows.
  check(stateFor(null, 'idle') === 0, 'idle is 0')
  check(stateFor(null, 'walk') === 1, 'walk is 1')
  check(stateFor(null, 'attack') === 2, 'attack is 2')
  check(stateFor(null, 'pain') === 3, 'pain is 3')
  check(stateFor(null, 'death') === 4, 'death is 4')
}

console.log('\n--- the derivation reads motion the client already has ---')
{
  check(derive(still) === 'idle', 'standing still is idle')
  check(derive({ grounded: true, vel: { x: FX(2), y: 0n, z: 0n } }) === 'walk', 'moving on the ground is walk')
  check(derive({ grounded: true, vel: { x: FX(9), y: 0n, z: 0n } }) === 'run', 'moving fast is run')
  check(derive({ grounded: false, vel: { x: 0n, y: 0n, z: FX(4) } }) === 'jump', 'rising off the ground is jump')
  check(derive({ grounded: false, vel: { x: 0n, y: 0n, z: FX(-4) } }) === 'fall', 'falling is fall')
  check(derive({ ...still, climbing: true }) === 'climb', 'climbing is climb')
  check(derive({ ...still, grappling: true }) === 'grapple', 'grappling is grapple')

  // Precedence, and it is the reason CONDITIONS is ordered: swinging while
  // walking is an attack animation, not a walk one.
  check(derive({ grounded: true, vel: { x: FX(3), y: 0n, z: 0n }, attackLeft: 8 }) === 'attack',
    'swinging while walking is attack, not walk')
  check(derive({ ...still, dead: true, attackLeft: 8 }) === 'death',
    'and death outranks even that')

  // A body resting on a moving platform carries a velocity of a few thousand
  // raw -- billionths of a unit per second. A threshold of exactly zero makes it
  // walk on the spot forever.
  check(derive({ grounded: true, vel: { x: 5000n, y: 3000n, z: 0n } }) === 'idle',
    'a few thousand raw is still standing still')

  check(derive(null) === 'idle', 'no motion at all is idle rather than a throw')
  check(derive({}) === 'idle', 'and so is an empty one')
}

console.log('\n--- CONTROL: it really discriminates ---')
{
  // A version that always answered 'idle' passes every check above that only
  // says what idle looks like. This is the assertion that it does not.
  const seen = new Set()
  for (let t = 0; t < 200; t++) {
    // A scripted trace: stand, walk, sprint, jump, fall, land, climb, swing.
    const phase = Math.floor(t / 25)
    const m = [
      still,
      { grounded: true, vel: { x: FX(2), y: 0n, z: 0n } },
      { grounded: true, vel: { x: FX(9), y: 0n, z: 0n } },
      { grounded: false, vel: { x: FX(2), y: 0n, z: FX(5) } },
      { grounded: false, vel: { x: FX(2), y: 0n, z: FX(-5) } },
      { ...still, climbing: true },
      { grounded: true, vel: { x: FX(2), y: 0n, z: 0n }, attackLeft: 4 },
      { ...still, grappling: true }
    ][phase] ?? still
    seen.add(derive(m))
  }
  check(seen.size > 3, `a 200-tick trace produced ${seen.size} distinct conditions: ${[...seen].join(', ')}`)
}

console.log('\n--- a remote body is measured, not read ---')
{
  const a = { x: 0, y: 0, z: 0 }
  const walked = { x: 0.05, y: 0, z: 0 }        // 3 units/s at 60 Hz
  check(deriveRemote(a, walked, 1 / 60, { grounded: true }) === 'walk',
    'two samples a walk apart give walk')
  const sprinted = { x: 0.2, y: 0, z: 0 }       // 12 units/s
  check(deriveRemote(a, sprinted, 1 / 60, { grounded: true }) === 'run',
    'and further apart gives run')
  check(deriveRemote(a, { x: 0, y: 0, z: 0.1 }, 1 / 60, { grounded: false }) === 'jump',
    'rising between samples is jump')
  check(deriveRemote(a, a, 1 / 60, { grounded: true }) === 'idle', 'not moving is idle')

  // CONTROL: a zero gap cannot be divided by, and must not produce NaN or a
  // spurious sprint. It falls back to whatever else is known.
  check(deriveRemote(a, walked, 0, { grounded: true }) === 'idle',
    'CONTROL: a zero time gap is idle rather than a division')
}

console.log('\n--- declared beats derived, and zero means derived ---')
{
  const moving = { grounded: true, vel: { x: FX(3), y: 0n, z: 0n } }

  // Both directions, because a version that always prefers one passes the other
  // half perfectly.
  check(resolveState(7, moving, null) === 7, 'a declared 7 wins over a walk')
  check(resolveState(0, moving, null) === 1, 'CONTROL: a declared 0 yields the derived walk')
  check(resolveState(0, still, null) === 0, 'and standing still resolves to idle')
  check(resolveState(4, still, null) === 4, 'a declared death wins over idle')
}

console.log('\n--- an avatar can point a condition somewhere else ---')
{
  const avatar = { when: { ...defaultStates(), climb: 7 } }
  check(stateFor(avatar, 'climb') === 7, 'a `when` override is used')
  check(stateFor(avatar, 'walk') === 1, 'and the others keep their defaults')
  check(resolveState(0, { ...still, climbing: true }, avatar) === 7,
    'so climbing resolves to the overridden state')

  // CONTROL. The default table must not be shared by reference between avatars
  // -- a Map or object handed out rather than copied is the exact bug, and this
  // catalogue exists so a module can differ from another one.
  const other = { when: defaultStates() }
  check(stateFor(other, 'climb') === 1, 'CONTROL: another avatar still sees the default')
  check(stateFor(null, 'climb') === 1, 'and so does one with no table at all')
}

console.log('\n--- the ease is frame-rate independent ---')
{
  // Extracted from Camera._blend so there is one of these. The property worth
  // asserting is the one that made it worth extracting: N small steps land where
  // one big step of the same total dt does.
  const run = (steps) => {
    let v = 0
    for (let i = 0; i < steps; i++) v = easeToward(v, 1, 4, 1 / steps)
    return v
  }
  const a = run(30), b = run(60), c = run(144)
  check(near(a, b, 0.01) && near(b, c, 0.01),
    `one second of ease agrees across 30/60/144 (${a.toFixed(4)}, ${b.toFixed(4)}, ${c.toFixed(4)})`)
  check(easeToward(1, 1, 4, 1 / 60) === 1, 'already-there stays there exactly')
  check(easeToward(0, 1, 0, 1 / 60) === 0, 'a rate of zero never moves')

  // CONTROL: a per-frame fraction does NOT have this property, which is why the
  // exponential is worth the exp() call. Stated as a ratio rather than an
  // absolute, because what matters is that one spread is orders larger.
  const naive = (steps) => {
    let v = 0
    for (let i = 0; i < steps; i++) v += (1 - v) * 0.1
    return v
  }
  const easeSpread = Math.abs(a - c)
  const naiveSpread = Math.abs(naive(30) - naive(144))
  check(naiveSpread > easeSpread * 100,
    `CONTROL: a plain per-frame fraction spreads ${naiveSpread.toFixed(4)} where the ease spreads ${easeSpread.toExponential(1)}`)
}

console.log(`\nanimstate: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
