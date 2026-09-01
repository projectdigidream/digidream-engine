-- Mobs that exist to exercise the vocabulary, not to be played against.
--
-- maps/mobs/doom.lua proves the vocabulary can say what Doom says. This one
-- reaches the corners Doom does not: a spawn budget, a resurrection, a homing
-- missile, a mid-sequence `goto`, a blast radius, and both halves of the `lua`
-- escape hatch.
--
-- Kept separate so doom.lua stays readable as a description of Doom, and so a
-- change here cannot quietly alter what a zombieman does.

mobs.load{
  missiles = {
    -- A rocket: a missile whose arrival is a blast. Nothing in the runtime
    -- knows what a rocket is.
    rocket = {
      prop = "fireball", speed = "8", gravity = 0, life = 120, radius = "0.25",
      on_hit = { { action = "damage_area", damage = 40, radius = "3",
                   falloff = true, hurts_owner = false } },
    },
    -- A seeker: the same missile with a turn rate. A revenant's.
    seeker = {
      prop = "fireball", speed = "6", gravity = 0, life = 300, radius = "0.25",
      homing = 8,
      on_hit = { { action = "damage", amount = 10 } },
    },
  },

  mobs = {
    -- Spawns children, with a budget. A pain elemental without one fills the
    -- entity table and every other spawn on the level then fails silently.
    brooder = {
      prop = "imp", hp = 100, initial = "idle",
      vars = { made = 0 },
      states = {
        idle = {
          anim = 0, move = "stop",
          transitions = { { to = "birth", when = { { cond = "has_target" } } } },
        },
        birth = {
          anim = 2, move = "stop", ticks = 10, next = "idle",
          sequence = {
            { at = 1, action = "spawn", mob = "spawnling", limit = 3 },
            { at = 1, action = "add_var", var = "made", by = 1 },
          },
        },
      },
    },

    -- `corpse` and a `death` state, because the healer below resurrects these
    -- and you cannot resurrect something that left no body. The loader refuses
    -- the one without the other.
    spawnling = {
      prop = "zombieman", hp = 5, initial = "idle", corpse = true,
      states = {
        idle = { anim = 0, move = "stop" },
        death = { anim = 4, move = "stop", solid = false,
                  ticks = 40, next = "death" },
      },
    },

    -- Puts corpses back on their feet.
    healer = {
      prop = "imp", hp = 100, initial = "idle",
      states = {
        idle = {
          anim = 0, move = "stop",
          sequence = { { at = 2, action = "revive", radius = "10" } },
          ticks = 4, next = "idle",
        },
      },
    },

    -- A mid-sequence `goto`, which must abandon the rest of the sequence.
    jumper = {
      prop = "zombieman", hp = 10, initial = "start",
      vars = { reached = 0, skipped = 0 },
      states = {
        start = {
          anim = 0, move = "stop", ticks = 20, next = "start",
          sequence = {
            { at = 1, action = "add_var", var = "reached", by = 1 },
            { at = 1, action = "goto", state = "landed" },
            -- Must NEVER run: `goto` ends the sequence.
            { at = 1, action = "add_var", var = "skipped", by = 1 },
          },
        },
        landed = { anim = 1, move = "stop" },
      },
    },

    -- Both halves of the escape hatch, and a condition that reads what the
    -- action wrote.
    scripted = {
      prop = "zombieman", hp = 10, initial = "count",
      vars = { n = 0 },
      states = {
        count = {
          anim = 0, move = "stop", ticks = 6, next = "count",
          sequence = {
            { at = 1, action = "lua",
              fn = function(mob) mob.vars.n = mob.vars.n + 1 end },
          },
          transitions = {
            { to = "done", when = {
                { cond = "lua", fn = function(mob) return mob.vars.n >= 3 end } } },
          },
        },
        done = { anim = 1, move = "stop" },
      },
    },

    -- Can hop. `jump_height` is a routing capability AND a promise: the graph
    -- plans through rises up to it, and the runtime presses jump where the
    -- route says to.
    hopper = {
      prop = "zombieman", hp = 30, initial = "chase", jump_height = "2",
      states = { chase = { anim = 1, move = "chase", face = "target" } },
    },

    -- The same mob without one, so the pair differ in exactly one field.
    walker = {
      prop = "zombieman", hp = 30, initial = "chase",
      states = { chase = { anim = 1, move = "chase", face = "target" } },
    },

    -- Fires a rocket, for damage_area.
    gunner = {
      prop = "zombieman", hp = 50, initial = "look",
      vars = { ready = 0 },
      states = {
        look = {
          anim = 0, move = "stop",
          transitions = { { to = "fire", when = {
              { cond = "has_target" },
              { cond = "cooldown_ready", var = "ready" } } } },
        },
        fire = {
          anim = 2, move = "stop", ticks = 30, next = "look",
          sequence = {
            { at = 0, action = "face" },
            { at = 2, action = "missile", kind = "rocket", offset = "1" },
            { at = 2, action = "set_cooldown", var = "ready", ticks = 200 },
          },
        },
      },
    },

    -- Fires a seeker, for homing.
    seeker_gun = {
      prop = "zombieman", hp = 50, initial = "look",
      vars = { ready = 0 },
      states = {
        look = {
          anim = 0, move = "stop",
          transitions = { { to = "fire", when = {
              { cond = "has_target" },
              { cond = "cooldown_ready", var = "ready" } } } },
        },
        fire = {
          anim = 2, move = "stop", ticks = 30, next = "look",
          sequence = {
            { at = 0, action = "face" },
            { at = 2, action = "missile", kind = "seeker", offset = "1" },
            { at = 2, action = "set_cooldown", var = "ready", ticks = 400 },
          },
        },
      },
    },
  },
}
