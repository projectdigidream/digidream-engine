-- Slice 5's demonstration: an effect over the movement-parameter table.
--
-- Design §7.2 calls the parameter table the only scriptable surface of the
-- physics core. Everything else Lua touches is state; this is the RULES the
-- next thousand ticks are computed from -- which is why a change here rides the
-- reliable channel and carries an epoch (docs/protocol.md §8.4).
--
-- The effect is applied and removed repeatedly on purpose. The engine keeps the
-- base untouched and recomputes from it, so the cycle is exactly the identity;
-- an implementation that scaled the live value would walk move_speed downward
-- all match (server/src/lua/effects.hpp).

local PERIOD = 120
local held = {}

function on_tick()
  local t = dnp.tick()
  if t % PERIOD ~= 0 or t == 0 then return end

  for _, e in ipairs(dnp.entities()) do
    local i = e:index()
    if held[i] then
      e:remove_effect(held[i])
      held[i] = nil
      print(("tick %d: removed haste from %s, speed back to %s")
            :format(t, tostring(e), tostring(e:param("move_speed"))))
    else
      -- x1/7 rather than a round factor: a factor that divides exactly would
      -- not exercise the rounding the base/recompute design exists to survive.
      held[i] = e:add_effect("move_speed", "mul", fx.frac(1, 7))
      print(("tick %d: slowed %s from %s"):format(t, tostring(e),
            tostring(e:base_param("move_speed"))))
    end
  end
end
