--
-- A fixture for server/edge/test/fps.test.js: says, once a second, what the
-- server sees of each player's view -- the scheme, the pitch it read off their
-- input, and the slope a shot would be fired at.
--
local next_at = dnp.tick() + 60
function on_tick()
  local t = dnp.tick()
  if t < next_at then return end
  next_at = t + 60
  for _, e in ipairs(dnp.players()) do
    local p = e:pitch()
    print(("[fps_test] controls %s pitch %d/65536"):format(dnp.controls(), (p * fx.int(65536)).int))
  end
end
