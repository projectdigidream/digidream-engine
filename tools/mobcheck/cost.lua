-- What a level of monsters costs per tick.
--
-- MAP01 reported 20 ms in on_tick against a 16.6 ms frame, with ~170 monsters
-- awake. The cause was a second pass over EVERY mob every tick to steer it,
-- added along with pathfinding: two pos() calls, a CORDIC atan2, two sector()
-- lookups and a dir() each. Steering now happens on the mob's own think tick,
-- where the rest of its decisions already were.
--
-- WHAT THIS MEASURES, and what it does not. It counts calls into the engine,
-- not microseconds: each one crosses the Lua/C boundary and several do real
-- work (a raycast, a sector lookup, a CORDIC). Timing this harness would
-- measure the harness. The absolute frame cost still has to come from a real
-- server -- said here so a green run is not mistaken for one.
--
-- The claim is a SHAPE: a mob costs work on its OWN think tick and nothing on
-- the other seven. A regression that reintroduces a per-tick pass over every
-- mob fails this whether or not it is slower on the machine running it.
--
-- The numbers below were measured, not chosen. At the time of writing a
-- chasing monster costs 1.03 engine calls a tick and a dormant one 0.12 --
-- which is exactly the bucket's own valid() check, one tick in eight.

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

-- Calls per tick with N monsters chasing a player, averaged over a run long
-- enough to cover every think bucket several times.
local function cost(n, dormant)
  W.reset(); mobs.reset()
  for i = 1, n do
    mobs.spawn("zombieman",
               { x = W.mkfx(i % 20), y = W.mkfx(math.floor(i / 20)), z = W.mkfx(0) },
               dormant and { dormant = true } or nil)
  end
  W.add("player", 5, 5, 0)
  W.see = true
  -- Warm up: let them acquire targets and settle into their states.
  for _ = 1, mobs.THINK * 4 do W.tick_n = W.tick_n + 1; W.advance(); on_tick() end
  local before = W.calls
  local ticks = mobs.THINK * 10
  for _ = 1, ticks do W.tick_n = W.tick_n + 1; W.advance(); on_tick() end
  return (W.calls - before) / ticks
end

print("mobs: the per-tick cost of a level of monsters")

local c20 = cost(20, false)
local c160 = cost(160, false)
local per_mob = (c160 - c20) / 140
print(("  chasing:  20 -> %.0f calls/tick, 160 -> %.0f; marginal %.2f per monster")
      :format(c20, c160, per_mob))

-- A chasing monster acts on one tick in THINK. Its whole decision -- two
-- position lookups, a line of sight, a sector, an input -- amortises to about
-- one call a tick. Measured at 1.03; the ceiling here is loose enough not to
-- fail on a fair change and tight enough to catch a per-tick pass, which took
-- this to 8 and MAP01 to 20 ms a frame.
CHECK(per_mob < 2.0,
      ("a chasing monster costs about one call a tick (%.2f)"):format(per_mob))

-- THE CONTROL, and it is the one that matters: a DORMANT monster costs
-- essentially nothing. If anything iterated every mob every tick, dormant ones
-- would pay for it too, and this is what would catch it.
local d20 = cost(20, true)
local d160 = cost(160, true)
local per_dormant = (d160 - d20) / 140
print(("  dormant:  20 -> %.0f calls/tick, 160 -> %.0f; marginal %.2f per monster")
      :format(d20, d160, per_dormant))
CHECK(per_dormant < 0.3,
      ("a dormant monster costs almost nothing (%.2f)"):format(per_dormant))

-- And the two must actually DIFFER, or both numbers could be small because the
-- harness counts nothing.
CHECK(per_mob > per_dormant * 3,
      ("a chasing monster costs more than a dormant one (%.2f vs %.2f)")
      :format(per_mob, per_dormant))

print(("== %d checks, %d failures =="):format(checks, fails))
os.exit(fails == 0 and 0 or 1)
