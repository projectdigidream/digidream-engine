-- The weapon vocabulary: every weapon field, phase, condition and action, as data.
--
-- NORMATIVE, in the same sense mob_vocab.lua and trigger_vocab.lua are:
-- maps/scripts/weapons.lua validates against this table, docs/WEAPONS.md is
-- written from it, and maps/scripts/weapon_vocab.json is generated from it.
-- Several programs need the same list, so they ask this one rather than each
-- carrying a copy that drifts.
--
-- It is DATA. Nothing here is a function -- a table is something an editor can
-- render, re-edit and write back, which generated Lua source is not once a
-- person has touched it.
--
-- THIS IS NOT DOOM'S WEAPON MODEL, DELIBERATELY.
--
-- The weapons this replaces were six flat records: damage, range, count,
-- spread, cooldown, ammo. That is enough for Doom and for nothing else. It
-- could not express a shot that charges while you hold the button, a second
-- press that continues a combo, a weapon that shoves you backwards, or a weapon
-- that fires differently on the other button -- and every one of those is
-- ordinary in a platformer.
--
-- It was also the exact mistake docs/MOBS.md:70-73 records about the framework
-- that preceded the mob runtime: `cooldown` as ONE NUMBER is a hardcoding of
-- "wind up, fire, recover" for the two weapons that happened to exist. So the
-- shape here is the mob runtime's, for the reason that argument already won.
--
-- TWO LEVELS, because they are two different questions:
--
--   the PHASES     which phase the weapon is in, and what moves it to another.
--                  Named, ~4-8 of them, with transitions whose conditions are
--                  checked in order and where the first match wins. `ready`,
--                  `fire`, `recover` is the shape most weapons have; `charge`
--                  and `fire2` are what make the other two kinds possible.
--   the SEQUENCE   what one phase DOES while it is in it: an ordered list of
--                  {at = <ticks after entry>, action}. Doom's whole firing
--                  routine is two entries and a duration.
--
-- WHAT FALLS OUT RATHER THAN BEING A FEATURE:
--
--   automatic fire   `ready` transitions to `fire` on `button_held` instead of
--                    on `button_pressed`. There is no `auto` flag.
--   charge-up        a `charge` phase with no `ticks` -- so it waits -- and
--                    transitions on `button_released`. `held_for` on an earlier
--                    transition picks a stronger phase for a longer hold.
--   combos           `recover` has `ticks` (which IS the window) and a
--                    transition on `button_pressed` to a second attack. A
--                    press one tick late finds the phase gone and reaches
--                    `ready` instead, which is what a window means.
--   alt-fire         a transition naming `secondary` rather than `primary`.
--   recoil           the `impulse` action, in a sequence, like everything else.
--
-- THE SWING IS NOT AN ACTION, and that is the one deliberate asymmetry here.
-- `swing` is a FIELD on the weapon, because the swept collider is the one
-- attack this engine PREDICTS: the start rule lives in the shared controller
-- (core/src/sim/world.cpp) so both sides run it, and selecting a weapon writes
-- its shape into the replicated parameter table. A `swing` action in a sequence
-- would move the decision to the server and the collider would arrive one
-- round trip late -- for the single attack that currently appears instantly.
--
-- Types, matching mob_vocab.lua's so one checker serves both:
--
--   "int"      a Lua number, integral
--   "fx"       exact fixed-point: a string like "0.25", or an integer
--   "string"   text
--   "bool"     true/false
--   "enum"     one of `values`
--   "table"    a nested table this file describes elsewhere
--   "phase"    the name of a phase in the same weapon
--   "weapon"   the name of a weapon in the same file
--   "ammo"     the name of an ammunition kind in the same file
--   "missile"  the name of a missile, resolved through the mob runtime
--   "var"      the name of a per-weapon variable
--   "prop"     a record in assets/props.txt. NOT checked by the loader, which
--              never opens the catalogue. `weapon`, `ammo` and `phase` ARE
--              checked, precisely because they are declared in this same file
--              and a wrong name is knowable at load.
--   "actions"  a nested action list
--
-- `optional` args may be omitted; `one_of` groups are mutually exclusive and
-- exactly one member must be present.
--
weapon_vocab = {
  version = 1,

  -- --------------------------------------------------------------- weapon
  --
  -- What a weapon definition may carry. Anything else is a typo, and a silently
  -- ignored `dammage = 20` is a weapon that looks configured and does nothing.
  weapon_fields = {
    { name = "label", type = "string",
      doc = "What the weapon panel calls it. The only string a player sees." },
    { name = "order", type = "int",
      doc = "Selection priority, low to high. It is what the panel sorts by " ..
            "AND what the empty-weapon fallback picks the highest of, so " ..
            "running a shotgun dry drops to the pistol rather than the fists." },
    { name = "ammo", type = "ammo", optional = true,
      doc = "Which pool this eats. Absent means it never runs out, which is " ..
            "what a fist and a chainsaw are." },
    { name = "per", type = "int", optional = true,
      doc = "What one shot costs, debited by the `consume` action. Default 1. " ..
            "A charged shot that costs more says so on its own `consume`." },
    { name = "phases", type = "table",
      doc = "The graph, by phase name." },
    { name = "initial", type = "phase", optional = true,
      doc = "Which phase the weapon rests in. Defaults to the first phase in " ..
            "declaration order, and the runtime returns here whenever a phase " ..
            "runs out with no `next`." },
    { name = "swing", type = "table", optional = true,
      doc = "The swept melee collider, whose shape is written into the " ..
            "REPLICATED parameter table when this weapon is selected. Absent " ..
            "means this weapon does not swing -- and that is enforced through " ..
            "the parameter `attack_enabled`, so the client predicts the " ..
            "absence rather than predicting a swing the server then denies. " ..
            "See swing_fields." },
    { name = "icon", type = "string", optional = true,
      doc = "A picture for the HUD's current-weapon slot: a sprite frame key " ..
            "from the module's sheets, or a texture name. Published as " ..
            "`weapon_icon` (docs/UI_LAYOUT.md 4.3)." },
    { name = "predict", type = "table", optional = true,
      doc = "What the player's OWN client plays the moment it predicts a shot, " ..
            "so they hear it at once rather than a round trip later: a list of " ..
            "{on = primary | primary_held | secondary | secondary_held | swing, " ..
            "sound = name, every = ticks between two shots}. `_held` repeats " ..
            "while the button is down (a chaingun); `swing` is the swept " ..
            "collider starting, which the client predicts exactly. The " ..
            "client also counts the weapon's shots down. The phase's own " ..
            "`sound` for the same shot should be `predicted`, so that client " ..
            "drops the server's copy." },
    { name = "aim_snap", type = "int", optional = true,
      doc = "How many directions aim snaps to while this is held. 0 or absent " ..
            "is free aim; 8 is eight-way. A replicated parameter, so the snap " ..
            "is predicted -- which matters, because an aim the client and the " ..
            "server round differently is a mispredict on every tick the stick " ..
            "is near a boundary." },
    { name = "vars", type = "table", optional = true,
      doc = "Per-weapon counters, by name, with their starting values. Each " ..
            "player gets their own copy. A refire count, a combo step, a heat " ..
            "level." },
    { name = "gives", type = "table", optional = true,
      doc = "Ammunition its own pickup comes with, as {kind = n}. Doom's " ..
            "shotgun arrives with eight shells." },
  },

  -- ---------------------------------------------------------------- swing
  --
  -- Every field here IS a movement parameter (core/include/dnp/physics/
  -- params.hpp), written through the effect stack under one tag when the
  -- weapon is selected and cleared when it is put away.
  --
  -- ANGLES ARE TURN FRACTIONS, not degrees and not radians, because that is the
  -- engine's own unit: brad is a turn in 2^32, so a turn fraction and a brad
  -- are the same number and the conversion cannot pick up a factor of pi.
  -- 0.125 is an eighth of the way round, which reads as an eighth.
  swing_fields = {
    { name = "damage", type = "int",
      doc = "What a connected swing costs the thing it hit. SCRIPT, not a " ..
            "parameter: putting damage in the replicated table would hand " ..
            "the client the numbers to compute it with (docs/ENGINE.md)." },
    { name = "reach", type = "fx", optional = true,
      doc = "To the target's SURFACE, not its centre." },
    { name = "half_arc", type = "fx", optional = true,
      doc = "How wide, either side of the aim. 0.125 is 45 degrees each way." },
    { name = "sweep_from", type = "fx", optional = true,
      doc = "Where the collider starts, relative to the aim." },
    { name = "sweep_to", type = "fx", optional = true,
      doc = "And where it ends. Equal values make a THRUST that does not " ..
            "rotate at all; the default sweeps right to left through the " ..
            "front, which is what a swing looks like." },
    { name = "min_reach", type = "fx", optional = true,
      doc = "The INNER edge, as a distance from the body's surface. Zero is a " ..
            "wedge starting at the body -- a fist. Non-zero makes a ring " ..
            "segment that misses anything pressed right up against you, which " ..
            "is a spear: getting inside its reach is how you beat it." },
    { name = "ticks", type = "int", optional = true,
      doc = "How long the collider is live. This is why a swing is a VOLUME " ..
            "MOVING THROUGH TIME rather than an instant: a target that steps " ..
            "out of the way mid-swing is missed." },
    { name = "pitch_from", type = "fx", optional = true,
      doc = "Where the collider starts VERTICALLY, as a turn fraction: 0 is " ..
            "level, 0.25 is straight up, -0.25 straight down." },
    { name = "pitch_to", type = "fx", optional = true,
      doc = "And where it ends. An overhead chop runs from above to level; a " ..
            "spear thrust upward runs the other way. Equal values are a fixed " ..
            "elevation, which is what a horizontal slash at chest height is." },
    { name = "half_pitch", type = "fx", optional = true,
      doc = "How tall the arc is, either side of the pitch. ABSENT OR ZERO " ..
            "MEANS NO VERTICAL CHECK AT ALL -- the swing is bounded only by " ..
            "the body's height, which is how every swing behaved before pitch " ..
            "existed and is right for a fist. Set it and an overhead swing " ..
            "genuinely misses something at your feet." },
    { name = "knockback", type = "fx", optional = true,
      doc = "How hard a connected swing throws the target, in world units per " ..
            "second, directly away from the swinger." },
    { name = "lift", type = "fx", optional = true,
      doc = "The vertical part of that shove. An uppercut that pops a target " ..
            "into the air is this and nothing else." },
    { name = "slow", type = "fx", optional = true,
      doc = "Multiplier on the target's move_speed while it lasts. 0.5 is " ..
            "half pace; needs `slow_ticks` beside it or nothing happens." },
    { name = "slow_ticks", type = "int", optional = true,
      doc = "How long the slow lasts. Tagged by the attacker, so hitting the " ..
            "same target twice REPLACES rather than stacking to a standstill." },
    { name = "hit_vfx", type = "int", optional = true,
      doc = "A particle kind to burst where this lands, by catalogue id. " ..
        "Drawn entirely by the client -- nothing about it crosses the " ..
        "wire or enters a hash." },
    { name = "hit_vfx_radius", type = "fx", optional = true,
      doc = "How wide a volume the burst comes out of. A blast wants its " ..
        "own radius here so the particles fill it rather than all " ..
        "starting at one point." },
    { name = "cooldown", type = "int", optional = true,
      doc = "Ticks before another swing may START. Capped at 255 by the " ..
            "parameter's width." },
  },

  -- ---------------------------------------------------------------- phase
  phase_fields = {
    { name = "anim", type = "int", optional = true,
      doc = "The animation state the player is put into for this phase -- the " ..
            "`state <n>` on a sprite line in assets/props.txt, and the same " ..
            "number client/src/render/animstate.js resolves. The server " ..
            "declares it and the client owns the timing (design §4), so this " ..
            "is a number, not a frame list." },
    { name = "ticks", type = "int", optional = true,
      doc = "How long the phase lasts. ABSENT MEANS IT WAITS -- the phase " ..
            "stays until a transition fires, which is exactly what a charge " ..
            "is. Present, it also defines a WINDOW: a transition on this " ..
            "phase can only fire while it is still running." },
    { name = "next", type = "phase", optional = true,
      doc = "Where to go when `ticks` run out and no transition matched. " ..
            "Absent returns to the weapon's `initial`." },
    { name = "sequence", type = "actions", optional = true,
      doc = "What this phase does, as an ordered list of {at = <ticks after " ..
            "entry>, action = ...}. `at` is a DEADLINE from entry, not a " ..
            "delay from the previous entry: entries fire on the first tick " ..
            "the phase has been running at least that long, so a dropped " ..
            "tick delays an entry rather than losing it." },
    { name = "swing", type = "table", optional = true,
      doc = "A swing shape for THIS PHASE, replacing the weapon's while it is " ..
            "in it. Same fields as the weapon's `swing`.\n" ..
            "THIS IS WHAT MAKES A COMBO A COMBO. Three presses reaching " ..
            "`hit1`, `hit2`, `hit3` was already the shape the phase graph " ..
            "gave -- and all three swung the identical volume for the " ..
            "identical damage, because `swing` was a property of the weapon " ..
            "and there was one of it. A finisher that hits harder and sweeps " ..
            "wider is the whole reason anyone writes a combo.\n" ..
            "A phase without one leaves the weapon's in place." },
    { name = "transitions", type = "table", optional = true,
      doc = "A list of {when = <condition>, to = <phase>, ...args}, checked " ..
            "IN ORDER every tick, first match wins. Order is the whole " ..
            "mechanism for a charge: put `held_for 60 -> fire_big` above " ..
            "`button_released -> fire` and a long hold takes the first.\n" ..
            "`when` is a condition NAME, whose arguments sit on the " ..
            "transition itself, OR a list of {cond = <name>, ...args} ALL of " ..
            "which must hold -- the mob vocabulary's shape. The list is what " ..
            "lets a transition say 'the button went down AND there is no " ..
            "ammunition', which one condition cannot: `no_ammo` on its own " ..
            "fires while a weapon is merely being HELD, so an empty weapon " ..
            "would be taken out of the player's hands the instant they chose " ..
            "it, every tick, and could never be carried at all.\n" ..
            "Any condition takes `negate = true`, which inverts it." },
  },

  -- ----------------------------------------------------------- conditions
  --
  -- Read by `transitions`. Every one of them is answerable from what the
  -- server already knows about the player this tick; none of them casts a ray
  -- or walks the level, because this runs for every player every tick.
  --
  -- EVERY condition accepts `negate = true`, which inverts it, as in the mob
  -- vocabulary: "var is not 0" is `var_is` with `negate`. It is not listed in
  -- each condition's args for the same reason it is not there.
  conditions = {
    { name = "always", args = {},
      doc = "Fires as soon as it is reached. For the last entry in a list, " ..
            "and for a phase whose `ticks` are the only gate." },

    { name = "button_pressed",
      args = { { name = "button", type = "enum", optional = true,
                 values = { "primary", "secondary" } } },
      doc = "The button went down THIS TICK. Edge-triggered, so holding it " ..
            "is one shot and not sixty a second. Default `primary`." },

    { name = "button_released",
      args = { { name = "button", type = "enum", optional = true,
                 values = { "primary", "secondary" } } },
      doc = "The button came up this tick. What ends a charge." },

    { name = "button_held",
      args = { { name = "button", type = "enum", optional = true,
                 values = { "primary", "secondary" } } },
      doc = "The button is down, edge or not. THIS IS WHAT AUTOMATIC FIRE IS: " ..
            "a chaingun's `ready` transitions on held where a pistol's " ..
            "transitions on pressed, and there is no `auto` flag anywhere." },

    { name = "held_for",
      args = { { name = "ticks", type = "int" },
               { name = "button", type = "enum", optional = true,
                 values = { "primary", "secondary" } } },
      doc = "The button has been down for at least this many consecutive " ..
            "ticks. A charge threshold. Several of these in descending order " ..
            "are charge LEVELS, which is why this is a condition rather than " ..
            "a field on a phase." },

    { name = "has_ammo",
      args = { { name = "n", type = "int", optional = true } },
      doc = "The weapon's own pool holds at least `n`, default its `per`. A " ..
            "weapon with no `ammo` always has it." },

    { name = "no_ammo", args = {},
      doc = "The complement, stated rather than negated, so a phase graph " ..
            "reads as a graph. This is what routes an empty weapon to a dry " ..
            "click instead of a shot." },

    { name = "phase_elapsed",
      args = { { name = "ticks", type = "int" } },
      doc = "This phase has been running at least this long. For a transition " ..
            "part way through a phase that has other business afterwards." },

    { name = "chance",
      args = { { name = "percent", type = "int" } },
      doc = "0-100, from the seeded generator, so it is reproducible with the " ..
            "rest of the tick." },

    { name = "var_at_least",
      args = { { name = "var", type = "var" }, { name = "value", type = "int" } },
      doc = "A per-weapon counter reached a number. A combo step, an overheat." },

    { name = "var_is",
      args = { { name = "var", type = "var" }, { name = "value", type = "int" } },
      doc = "Exactly equal, for a counter used as a state rather than a level." },

    { name = "grounded",
      args = { { name = "value", type = "bool", optional = true } },
      doc = "Whether the body is standing on something. `value = false` is " ..
            "AIRBORNE, and that is what a down-slash is written with: put the " ..
            "airborne branch above the grounded one in `ready` and the same " ..
            "button does two different things. Default true." },

    { name = "moving",
      args = { { name = "speed", type = "fx", optional = true } },
      doc = "Moving horizontally faster than `speed` (default a half unit a " ..
            "second). A running attack that differs from a standing one." },

    { name = "falling",
      args = { { name = "speed", type = "fx", optional = true } },
      doc = "Descending faster than `speed`. A stomp is this plus a press, " ..
            "and it is deliberately not the same as `grounded = false`: the " ..
            "top of a jump is airborne and is not a dive." },

    { name = "lua",
      args = { { name = "fn", type = "string" } },
      doc = "The escape hatch: the name of a global function, called with " ..
            "(entity, weapon_name, phase_name), truthy to fire. Meant to be " ..
            "used -- a vocabulary that covers everything is a programming " ..
            "language -- but everything expressible above should be." },
  },

  -- -------------------------------------------------------------- actions
  --
  -- Placed in a phase's `sequence`. Several of these are implemented by calling
  -- straight into the mob runtime's own action table, which is public: one
  -- implementation, two vocabularies, and a monster's hitscan and a player's
  -- are then the same arithmetic rather than two that agree today.
  actions = {
    { name = "hitscan",
      args = {
        { name = "damage", type = "int" },
        { name = "range", type = "fx" },
        { name = "count", type = "int", optional = true,
          doc = "Rays in one shot. Default 1. A shotgun is 7." },
        { name = "spread", type = "int", optional = true,
          doc = "Degrees of random cone, rolled PER RAY. Without it all " ..
                "seven of a shotgun's pellets travel the identical line and " ..
                "a shotgun is a pistol that hits seven times as hard at any " ..
                "range whatever." },
        { name = "tracer", type = "int", optional = true,
          doc = "Tracer style to draw, or absent for none -- which is what " ..
                "makes a hitscan a punch. Doom's own fist is a line attack " ..
                "with a short range, so a melee weapon and a rifle share " ..
                "every line of the code that fires them." },
        { name = "knockback", type = "fx", optional = true,
          doc = "World units per second, away from the muzzle. PER RAY, so a " ..
                "seven-pellet shotgun at point blank shoves seven times -- " ..
                "which is what a shotgun at point blank should do." },
        { name = "lift", type = "fx", optional = true,
          doc = "The vertical part of that shove." },
        { name = "slow", type = "fx", optional = true,
          doc = "Multiplier on the target's move_speed. Needs `slow_ticks`." },
        { name = "hit_vfx", type = "int", optional = true,
          doc = "A particle kind to burst where this lands, by catalogue id. " ..
                "Drawn entirely by the client -- nothing about it crosses the " ..
                "wire or enters a hash." },
        { name = "hit_vfx_radius", type = "fx", optional = true,
          doc = "How wide a volume the burst comes out of. A blast wants its " ..
                "own radius here so the particles fill it rather than all " ..
                "starting at one point." },
        { name = "slow_ticks", type = "int", optional = true },
      },
      doc = "An instant shot. The engine answers where the ray ended and what " ..
            "was there; what that COSTS is this action." },

    { name = "launch",
      args = {
        { name = "missile", type = "missile" },
        { name = "count", type = "int", optional = true },
        { name = "arc", type = "int", optional = true,
          doc = "Degrees between shots when `count` > 1." },
        { name = "offset", type = "fx", optional = true,
          doc = "How far in front of the body it appears, so a rocket does " ..
                "not spawn inside its owner." },
        { name = "angle", type = "int", optional = true,
          doc = "Degrees off the aim, for a weapon that throws sideways." },
      },
      doc = "A projectile, through the mob runtime's own launcher. The owner " ..
            "is the PLAYER and the heading is their facing, which is why this " ..
            "goes through mobs.launch_from rather than the launcher a monster " ..
            "uses: a mob's `aim` is a steering field a player does not have." },

    { name = "damage_area",
      args = {
        { name = "damage", type = "int" },
        { name = "radius", type = "fx" },
        { name = "offset", type = "fx", optional = true,
          doc = "How far IN FRONT of the body the circle is centred. Absent " ..
                "or zero is centred on the player, which is a ground slam; a " ..
                "few units is a shockwave thrown forward. This is the " ..
                "\"chosen point\", and it is relative because a weapon does " ..
                "not know where it will be fired from." },
        { name = "height", type = "fx", optional = true,
          doc = "And how far above the feet, for a blast that goes off at " ..
                "chest height rather than on the floor." },
        { name = "falloff", type = "bool", optional = true,
          doc = "Damage linear in distance rather than flat to the edge. " ..
                "Linear in distance and not in its square: full strength " ..
                "until the last inch and then nothing reads as a bug." },
        { name = "hurts_owner", type = "bool", optional = true,
          doc = "Default true. False is what stops a rocket jump killing you." },
        { name = "knockback", type = "fx", optional = true,
          doc = "Outward from the CENTRE, not from the player -- so a blast " ..
                "thrown forward scatters things away from where it landed." },
        { name = "lift", type = "fx", optional = true },
        { name = "slow", type = "fx", optional = true },
        { name = "hit_vfx", type = "int", optional = true,
          doc = "A particle kind to burst where this lands, by catalogue id. " ..
                "Drawn entirely by the client -- nothing about it crosses the " ..
                "wire or enters a hash." },
        { name = "hit_vfx_radius", type = "fx", optional = true,
          doc = "How wide a volume the burst comes out of. A blast wants its " ..
                "own radius here so the particles fill it rather than all " ..
                "starting at one point." },
        { name = "slow_ticks", type = "int", optional = true },
      },
      doc = "A circle of damage around a chosen point. Everything in range is " ..
            "hit once, walls are NOT consulted -- this is a shockwave, not a " ..
            "ray -- and it is the mob runtime's own blast, the one rockets " ..
            "have used since the cyberdemon." },

    { name = "telegraph",
      args = {
        { name = "effect", type = "int",
          doc = "Which warning to draw, by catalogue id. What it LOOKS like is " ..
                "the catalogue's business, exactly as a tracer style is." },
        { name = "radius", type = "fx", optional = true,
          doc = "How big the warned area is." },
        { name = "ticks", type = "int", optional = true,
          doc = "How long until the blow actually lands. The number a player " ..
                "needs to decide whether to move." },
        { name = "offset", type = "fx", optional = true,
          doc = "How far in front of the body it is centred, matching " ..
                "`damage_area`'s so a warning and its blast agree." },
      },
      doc = "ANNOUNCE AN INCOMING ATTACK, so a client can draw the warning " ..
            "before it lands. Put one at `at = 0` of a wind-up phase and the " ..
            "blow itself later in the same phase; the gap between them IS the " ..
            "time the player has.\n" ..
            "It carries no wire format of its own: it rides the vfx event, " ..
            "with the radius in 1/256 units in the low sixteen bits of `param` " ..
            "and the ticks remaining in the high sixteen." },

    { name = "consume",
      args = { { name = "n", type = "int", optional = true },
               { name = "ammo", type = "ammo", optional = true } },
      doc = "Debit the pool. Default is the weapon's own `per` from its own " ..
            "`ammo`. AN ACTION RATHER THAN AUTOMATIC, because a charged shot " ..
            "costs more than a tapped one and a phase that fires twice pays " ..
            "twice -- neither of which a field on the weapon can say." },

    { name = "impulse",
      args = { { name = "speed", type = "fx" },
               { name = "towards", type = "enum", optional = true,
                 values = { "facing", "away", "random" } },
               { name = "vertical", type = "fx", optional = true },
               { name = "jitter", type = "int", optional = true,
                 doc = "Degrees of random slop on the direction." } },
      doc = "A shove, in world units per second. RECOIL is `towards = \"away\"`; " ..
            "with a `vertical` it is a rocket jump. NOT PREDICTED -- the " ..
            "client learns about it from the next snapshot and reconciles, " ..
            "which is already classified as SNAP_LUA_APPLIED rather than as a " ..
            "fault. Making it predictable would mean the engine learning what " ..
            "a weapon is." },

    { name = "set_var",
      args = { { name = "var", type = "var" }, { name = "value", type = "int" } } },

    { name = "add_var",
      args = { { name = "var", type = "var" }, { name = "value", type = "int" } },
      doc = "For a combo step or a refire count." },

    { name = "goto",
      args = { { name = "phase", type = "phase" } },
      doc = "Enter another phase immediately. The REST OF THE SEQUENCE IS " ..
            "ABANDONED, which is the point: it is how a shot that finds no " ..
            "ammunition stops before it draws a tracer." },

    { name = "set_anim",
      args = { { name = "state", type = "int" } },
      doc = "Change the animation state part way through a phase, for a phase " ..
            "whose `anim` covers only its beginning." },

    { name = "switch_if_empty", args = {},
      doc = "If the weapon can no longer fire, select the best one that can. " ..
            "Doom does this and it is not a nicety: a player who empties a " ..
            "shotgun while something is chasing them is otherwise holding an " ..
            "inert object, pressing fire and getting nothing, with fifty " ..
            "bullets in their pocket." },

    { name = "vfx",
      args = { { name = "effect", type = "int" },
               { name = "length", type = "int", optional = true },
               { name = "attach", type = "bool", optional = true,
                 doc = "Send the player's ENTITY INDEX instead of the length, " ..
                       "which an `attach` particle kind reads as \"follow " ..
                       "this body\". That is what makes a continuous spray " ..
                       "stay on the weapon: without it each burst spawns where " ..
                       "the event said and a moving player runs out from under " ..
                       "it, leaving the sparks behind in a line.\n" ..
                       "There is no stop event and none is needed -- a phase " ..
                       "that repeats pushes the emitter's deadline out, and " ..
                       "letting go of the trigger lets it lapse." } },
      doc = "A visual effect at the player, by catalogue id. A muzzle flash, " ..
            "or with `attach` a spray that follows them." },

    { name = "sound",
      args = { { name = "name", type = "string",
                 doc = "The sound's name in the module's bank " ..
                       "(`weapon.shotgun.fire`)." },
               { name = "predicted", type = "bool", optional = true,
                 doc = "The player's own client has already played this -- it " ..
                       "predicts its own shots from the weapon's `predict` " ..
                       "entry -- so that client drops this copy instead of " ..
                       "hearing the shot twice, a round trip apart. Everybody " ..
                       "else hears it." } },
      doc = "Play a sound, from the player and moving with them. Never the " ..
            "only sign of anything: a sound may be dropped." },

    { name = "lua",
      args = { { name = "fn", type = "string" } },
      doc = "The escape hatch, called with (entity, weapon_name, phase_name)." },
  },

  -- ----------------------------------------------------------------- ammo
  --
  -- AUTHORED, not compiled in. Doom's four kinds were named independently in
  -- three places in the runtime -- the cap table, the UI widget ids, and the
  -- starting inventory -- so adding a fifth meant editing code in three, and
  -- forgetting one of them was a pool that could be picked up and never shown.
  ammo_fields = {
    { name = "label", type = "string", optional = true,
      doc = "What the panel calls this pool. Defaults to its own name." },
    { name = "max", type = "int",
      doc = "The most a player may hold." },
    { name = "initial", type = "int", optional = true,
      doc = "What they start with. Default 0." },
    { name = "order", type = "int", optional = true,
      doc = "Where it sits in the panel. The read-out list is GENERATED from " ..
            "this, which is the thing that stops a new pool being invisible." },
  },

  -- -------------------------------------------------------------- pickups
  --
  -- Keyed by catalogue NAME, so renumbering assets/props.txt cannot make a
  -- medikit grant cells. The same rule doom.lua's key ring follows.
  --
  -- A REFUSED PICKUP STAYS ON THE FLOOR. Walking over a medikit at full health
  -- must not consume it, or a player who is topped up destroys the health they
  -- will need in a minute.
  pickup_fields = {
    { name = "weapon", type = "weapon", optional = true,
      doc = "Grants this weapon, once, and selects it. Its ammunition is " ..
            "granted EVERY time, which is why a second shotgun on a map is " ..
            "still worth walking over." },
    { name = "ammo", type = "table", optional = true,
      doc = "Grants ammunition, as {kind = n}, capped at each pool's `max`." },
    { name = "heal", type = "int", optional = true,
      doc = "Adds hit points, up to `ceiling`." },
    { name = "ceiling", type = "enum", optional = true,
      values = { "max", "bonus_max" },
      doc = "Which health ceiling this pickup may fill to. A health bonus " ..
            "trickles past the normal one, which is why this is a named " ..
            "ceiling rather than a comparison against the pickup's own name " ..
            "buried in the runtime." },
    { name = "armour", type = "int", optional = true,
      doc = "Sets armour to this if it is higher. Doom's green and blue." },
    { name = "sound", type = "string", optional = true,
      doc = "What taking it sounds like. Absent takes the module's default " ..
            "for what the pickup gives (`weapons.pickup_sounds`). A refused " ..
            "pickup makes no sound, because nothing happened." },
    { name = "not_yet", type = "string", optional = true,
      doc = "This pickup is declared and does nothing, and says so when " ..
            "walked over, naming itself. A pickup that silently does nothing " ..
            "is indistinguishable from a bug -- which is the whole reason " ..
            "this field exists rather than the entry simply being absent." },
  },

  -- --------------------------------------------------------------- health
  health_fields = {
    { name = "max", type = "int",
      doc = "The normal ceiling. 100 in Doom." },
    { name = "bonus_max", type = "int", optional = true,
      doc = "The ceiling a `bonus_max` pickup may trickle up to. 200 in Doom." },
  },
}
