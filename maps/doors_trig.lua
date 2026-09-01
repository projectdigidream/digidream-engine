-- maps/doors_trig.map's behaviour, written as triggers.
--
-- The same map as maps/doors.map, whose behaviour is a doom.lua line table.
-- Both are under test, on identical geometry, so the trigger vocabulary is
-- measured against something that already works rather than against itself.
--
-- What the two share, and where they differ, is worth stating because the
-- differences are the vocabulary's argument:
--
--   doors_lines.lua                    this file
--   ---------------------------------  --------------------------------------
--   a line table keyed by side index   triggers keyed by nothing; each NAMES
--                                      the side it watches, in a condition
--   `mode="openclose"` -- one word     two triggers and a condition on the
--   naming a behaviour doom.lua        door's own offset, which is the same
--   implements                         truth the client sees
--   `wait=90` measured inside the      run_after ... after = the door sector,
--   action's own callback chain        which is a tick deadline and says so
--   a converter emits it               a person or an editor writes it
--
-- The side indices are the same in both, because they are the same map:
--
--   side 2       the door between room A and room B1, worked by pressing it
--   sides 11,21  the trip-wire at x = 17, crossed either way
--   side 18      the exit switch
--
-- The speeds match doors_lines.lua's so the two maps can be compared tick for
-- tick, and they are faster than Doom's so a test does not spend six seconds
-- watching a door.

-- Two monsters, north of the spawn. DORMANT, for the reason doors_lines.lua
-- gives: a live monster chases, so it eventually arrives wherever the test
-- walks, and "placed where the door test does not go" stops being true the
-- moment either of them takes a step.
enemies.spawn("zombieman", 288, 384, 48, { dormant = true })
enemies.spawn("imp",       144, 384, 48, { dormant = true })

-- Doom's map unit is 1/48 of ours. Props are placed here rather than by the
-- trigger table because placing a prop at load is not an event -- there is no
-- hook for "the map started", and inventing one would be an engine change.
dnp.spawn_prop("key_blue", vec3.new(fx.frac(384, 48), fx.frac(240, 48), fx.frac(48, 48)))
dnp.spawn_prop("barrel",   vec3.new(fx.frac(240, 48), fx.frac(384, 48), fx.frac(48, 48)))

triggers.load{
  name = "doors_trig",

  triggers = {
    -- ------------------------------------------------------------ the door
    --
    -- A Doom manual door: repeatable, and the sector it moves is the one
    -- BEHIND the side that was pressed. `behind` is why the side index appears
    -- once, in the condition, rather than twice.
    --
    -- Open and close are two triggers rather than one "openclose" mode,
    -- because there is no mode: what decides is the door's own vertical
    -- offset, which is the value a client also has. A flag remembered here
    -- could disagree with the geometry; this cannot.
    { name = "open the door",
      event = "on_interact",
      conditions = {
        -- The neighbour that side had when this was written. Not optional, and
        -- not ceremony: one added vertex renumbers every side after it, and
        -- these four values are exactly doors_lines.lua's own check_neighbor
        -- numbers -- two independently written tables agreeing about the same
        -- geometry.
        { c = "side_is", side = 2, expect_neighbour = 1 },
        -- Pressing a door that is still travelling must not restart it
        -- half-way.
        { c = "sector_at_rest", sector = "behind" },
        -- Below half open, so this is the opening half of the toggle. The door
        -- opens to 17/12 and half of that is 17/24.
        { c = "sector_offset_at_least", sector = "behind", z = {17, 24},
          negate = true },
      },
      actions = {
        -- Doom's open height: four map units below the lowest neighbouring
        -- ceiling, which is why a door in a low corridor does not punch
        -- through the ceiling above it. 4/48 has no finite decimal, hence the
        -- fraction.
        { a = "sector_move", sector = "behind", surface = "ceiling",
          to = "lowest_neighbour_ceiling", offset = {-4, 48}, speed = {1, 4} },
        -- Ninety ticks after it has FINISHED opening, not ninety after the
        -- button.
        { a = "run_after", trigger = "shut the door", after = "behind", ticks = 90 },
      } },

    { name = "close the door",
      event = "on_interact",
      conditions = {
        { c = "side_is", side = 2, expect_neighbour = 1 },
        { c = "sector_at_rest", sector = "behind" },
        { c = "sector_offset_at_least", sector = "behind", z = {17, 24} },
      },
      actions = {
        { a = "sector_move", sector = "behind", surface = "ceiling",
          to = "base_ceiling", speed = {1, 4} },
      } },

    -- The deferred half. No `event`: it runs only because something ran it.
    --
    -- The rest condition is what makes a re-press cancel the automatic close:
    -- if the door was worked again in the meantime it is moving, and this
    -- declines rather than fighting it.
    { name = "shut the door",
      conditions = { { c = "sector_at_rest", sector = 1, expect_tag = 2 } },
      actions = {
        { a = "sector_move", sector = 1, expect_tag = 2, surface = "ceiling",
          to = "base_ceiling", speed = {1, 4} },
      } },

    -- ------------------------------------------------------------ the lift
    --
    -- One trip-wire, two side indices. A crossing is reported as the side of
    -- the sector the entity LEFT, so eastbound and westbound are different
    -- sides of the same line and both have to carry the action. Conditions are
    -- ANDed, so this is two triggers and not one with two `side_is`.
    { name = "the trip-wire, eastbound",
      event = "on_cross",
      conditions = { { c = "side_is", side = 11, expect_neighbour = 3 },
                     { c = "sector_at_rest", sector = 4, expect_tag = 5 } },
      actions = { { a = "run", trigger = "lower the lift" } } },

    { name = "the trip-wire, westbound",
      event = "on_cross",
      conditions = { { c = "side_is", side = 21, expect_neighbour = 2 },
                     { c = "sector_at_rest", sector = 4, expect_tag = 5 } },
      actions = { { a = "run", trigger = "lower the lift" } } },

    -- Named once and run from both, which is the whole reason `run` exists:
    -- two trip-wires that each carried a copy of the lift would be two places
    -- to change its speed.
    { name = "lower the lift",
      actions = {
        { a = "sector_move", sector = 4, expect_tag = 5, surface = "floor",
          to = "lowest_neighbour_floor", speed = {1, 8} },
        { a = "run_after", trigger = "raise the lift", after = 4, expect_tag = 5,
          ticks = 60 },
      } },

    { name = "raise the lift",
      actions = {
        { a = "sector_move", sector = 4, expect_tag = 5, surface = "floor",
          to = "base_floor", speed = {1, 8} },
      } },

    -- ----------------------------------------------------------- the exit
    { name = "the exit switch",
      event = "on_interact",
      once = true,
      conditions = { { c = "side_is", side = 18, expect_neighbour = 4 } },
      actions = {
        { a = "log", text = "exit reached" },
        { a = "change_map" },
      } },

    -- ------------------------------------------------------- props, briefly
    --
    -- Not required by the door test, and here because they are what an author
    -- reaches for next: a thing you pick up and a thing you hit. Both are
    -- three lines of table where doom.lua needs a function each.
    { name = "pick up the blue key",
      event = "on_touch",
      conditions = { { c = "target_name_is", name = "key_blue" } },
      actions = {
        -- The engine reports the overlap every tick it holds; taking the prop
        -- away is what makes this happen once.
        { a = "log", text = "picked up the blue key" },
        { a = "despawn", who = "target" },
        -- A state with a name rather than a number nobody can read back.
        { a = "set_property", name = "blue_key", value = true },
        -- And the escape hatch, in the form a hand-written map should use: a
        -- function, which needs no compiler at run time and so works under
        -- every trust profile. `code` is what the editor will emit, because a
        -- string is the only form it can keep in a sidecar and write back.
        --
        -- Nothing here needs to be custom code -- it is two counters and a
        -- log. It is written this way because an escape hatch nobody exercises
        -- is an escape hatch that has already rotted, and this is the only
        -- place `lua` runs against the real bindings rather than against test
        -- doubles.
        { a = "lua", fn = function (ev)
            local n = (triggers.property("keys") or 0) + 1
            triggers.set_property("keys", n)
            print(("[triggers] doors_trig: custom code counted key %d for entity %s")
                  :format(n, tostring(ev.entity:index())))
          end },
      } },

    { name = "set off a barrel",
      event = "on_attack",
      conditions = { { c = "target_name_is", name = "barrel" } },
      actions = {
        { a = "set_animation", state = 1, who = "target" },
        -- It stops being an obstacle the moment it is destroyed rather than
        -- when it disappears: what you can still see is a fireball, and
        -- walking through a fireball is more correct than bumping into one.
        { a = "set_solid", solid = false, who = "target" },
        { a = "run_after", trigger = "the barrel is gone", ticks = 30 },
      } },

    -- The target travels with the deadline as a generational handle, so a
    -- barrel that went some other way in the meantime is a defined no-op here
    -- rather than a despawn aimed at whatever now occupies the slot.
    { name = "the barrel is gone",
      actions = { { a = "despawn", who = "target" } } },
  },
}
