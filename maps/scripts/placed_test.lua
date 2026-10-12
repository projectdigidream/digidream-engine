--
-- A fixture for server/edge/test/persist.test.js: PLACED PROPS (docs/SAVING.md,
-- "Placed props").
--
-- Written the way the studio writes a level's placement: three props put down
-- as the script loads, with no idea of saving or restoring -- no dnp.persist,
-- no dnp.restoring(). What a restored level gets back is then the engine's
-- doing alone: the props still there, where they were left, and not the one
-- taken.
--
-- A second later the first is taken and the second is moved; the third stays.
--

local mine = {}
for i = 1, 3 do
  mine[i] = dnp.spawn_prop("key_red", vec3.new(fx.int(10 + 4 * i), fx.int(26), fx.int(0)))
end
local function count()
  local n = 0
  for i = 1, 3 do if mine[i] ~= nil and mine[i]:valid() then n = n + 1 end end
  return n
end
local function where()
  local out = {}
  for i = 1, 3 do
    local e = mine[i]
    out[#out + 1] = (e ~= nil and e:valid()) and tostring(e:pos().x):gsub("%.0+$", "") or "-"
  end
  return table.concat(out, ",")
end
print(("[placed_test] placed %d at %s"):format(count(), where()))

-- Only on a fresh placement: a restored level has already had this done.
local due = count() == 3 and dnp.tick() + 60 or nil
function on_tick()
  if due == nil or dnp.tick() < due then return end
  due = nil
  mine[1]:despawn()
  local p = mine[2]:pos()
  mine[2]:set_pos(vec3.new(fx.int(30), p.y, p.z))
  print(("[placed_test] changed: %d left at %s"):format(count(), where()))
end
