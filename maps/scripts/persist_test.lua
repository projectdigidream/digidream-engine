--
-- A fixture for server/edge/test/persist.test.js (docs/SAVING.md).
--
-- Four keys on the floor, and every two seconds one goes, as if somebody were
-- picking them up. Which are left is this script's state, saved through
-- dnp.persist and put back when the level is restored -- the same shape a real
-- library's state takes (doom.lua's props, mobs.lua's monsters).
--

local keys = {}   -- the props this placed, by number

local function place(n, at)
  local e = dnp.spawn_prop("key_blue", at)
  if e then keys[n] = e end
end

-- Placed only when the level is NOT being restored: a restored level gets back
-- the keys that were still there, from the load hook below.
local function left()
  local c = 0
  for n = 1, 4 do if keys[n] and keys[n]:valid() then c = c + 1 end end
  return c
end

if not dnp.restoring() then
  for n = 1, 4 do place(n, vec3.new(fx.int(6 + 6 * n), fx.int(20), fx.int(0))) end
  print(("[persist_test] placed %d keys"):format(left()))
  -- And a sky (docs/SKY.md), only here: a restored level must get it back from
  -- its saved state, since this line does not run again.
  dnp.sky("strip SKY2 repeat 2")
end

local next_at = dnp.tick() + 120
function on_tick()
  local t = dnp.tick()
  if t < next_at then return end
  next_at = t + 120
  for n = 1, 4 do
    local e = keys[n]
    if e and e:valid() then
      e:despawn()
      keys[n] = nil
      print(("[persist_test] keys left: %d"):format(left()))
      return
    end
  end
end

dnp.persist("persist_test", function()
  print(("[persist_test] saving %d keys"):format(left()))
  local out = {}
  for n = 1, 4 do
    local e = keys[n]
    if e and e:valid() then out[#out + 1] = { n = n, at = e:pos() } end
  end
  return out
end, function(list)
  for _, s in ipairs(list) do place(s.n, s.at) end
  print(("[persist_test] restored %d keys"):format(left()))
end)
