-- >>> level editor: entities
--
-- Generated. Everything between these two markers is rewritten by the level
-- editor on every save; edit it there, not here. Everything OUTSIDE them is
-- left exactly as it was.
--
-- Positions are Doom units, 48 to the engine unit -- the same units
-- maps/scripts/enemies.lua and every converted level are written in.
do
  local S = 48
  local function du(n) return fx.frac(n, S) end
  -- props
  for _, p in ipairs({
  }) do dnp.spawn_prop(p.name, vec3.new(du(p.x), du(p.y), du(p.z))) end
  -- end props
  -- monsters
  local M = {
  }
  -- end monsters
  if enemies ~= nil and #M > 0 then enemies.place(M) end
end
-- <<< level editor: entities
