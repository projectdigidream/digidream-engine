-- >>> level editor: entities
--
-- Generated. Everything between these two markers is rewritten by the level
-- editor on every save; edit it there, not here. Everything OUTSIDE them is
-- left exactly as it was.
--
-- Positions are Doom units, 48 to the engine unit -- the same units
-- every converted level is written in.
do
  local S = 48
  local function du(n) return fx.frac(n, S) end
  -- props
  for _, p in ipairs({
  }) do dnp.spawn_prop(p.name, vec3.new(du(p.x), du(p.y), du(p.z))) end
  -- end props
  -- monsters
  local M = {
    {name="imp", x=-499, y=28, z=240},
    {name="imp", x=-858, y=412, z=0},
    {name="imp", x=-454, y=-53, z=144},
  }
  -- end monsters
  if #M > 0 then
    local placed, unknown = 0, {}
    for _, e in ipairs(M) do
      local m = nil
      if mobs ~= nil then
        m = mobs.spawn(e.name, vec3.new(du(e.x), du(e.y), du(e.z)))
      end
      if m ~= nil then placed = placed + 1
      else unknown[#unknown + 1] = e.name end
    end
    print(("[editor] %d monster(s) placed%s"):format(placed,
      #unknown > 0 and (", " .. #unknown .. " the mob runtime does not define: " .. table.concat(unknown, ", ")) or ""))
  end
end
-- <<< level editor: entities
