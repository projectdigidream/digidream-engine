-- Every monster in maps/mobs/doom_monsters.lua spawns, thinks and dies.
--
-- The loader proves a table is well formed. That is not the same as "all
-- eighteen of these work": a mob whose `prop` names nothing, whose sequence
-- reaches an action that raises, or whose graph has no path out of `look`
-- would load clean and be broken in play.
--
-- So each one is spawned, given a target, run, killed, and run again. It is a
-- smoke test and it says so -- it asserts that nothing raises and that the
-- graph moves, not that a mancubus feels like Doom's.

local W = dofile("tools/mobcheck/world.lua")
dofile("maps/scripts/mob_vocab.lua")
dofile("maps/scripts/mobs.lua")
local mobs = _G.mobs

local checks, fails = 0, 0
local function CHECK(cond, what)
  checks = checks + 1
  if not cond then fails = fails + 1; print("  FAIL " .. what) end
end

local ok, problems = true, {}
local real = mobs.load
mobs.load = function(d) ok, problems = real(d); return ok, problems end
dofile("maps/mobs/doom_monsters.lua")
mobs.load = real
if not ok then
  for _, p in ipairs(problems) do print("  " .. p) end
  os.exit(1)
end

-- Names in declaration-independent order, so the report reads the same twice.
local names = {}
for name in pairs(mobs.defs) do names[#names + 1] = name end
table.sort(names)

print(("mobs: the roster -- %d kinds"):format(#names))

for _, name in ipairs(names) do
  W.reset(); mobs.reset()
  local def = mobs.defs[name]

  local e = mobs.spawn(name, { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  if e == nil then
    CHECK(false, name .. ": did not spawn")
  else
    local m = mobs.live[e:index()]
    CHECK(m.state == def.initial, name .. ": starts in its `initial` state")

    -- A player in front of it, visible and in range of everything.
    local p = W.add("player", 6, 0, 0)
    W.see = true

    -- Run it. Any action that raises takes the whole harness down, which is
    -- the point: an unprotected error here is a monster that would kill the
    -- server's tick.
    local states = {}
    for _ = 1, 400 do
      W.tick_n = W.tick_n + 1
      on_tick()
      if mobs.live[e:index()] then states[mobs.live[e:index()].state] = true end
    end

    local n = 0
    for _ in pairs(states) do n = n + 1 end
    -- It has to LEAVE its initial state. A monster that never reacts to a
    -- player standing in front of it is the failure this catches.
    CHECK(n > 1, name .. ": reacts to a player (visited " .. n .. " states)")

    -- And it can die.
    mobs.hurt(e, 99999, nil)
    local after = mobs.live[e:index()]
    CHECK(after ~= nil and after.dead, name .. ": dies when killed")
    for _ = 1, 120 do W.tick_n = W.tick_n + 1; W.advance(); on_tick() end
  end
end

print(("== %d checks, %d failures =="):format(checks, fails))
os.exit(fails == 0 and 0 or 1)
