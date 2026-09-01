-- Doom's monsters, as data.
--
-- This file is the VOCABULARY'S TEST, not its shape. Nothing in
-- maps/scripts/mob_vocab.lua knows what an imp is; if a monster here needs
-- something the vocabulary cannot say, that is a finding about the vocabulary
-- and it gets fixed there, generally, rather than by adding an `imp` case.
--
-- The numbers are Doom's where Doom has one that transfers -- hit points, pain
-- chance, damage -- and this engine's where they do not. Doom runs at 35 tics
-- a second and this runs at 60, so every DURATION here is Doom's tics times
-- 60/35, rounded. Doom's map unit is 1/48 of an engine unit, which is what
-- du() spells.
--
-- What is deliberately NOT copied: Doom's per-frame state list. A zombieman
-- there is nine states; here it is five, because the client owns animation
-- timing (design §4) and the frames between "wind up" and "fire" are the
-- client's business. See the long comment at the top of mob_vocab.lua.

-- Doom units to engine units, as an exact decimal string. A string rather than
-- a Lua number because the loader refuses non-integral numbers by name: 2048/48
-- is not representable in a double and quantising it silently is how a mob
-- ends up a hair out of range on one build.
local function du(n)
  local whole = math.floor(n / 48)
  local rem = n - whole * 48
  -- 48ths are exact in decimal to four places (1/48 = 0.0208333...), so this
  -- rounds. Anything that has to be EXACT uses a fraction instead.
  return string.format("%d.%04d", whole, math.floor(rem * 10000 / 48 + 0.5))
end

local function tics(n) return math.floor(n * 60 / 35 + 0.5) end

-- The state graph almost every Doom monster shares.
--
-- Written once and specialised, because they genuinely do share it: look until
-- something is visible, walk towards it, attack when in range, flinch when
-- hurt, die. The difference between a zombieman and a baron is the numbers and
-- what the `missile` state performs -- which is the observation this whole
-- framework rests on. A monster that does NOT share it just writes its own
-- `states` table; nothing here is privileged.
local function humanoid(o)
  -- Transitions are built as a LIST and appended to, rather than declared with
  -- holes. A `nil` in the middle of a Lua array truncates it at that point, so
  -- a table with a conditional entry silently loses everything after it.
  local chase_to = {
    { to = "look", when = { { cond = "has_target", negate = true } } },
  }
  if o.melee then
    chase_to[#chase_to + 1] = {
      to = "melee",
      when = { { cond = "target_within", distance = o.melee.range },
               { cond = "target_visible" },
               { cond = "cooldown_ready", var = "ready" } },
    }
  end
  if o.attack then
    chase_to[#chase_to + 1] = {
      to = "missile",
      when = { { cond = "target_within", distance = o.attack_range },
               { cond = "target_visible" },
               -- TIGHT. Doom snaps a monster to face its target on the frame
               -- it attacks (A_FaceTarget); here turning is a rate, so the
               -- monster has to WAIT until it is genuinely lined up. At 60
               -- degrees it fired while half-turned, the shot went where its
               -- body was pointing rather than at the player, and it hit
               -- whichever of its neighbours was actually there -- which is
               -- also why the tracer was a stub drawn inside the shooter.
               { cond = "target_in_front", arc = 12 },
               { cond = "cooldown_ready", var = "ready" } },
    }
  end

  local m = {
    prop = o.prop,
    hp = o.hp,
    -- Faster than the engine's default of 4. A monster shoots along its BODY's
    -- facing, which trails the target by up to a think interval, and in a Doom
    -- corridor a few degrees of lag is a wall a metre away rather than the
    -- player twenty away. Doom does not have this problem because it snaps a
    -- monster to face its target on the frame it attacks; turning here is a
    -- rate, so the rate is what has to be quick enough.
    turn_speed = o.turn_speed or 12,
    sight = o.sight or du(2048),
    pain_chance = o.pain_chance,
    infighting = true,
    corpse = true,
    initial = "lok",
    vars = { ready = 0 },
    states = {
      look = {
        anim = 0, move = "stop", face = "hold",
        transitions = {
          { to = "chase", when = { { cond = "has_target" },
                                   { cond = "target_visible" } } },
        },
      },
      chase = {
        anim = 1, move = "chase", face = "target", interruptible = true,
        transitions = chase_to,
      },

      pain = {
        anim = 3, move = "stop", face = "hold",
        ticks = tics(6), next = "chase",
      },
      -- `next` is itself: a corpse has arrived and stays. The loader refuses a
      -- state with `ticks` and no `next` precisely so that "it ends and stops"
      -- has to be written down rather than left to be discovered.
      death = {
        anim = 4, move = "stop", face = "hold", solid = false,
        ticks = o.death_ticks or tics(30), next = "death",
      },
    },
  }

  -- A ranged state only where there is a ranged attack. A demon has none, and
  -- an empty state nothing enters would be caught by the unreachable-state
  -- check -- which is the check doing its job, not a reason to suppress it.
  if o.attack then
    m.states.missile = {
      anim = 2, move = "stop", face = "target",
      ticks = o.attack_ticks, next = "chase", interruptible = true,
      sequence = o.attack,
    }
  end

  if o.flying then
    m.flying = true
    m.hover_z = o.hover_z or du(24)
  end
  if o.jump_height then m.jump_height = o.jump_height end

  if o.melee then
    m.states.melee = {
      anim = 2, move = "stop", face = "target",
      ticks = o.melee.ticks, next = "chase", interruptible = true,
      sequence = {
        { at = 0, action = "face" },
        { at = o.melee.at, action = "melee",
          damage = o.melee.damage, range = o.melee.range },
        { at = o.melee.at, action = "set_cooldown", var = "ready",
          ticks = o.cooldown },
      },
    }
  end
  return m
end

-- A ball of fire, in the shapes Doom uses it.
local function ball(prop, speed, damage, extra)
  local m = {
    prop = prop, speed = du(speed * 35), gravity = 0, life = 300,
    -- OFF THE FLOOR. Doom launches from mid-body; a missile born at the
    -- shooter's feet is cast against the geometry at ankle height, so the
    -- first step or sill it crosses stops it -- which reads as a fireball
    -- frozen in mid-air rather than as one that hit something.
    z_offset = du(32),
    anim_fly = 0, anim_burst = 1, burst_ticks = 20,
    on_hit = { { action = "damage", amount = damage } },
  }
  for k, v in pairs(extra or {}) do m[k] = v end
  return m
end

mobs.load{
  missiles = {
    -- `on_hit` is where the damage lives, so nothing in the runtime knows what
    -- a fireball is: a rocket is one of these with a blast on arrival, and a
    -- revenant's is one with a turn rate.
    fireball       = ball("fireball", 10, 12),
    caco_ball      = ball("caco_ball", 10, 15),
    baron_ball     = ball("baron_ball", 15, 20),
    arachno_plasma = ball("arachno_plasma", 25, 12),
    mancubus_ball  = ball("mancubus_ball", 20, 16),
    revenant_ball  = ball("revenant_ball", 10, 20, { homing = 6 }),
    rocket = {
      prop = "rocket", speed = du(20 * 35), gravity = 0, life = 300,
      z_offset = du(32),
      anim_fly = 0, anim_burst = 1, burst_ticks = 20,
      -- The blast, not the impact: what makes a rocket a rocket, and the
      -- cyberdemon does not kill itself with its own.
      on_hit = {
        { action = "damage", amount = 20 },
        { action = "damage_area", damage = 60, radius = du(128),
          falloff = true, hurts_owner = false },
      },
    },
  },

  mobs = {
    -- --- the hitscan family -------------------------------------------------
    --
    -- Identical but for their numbers, which is the observation the whole
    -- framework rests on: a shotgunner is a zombieman that fires three pellets.
    zombieman = humanoid{
      prop = "zombieman", hp = 20, pain_chance = 200,
      attack_range = du(2048), attack_ticks = tics(16), cooldown = tics(50),
      attack = {
        { at = 0, action = "face" },
        { at = tics(10), action = "hitscan", damage = 15, range = du(2048),
          count = 1, spread = 5, tracer = 1 },
        { at = tics(10), action = "set_cooldown", var = "ready", ticks = tics(50) },
      },
    },

    shotgun_guy = humanoid{
      prop = "shotgun_guy", hp = 30, pain_chance = 170,
      attack_range = du(2048), attack_ticks = tics(20), cooldown = tics(60),
      attack = {
        { at = 0, action = "face" },
        { at = tics(10), action = "hitscan", damage = 10, range = du(2048),
          count = 3, spread = 12, tracer = 1 },
        { at = tics(10), action = "set_cooldown", var = "ready", ticks = tics(60) },
      },
    },

    -- Doom's chaingunner fires in a burst and decides each time whether to
    -- keep going. Here that is two shots in one state and a shorter cooldown --
    -- the vocabulary has no "refire" and does not need one, because a sequence
    -- with two entries IS a burst.
    chaingunner = humanoid{
      prop = "chaingunner", hp = 70, pain_chance = 170,
      attack_range = du(2048), attack_ticks = tics(14), cooldown = tics(20),
      attack = {
        { at = 0, action = "face" },
        { at = tics(4), action = "hitscan", damage = 10, range = du(2048),
          count = 1, spread = 8, tracer = 1 },
        { at = tics(8), action = "hitscan", damage = 10, range = du(2048),
          count = 1, spread = 8, tracer = 1 },
        { at = tics(8), action = "set_cooldown", var = "ready", ticks = tics(20) },
      },
    },

    wolfenstein_ss = humanoid{
      prop = "wolfenstein_ss", hp = 50, pain_chance = 170,
      attack_range = du(2048), attack_ticks = tics(16), cooldown = tics(40),
      attack = {
        { at = 0, action = "face" },
        { at = tics(10), action = "hitscan", damage = 10, range = du(2048),
          count = 1, spread = 6, tracer = 1 },
        { at = tics(10), action = "set_cooldown", var = "ready", ticks = tics(40) },
      },
    },

    spider_boss = humanoid{
      prop = "spider_boss", hp = 3000, pain_chance = 40,
      attack_range = du(2048), attack_ticks = tics(20), cooldown = tics(25),
      attack = {
        { at = 0, action = "face" },
        { at = tics(4),  action = "hitscan", damage = 12, range = du(2048), count = 1, spread = 6, tracer = 1 },
        { at = tics(8),  action = "hitscan", damage = 12, range = du(2048), count = 1, spread = 6, tracer = 1 },
        { at = tics(12), action = "hitscan", damage = 12, range = du(2048), count = 1, spread = 6, tracer = 1 },
        { at = tics(12), action = "set_cooldown", var = "ready", ticks = tics(25) },
      },
    },

    -- --- claws and teeth ----------------------------------------------------
    --
    -- No `attack`, so `humanoid` gives them no missile state at all. That is
    -- the helper being general rather than a demon being a special case.
    demon = humanoid{
      prop = "demon", hp = 150, pain_chance = 180, cooldown = tics(20),
      melee = { range = du(80), damage = 20, ticks = tics(16), at = tics(8) },
    },

    -- The same animal, invisible. Doom draws it with a fuzz effect the client
    -- does not have yet, so for now it is a demon that is quieter about it.
    spectre = humanoid{
      prop = "spectre", hp = 150, pain_chance = 180, cooldown = tics(20),
      melee = { range = du(80), damage = 20, ticks = tics(16), at = tics(8) },
    },

    -- --- claws and a fireball ----------------------------------------------
    imp = humanoid{
      prop = "imp", hp = 60, pain_chance = 200,
      attack_range = du(2048), attack_ticks = tics(18), cooldown = tics(55),
      melee = { range = du(64), damage = 9, ticks = tics(18), at = tics(10) },
      attack = {
        { at = 0, action = "face" },
        { at = tics(10), action = "missile", kind = "fireball", offset = du(40) },
        { at = tics(10), action = "set_cooldown", var = "ready", ticks = tics(55) },
      },
    },

    hell_knight = humanoid{
      prop = "hell_knight", hp = 500, pain_chance = 50,
      attack_range = du(2048), attack_ticks = tics(20), cooldown = tics(60),
      melee = { range = du(96), damage = 30, ticks = tics(20), at = tics(10) },
      attack = {
        { at = 0, action = "face" },
        { at = tics(12), action = "missile", kind = "baron_ball", offset = du(48) },
        { at = tics(12), action = "set_cooldown", var = "ready", ticks = tics(60) },
      },
    },

    baron = humanoid{
      prop = "baron", hp = 1000, pain_chance = 50,
      attack_range = du(2048), attack_ticks = tics(20), cooldown = tics(60),
      melee = { range = du(96), damage = 30, ticks = tics(20), at = tics(10) },
      attack = {
        { at = 0, action = "face" },
        { at = tics(12), action = "missile", kind = "baron_ball", offset = du(48) },
        { at = tics(12), action = "set_cooldown", var = "ready", ticks = tics(60) },
      },
    },

    revenant = humanoid{
      prop = "revenant", hp = 300, pain_chance = 100,
      attack_range = du(2048), attack_ticks = tics(20), cooldown = tics(50),
      melee = { range = du(80), damage = 30, ticks = tics(18), at = tics(9) },
      attack = {
        { at = 0, action = "face" },
        { at = tics(10), action = "missile", kind = "revenant_ball", offset = du(48) },
        { at = tics(10), action = "set_cooldown", var = "ready", ticks = tics(50) },
      },
    },

    arachnotron = humanoid{
      prop = "arachnotron", hp = 500, pain_chance = 128,
      attack_range = du(2048), attack_ticks = tics(18), cooldown = tics(30),
      attack = {
        { at = 0, action = "face" },
        { at = tics(4),  action = "missile", kind = "arachno_plasma", offset = du(60) },
        { at = tics(9),  action = "missile", kind = "arachno_plasma", offset = du(60) },
        { at = tics(9),  action = "set_cooldown", var = "ready", ticks = tics(30) },
      },
    },

    -- Three at once, fanned. `count` and `arc` on one action rather than three
    -- actions, because that is what a volley IS.
    mancubus = humanoid{
      prop = "mancubus", hp = 600, pain_chance = 80,
      attack_range = du(2048), attack_ticks = tics(24), cooldown = tics(60),
      attack = {
        { at = 0, action = "face" },
        { at = tics(12), action = "missile", kind = "mancubus_ball",
          count = 3, arc = 12, offset = du(60) },
        { at = tics(12), action = "set_cooldown", var = "ready", ticks = tics(60) },
      },
    },

    cyberdemon = humanoid{
      prop = "cyberdemon", hp = 4000, pain_chance = 20,
      attack_range = du(2048), attack_ticks = tics(36), cooldown = tics(60),
      attack = {
        { at = 0, action = "face" },
        { at = tics(6),  action = "missile", kind = "rocket", offset = du(80) },
        { at = tics(18), action = "face" },
        { at = tics(18), action = "missile", kind = "rocket", offset = du(80) },
        { at = tics(30), action = "face" },
        { at = tics(30), action = "missile", kind = "rocket", offset = du(80) },
        { at = tics(30), action = "set_cooldown", var = "ready", ticks = tics(60) },
      },
    },

    -- --- the ones that float ------------------------------------------------
    --
    -- `flying` costs no engine support: gravity_scale is a replicated
    -- parameter, so a cacodemon floats on every client without a word of it
    -- crossing the wire.
    cacodemon = humanoid{
      prop = "cacodemon", hp = 400, pain_chance = 128, flying = true,
      hover_z = du(48),
      attack_range = du(2048), attack_ticks = tics(20), cooldown = tics(50),
      melee = { range = du(80), damage = 20, ticks = tics(16), at = tics(8) },
      attack = {
        { at = 0, action = "face" },
        { at = tics(10), action = "missile", kind = "caco_ball", offset = du(48) },
        { at = tics(10), action = "set_cooldown", var = "ready", ticks = tics(50) },
      },
    },

    -- Doom's lost soul does not attack, it CHARGES: it launches itself and the
    -- collision is the attack. `impulse` plus a melee on arrival says that
    -- without the runtime needing a skull-fly mode.
    lost_soul = humanoid{
      prop = "lost_soul", hp = 100, pain_chance = 256, flying = true,
      hover_z = du(32),
      attack_range = du(1024), attack_ticks = tics(20), cooldown = tics(40),
      attack = {
        { at = 0, action = "face" },
        { at = tics(4), action = "impulse", speed = du(20 * 35), towards = "target" },
        { at = tics(10), action = "melee", damage = 12, range = du(64) },
        { at = tics(10), action = "set_cooldown", var = "ready", ticks = tics(40) },
      },
    },

    -- It does not shoot, it makes more of them -- with a budget, because one
    -- without fills the entity table and every other spawn on the level then
    -- fails silently.
    pain_elemental = humanoid{
      prop = "pain_elemental", hp = 400, pain_chance = 128, flying = true,
      hover_z = du(48),
      attack_range = du(2048), attack_ticks = tics(24), cooldown = tics(80),
      attack = {
        { at = 0, action = "face" },
        { at = tics(12), action = "spawn", mob = "lost_soul", limit = 3,
          offset = du(72) },
        { at = tics(12), action = "set_cooldown", var = "ready", ticks = tics(80) },
      },
    },

    -- --- and the one that undoes your work ----------------------------------
    --
    -- Doom's arch-vile does two things: it sets you on fire at a distance with
    -- no projectile to dodge, and it resurrects what you have already killed.
    -- Both are actions in the vocabulary, so this is a table like any other.
    archvile = humanoid{
      prop = "archvile", hp = 700, pain_chance = 10,
      attack_range = du(1024), attack_ticks = tics(30), cooldown = tics(70),
      attack = {
        { at = 0, action = "face" },
        { at = tics(8), action = "revive", radius = du(256) },
        { at = tics(20), action = "damage", amount = 20 },
        { at = tics(20), action = "damage_area", damage = 40, radius = du(96),
          falloff = true, hurts_owner = false },
        { at = tics(20), action = "set_cooldown", var = "ready", ticks = tics(70) },
      },
    },
  },
}
