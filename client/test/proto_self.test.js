//
// THE SELF BLOCK, as the server's own encoder writes it (core/src/proto/
// messages.cpp encode_self, through the wasm build) and as this client reads it.
//
// Every field carries a value of its own, so two fields read in each other's
// place cannot both come out right. That is the bug this exists for: proto.js
// read jumps, dashes, swing, cooldown where the encoder writes jumps, swing,
// cooldown, dashes -- and since all three counters are zero most of the time,
// reconciliation adopted the wrong ones only while somebody swung or dashed.
//
import { Core } from '../src/core.js'
import { decodeSnapshot } from '../src/proto.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  if (!ok) { fails++; console.log(`  FAIL ${label}`) }
}

const core = await Core.load()
const M = core.M
const FX = (n) => BigInt(n) << 32n

const snapshot = (grappling, armed = false) => {
  const cap = 512
  const ptr = M._malloc(cap)
  const n = M._dnp_test_snapshot(ptr, cap, (grappling ? 1 : 0) | (armed ? 2 : 0))
  const bytes = M.HEAPU8.slice(ptr, ptr + n)
  M._free(ptr)
  return decodeSnapshot(bytes)
}

for (const grappling of [false, true]) {
  const what = grappling ? 'grappling' : 'not grappling'
  const s = snapshot(grappling)
  check(s !== null, `${what}: the encoder's snapshot decodes`)
  if (!s) continue
  const v = s.self
  check(s.serverTick === 1001 && s.lastInputTick === 999 && s.inputMargin === -3 && s.flags === 2, `${what}: the header`)
  check(v.pos.x === FX(11) && v.pos.y === FX(12) && v.pos.z === FX(13), `${what}: pos`)
  check(v.vel.x === FX(21) && v.vel.y === FX(22) && v.vel.z === FX(23), `${what}: vel`)
  check(v.facing === 0x31000000 && v.controllerMode === 4 && v.anchor === 0x55, `${what}: facing, mode, anchor`)
  check(v.jumpsUsed === 1, `${what}: jumps (${v.jumpsUsed})`)
  check(v.attackLeft === 2, `${what}: swing left (${v.attackLeft})`)
  check(v.attackCooldownLeft === 3, `${what}: swing cooldown (${v.attackCooldownLeft})`)
  check(v.dashesUsed === 5, `${what}: dashes (${v.dashesUsed})`)
  check(v.sector === 66, `${what}: sector`)
  check(v.dashTimer === FX(7) && v.dashCooldown === FX(8) && v.hoverFuel === FX(9) && v.flightFuel === FX(10), `${what}: the ability timers`)
  if (grappling) {
    check(v.grappleAnchor?.x === FX(41) && v.grappleAnchor?.z === FX(43) && v.grappleDist === FX(44), `${what}: the grapple block`)
  } else {
    check(v.grappleAnchor === undefined, `${what}: no grapple block`)
  }
  check(v.avatar === 77 && v.animationState === 88 && v.paramEpoch === 99, `${what}: avatar, animation, epoch`)
  check(v.stateHash === 0x0123456789ABCDEFn, `${what}: the hash`)
  check(s.hp === 123, `${what}: hp, which follows the self block`)
}

// THE WEAPON MACHINE (docs/WEAPON_PREDICTION.md), only when the body is armed,
// between the grapple block and the avatar.
{
  const s = snapshot(false, true)
  check(s !== null, 'armed: the encoder\'s snapshot decodes')
  const k = s?.self.weapon
  check(k !== undefined, 'armed: there is a weapon block')
  if (k) {
    check(k.have === 0xA5 && k.current === 7 && k.phase === 3 && k.armed === 6 && k.swingPhase === 2,
          `armed: have, current, phase, armed, swing (${JSON.stringify(k)})`)
    check(k.prevButtons === 1 && k.entered === 70001 && k.done === 0x15, 'armed: buttons, entered, done')
    check(k.heldSince[0] === 70003 && k.heldSince[1] === 70004, 'armed: the held-since ticks')
    check(k.vars.join() === '-100,-101,-102,-103', `armed: vars (${k.vars})`)
    check(k.ammo.join() === '200,201,202,203,204,205,206,207', `armed: ammo (${k.ammo})`)
    check(k.cool.join() === '300,301,302,303,304,0,0,0', `armed: cooldowns, up to the last set (${k.cool})`)
    check(k.swungWeapon === 4 && k.swungPhase === 9 && k.swungStep === 11,
          `armed: the step whose swing is its latest (${k.swungWeapon}/${k.swungPhase}/${k.swungStep})`)
  }
  // And everything after it still lands: the block's length is right.
  check(s?.self.avatar === 77 && s?.self.stateHash === 0x0123456789ABCDEFn && s?.hp === 123,
        'armed: the avatar, the hash and hp after it')
  check(snapshot(false, false)?.self.weapon === undefined, 'CONTROL: not armed, no weapon block')
}

// AND THE WASM SIDE OF RECONCILIATION: what a snapshot says goes into the world
// and comes back out the same, word for word.
{
  const world = core.createWorld({})
  const agent = world.spawn({ x: 0n, y: 0n, z: 0n })
  const k = snapshot(false, true).self.weapon
  world.writeWeapon(agent, k)
  const back = world.readWeapon(agent)
  check(JSON.stringify(back) === JSON.stringify(k), `the weapon state round-trips through the world (${JSON.stringify(back)})`)
  world.writeWeapon(agent, null)
  check(world.readWeapon(agent).have === 0 && world.readWeapon(agent).current === 255, 'CONTROL: cleared, it holds nothing')
}

console.log(`proto_self: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
