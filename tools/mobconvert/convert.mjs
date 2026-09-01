//
// Convert maps/mobs/doom_monsters.lua into the editor's data format.
//
//   node tools/mobconvert/convert.mjs
//
// WHY A SCRIPT AND NOT EIGHTEEN HAND-TRANSCRIPTIONS. Eighteen monsters copied
// by hand is eighteen chances to mistype a number, and every one of them would
// be a behaviour change nobody asked for. This runs the same helpers the Lua
// runs -- du(), tics(), humanoid(), ball() -- reimplemented once, so a mistake
// is systematic and the control catches it on the first monster rather than
// being scattered through all eighteen.
//
// THE SOURCE IS maps/mobs/doom_monsters.lua. There is a byte-identical copy at
// modules/doom2/maps/doom_monsters.lua and it is NOT the source: it is what
// mapc leaves beside the baked map, and editing it works until the next bake.
//
// The control that makes this trustworthy is tools/mobconvert/dumpdefs.lua:
// load both files with the REAL runtime and compare the tables they produce.
// Identical tables means identical behaviour -- every number, every state,
// every transition, in order.
//
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { emitMobLua, emptyMobs, problemsIn } from '../../editor/src/mobgraph.js'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')

// --- the helpers, reimplemented exactly --------------------------------------
//
// Doom units to engine units, as an exact decimal STRING. A string because the
// loader refuses non-integral numbers by name: 2048/48 is not representable in
// a double and quantising it silently is how a mob ends up a hair out of range
// on one build. Same arithmetic as doom_monsters.lua's du(), including the
// rounding, because a different rounding is a different monster.
const du = (n) => {
  const whole = Math.floor(n / 48)
  const rem = n - whole * 48
  return `${whole}.${String(Math.floor(rem * 10000 / 48 + 0.5)).padStart(4, '0')}`
}

// Doom runs at 35 tics a second and this engine at 60.
const tics = (n) => Math.floor(n * 60 / 35 + 0.5)

// --- humanoid ----------------------------------------------------------------
//
// The state graph almost every Doom monster shares, expanded faithfully from
// the Lua. The ORDER things are appended in is behaviour: transitions are
// first-match-wins and order is the priority, so `look` first, then `melee` if
// present, then `missile` if present -- exactly as the helper appends them.
function humanoid (o) {
  const chase_to = [
    { to: 'look', when: [{ cond: 'has_target', negate: true }] }
  ]
  if (o.melee) {
    chase_to.push({
      to: 'melee',
      when: [{ cond: 'target_within', distance: o.melee.range },
             { cond: 'target_visible' },
             { cond: 'cooldown_ready', var: 'ready' }]
    })
  }
  if (o.attack) {
    chase_to.push({
      to: 'missile',
      when: [{ cond: 'target_within', distance: o.attack_range },
             { cond: 'target_visible' },
             // TIGHT, and deliberately: the monster waits until it is genuinely
             // lined up, because turning here is a rate rather than Doom's snap.
             { cond: 'target_in_front', arc: 12 },
             { cond: 'cooldown_ready', var: 'ready' }]
    })
  }

  // States as an ARRAY -- the sidecar's shape, because the editor shows them in
  // an order and a Lua map has none. Built in the order the Lua assigns them,
  // so the file reads the way the original does.
  const states = [
    { name: 'look', anim: 0, move: 'stop', face: 'hold',
      transitions: [{ to: 'chase', when: [{ cond: 'has_target' },
                                          { cond: 'target_visible' }] }] },
    { name: 'chase', anim: 1, move: 'chase', face: 'target', interruptible: true,
      transitions: chase_to },
    { name: 'pain', anim: 3, move: 'stop', face: 'hold',
      ticks: tics(6), next: 'chase' },
    // `next` is itself: a corpse has arrived and stays. The loader refuses a
    // state with `ticks` and no `next`.
    { name: 'death', anim: 4, move: 'stop', face: 'hold', solid: false,
      ticks: o.death_ticks ?? tics(30), next: 'death' }
  ]

  // A ranged state only where there is a ranged attack. A demon has none, and
  // an empty state nothing enters is caught by the unreachable-state check.
  if (o.attack) {
    states.push({
      name: 'missile', anim: 2, move: 'stop', face: 'target',
      ticks: o.attack_ticks, next: 'chase', interruptible: true,
      sequence: o.attack
    })
  }
  if (o.melee) {
    states.push({
      name: 'melee', anim: 2, move: 'stop', face: 'target',
      ticks: o.melee.ticks, next: 'chase', interruptible: true,
      sequence: [
        { at: 0, action: 'face' },
        { at: o.melee.at, action: 'melee',
          damage: o.melee.damage, range: o.melee.range },
        { at: o.melee.at, action: 'set_cooldown', var: 'ready', ticks: o.cooldown }
      ]
    })
  }

  const m = {
    name: o.name,
    prop: o.prop,
    hp: o.hp,
    // Faster than the engine's default of 4: a monster shoots along its BODY's
    // facing, which trails the target by up to a think interval.
    turn_speed: o.turn_speed ?? 12,
    sight: o.sight ?? du(2048),
    pain_chance: o.pain_chance,
    infighting: true,
    corpse: true,
    initial: 'look'
  }
  if (o.flying) {
    m.flying = true
    m.hover_z = o.hover_z ?? du(24)
  }
  if (o.jump_height) m.jump_height = o.jump_height
  m.vars = { ready: 0 }
  m.states = states
  return m
}

// A ball of fire, in the shapes Doom uses it.
const ball = (name, prop, speed, damage, extra = {}) => ({
  name,
  prop,
  speed: du(speed * 35),
  gravity: 0,
  life: 300,
  // OFF THE FLOOR: a missile born at the shooter's feet is cast against the
  // geometry at ankle height, which reads as a fireball frozen in mid-air.
  z_offset: du(32),
  anim_fly: 0,
  anim_burst: 1,
  burst_ticks: 20,
  on_hit: [{ action: 'damage', amount: damage }],
  ...extra
})

// --- the missiles ------------------------------------------------------------
const missiles = [
  ball('fireball', 'fireball', 10, 12),
  ball('caco_ball', 'caco_ball', 10, 15),
  ball('baron_ball', 'baron_ball', 15, 20),
  ball('arachno_plasma', 'arachno_plasma', 25, 12),
  ball('mancubus_ball', 'mancubus_ball', 20, 16),
  ball('revenant_ball', 'revenant_ball', 10, 20, { homing: 6 }),
  {
    name: 'rocket', prop: 'rocket', speed: du(20 * 35), gravity: 0, life: 300,
    z_offset: du(32), anim_fly: 0, anim_burst: 1, burst_ticks: 20,
    // The blast, not the impact: what makes a rocket a rocket, and the
    // cyberdemon does not kill itself with its own.
    on_hit: [
      { action: 'damage', amount: 20 },
      { action: 'damage_area', damage: 60, radius: du(128),
        falloff: true, hurts_owner: false }
    ]
  }
]

// --- the monsters ------------------------------------------------------------
const hitscan = (at, damage, count, spread) =>
  ({ at, action: 'hitscan', damage, range: du(2048), count, spread, tracer: 1 })
const cooldownAt = (at, ticks) =>
  ({ at, action: 'set_cooldown', var: 'ready', ticks })

const mobs = [
  // --- the hitscan family ----------------------------------------------------
  humanoid({
    name: 'zombieman', prop: 'zombieman', hp: 20, pain_chance: 200,
    attack_range: du(2048), attack_ticks: tics(16), cooldown: tics(50),
    attack: [
      { at: 0, action: 'face' },
      hitscan(tics(10), 15, 1, 5),
      cooldownAt(tics(10), tics(50))
    ]
  }),
  humanoid({
    name: 'shotgun_guy', prop: 'shotgun_guy', hp: 30, pain_chance: 170,
    attack_range: du(2048), attack_ticks: tics(20), cooldown: tics(60),
    attack: [
      { at: 0, action: 'face' },
      hitscan(tics(10), 10, 3, 12),
      cooldownAt(tics(10), tics(60))
    ]
  }),
  // Doom's chaingunner fires in a burst; here that is two shots in one state.
  humanoid({
    name: 'chaingunner', prop: 'chaingunner', hp: 70, pain_chance: 170,
    attack_range: du(2048), attack_ticks: tics(14), cooldown: tics(20),
    attack: [
      { at: 0, action: 'face' },
      hitscan(tics(4), 10, 1, 8),
      hitscan(tics(8), 10, 1, 8),
      cooldownAt(tics(8), tics(20))
    ]
  }),
  humanoid({
    name: 'wolfenstein_ss', prop: 'wolfenstein_ss', hp: 50, pain_chance: 170,
    attack_range: du(2048), attack_ticks: tics(16), cooldown: tics(40),
    attack: [
      { at: 0, action: 'face' },
      hitscan(tics(10), 10, 1, 6),
      cooldownAt(tics(10), tics(40))
    ]
  }),
  humanoid({
    name: 'spider_boss', prop: 'spider_boss', hp: 3000, pain_chance: 40,
    attack_range: du(2048), attack_ticks: tics(20), cooldown: tics(25),
    attack: [
      { at: 0, action: 'face' },
      hitscan(tics(4), 12, 1, 6),
      hitscan(tics(8), 12, 1, 6),
      hitscan(tics(12), 12, 1, 6),
      cooldownAt(tics(12), tics(25))
    ]
  }),

  // --- claws and teeth -------------------------------------------------------
  // No `attack`, so no missile state at all: the helper being general rather
  // than a demon being a special case.
  humanoid({
    name: 'demon', prop: 'demon', hp: 150, pain_chance: 180, cooldown: tics(20),
    melee: { range: du(80), damage: 20, ticks: tics(16), at: tics(8) }
  }),
  // The same animal, invisible -- until the client has a fuzz effect.
  humanoid({
    name: 'spectre', prop: 'spectre', hp: 150, pain_chance: 180, cooldown: tics(20),
    melee: { range: du(80), damage: 20, ticks: tics(16), at: tics(8) }
  }),

  // --- claws and a fireball --------------------------------------------------
  humanoid({
    name: 'imp', prop: 'imp', hp: 60, pain_chance: 200,
    attack_range: du(2048), attack_ticks: tics(18), cooldown: tics(55),
    melee: { range: du(64), damage: 9, ticks: tics(18), at: tics(10) },
    attack: [
      { at: 0, action: 'face' },
      { at: tics(10), action: 'missile', kind: 'fireball', offset: du(40) },
      cooldownAt(tics(10), tics(55))
    ]
  }),
  humanoid({
    name: 'hell_knight', prop: 'hell_knight', hp: 500, pain_chance: 50,
    attack_range: du(2048), attack_ticks: tics(20), cooldown: tics(60),
    melee: { range: du(96), damage: 30, ticks: tics(20), at: tics(10) },
    attack: [
      { at: 0, action: 'face' },
      { at: tics(12), action: 'missile', kind: 'baron_ball', offset: du(48) },
      cooldownAt(tics(12), tics(60))
    ]
  }),
  humanoid({
    name: 'baron', prop: 'baron', hp: 1000, pain_chance: 50,
    attack_range: du(2048), attack_ticks: tics(20), cooldown: tics(60),
    melee: { range: du(96), damage: 30, ticks: tics(20), at: tics(10) },
    attack: [
      { at: 0, action: 'face' },
      { at: tics(12), action: 'missile', kind: 'baron_ball', offset: du(48) },
      cooldownAt(tics(12), tics(60))
    ]
  }),
  humanoid({
    name: 'revenant', prop: 'revenant', hp: 300, pain_chance: 100,
    attack_range: du(2048), attack_ticks: tics(20), cooldown: tics(50),
    melee: { range: du(80), damage: 30, ticks: tics(18), at: tics(9) },
    attack: [
      { at: 0, action: 'face' },
      { at: tics(10), action: 'missile', kind: 'revenant_ball', offset: du(48) },
      cooldownAt(tics(10), tics(50))
    ]
  }),
  humanoid({
    name: 'arachnotron', prop: 'arachnotron', hp: 500, pain_chance: 128,
    attack_range: du(2048), attack_ticks: tics(18), cooldown: tics(30),
    attack: [
      { at: 0, action: 'face' },
      { at: tics(4), action: 'missile', kind: 'arachno_plasma', offset: du(60) },
      { at: tics(9), action: 'missile', kind: 'arachno_plasma', offset: du(60) },
      cooldownAt(tics(9), tics(30))
    ]
  }),
  // Three at once, fanned: `count` and `arc` on one action, because that is
  // what a volley IS.
  humanoid({
    name: 'mancubus', prop: 'mancubus', hp: 600, pain_chance: 80,
    attack_range: du(2048), attack_ticks: tics(24), cooldown: tics(60),
    attack: [
      { at: 0, action: 'face' },
      { at: tics(12), action: 'missile', kind: 'mancubus_ball',
        count: 3, arc: 12, offset: du(60) },
      cooldownAt(tics(12), tics(60))
    ]
  }),
  humanoid({
    name: 'cyberdemon', prop: 'cyberdemon', hp: 4000, pain_chance: 20,
    attack_range: du(2048), attack_ticks: tics(36), cooldown: tics(60),
    attack: [
      { at: 0, action: 'face' },
      { at: tics(6), action: 'missile', kind: 'rocket', offset: du(80) },
      { at: tics(18), action: 'face' },
      { at: tics(18), action: 'missile', kind: 'rocket', offset: du(80) },
      { at: tics(30), action: 'face' },
      { at: tics(30), action: 'missile', kind: 'rocket', offset: du(80) },
      cooldownAt(tics(30), tics(60))
    ]
  }),

  // --- the ones that float ---------------------------------------------------
  humanoid({
    name: 'cacodemon', prop: 'cacodemon', hp: 400, pain_chance: 128,
    flying: true, hover_z: du(48),
    attack_range: du(2048), attack_ticks: tics(20), cooldown: tics(50),
    melee: { range: du(80), damage: 20, ticks: tics(16), at: tics(8) },
    attack: [
      { at: 0, action: 'face' },
      { at: tics(10), action: 'missile', kind: 'caco_ball', offset: du(48) },
      cooldownAt(tics(10), tics(50))
    ]
  }),
  // Doom's lost soul does not attack, it CHARGES: `impulse` plus a melee on
  // arrival says that without the runtime needing a skull-fly mode.
  humanoid({
    name: 'lost_soul', prop: 'lost_soul', hp: 100, pain_chance: 256,
    flying: true, hover_z: du(32),
    attack_range: du(1024), attack_ticks: tics(20), cooldown: tics(40),
    attack: [
      { at: 0, action: 'face' },
      { at: tics(4), action: 'impulse', speed: du(20 * 35), towards: 'target' },
      { at: tics(10), action: 'melee', damage: 12, range: du(64) },
      cooldownAt(tics(10), tics(40))
    ]
  }),
  // It makes more of them -- with a budget, because one without fills the
  // entity table and every other spawn on the level then fails silently.
  humanoid({
    name: 'pain_elemental', prop: 'pain_elemental', hp: 400, pain_chance: 128,
    flying: true, hover_z: du(48),
    attack_range: du(2048), attack_ticks: tics(24), cooldown: tics(80),
    attack: [
      { at: 0, action: 'face' },
      { at: tics(12), action: 'spawn', mob: 'lost_soul', limit: 3, offset: du(72) },
      cooldownAt(tics(12), tics(80))
    ]
  }),

  // --- and the one that undoes your work -------------------------------------
  humanoid({
    name: 'archvile', prop: 'archvile', hp: 700, pain_chance: 10,
    attack_range: du(1024), attack_ticks: tics(30), cooldown: tics(70),
    attack: [
      { at: 0, action: 'face' },
      { at: tics(8), action: 'revive', radius: du(256) },
      { at: tics(20), action: 'damage', amount: 20 },
      { at: tics(20), action: 'damage_area', damage: 40, radius: du(96),
        falloff: true, hurts_owner: false },
      cooldownAt(tics(20), tics(70))
    ]
  })
]

// --- write it out ------------------------------------------------------------
const doc = emptyMobs()
doc.missiles = missiles
doc.mobs = mobs

const vocab = JSON.parse(readFileSync(join(ROOT, 'maps/scripts/mob_vocab.json'), 'utf8'))
const problems = problemsIn(doc, vocab)
if (problems.length) {
  console.error(`problemsIn() reports ${problems.length}:`)
  for (const p of problems) console.error('  ' + (p.message ?? JSON.stringify(p)))
} else {
  console.log('problemsIn(): clean')
}

const sidecar = join(ROOT, 'modules/doom2/mobs.editor.json')
const lua = join(ROOT, 'modules/doom2/maps/editor_mobs.lua')
mkdirSync(dirname(sidecar), { recursive: true })
writeFileSync(sidecar, JSON.stringify(doc, null, 2) + '\n')
writeFileSync(lua, emitMobLua(doc, { module: 'doom2' }))

console.log(`${doc.mobs.length} mobs, ${doc.missiles.length} missiles`)
console.log(`wrote ${sidecar}`)
console.log(`wrote ${lua}`)
