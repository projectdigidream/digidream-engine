--
-- A fixture for server/edge/test/game.test.js: a LEVEL script, beside the game
-- script maps/scripts/game_test.lua.
--
-- It opens a panel of its own -- a map's panel, which must close when the
-- player leaves the level -- asks the game every second how long it has seen
-- the player (dnp.call_game), and after four seconds in the first level sends
-- them on to the second when that is level_a (in the test's region the player
-- walks instead).
--

local opened = {}
local arrived = nil
local sent = false

function on_tick()
  local t = dnp.tick()
  for _, e in ipairs(dnp.players()) do
    local i = e:index()
    if opened[i] == nil then
      opened[i] = dnp.ui_open(e, { title = "Level " .. dnp.level(),
        widgets = { { kind = "label", id = 0, label = "this map only" } } })
      print(("[game_level] level %d opened panel %d"):format(dnp.level(), opened[i]))
    end
    if t % 60 == 0 then
      local n, game = dnp.call_game("ticks_of", e)
      print(("[game_level] level %d asked the game: %s ticks (from the game VM: %s)")
            :format(dnp.level(), tostring(n), tostring(game)))
    end
    arrived = arrived or t
    if dnp.level() == 0 and dnp.levels()[1] == "level_a" and not sent and t - arrived >= 240 then
      sent = true
      e:change_map(1)
    end
  end
end
