-- Line actions for maps/doors.map, in the format tools/wad/convert.js emits.
--
-- Hand-written rather than converted, and deliberately in the generated format:
-- what is under test is the path a converted map takes, so the data has to look
-- like converted data. The speeds are faster than Doom's so a test does not
-- spend six seconds watching a door.
--
--   side 2       the door between room A and room B1, worked by pressing it
--   sides 11,21  the trip-wire at x = 17, crossed either way
--   side 18      the exit switch

doom.load{
  name = "doors",
  lines = {
    -- A manual door: repeatable, and its target is the door sector itself.
    [2] = {a="door", trig="use", once=false, mode="openclose",
           speed_num=1, speed_den=4, wait=90, targets={1}, check_neighbor=1},

    -- One trip-wire, two side indices. A crossing is reported as the side of
    -- the sector the entity LEFT, so eastbound and westbound are different
    -- sides of the same line and both must carry the action.
    [11] = {a="lift", trig="cross", once=false,
            speed_num=1, speed_den=8, wait=60, targets={4}, pair=21, check_neighbor=3},
    [21] = {a="lift", trig="cross", once=false,
            speed_num=1, speed_den=8, wait=60, targets={4}, pair=11, check_neighbor=2},

    [18] = {a="exit", trig="use", once=true, secret=false, targets={}, check_neighbor=4},
  },
  teleports = {},

  -- A key on the path east, three units from the spawn. The point is not the
  -- key: it is that a prop declared in a text file, spawned by a script and
  -- replicated as an ordinary entity arrives at a client that has never been
  -- compiled with any knowledge of what a key is.
  props = {
    {name="key_blue", x=384, y=240, z=48},
  },
}
