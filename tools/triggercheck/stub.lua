-- The smallest world a trigger table can be validated against.
--
-- tools/triggercheck answers one question: is this TABLE well formed -- do its
-- actions exist, do its arguments type-check, do its references resolve, does
-- its custom code compile? That is the question an editor wants answered while
-- somebody is typing, and answering it needs no map.
--
-- It deliberately does NOT answer whether the table fits a particular map.
-- expect_neighbour and expect_tag are checked by the server at load, against
-- the geometry that actually arrived, and there is no way to do that here
-- without a baked map -- so the runtime says so on its own line rather than
-- letting this look like a clean bill of health.
--
-- Everything below is a stub, and nothing in it runs: triggercheck loads the
-- table and exits. No trigger ever fires, so no action is ever performed.
dnp = {
  tick = function() return 0 end,
  sectors = function() return {} end,
  sector = function() return nil end,
  side = function() return nil end,
  prop = function() return nil end,
  props = function() return {} end,
  entities = function() return {} end,
  players = function() return {} end,
  player_count = function() return 0 end,
  spawn_prop = function() return nil end,
  spawn_enemy = function() return nil end,
  ui_open = function() return 0 end,
  ui_close = function() end,
  ui_set = function() end,
  camera = function() end,
  vfx = function() return false end,
  random = function(lo) return lo end,
}

-- A map's trigger file may place monsters and props at its top level, the way
-- maps/doors_trig.lua does. That is not a trigger and this tool has no opinion
-- about it, but the file has to RUN to reach triggers.load -- so the names it
-- calls have to exist.
enemies = enemies or { spawn = function() return nil end,
                       place = function() return 0 end }
vec3 = vec3
