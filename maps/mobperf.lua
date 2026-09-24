--
-- The mob scaling fixture: N monsters, all awake, all looking at you.
--
-- WHAT THIS IS FOR. "Some ticks are taking way too long" was reported against a
-- number measured on an Android emulator, while the native server answered
-- `0 stalls` on the same level. There was no way to ask where a tick goes, or
-- how that changes with the number of monsters, so this is the thing that
-- asks. Drive it with tools/mobperf/run.js, which rewrites the N below and
-- reads the server's `[perf]` lines back.
--
-- WHY A GENERATED LANDMASS and not one big room. A single convex sector would
-- make three of the costs under measurement disappear: the nav graph would have
-- one node, `steer_target` would never path, and `physics::resolve_slabs` --
-- which scans EVERY sector for EVERY agent EVERY tick -- would scan one. The
-- fixture has 144 sectors so those costs are present and proportional, which is
-- the whole point of measuring rather than estimating.
--
-- WHY THEY ARE PACKED AROUND THE SPAWN. The question is what 1000 monsters cost
-- *while they see the player*: acquiring, pathing, facing, shooting. A monster
-- asleep in another room costs almost nothing -- mobs.lua's own bucket test
-- measures that at 0.12 calls a tick -- and a fixture full of them would report
-- a number nobody asked about.
--
-- NOT A LEVEL. Nothing here is playable and nothing should copy it.
--

-- >>> mobperf N (rewritten by tools/mobperf/run.js)
local N = 200
local DORMANT = false
-- <<< mobperf N

-- DORMANT IS THE CONTROL, and it is what makes the rest of the numbers mean
-- something.
--
-- A dormant monster does not think, does not path and does not shoot -- but it
-- is still an agent, so it still goes through `World::step()` and still takes
-- part in `resolve_agent_collisions`, which is a flat double loop over every
-- agent with no broadphase. So a dormant sweep measures the PHYSICS cost of N
-- bodies with the decision cost removed, and the difference between the two
-- sweeps is what the mob runtime actually costs.
--
-- It also holds the population still. An awake fixture kills itself: monsters
-- infight, the player shoots back, and a run that started with 350 is measuring
-- 238 by the end -- so a per-agent figure taken from it is dividing a cost by a
-- number that was never true for the whole window.

-- The spawn the driver connects to, from maps/mobperf.map.
local CX, CY, CZ = 98.689, 118.944, 3

-- A phyllotaxis spiral rather than a grid.
--
-- A grid puts every monster on the same two axes as its neighbours, so they
-- resolve against each other in lockstep and the collision cost is unnaturally
-- regular. The spiral spreads them evenly with no preferred direction, which is
-- closer to a room that has filled up, and it stays even at any N -- so the
-- N=50 and N=1000 runs differ in count and not in shape.
local GOLDEN = 2.399963
local SPACING = 0.9

local kinds = { "zombieman", "imp" }

local spawned, refused = 0, 0
for i = 1, N do
  local r = SPACING * math.sqrt(i)
  local a = i * GOLDEN
  local x = CX + r * math.cos(a)
  local y = CY + r * math.sin(a)
  -- Well above the floor: the mover settles a body down onto whatever is under
  -- it, and a spawn below the floor is refused outright. The landmass varies by
  -- several units across its sheet, so a fixed z from the spawn point alone
  -- would silently lose the monsters on the high ground.
  local m = mobs.spawn(kinds[(i % #kinds) + 1],
                       vec3.new(fx.approx(x), fx.approx(y), fx.approx(CZ + 6)),
                       DORMANT and { dormant = true } or nil)
  if m ~= nil then spawned = spawned + 1 else refused = refused + 1 end
end

-- PRINTED, because the driver asserts on it. A run that meant to measure 1000
-- monsters and measured 340 -- because the rest landed outside a sector and
-- were refused -- would otherwise report a wonderfully low cost per monster.
print(("[mobperf] asked for %d, spawned %d, refused %d, dormant %s")
      :format(N, spawned, refused, tostring(DORMANT)))
