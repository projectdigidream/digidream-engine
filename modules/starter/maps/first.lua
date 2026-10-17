-- first.lua -- written by the studio from first.studio.json.
-- Edit the level there; this file is rewritten on every bake.

-- >>> level editor: entities
-- Placed props and monsters. Positions are eighths of a unit, which is the
-- studio's grid, so fx.frac(n, 8) is exact.
if dnp ~= nil and fx ~= nil and vec3 ~= nil then
  local function du(n) return fx.frac(n, 8) end
  -- props
  for _, p in ipairs({
  }) do
    local o = dnp.spawn_prop(p.name, vec3.new(du(p.x), du(p.y), du(p.z)))
    if o ~= nil and p.a ~= nil and o.set_facing ~= nil then o:set_facing(p.a) end
  end
  -- end props
  -- monsters
  local M = {
    {name="spirit", x=144, y=0, z=0},
  }
  -- end monsters
  if #M > 0 and mobs ~= nil then
    local placed, unknown = 0, {}
    for _, e in ipairs(M) do
      local m = mobs.spawn(e.name, vec3.new(du(e.x), du(e.y), du(e.z)), { dormant = e.dormant == true, facing = e.a })
      if m ~= nil then placed = placed + 1 else unknown[#unknown + 1] = e.name end
    end
    print(("[studio] %d monster(s) placed%s"):format(placed,
      #unknown > 0 and (", " .. #unknown .. " the mob runtime does not define: " .. table.concat(unknown, ", ")) or ""))
  end
end
-- <<< level editor: entities

-- >>> level editor: triggers
triggers.load{
  name = "first",
  triggers = {
    {
      name = "door: shut",
      conditions = {
        { c = "sector_at_rest", sector = 1, expect_tag = 0 },
      },
      actions = {
        {
          a = "sector_move",
          sector = 1,
          expect_tag = 0,
          surface = "ceiling",
          to = "base_ceiling",
          speed = { 1, 4 },
        },
      },
    },
    {
      name = "door: open from s19",
      event = "on_interact",
      conditions = {
        { c = "side_is", side = 18, expect_neighbour = 1 },
        { c = "sector_at_rest", sector = "behind" },
        { c = "sector_offset_at_least", sector = "behind", z = "0.25", negate = true },
      },
      actions = {
        {
          a = "sector_move",
          sector = "behind",
          surface = "ceiling",
          to = "lowest_neighbour_ceiling",
          offset = { -1, 8 },
          speed = { 1, 4 },
        },
        { a = "run_after", trigger = "door: shut", after = "behind", ticks = 240 },
      },
    },
    {
      name = "door: open from s1",
      event = "on_interact",
      conditions = {
        { c = "side_is", side = 2, expect_neighbour = 1 },
        { c = "sector_at_rest", sector = "behind" },
        { c = "sector_offset_at_least", sector = "behind", z = "0.25", negate = true },
      },
      actions = {
        {
          a = "sector_move",
          sector = "behind",
          surface = "ceiling",
          to = "lowest_neighbour_ceiling",
          offset = { -1, 8 },
          speed = { 1, 4 },
        },
        { a = "run_after", trigger = "door: shut", after = "behind", ticks = 240 },
      },
    },
    {
      name = "raise the pit",
      event = "on_interact",
      conditions = {
        { c = "side_is", side = 15, expect_neighbour = -1 },
        { c = "sector_at_rest", sector = 3, expect_tag = 0 },
      },
      actions = {
        {
          a = "sector_move",
          sector = 3,
          expect_tag = 0,
          surface = "floor",
          to = "highest_neighbour_floor",
          speed = { 1, 8 },
        },
      },
    },
    {
      name = "exit",
      event = "on_interact",
      once = true,
      conditions = {
        { c = "side_is", side = 16, expect_neighbour = -1 },
      },
      actions = {
        { a = "log", text = "exit reached" },
        { a = "change_map" },
      },
    },
  },
}
-- <<< level editor: triggers
