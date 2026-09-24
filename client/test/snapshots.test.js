//
// Unit tests for snapshot reassembly.
//
// The end-to-end delta test covers the healthy path well, but the paths that
// matter most when things go wrong are the ones it CANNOT reach reliably: a
// baseline that has been pruned, and a slot recycled to a different entity.
// Both are rare by design, and rare-and-untested is exactly how a delta encoder
// ends up failing quietly in production.
//
// No network, no wasm, no dependencies -- plain node.
//
import { SnapshotReassembler } from '../src/snapshots.js'
import { ENT_FIELD, MAX_ENTITIES } from '../src/proto.js'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  if (!ok) { fails++; console.log(`  FAIL ${label}`) }
}

const handle = (index, generation) => ((generation & 0xff) << 24) | (index & 0x00ffffff)
const snap = (serverTick, baselineTick, entities) => ({
  serverTick, baselineTick, lastInputTick: 0, inputMargin: 0, flags: 0,
  self: {}, entities, vfx: []
})
const ent = (index, gen, fields = {}) => {
  let mask = 0
  const e = { handle: handle(index, gen) }
  if (fields.pos !== undefined) { mask |= ENT_FIELD.POS; e.pos = fields.pos }
  if (fields.facing !== undefined) { mask |= ENT_FIELD.FACING; e.facing = fields.facing }
  if (fields.animationState !== undefined) { mask |= ENT_FIELD.ANIMATION_STATE; e.animationState = fields.animationState }
  if (fields.destroyed) mask |= ENT_FIELD.DESTROYED
  e.mask = mask
  return e
}

// --- full snapshot establishes the baseline ---------------------------------
{
  const r = new SnapshotReassembler()
  const res = r.apply(snap(10, 0, [ent(1, 1, { pos: { x: 1n, y: 2n, z: 3n }, facing: 100 })]))
  check(res.ok, 'full snapshot applies')
  check(r.entities().size === 1, 'one entity present')
  check(r.entities().get(1).facing === 100, 'field decoded')
  check(r.ackTick === 10, 'ack advanced to the applied tick')
}

// --- omitted entities are carried forward -----------------------------------
{
  const r = new SnapshotReassembler()
  r.apply(snap(10, 0, [
    ent(1, 1, { pos: { x: 1n, y: 0n, z: 0n }, facing: 10 }),
    ent(2, 1, { pos: { x: 5n, y: 0n, z: 0n }, facing: 20 })
  ]))
  // Only entity 1 changed; 2 is omitted entirely.
  r.apply(snap(12, 10, [ent(1, 1, { facing: 11 })]))

  check(r.entities().size === 2, 'omitted entity survives the delta')
  check(r.entities().get(1).facing === 11, 'changed field updated')
  check(r.entities().get(1).pos.x === 1n, 'unchanged field of a changed entity is preserved')
  check(r.entities().get(2).facing === 20, 'wholly omitted entity is carried forward unchanged')
  check(r.ackTick === 12, 'ack advanced')
}

// --- a missing baseline is refused, and the ack does NOT advance -------------
{
  const r = new SnapshotReassembler()
  r.apply(snap(10, 0, [ent(1, 1, { pos: { x: 1n, y: 0n, z: 0n } })]))

  const res = r.apply(snap(12, 999, [ent(1, 1, { facing: 7 })]))
  check(!res.ok, 'snapshot with an unknown baseline is refused')
  check(res.reason === 'no-baseline', 'refusal reason is reported')
  check(r.stats.droppedNoBaseline === 1, 'refusal is counted')

  // The ack must stay put. Advancing it would tell the server we hold a
  // baseline we do not, and every later delta would build on fiction -- the
  // failure would surface as entities frozen at stale positions, with nothing
  // anywhere reporting an error.
  check(r.ackTick === 10, 'ack does NOT advance past a refused snapshot')
  check(r.entities().get(1).facing !== 7, 'refused snapshot did not mutate state')

  // ...and the server's fallback to a full snapshot recovers cleanly.
  const rec = r.apply(snap(14, 0, [ent(1, 1, { pos: { x: 9n, y: 0n, z: 0n }, facing: 3 })]))
  check(rec.ok, 'a full snapshot recovers after a refusal')
  check(r.ackTick === 14, 'ack advances again once recovered')
}

// --- a recycled slot is a DIFFERENT entity (protocol R8) ---------------------
{
  const r = new SnapshotReassembler()
  r.apply(snap(10, 0, [ent(1, 1, { pos: { x: 100n, y: 0n, z: 0n }, facing: 42, animationState: 5 })]))

  // Same index, new generation, and the server sends the full field set.
  r.apply(snap(12, 10, [ent(1, 2, { pos: { x: 0n, y: 0n, z: 0n }, facing: 0, animationState: 0 })]))

  const e = r.entities().get(1)
  check(e.generation === 2, 'generation updated')
  check(e.pos.x === 0n, 'recycled slot does not inherit the old position')
  check(e.animationState === 0, 'recycled slot does not inherit the old animation state')
  check(r.stats.regenerated === 1, 'regeneration is counted')
}

// --- a recycled slot with a PARTIAL update must not merge onto the old -------
{
  // Defensive: if a buggy or hostile server sent a partial update under a new
  // generation, merging it onto the previous occupant would hand a fresh entity
  // a dead one's position. Fields not supplied come out null/zero instead.
  const r = new SnapshotReassembler()
  r.apply(snap(10, 0, [ent(1, 1, { pos: { x: 100n, y: 0n, z: 0n }, facing: 42 })]))
  r.apply(snap(12, 10, [ent(1, 2, { facing: 7 })]))

  const e = r.entities().get(1)
  check(e.generation === 2, 'generation updated on partial recycle')
  check(e.pos === null, 'no position inherited from the previous occupant')
  check(e.facing === 7, 'supplied field applied')
}

// --- destruction ------------------------------------------------------------
{
  const r = new SnapshotReassembler()
  r.apply(snap(10, 0, [ent(1, 1, { pos: { x: 1n, y: 0n, z: 0n } }), ent(2, 1, { pos: { x: 2n, y: 0n, z: 0n } })]))
  r.apply(snap(12, 10, [ent(2, 1, { destroyed: true })]))

  check(r.entities().size === 1, 'destroyed entity removed')
  check(r.entities().has(1), 'the other entity survived')
  check(!r.entities().has(2), 'the destroyed one is gone')

  // And it stays gone when the next delta simply omits it.
  r.apply(snap(14, 12, []))
  check(!r.entities().has(2), 'destruction is not undone by a later carry-forward')
}

// --- history pruning does not break active baselines ------------------------
{
  const r = new SnapshotReassembler()
  r.apply(snap(1, 0, [ent(1, 1, { pos: { x: 0n, y: 0n, z: 0n } })]))
  for (let t = 2; t < 200; t++) r.apply(snap(t, t - 1, [ent(1, 1, { facing: t })]))

  check(r.entities().get(1).facing === 199, 'long delta chain stays correct')
  check(r.stats.droppedNoBaseline === 0, 'no baseline was lost mid-chain')
  check(r.history.size <= 65, `history is bounded (${r.history.size})`)
}

// --- the decoder's entity cap, against the header that defines it -----------
//
// `MAX_ENTITIES` in client/src/proto.js is a copy of a C++ constant, because
// JavaScript cannot include a header. This is what makes the copy survivable:
// the day somebody raises the simulation's entity space, this fails HERE rather
// than presenting as a snapshot that decodes to nothing on a busy level.
//
// It is not hypothetical. The constant said 768 while naming itself
// `proto::MAX_ENTITIES`, which was 1152, and had said so for long enough that
// nobody could say when it drifted -- harmless only because a 1200-byte
// datagram holds about 46 entities and the cap was never approached.
{
  const hpp = readFileSync(
    join(ROOT, 'core', 'include', 'dnp', 'proto', 'messages.hpp'), 'utf8')
  const m = /inline constexpr u32 MAX_ENTITIES\s*=\s*(\d+);/.exec(hpp)
  check(m !== null, `messages.hpp still declares MAX_ENTITIES (${m?.[1] ?? 'not found'})`)
  check(m !== null && Number(m[1]) === MAX_ENTITIES,
        `and proto.js agrees: header ${m?.[1]}, client ${MAX_ENTITIES}`)
  // CONTROL: the comparison can fail. Without this the two checks above pass
  // for a test that read the wrong number out of the wrong file.
  check(Number(m?.[1]) !== MAX_ENTITIES + 1,
        'CONTROL: it is comparing values, not just finding a digit')
}

console.log(`snapshots: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
