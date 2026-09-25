-- The mob vocabulary: every state field, condition and action, as data.
--
-- NORMATIVE, in the same sense trigger_vocab.lua is: maps/scripts/mobs.lua
-- validates against this table, docs/MOBS.md is written from it, and the mob
-- editor's UI is generated from it. Three programs need the same list, so two
-- of them ask the third rather than carrying a copy.
--
-- It is DATA. Nothing here is a function -- a table is something an editor can
-- render, re-edit and write back, which generated Lua source is not once a
-- person has touched it.
--
-- THIS IS NOT DOOM'S MODEL, DELIBERATELY.
--
-- Doom's actor is one flat list of {sprite, tics, action, next}, about
-- twenty-five entries per monster, and every behaviour it has -- including
-- "walk towards the player" -- is a codepointer hung off a frame. Copying that
-- would have bought exact fidelity and cost three things worth more: the graph
-- would not be drawable (twenty-five nodes a monster), the per-frame tics
-- would fight design §4, where the server declares an animation STATE and the
-- client owns its timing, and every behaviour that is not a Doom behaviour
-- would be a special case.
--
-- So the vocabulary is general and Doom is a TEST CASE for it. A zombieman is
-- a mob whose `missile` state fires one hitscan; that its numbers match Doom's
-- is a property of maps/mobs/doom.lua, not of this file. Anything the
-- vocabulary cannot express reaches `lua`, which is an escape hatch and is
-- meant to be used -- see "Custom" at the bottom.
--
-- TWO LEVELS, because they are two different questions:
--
--   the GRAPH      which state a mob is in, and what moves it to another.
--                  Named states, transitions with conditions. ~6-10 nodes,
--                  which is what a person draws.
--   the SEQUENCE   what one state DOES while it is in it: an ordered list of
--                  {at = <ticks after entry>, action}. Doom's three-frame
--                  attack -- wind up, fire, recover -- is two entries and a
--                  duration, which is what `windup`/`cooldown` in the old
--                  enemies.lua were a hardcoding of.
--
-- Types, matching trigger_vocab.lua's so one checker serves both:
--
--   "int"     a Lua number, integral
--   "fx"      exact fixed-point: a string like "0.25", or an integer
--   "string"  text
--   "bool"    true/false
--   "enum"    one of `values`
--   "state"   the name of a state in the same mob
--   "mob"     the name of a mob in the same file
--   "missile" the name of a missile in the same file
--   "var"     the name of a per-mob variable
--   "actions" a nested action list
--
-- `optional` args may be omitted; `one_of` groups are mutually exclusive and
-- exactly one member must be present.

mob_vocab = {
  version = 1,

  -- ------------------------------------------------------------------ mob
  --
  -- What a mob definition itself may carry. Anything else is a typo, and a
  -- silently ignored `helth = 60` is a monster that looks configured and has
  -- the default hit points.
  mob_fields = {
    { name = "prop", type = "string", optional = true,
      doc = "Which catalogue record is this mob's BODY. Defaults to the mob's " ..
            "own name, which is what you want: a mob and its body are one " ..
            "thing said in two files, and restating the name in the second " ..
            "one is how they come to disagree. assets/props.txt owns the " ..
            "radius, height, speed, solid flag and the sprites, because the " ..
            "client reads that file too. NOTHING in THIS file crosses the " ..
            "wire: a client that needed a pain chance to draw a monster " ..
            "would be a protocol leak. Name a record here only to make two " ..
            "mobs share one body deliberately." },
    { name = "hp", type = "int",
      doc = "The engine has no notion of health. This is the script's." },
    { name = "initial", type = "state", optional = true,
      doc = "Which state a freshly spawned mob enters. Defaults to the first " ..
            "state in declaration order." },
    { name = "vars", type = "table", optional = true,
      doc = "Per-mob counters, by name, with their starting values. Each mob " ..
            "gets its own copy at spawn. This is where a refire count, a " ..
            "spawn budget or a grudge lives." },
    { name = "states", type = "table",
      doc = "The graph, by state name." },
    { name = "sight", type = "fx", optional = true,
      doc = "How far this mob notices a target at all. A cheap distance gate " ..
            "checked before anything expensive -- `target_visible` casts a " ..
            "ray, and a ray for every monster on a Doom level every tick is " ..
            "the cost this exists to avoid." },
    { name = "pain_chance", type = "int", optional = true,
      doc = "0-255, rolled when the mob is hurt: the chance it flinches at " ..
            "all. Doom's number, because it is a good one -- a monster that " ..
            "ALWAYS flinches can be stun-locked, and one that never does " ..
            "gives no feedback that a hit landed." },
    { name = "turn_speed", type = "fx", optional = true,
      doc = "Turns per second the body may rotate. The engine's default is 4, " ..
            "which is a quarter of a second to come ninety degrees -- and a " ..
            "monster fires along the direction its BODY is pointing, not the " ..
            "one it is aiming at, so a body that lags its target wastes shots " ..
            "into the nearest wall. In a corridor a few degrees off is a wall " ..
            "a metre away, which is what a tracer that renders as a stub is " ..
            "showing you." },
    { name = "jump_height", type = "fx", optional = true,
      doc = "How much MORE than a step this mob can gain by hopping. Absent " ..
            "or zero is design §3.2.2's rule -- enemies do not jump -- and " ..
            "routes exactly as it always did.\n" ..
            "It is a ROUTING capability and a promise: the nav graph will " ..
            "plan through rises up to this, and the runtime then presses " ..
            "jump where the route says to. Setting it higher than the mob's " ..
            "`jump_impulse` can actually reach gives routes it cannot walk, " ..
            "and the symptom is a monster pressed into a ledge forever." },
    { name = "flying", type = "bool", optional = true,
      doc = "Sets gravity_scale to zero at spawn and lets `move` drive Z. " ..
            "Costs no engine support: gravity_scale is a replicated " ..
            "parameter, so a flying monster is flying on every client " ..
            "without a word of it crossing the wire." },
    { name = "hover_z", type = "fx", optional = true,
      doc = "For a flyer: how far above its target's feet it tries to sit." },
    { name = "infighting", type = "bool", optional = true,
      doc = "Whether being hurt by another mob makes that mob the target. " ..
            "Doom's monsters do this and it is most of what makes a crowded " ..
            "room interesting." },
    { name = "corpse", type = "bool", optional = true,
      doc = "Whether the body stays after the death state's sequence ends. " ..
            "Corpses are capped and recycled oldest-first by the runtime." },
  },

  -- ---------------------------------------------------------------- state
  state_fields = {
    { name = "anim", type = "int",
      doc = "The animation state number the client already understands -- the " ..
            "`state <n>` on a sprite line in assets/props.txt. The server " ..
            "declares it and the client owns the timing (design §4), so this " ..
            "is a number, not a frame list." },
    { name = "move", type = "enum", optional = true,
      values = { "stop", "chase", "flee", "wander", "strafe", "charge", "free" },
      doc = "How the body is driven while in this state. `free` means the " ..
            "state's own actions drive it and the runtime keeps its hands " ..
            "off -- what a scripted leap or a custom controller wants." },
    { name = "face", type = "enum", optional = true,
      values = { "target", "movement", "hold" },
      doc = "Where it looks. Separate from `move` because a monster backing " ..
            "away from you while still facing you is a different thing from " ..
            "one turning its back, and Doom's flee does the first." },
    { name = "ticks", type = "int", optional = true,
      doc = "How long the state lasts. Absent means until a transition fires " ..
            "-- which is what `chase` wants and what an attack does not." },
    { name = "next", type = "state", optional = true,
      doc = "Where it goes when `ticks` runs out. Absent with `ticks` set is " ..
            "a state that ends and stays, which the loader refuses." },
    { name = "sequence", type = "table", optional = true,
      doc = "Ordered {at = <ticks after entry>, ...action}. `at` is relative " ..
            "to entering the state and is a DEADLINE, not a countdown: a mob " ..
            "nobody is near costs nothing between its own decisions." },
    { name = "transitions", type = "table", optional = true,
      doc = "Ordered { to = <state>, when = {conditions} }. FIRST MATCH WINS, " ..
            "and order is the priority -- there is no scoring and no " ..
            "backtracking, because a graph whose behaviour depends on a " ..
            "weighting nobody can see is not one an editor can show you." },
    { name = "interruptible", type = "bool", optional = true,
      doc = "Whether pain and death may cut this state short. Death ignores " ..
            "it; nothing interrupts dying." },
    { name = "solid", type = "bool", optional = true,
      doc = "Collision while in this state. A corpse sets it false, which is " ..
            "why you can walk over one." },
  },

  -- ----------------------------------------------------------- conditions
  --
  -- EVERY condition accepts `negate = true`, which inverts it. One universal
  -- modifier rather than a `no_target` beside `has_target` and a
  -- `target_hidden` beside `target_visible`: the pairs would double the
  -- vocabulary, and an editor can render one checkbox next to any condition.
  --
  -- There is no `any_of` and no nesting. A transition's `when` is an AND of
  -- its conditions, and an OR is two transitions to the same state. That is a
  -- deliberate limit: a boolean tree is not something an editor can show at a
  -- glance, and the flat list is what makes "first match wins, in declaration
  -- order" a rule an author can hold in their head. triggers.lua made the same
  -- call for the same reason -- see "Why there is no boolean tree" in
  -- docs/TRIGGERS.md.
  --
  -- PURE READS. A condition never mutates, never spawns, and never touches
  -- the RNG -- except `chance`, which is listed here anyway because the
  -- alternative is every author writing the same `lua` block. It is called
  -- exactly once per evaluation and the runtime says so.
  conditions = {
    { name = "has_target",
      args = {},
      doc = "Something is being hunted at all." },

    { name = "target_within",
      args = { { name = "distance", type = "fx" } },
      doc = "Plan distance, not 3D: a cacodemon above you is still near you." },

    { name = "target_visible",
      args = {},
      doc = "A ray reaches it. NOT the same question as `target_within`, and " ..
            "conflating them is what had monsters shooting through two walls " ..
            "at somebody across the map. Costs a raycast -- put a " ..
            "`target_within` before it." },

    { name = "target_in_front",
      args = { { name = "arc", type = "int", optional = true,
                 doc = "Degrees either side of facing. Default 90." } },
      doc = "Whether the target is within the arc the mob is facing." },

    { name = "hp_below",
      args = { { name = "percent", type = "int" } },
      doc = "Of its own starting hp." },

    { name = "state_elapsed",
      args = { { name = "ticks", type = "int" } },
      doc = "How long it has been in the current state." },

    { name = "chance",
      args = { { name = "percent", type = "int" } },
      doc = "Rolls dnp.random, which is seeded per world and replays " ..
            "identically. The ONLY condition that is not a pure read; it is " ..
            "here rather than in `lua` because refire, wander and pain all " ..
            "want it and none of them should have to compile a chunk." },

    { name = "var_at_least",
      args = { { name = "var", type = "var" }, { name = "value", type = "int" } },
      doc = "A per-mob counter." },

    { name = "var_is",
      args = { { name = "var", type = "var" }, { name = "value", type = "int" } } },

    { name = "cooldown_ready",
      args = { { name = "var", type = "var" } },
      doc = "The named var holds an absolute tick; true once it has passed. " ..
            "A deadline rather than a counter, so a mob nobody is near is " ..
            "not paying to tick one down." },

    { name = "was_hurt",
      args = { { name = "within", type = "int", optional = true,
                 doc = "Ticks. Default 1 -- this tick." } },
      doc = "Something damaged it recently. The pain-state edge." },

    { name = "grounded", args = {} },

    { name = "hurt_by_mob",
      args = {},
      doc = "What hurt it was another mob, not a player. The infighting edge." },



    { name = "lua",
      args = { { name = "code", type = "string", one_of = "body", optional = true,
                 doc = "An expression over `mob`, returning truthy." },
               { name = "fn", type = "function", one_of = "body", optional = true,
                 doc = "A Lua function taking `mob`. Needs no compiler at run " ..
                       "time, so it works under Trust::Strict where `code` " ..
                       "does not." } },
      doc = "The escape hatch. Anything the vocabulary cannot say." },
  },

  -- -------------------------------------------------------------- actions
  actions = {
    { name = "hitscan",
      args = {
        { name = "damage", type = "int" },
        { name = "range", type = "fx" },
        { name = "count", type = "int", optional = true,
          doc = "Bullets in one pull. Default 1. A shotgunner is 3." },
        { name = "spread", type = "int", optional = true,
          doc = "Degrees of random cone, per bullet." },
        { name = "tracer", type = "int", optional = true,
          doc = "Tracer style to draw, or absent for none." },
      },
      doc = "An instant shot. The engine answers where the ray ended and what " ..
            "was there; what that COSTS is this action." },

    { name = "missile",
      args = {
        { name = "kind", type = "missile" },
        { name = "count", type = "int", optional = true },
        { name = "arc", type = "int", optional = true,
          doc = "Degrees between shots when `count` > 1. A mancubus volley." },
        { name = "offset", type = "fx", optional = true,
          doc = "How far in front of the body it appears. Launched clear or " ..
                "it is touching its owner on the tick it exists." },
      },
      doc = "Launch one of the `missiles` defined in the same file." },

    { name = "damage",
      args = { { name = "amount", type = "int" } },
      doc = "Hurt the current target, with no range test. For a missile's " ..
            "`on_hit`, where contact has ALREADY been established by the " ..
            "flight -- `melee` measures from the actor, and a missile's " ..
            "actor is the mob that fired it, which by then is somewhere " ..
            "else entirely. That was a real bug: a fireball burst on the " ..
            "player and did nothing, because the range was being tested from " ..
            "twelve units back where the imp was standing." },

    { name = "melee",
      args = { { name = "damage", type = "int" },
               { name = "range", type = "fx" } },
      doc = "Damage if the target is inside `range` right now. Separate from " ..
            "`hitscan` because a monster with only a ranged attack is " ..
            "harmless the moment you stand inside it -- a missile spawned " ..
            "inside somebody bursts on its owner or sails past." },

    { name = "impulse",
      args = { { name = "speed", type = "fx" },
               { name = "towards", type = "enum", optional = true,
                 values = { "target", "facing", "away" } },
               { name = "vertical", type = "fx", optional = true } },
      doc = "A shove, in world units per second. A lost soul's charge, a leap." },

    { name = "spawn",
      args = { { name = "mob", type = "mob" },
               { name = "count", type = "int", optional = true },
               { name = "limit", type = "int", optional = true,
                 doc = "Refuse if this many of that kind are already alive " ..
                       "and owned by this one. A pain elemental without a " ..
                       "budget fills the entity table." },
               { name = "offset", type = "fx", optional = true } } },

    { name = "revive",
      args = { { name = "radius", type = "fx" },
               { name = "mob", type = "mob", optional = true,
                 doc = "Only this kind. Absent means any." } },
      doc = "Put a corpse in reach back on its feet. Costs no engine support: " ..
            "corpses are already a script-side list." },

    { name = "damage_area",
      args = { { name = "damage", type = "int" },
               { name = "radius", type = "fx" },
               { name = "falloff", type = "bool", optional = true },
               { name = "hurts_owner", type = "bool", optional = true } },
      doc = "A blast. `hurts_owner` false is a rocket that does not kill the " ..
            "cyberdemon that fired it." },

    { name = "set_var",
      args = { { name = "var", type = "var" }, { name = "value", type = "int" } } },

    { name = "add_var",
      args = { { name = "var", type = "var" }, { name = "by", type = "int" } } },

    { name = "set_cooldown",
      args = { { name = "var", type = "var" }, { name = "ticks", type = "int" } },
      doc = "Writes tick + ticks into the var, for `cooldown_ready` to read." },

    { name = "goto",
      args = { { name = "state", type = "state" } },
      doc = "Leave now, mid-sequence. The rest of the sequence does not run." },

    { name = "face",
      args = { { name = "towards", type = "enum", optional = true,
                 values = { "target", "movement", "away", "angle", "var" } },
               { name = "angle", type = "int", optional = true,
                 doc = "Degrees, an ABSOLUTE world heading, for " ..
                       "`towards = angle`. 0 is +x and it counts " ..
                       "anticlockwise, which is what dnp.atan2 answers in." },
               { name = "var", type = "var", optional = true,
                 doc = "A per-mob variable holding a heading, for " ..
                       "`towards = var`. Written by `set_angle`, which is how " ..
                       "a mob turns to something it worked out earlier." },
               { name = "offset", type = "int", optional = true,
                 doc = "Degrees added after, signed. `towards = target` with " ..
                       "an offset of 90 is a monster sidling across your " ..
                       "front rather than walking into it." },
               { name = "jitter", type = "int", optional = true,
                 doc = "Degrees of random slop either side, added last. Draws " ..
                       "once, off the world's own stream, exactly as " ..
                       "`chance` does -- so it replays." } },
      -- NOT `at`. A sequence entry's own `at` is the tick it fires on, and an
      -- action argument by the same name is unreachable -- the checker caught
      -- this the first time it ran, which is the argument for having it.
      doc = "Turn to face, once, at this instant. Doom's A_FaceTarget, and " ..
            "the reason a monster's shot goes where you are rather than " ..
            "where you were when it started winding up." },

    { name = "set_angle",
      args = { { name = "var", type = "var" },
               { name = "towards", type = "enum", optional = true,
                 values = { "target", "facing", "away", "random", "angle" } },
               { name = "angle", type = "int", optional = true,
                 doc = "Degrees, for `towards = angle`." },
               { name = "jitter", type = "int", optional = true,
                 doc = "Degrees of random slop either side." } },
      doc = "Work out a heading and keep it in a variable, for `face` and for " ..
            "`move = \"wander\"` to read.\n" ..
            "AN ANGLE IS A NUMBER, so it lives in the same per-mob variables " ..
            "everything else does rather than in a second kind of storage: a " ..
            "turn is 2^32 and that is exact in a double, which is why brad " ..
            "are plain Lua numbers here and `homing` on a missile is an int.\n" ..
            "`towards = random` is the one that makes wandering wander. " ..
            "Without it `move = \"wander\"` reads a heading nothing ever " ..
            "wrote and walks straight at the aim -- which it did, silently, " ..
            "for as long as the mode has existed." },

    { name = "set_param",
      args = { { name = "param", type = "string" },
               { name = "value", type = "fx" } },
      doc = "A replicated movement parameter, by the name in " ..
            "core/include/dnp/physics/params_table.hpp. This is how a mob " ..
            "changes its own speed -- and because the parameter replicates, " ..
            "every client agrees without this file knowing the wire exists." },

    { name = "effect",
      args = { { name = "param", type = "string" },
               { name = "mul", type = "fx", optional = true },
               { name = "add", type = "fx", optional = true },
               { name = "ticks", type = "int", optional = true },
               { name = "tag", type = "string", optional = true } },
      doc = "A temporary modifier through the effect stack, which recomputes " ..
            "from base every tick and so cannot drift." },

    { name = "vfx",
      args = { { name = "kind", type = "int" },
               { name = "at", type = "enum", optional = true,
                 values = { "self", "target", "impact" } } } },

    { name = "set_anim",
      args = { { name = "anim", type = "int" } },
      doc = "Override the state's own `anim` mid-sequence." },

    { name = "despawn", args = {} },

    { name = "lua",
      args = { { name = "code", type = "string", one_of = "body", optional = true },
               { name = "fn", type = "function", one_of = "body", optional = true } },
      doc = "The escape hatch. `mob` is in scope, and so is everything in " ..
            "dnp.*. Anything this vocabulary cannot say goes here, and that " ..
            "is what it is for -- a framework whose only extension point is " ..
            "adding to the framework is one nobody can build on." },
  },

  -- ------------------------------------------------------------- missiles
  --
  -- Projectiles are their OWN definitions rather than arguments to `missile`,
  -- because two mobs share one and because an editor wants to show a list of
  -- them. A missile is not a mob: it has no graph, it has a flight and an
  -- arrival.
  missile_fields = {
    { name = "prop", type = "string", optional = true,
      doc = "Which catalogue record this missile is drawn from, and whose " ..
            "radius it hits with. Defaults to the missile's own name, as a " ..
            "mob's does." },
    { name = "speed", type = "fx", doc = "World units per second." },
    { name = "damage", type = "int", optional = true },
    { name = "radius", type = "fx", optional = true,
      doc = "What it collides with, as a radius. Absent means the `prop`'s " ..
            "own radius from assets/props.txt, which is usually right -- a " ..
            "missile that hits with a different size than it is drawn is a " ..
            "missile players learn to distrust." },
    { name = "z_offset", type = "fx", optional = true,
      doc = "How far above the shooter's feet it appears. Doom launches from " ..
            "mid-body; a missile born at ankle height clips the first step " ..
            "it crosses." },
    { name = "gravity", type = "fx", optional = true,
      doc = "Zero for a Doom fireball, which flies flat. Nonzero for a " ..
            "grenade." },
    { name = "life", type = "int", optional = true,
      doc = "Ticks before it expires on its own. A missile with no ceiling " ..
            "on its life leaks a prop slot per shot." },
    { name = "homing", type = "int", optional = true,
      doc = "Degrees per tick it may turn towards its target. A revenant's " ..
            "missile. Absent means it flies straight.\n" ..
            "An INT, not an fx: angles here are brad -- a turn is 2^32 -- " ..
            "and brad is a plain Lua number, because 2^32 is exact in a " ..
            "double. Declaring it `fx` made it an fx userdata that then met " ..
            "plain arithmetic and raised." },
    { name = "anim_fly", type = "int", optional = true },
    { name = "anim_burst", type = "int", optional = true },
    { name = "burst_ticks", type = "int", optional = true },
    { name = "on_hit", type = "actions", optional = true,
      doc = "Runs when it reaches something, with the thing it reached as the " ..
            "target. This is where the damage and the splash go -- so a " ..
            "rocket is a missile whose on_hit is a damage_area, and nothing " ..
            "in the runtime knows what a rocket is." },
    { name = "on_expire", type = "actions", optional = true },
  },
}

-- No `return`: this is loaded as one of a map's scripts and the VM discards
-- the value. `mob_vocab` above is a global, which is how mobs.lua finds it.
