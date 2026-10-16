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
      -- A flinch that grows with the hit, so both runtimes have to agree on
      -- its length: a 4-point shot holds it 8 + 6 ticks.
      pain_time = { per_damage = '1.5', max = 20 },
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
          -- ORDER IS PRIORITY, and in a fixture that is a trap of its own: the
          -- first version put `count_within` above `prop_within`, and since
          -- three dervishes are always within fourteen units of each other the
          -- prop edge never fired once. It looked exactly like a condition
          -- that does not work. So every edge below is gated by a `chance`
          -- except the two that gate themselves -- `strike` on a cooldown and
          -- `look` on having no target -- which is what gives each of them a
          -- turn over a run.
          transitions = {
            { to = 'look',  when = { { cond = 'has_target', negate = true } } },
            -- THE HEIGHT PAIR HIGH IN THE LIST, because this terrain's floors
            -- run 1.5 to 8.5 and a monster is only a unit above or below you
            -- while it is crossing between them. Behind the chance-gated edges
            -- it never got a turn, and an edge that never fires proves nothing
            -- about the condition on it.
            { to = 'peer_up',   when = { { cond = 'target_above', by = '0.35' },
                                         { cond = 'chance', percent = 50 } } },
            { to = 'peer_down', when = { { cond = 'target_below', by = '0.35' },
                                         { cond = 'chance', percent = 50 } } },
            -- CAN_REACH, NOT NEGATED. This map is connected, so "no route"
            -- cannot be produced in it and an edge waiting for one would never
            -- fire -- which would leave the A* unexercised in both runtimes
            -- and the budget they share unspent. Asked the other way up it
            -- runs the search, every think this edge is reached, in both.
            { to = 'giveup', when = { { cond = 'target_within', distance = '20' },
                                      { cond = 'cooldown_ready', var = 'ready' },
                                      { cond = 'can_reach' },
                                      { cond = 'chance', percent = 15 } } },
            -- PRESSED INTO GEOMETRY AND GOING NOWHERE: back off and wander
            -- rather than grind. High in the list because it is the one edge
            -- here that is about a failure -- a mob that cannot move is not
            -- helped by being offered four other things to do first -- and
            -- because an edge at the bottom of a list of chance-gated ones
            -- never fires at all, which is how the first version of this
            -- fixture proved nothing.
            --
            -- `stuck` is literal: it asks whether the body got anywhere
            -- between two decisions. A monster that spent them standing in
            -- `strike` did not, and that is a true answer rather than a
            -- lenient one.
            { to = 'shrug',  when = { { cond = 'stuck', ticks = 24 },
                                      { cond = 'chance', percent = 50 } } },
            { to = 'strike', when = { { cond = 'target_within', distance = '12' },
                                      { cond = 'target_visible' },
                                      { cond = 'target_in_front', arc = 20 },
                                      -- Not through a neighbour. In Doom
                                      -- hitting one is a feature; here it is
                                      -- what this condition exists to refuse.
                                      { cond = 'ally_in_front', arc = 14,
                                        distance = '12', negate = true },
                                      { cond = 'cooldown_ready', var = 'ready' } } },
            -- Cheap gates FIRST, then the search. The vocabulary says so and
            -- this is what it looks like: a distance and a cooldown before an
            -- A* that the engine refuses past sixty-four a tick.
            -- EACH TO ITS OWN STATE, again. Pointing two conditions at one
            -- destination is what made `prop_within` unprovable the first
            -- time: the trace shows the state and says nothing about which
            -- edge took it there, so a condition that is always false reads
            -- exactly like one that works.

            { to = 'guard',  when = { { cond = 'prop_within', distance = '8',
                                        prop = 'barrel' },
                                      { cond = 'chance', percent = 35 } } },
            { to = 'huddle', when = { { cond = 'count_within', distance = '14',
                                        at_least = 2, mob = 'dervish' },
                                      { cond = 'chance', percent = 35 } } },
            { to = 'circle', when = { { cond = 'mob_within', distance = '6',
                                        mob = 'dervish' },
                                      { cond = 'chance', percent = 35 } } },
            -- The rolled variable, read rather than re-rolled.
            { to = 'roam',   when = { { cond = 'var_at_least', var = 'mood', value = 7 } } },
            -- TELEPORTS, both kinds, LAST so they cost the edges above nothing:
            -- around the target when it is far ("further than 14", a negated
            -- target_within), and onto its back when it is close.
            { to = 'blink',  when = { { cond = 'target_within', distance = '14', negate = true },
                                      { cond = 'chance', percent = 25 } } },
            { to = 'pounce', when = { { cond = 'target_within', distance = '8' },
                                      { cond = 'chance', percent = 20 } } },
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
            { at = 10, action = 'set_cooldown', var = 'ready', ticks = 45, jitter = 20 },
            { at = 14, action = 'set_angle', var = 'wander', towards = 'away' },
          },
        },

        -- The two states the proximity conditions lead to, so the decision
        -- trace names which condition fired.
        huddle = {
          -- A jittered duration, so three monsters that entered together do
          -- not leave together and move as one animal.
          anim = 1, move = 'strafe', face = 'target',
          ticks = 16, ticks_jitter = 12, next = 'stalk',
          sequence = {
            { at = 0, action = 'add_var', var = 'mood', by = 1 },
            -- ONE ROLL, read by two edges below. A `chance` on each would be
            -- two rolls and two independent answers, which is a different
            -- behaviour and not an equivalent one.
            { at = 1, action = 'var_random', var = 'mood', min = 0, max = 9 },
          },
        },
        guard = {
          anim = 1, move = 'chase', face = 'target',
          ticks = 20, ticks_jitter = 10, next = 'stalk',
          sequence = { { at = 0, action = 'set_angle', var = 'wander',
                         towards = 'away', jitter = 30 } },
        },

        -- `stuck` leads here, and this is also where the ambush verbs live:
        -- waking the sleeper placed beside the others, and shouting so the
        -- rest of the room learns what this one is hunting.
        shrug = {
          anim = 1, move = 'flee', face = 'target', ticks = 14, next = 'roam',
          sequence = {
            { at = 0, action = 'set_angle', var = 'wander', towards = 'random' },
            { at = 2, action = 'wake', radius = '30', mob = 'dervish' },
            { at = 4, action = 'alert_nearby', radius = '24', mob = 'dervish' },
          },
        },

        blink  = { anim = 1, move = 'stop', face = 'target', ticks = 10, next = 'stalk',
                   sequence = { { at = 2, action = 'teleport', to = 'near', distance = '7',
                                  min = '3', effect = 1 } } },
        -- Onto the target's back, and a slam where it lands: everything within
        -- four units is hit, the other dervishes too, so a run has flinches to
        -- compare whatever the random stream did to the strikes.
        pounce = { anim = 1, move = 'stop', face = 'target', ticks = 10, next = 'stalk',
                   sequence = { { at = 2, action = 'teleport', to = 'behind', distance = '2',
                                  effect = 1 },
                                { at = 4, action = 'damage_area', damage = 3, radius = '4' } } },

        -- One per costly condition, so the decision trace names which fired.
        giveup    = { anim = 1, move = 'wander', face = 'movement',
                      ticks = 20, ticks_jitter = 8, next = 'look',
                      sequence = { { at = 0, action = 'set_angle', var = 'wander',
                                     towards = 'random' } } },
        peer_up   = { anim = 1, move = 'stop', face = 'target', ticks = 10, next = 'stalk' },
        peer_down = { anim = 1, move = 'stop', face = 'target', ticks = 10, next = 'stalk' },

        -- A reaction: back to whatever the hit interrupted, resumed where it was.
        pain = { anim = 3, move = 'stop', face = 'hold', ticks = 8, next = '@previous' },
        death = { anim = 4, move = 'stop', face = 'hold', solid = false,
                  ticks = 60, next = 'death' },
      },
    },
  },
}

-- PROPS, so `prop_within` has something to find. Without them it is evaluated
-- on every think and is always false, which reads in a passing run exactly
-- like a condition that works.
for _, at in ipairs{ {18, 32}, {24, 28}, {22, 38} } do
  dnp.spawn_prop('barrel', vec3.new(fx.int(at[1]), fx.int(at[2]), fx.int(4)))
end

mobs.place{
  {kind = 'dervish', x = 20, y = 30, z = 4},
  {kind = 'dervish', x = 26, y = 34, z = 4},
  {kind = 'dervish', x = 16, y = 36, z = 4},
  -- DORMANT, so `wake` has something to wake. Until that action existed the
  -- flag was set at placement and nothing anywhere could clear it.
  {kind = 'dervish', x = 30, y = 26, z = 4, dormant = true},
}
