-- The vocabulary fixture: one mob whose graph uses every action the two mob
-- runtimes both have to implement.
--
-- WHAT IT IS FOR. maps/scripts/mobs.lua and server/src/mobs/ are two
-- implementations of one vocabulary, and tools/mobdiff proves they agree by
-- running a scenario through each and comparing the world tick by tick. That
-- proof is only worth what the scenario exercises -- and maps/monsters.map's
-- Doom monsters use eleven conditions and four actions between them, so a
-- vocabulary entry neither of them touches is unproven however green the run.
--
-- So this map's monster is deliberately not a monster anybody would ship. It
-- is a graph that touches the parts of the vocabulary the Doom set does not,
-- and every addition to mob_vocab.lua should gain a state here.
--
-- DELIBERATELY RANDOM, in three places. `chance`, `set_angle towards =
-- random` and `face jitter` all draw from the world's own stream, and the two
-- runtimes have to draw the SAME number of times in the SAME order -- which is
-- the failure mode that does not show up in a graph that never rolls
-- anything, and is the single easiest way for two runtimes to drift apart.
mobs.load{
  mobs = {
    dervish = {
      hp = 200,
      prop = 'zombieman',          -- a body that exists; the brain is the point
      initial = 'look',
      sight = mobs.du(2048),
      turn_speed = 8,
      pain_chance = 128,
      infighting = true,
      corpse = true,
      -- `wander` by name: `move = "wander"` reads this variable, and before
      -- `set_angle` existed nothing anywhere wrote it.
      vars = { ready = 0, wander = 0, mood = 0 },
      states = {
        look = {
          anim = 0, move = 'stop', face = 'hold',
          ticks = 24, next = 'roam',
          transitions = {
            { to = 'stalk', when = { { cond = 'has_target' },
                                     { cond = 'target_visible' } } },
          },
        },

        -- WANDERING, which is the bug this fixture was written around: a new
        -- heading rolled on entry, then walked.
        roam = {
          anim = 1, move = 'wander', face = 'movement',
          ticks = 40, next = 'look',
          sequence = {
            { at = 0, action = 'set_angle', var = 'wander', towards = 'random' },
            { at = 20, action = 'set_angle', var = 'wander', towards = 'facing',
              jitter = 60 },
          },
          transitions = {
            { to = 'stalk', when = { { cond = 'has_target' },
                                     { cond = 'target_visible' } } },
          },
        },

        -- Facing off the target rather than at it: a monster sidling across
        -- your front. `offset` is what makes that expressible at all.
        stalk = {
          anim = 1, move = 'chase', face = 'target',
          transitions = {
            { to = 'look',  when = { { cond = 'has_target', negate = true } } },
            { to = 'circle', when = { { cond = 'target_within', distance = '6' },
                                      { cond = 'chance', percent = 40 } } },
            { to = 'strike', when = { { cond = 'target_within', distance = '12' },
                                      { cond = 'target_visible' },
                                      { cond = 'target_in_front', arc = 20 },
                                      { cond = 'cooldown_ready', var = 'ready' } } },
          },
        },

        circle = {
          anim = 1, move = 'strafe', face = 'target',
          ticks = 30, next = 'stalk',
          sequence = {
            { at = 0, action = 'face', towards = 'target', offset = 75 },
            { at = 12, action = 'add_var', var = 'mood', by = 1 },
          },
        },

        strike = {
          anim = 2, move = 'stop', face = 'target',
          ticks = 32, next = 'stalk',
          sequence = {
            { at = 0, action = 'face', towards = 'target', jitter = 4 },
            { at = 6, action = 'hitscan', damage = 4, range = '20', spread = 3 },
            { at = 10, action = 'set_cooldown', var = 'ready', ticks = 45 },
            { at = 14, action = 'set_angle', var = 'wander', towards = 'away' },
          },
        },

        pain = { anim = 3, move = 'stop', face = 'hold', ticks = 8, next = 'stalk' },
        death = { anim = 4, move = 'stop', face = 'hold', solid = false,
                  ticks = 60, next = 'death' },
      },
    },
  },
}

mobs.place{
  {kind = 'dervish', x = 20, y = 30, z = 4},
  {kind = 'dervish', x = 26, y = 34, z = 4},
  {kind = 'dervish', x = 16, y = 36, z = 4},
}
