-- The map's own script: sector motion, driven entirely from Lua.
--
-- Until Phase 4 this lived in server/src/main.cpp as a block of `if (tag >= 400)
-- ... else if (tag >= 300)` branches, marked "Lua takes this over in Phase 4;
-- the tag convention is the bridge". This is that takeover. The convention is
-- unchanged, so every existing map behaves exactly as before -- what changed is
-- who decides.
--
-- The tags, and what each costs (design §3.1.2, docs/protocol.md §8.2):
--
--   100+N  a lift that rises N units, travelling as a volume.  PARAMETRIC:
--          a pure function of the tick, replicated once, predicted free.
--   200+N  a crusher whose CEILING descends N units. Only a sector that moves
--          one surface can close the gap a body occupies (§3.1).
--   300+N  a footprint that slides N units south, sweeping anything in its way.
--   400+N  a drifter with N units of range. SCRIPTED-DISCRETE: no closed form,
--          so its offset is streamed every snapshot and its riders fall back to
--          anchor-relative prediction. Costs bandwidth AND prediction, which is
--          exactly why the two kinds are distinguished.
--
-- A map author reaching for 400 where 100 would do pays for both, and now that
-- choice is visible in the map file rather than buried in the engine.

local movers = {}      -- sector -> { range, now, target }

for _, s in ipairs(dnp.sectors()) do
  local tag = s:tag()
  if tag and tag >= 100 then
    if tag >= 400 then
      local range = tag - 400
      -- No path is declared, because there is none to declare. Registering it
      -- as discrete is what makes the server stream its offset.
      s:set_offset(vec3.new(0, 0, 0))
      movers[s:index()] = { sector = s, range = range, now = fx.int(0), target = fx.int(0) }
      print(("sector %d drifts (scripted-discrete, range %d)"):format(s:index(), range))

    elseif tag >= 300 then
      local slide = tag - 300
      s:move_parametric(vec3.new(0, 0, 0), vec3.new(0, -slide, 0), 240, "pingpong")
      print(("sector %d sweeps (slides %d south)"):format(s:index(), slide))

    elseif tag >= 200 then
      local drop = tag - 200
      s:move_parametric(vec3.new(0, 0, 0), vec3.new(0, 0, -drop), 240, "pingpong", "ceiling")
      print(("sector %d crushes (ceiling drops %d)"):format(s:index(), drop))

    else
      local rise = tag - 100
      s:move_parametric(vec3.new(0, 0, 0), vec3.new(0, 0, rise), 240, "pingpong")
      print(("sector %d moves (rise %d)"):format(s:index(), rise))
    end
  end
end

-- The discrete movers, one arbitrary step per tick.
--
-- Deliberately NOT a function of the tick counter. A schedule a client could
-- re-derive would make this a parametric mover wearing a disguise, and the
-- whole reason the protocol streams SectorOffset every snapshot is that no such
-- derivation exists. dnp.random is seeded from the map, so this is
-- unpredictable to a CLIENT while still being replayable by the server.
local STEP = fx.frac(1, 100)

function on_tick()
  for _, m in pairs(movers) do
    if dnp.random(0, 63) == 0 then
      m.target = fx.frac(dnp.random(0, 100), 100) * fx.int(m.range)
    end
    -- Crawl toward the target rather than jumping: a rider is carried rigidly,
    -- so a mover that teleported would teleport its passengers. That should be
    -- a choice a map author makes on purpose, not one inherited from the
    -- scheduler.
    if m.now < m.target then
      m.now = m.now + STEP
      if m.now > m.target then m.now = m.target end
    elseif m.now > m.target then
      m.now = m.now - STEP
      if m.now < m.target then m.now = m.target end
    end
    m.sector:set_offset(vec3.new(0, 0, m.now))
  end
end
