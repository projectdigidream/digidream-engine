-- Level A's line actions, in the format tools/wad/convert.js emits.
--
-- One exit switch. doom.lua's on_exit calls entity:change_map(), which moves
-- THAT session to the next level in the server's progression.
doom.load{
  name = "level_a",
  lines = {
    [1] = {a="exit", trig="use", once=true, secret=false, targets={}, check_neighbor=-1},
  },
  teleports = {},
}
