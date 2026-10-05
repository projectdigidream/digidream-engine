--
-- A fixture for server/edge/test/sector.test.js: a sector's looks changed by a
-- script (SECTOR_SET). At load, sector 2's floor and fog in sector 0; four
-- seconds after the first player arrives, sector 0 dims with a 2 s fade. Said on the server's output.
--
dnp.set_sector_texture(2, "floor", "LAVA1")
dnp.set_sector_effect(0, "fog", { density = 0.5, color = "#203040" })
print("[sector_test] floor and fog set")

local dim_at = nil
local dimmed = false
function on_tick()
  if not dimmed and dim_at == nil and #(dnp.players() or {}) > 0 then dim_at = dnp.tick() + 240 end
  if not dimmed and dim_at and dnp.tick() >= dim_at then
    dimmed = true
    dim_at = nil
    dnp.set_sector_light(0, 30, 2)
    print("[sector_test] dimming")
  end
end
