--
-- A fixture for server/edge/test/sound.test.js (docs/protocol.md §5.7).
--
-- Every half second, three sounds: one near where the player stands, one far
-- outside earshot, and one from the player themself marked as predicted -- the
-- shape of their own jump or shot. The test reads what arrives.
--
local next_at = dnp.tick() + 30
function on_tick()
  local t = dnp.tick()
  if t < next_at then return end
  next_at = t + 30
  for _, e in ipairs(dnp.players()) do
    local p = e:pos()
    dnp.sound("test.near", vec3.new(p.x + fx.int(2), p.y, p.z))
    dnp.sound("test.far", vec3.new(p.x + fx.int(500), p.y, p.z))
    dnp.sound("test.self", e, { predicted = true })
  end
end
