//
// SOUNDS, the client's half: the name hash the wire uses, and the predictor of
// this client's own sounds (client/src/audio/ownsounds.js).
//
// Each rule has its control. A predictor that fired on everything would pass
// "the jump plays"; it fails "a soft landing is silent" and "a held pistol does
// not repeat". One that fired on nothing fails every positive.
//
import { soundHash, PREDICT, BUTTON } from '../src/proto.js'
import { OwnSounds } from '../src/audio/ownsounds.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// The hash, pinned to the number core/tests/test_proto.cpp pins: the two
// implementations agreeing is the whole of how a sound is named on the wire.
check(soundHash('weapon.shotgun.fire') === 0x31b9dc0d, 'soundHash agrees with the server (31b9dc0d)')
check(soundHash('a') !== soundHash('b'), 'CONTROL: different names, different hashes')

const JUMP = 11, LAND = 12, SHOT = 13, AUTO = 14, SWING = 15
const own = new OwnSounds()
own.set(1, [{ trigger: PREDICT.JUMP, sound: JUMP, param: 0, shots: -1 },
            { trigger: PREDICT.LAND, sound: LAND, param: 12 * 16, shots: -1 }])
const idle = { grounded: true, velZ: 0, jumps: 0, attackLeft: 0, buttons: 0, tick: 0 }
let t = 0
const step = (s) => own.step({ ...idle, tick: ++t, ...s })

step({})
check(step({ grounded: false, velZ: 9, jumps: 1 }).includes(JUMP), 'a jump is heard')
check(!step({ grounded: false, velZ: 3, jumps: 1 }).includes(JUMP), 'CONTROL: once, on the tick it is made')
check(step({ grounded: false, velZ: 9, jumps: 2 }).includes(JUMP), 'and a double jump, in the air')
check(!step({ grounded: false, velZ: -3, jumps: 0 }).includes(JUMP), 'CONTROL: the count resetting is not a jump')
step({ grounded: false, velZ: -9 })
check(!step({ grounded: true }).includes(LAND), 'CONTROL: landing from an ordinary jump is silent')
step({ grounded: false, velZ: -20 })
check(step({ grounded: true }).includes(LAND), 'landing hard is heard')
step({ grounded: false, velZ: -2 })
check(!step({ grounded: true }).includes(JUMP), 'CONTROL: landing is not a jump')

// Shots: a press fires; holding it does not, until it is pressed again and the
// weapon can fire again; a held entry repeats; shots run out.
own.set(2, [{ trigger: PREDICT.PRIMARY, sound: SHOT, param: 10, shots: 2 }])
const P = BUTTON.ATTACK_PRIMARY
check(step({ buttons: P }).includes(SHOT), 'a press fires')
let repeated = 0
for (let i = 0; i < 30; i++) if (step({ buttons: P }).includes(SHOT)) repeated++
check(repeated === 0, 'CONTROL: holding a semi-automatic does not fire again')
step({})
check(step({ buttons: P }).includes(SHOT), 'a second press fires')
for (let i = 0; i < 15; i++) step({ buttons: i % 2 ? P : 0 })
check(own.groups.get(2)[0].shots === 0, 'and the shots are counted down to none')
let dry = 0
for (let i = 0; i < 30; i++) if (step({ buttons: i % 2 ? P : 0 }).includes(SHOT)) dry++
check(dry === 0, 'an empty weapon is silent')

own.set(2, [{ trigger: PREDICT.PRIMARY_HELD, sound: AUTO, param: 5, shots: -1 }])
let auto = 0
for (let i = 0; i < 30; i++) if (step({ buttons: P }).includes(AUTO)) auto++
check(auto === 6, `a held automatic repeats every 5 ticks (${auto} in 30)`)
own.set(2, [{ trigger: PREDICT.PRIMARY, sound: SHOT, param: 0, shots: -1 }])
step({})
check(step({ buttons: P }).includes(SHOT), 'replacing a group replaces what it plays')
check(!step({ buttons: P }).includes(AUTO), 'CONTROL: and the old entry is gone')

own.set(2, [{ trigger: PREDICT.SWING, sound: SWING, param: 0, shots: -1 }])
step({ attackLeft: 0 })
check(step({ attackLeft: 12 }).includes(SWING), 'the attack timer starting is a swing')
check(!step({ attackLeft: 11 }).includes(SWING), 'CONTROL: once per swing')

own.clear()
step({})
check(step({ grounded: false, velZ: 9, jumps: 1 }).length === 0, 'cleared (a new body): nothing until the game says again')

console.log(`\nsfx: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
