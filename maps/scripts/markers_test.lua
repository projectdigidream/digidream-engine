-- server/edge/test/markers.test.js's script (docs/MARKERS.md).
--
-- When the first player arrives: a sign at a place, a health bar on that
-- player, and a pointer only they see. When a second arrives: the bar changes
-- and the sign comes down -- told to both.
local placed, changed = false, false
local sign, bar
function on_tick()
  local ps = dnp.players()
  if not placed and #ps >= 1 then
    placed = true
    sign = dnp.mark(vec3.new(fx.int(1), fx.int(2), fx.int(0)), 'label', { text = 'Hello' })
    bar = dnp.mark(ps[1], 'health', { value = 3, max = 4 }, { height = fx.frac(1, 2) })
    dnp.mark(vec3.new(fx.int(0), fx.int(0), fx.int(1)), 'marker', { text = 'yours' }, { to = ps[1] })
  end
  if placed and not changed and #ps >= 2 then
    changed = true
    dnp.mark_set(bar, { value = 1 })
    dnp.unmark(sign)
  end
end
