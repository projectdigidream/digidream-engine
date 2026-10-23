//
// Which animation a body should be playing, and how it got there.
//
// PURE. No THREE, no wasm, no DOM — it takes numbers and returns a description,
// which is the whole reason it is its own file rather than a few methods on
// PropRegistry. The sprite renderer consumes that description by swapping frames
// and, when a record opts in, blending two quads; a mesh renderer would consume
// the identical description by cross-fading two clips. The mix, the durations and
// the interruption rule are decided ONCE, here, renderer-agnostically.
//
// Getting that seam wrong means writing the state machine twice and finding out
// later that the two disagree. It is the one part of mesh support worth being
// deliberate about before any mesh exists.
//
// DECLARED OR DERIVED. An animation state is a u16 that means whatever a
// catalogue record says it means; the server sets it with entity:set_animation
// and the protocol carries it, per body, saying nothing about timing (§6: "the
// server MUST NOT send blend curves, transition durations or frame indices").
// For a PLAYER, zero means "nothing declared":
//
//     state = declared !== 0 ? declared : stateFor(avatar, derive(motion))
//
// which puts each half where the knowledge is. Locomotion is derived on the
// client because the client already has it and has it EARLY: velocity, grounded,
// climbing and attackLeft are all predicted, so a jump animates on the tick the
// body leaves the ground rather than one round trip later. Action -- pain, death,
// a scripted pose -- is declared by the server, because nothing on the client
// could know it.
//
// The happy exception is the swing: attackLeft is predicted AND replicated, so a
// melee animation starts instantly from the derivation and the server's own
// declaration agrees a moment later. That is the payoff for leaving the swing's
// timing in the shared controller.
//

// The conditions a body can be in, as far as ANIMATION is concerned.
//
// A closed list, in one place, because the alternative is a number baked into
// the client -- and a catalogue whose whole purpose is to let a module name its
// own states cannot have the mapping compiled in. `avatar <id> when <c> state
// <n>` validates against this and warns on a name that is not here.
//
// Ordered by precedence, most specific first: derive() returns the first that
// holds. That ordering is the only reason `attack` beats `walk` -- swinging
// while walking is an attack animation, not a walk one.
export const CONDITIONS = [
  'death', 'pain', 'attack',
  'grapple', 'hop', 'hang', 'climb', 'fly', 'hover',
  'jump', 'fall', 'land',
  'crouch', 'run', 'walk', 'idle'
]

// The five-value convention every catalogue in this repository already follows
// (0 idle, 1 walk, 2 attack, 3 pain, 4 death), spread over the longer condition
// list so that a record which declares nothing but `avatar <id>` still animates.
//
// Everything not drawn separately falls back to a state that exists: running is
// walking, a jump and a fall are whatever "moving" looks like, and crouching is
// idle. A module that draws more points `when` at the numbers it drew.
const DEFAULTS = {
  idle: 0, crouch: 0, land: 0,
  walk: 1, run: 1, jump: 1, fall: 1, fly: 1, hover: 1, grapple: 1, climb: 1, hang: 1, hop: 1,
  attack: 2,
  pain: 3,
  death: 4
}

/** The default condition -> state table, as a fresh object. */
export function defaultStates () { return { ...DEFAULTS } }

// Raw fx: the speed below which a body is standing still, and above which it is
// running rather than walking. In Q32.32, so one unit per second is 2^32.
//
// Not zero for "moving", deliberately. A body resting on a moving platform, or
// settling after a landing, carries a velocity of a few thousand raw -- billionths
// of a unit per second -- and a threshold of exactly zero makes it walk on the
// spot forever.
const STILL = 1n << 24n          // ~0.004 units/second
const RUN = 6n << 32n            // 6 units/second

/**
 * Which condition a body is in, from motion the client already has.
 *
 * `m` is the shape client/src/core.js readAgent returns -- vel as raw fx
 * BigInts, grounded/climbing/grappling as booleans, attackLeft as a countdown --
 * or the smaller shape a remote body can be measured into (see deriveRemote).
 * Every field is optional; a caller that knows less gets a coarser answer rather
 * than an exception.
 */
export function derive (m) {
  if (!m) return 'idle'
  if (m.dead) return 'death'
  if (m.hurt) return 'pain'
  // Before locomotion: swinging while walking is an attack.
  if (m.attackLeft) return 'attack'
  if (m.grappling) return 'grapple'
  // Hanging from a ledge (ledge_grab) before climbing a wall: both climb,
  // and the ledge is the more specific.
  // A hop off a ledge (its small hop, or the pull-up onto it) before the
  // hang it starts from: JUMP on a ledge plays `hop`, not `jump`.
  if (m.hopping) return 'hop'
  if (m.hanging) return 'hang'
  if (m.climbing) return 'climb'
  if (m.flying) return 'fly'
  if (m.hovering) return 'hover'

  const vz = m.vel ? BigInt(m.vel.z) : 0n
  if (m.grounded === false) return vz > 0n ? 'jump' : 'fall'

  if (m.crouching) return 'crouch'
  const vx = m.vel ? BigInt(m.vel.x) : 0n
  const vy = m.vel ? BigInt(m.vel.y) : 0n
  // Squared comparison, so there is no square root and no float anywhere on this
  // path. The engine's own speeds are fx and stay fx.
  const speed2 = vx * vx + vy * vy
  if (speed2 <= STILL * STILL) return 'idle'
  return speed2 >= RUN * RUN ? 'run' : 'walk'
}

/**
 * The same question for a body the client does not predict.
 *
 * A remote player arrives as interpolated samples, so its condition is MEASURED
 * rather than read: horizontal distance over the gap gives idle/walk/run, and the
 * sign of dz gives jump/fall. A rendering guess, and allowed to be approximately
 * right -- anything that has to be exact is declared by the server instead.
 *
 * `dt` is in seconds and may be zero, which is a body that has not moved yet.
 */
export function deriveRemote (a, b, dt, extra = {}) {
  if (!a || !b || !(dt > 0)) return derive(extra)
  // Units per second, in RAW FX -- the domain derive() compares in. Forgetting
  // the 2^32 gives a speed of three *billionths* of a unit per second, which
  // reads as standing perfectly still no matter how fast the body is moving.
  const per = (v) => BigInt(Math.round((v / dt) * 4294967296))
  return derive({
    ...extra,
    grounded: extra.grounded,
    vel: { x: per(b.x - a.x), y: per(b.y - a.y), z: per(b.z - a.z) }
  })
}

/**
 * The state number a condition maps to, for this avatar.
 *
 * Falls back to the condition's default, then to 0. Never undefined: a caller
 * handed undefined would ask the registry for state `undefined`, which resolves
 * to state 0 anyway but silently and one layer further from the mistake.
 */
export function stateFor (avatar, condition) {
  const table = avatar?.when ?? DEFAULTS
  const n = table[condition]
  if (typeof n === 'number') return n
  const d = DEFAULTS[condition]
  return typeof d === 'number' ? d : 0
}

/**
 * The state to draw: the server's if it declared one, else the motion's.
 *
 * Zero is "nothing declared" and not "idle", which is the one piece of meaning
 * this function adds over reading the field directly. It works because 0 already
 * IS idle by convention, so a body whose script never speaks animates from its
 * motion and a body whose script says 0 gets idle either way.
 */
export function resolveState (declared, motion, avatar) {
  if (declared) return declared
  return stateFor(avatar, derive(motion))
}

/**
 * One frame-rate-independent step of an exponential ease.
 *
 * Lifted out of Camera._blend so there is one of these rather than two. The
 * virtue is the `1 - exp(-rate * dt)`: a plain `current += (want - current) *
 * 0.1` per frame converges twice as fast at 120 Hz as at 60, which is how a feel
 * ends up depending on the machine it runs on.
 *
 * Note what this is NOT used for: the cross-fade between two animation states.
 * That needs a duration rather than a rate, because an exponential ease never
 * arrives -- and a cross-fade must terminate so the outgoing clock can be
 * dropped and its material disposed.
 */
export function easeToward (current, want, rate, dt) {
  const k = 1 - Math.exp(-Math.max(0, rate) * dt)
  let next = current + (want - current) * k
  if (Math.abs(next - want) < 1e-6) next = want
  return next
}
