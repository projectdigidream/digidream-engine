--
-- A fixture for server/edge/test/game.test.js (docs/MODULES.md, "Game
-- scripts"): a GAME script, run once for the whole game.
--
-- Per player it counts the ticks it has seen them for and opens one panel --
-- state that must go with them from level to level, which is the whole reason
-- game scripts exist. The game's own state is how many arrivals it has seen,
-- saved through dnp.persist. It says what it is doing on the server's output,
-- which is what the test reads.
--
-- Loaded as an ordinary --script instead it is the CONTROL: every level then
-- has its own copy, which counts from nothing and opens its own panel.
--

local seen = {}       -- player -> { ticks, panel }
local visits = 0      -- arrivals, over the whole game

local function who(e) return e:player() or e:index() end

function on_player_enter(e)
  visits = visits + 1
  print(("[game_test] player %s entered level %d (visit %d)")
        :format(tostring(who(e)), dnp.level(), visits))
end

function on_player_leave(p)
  seen[p] = nil
  print(("[game_test] player %s left the game"):format(tostring(p)))
end

function on_tick()
  for _, e in ipairs(dnp.players()) do
    local p = who(e)
    local s = seen[p]
    if s == nil then s = { ticks = 0 }; seen[p] = s end
    s.ticks = s.ticks + 1
    if s.panel == nil then
      -- A TEMPLATE name (docs/UI_LAYOUT.md 5) rides along to the client; one
      -- the wire could not carry is refused here, in the script, by name.
      local ok, err = pcall(dnp.ui_open, e, { title = "x", template = "Not-A-Name" })
      print(("[game_test] a bad template refused: %s (%s)"):format(tostring(not ok), tostring(err)))
      s.panel = dnp.ui_open(e, { title = "Game", template = "game_hud",
        widgets = { { kind = "label", id = 0, label = "follows you" } } })
      print(("[game_test] opened panel %d for player %s"):format(s.panel, tostring(p)))
    end
  end
end

-- What a level may ask.
dnp.export("ticks_of", function(e)
  local s = seen[who(e)]
  return s and s.ticks or 0, dnp.is_game()
end)

dnp.persist("game_test", function()
  print(("[game_test] saving %d visits"):format(visits))
  return { visits = visits }
end, function(t)
  visits = t.visits or 0
  print(("[game_test] restored %d visits"):format(visits))
end)

dnp.persist_player("game_test", function(e)
  local s = seen[who(e)]
  return s and { ticks = s.ticks } or nil
end, function(e, t)
  if t == nil then return end
  local s = seen[who(e)] or {}
  s.ticks = t.ticks
  seen[who(e)] = s
  print(("[game_test] restored player %s at %d ticks"):format(tostring(who(e)), t.ticks))
end)
