-- Slice 4's demonstration: a script that moves a player.
--
-- This is the first thing in the engine a client CANNOT predict. The client
-- never runs server Lua by design, so when this fires the client's prediction
-- for that tick is wrong for a reason no amount of local determinism could have
-- anticipated -- which is exactly what LUA_APPLIED exists to say.
--
-- The shape matters as much as the fact: a scripted change mispredicts on
-- EXACTLY ONE tick and then self-heals, because reconciliation adopts the
-- authoritative state and re-derives everything after it. A determinism
-- regression mispredicts forever. The e2e test asserts the former.

local INTERVAL = 120          -- every 2 seconds
local anchor   = vec3.new(10, 10, 1)

function on_tick()
  local t = dnp.tick()
  if t % INTERVAL ~= 0 or t == 0 then return end

  for _, e in ipairs(dnp.entities()) do
    -- Reads are immediate, writes are deferred. `pos()` here is the live
    -- world; the teleport lands at the next flush point.
    local p = e:pos()
    if p then
      e:teleport(anchor)
      print(("tick %d: teleported %s from %s"):format(t, tostring(e), tostring(p)))
    end
  end
end
