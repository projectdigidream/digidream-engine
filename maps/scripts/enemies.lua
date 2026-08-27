--
-- Doom's monsters, as script.
--
-- The engine knows nothing about any of this. It moves agents through the
-- controller a player uses, answers "what is along this ray", and draws what
-- assets/props.txt describes; whether a thing chases you, when it shoots, and
-- what it looks like while doing so is all here. That division is the same one
-- doors and switches follow, and for the same reason: a client predicts the
-- engine and never runs a line of this file.
--
-- NO DAMAGE YET, deliberately. They shoot and they bite and nothing is hurt --
-- health, armour and dying are a slice of their own, and building them into the
-- first version of the AI would mean unpicking them from it later.
--
-- Two kinds of attack, because they need different machinery:
--
--   HITSCAN (zombieman)  the shot arrives instantly. dnp.hitscan answers what
--                        it hit and where it stopped, and a tracer is drawn
--                        from muzzle to impact -- which is why the impact point
--                        matters and not just the target.
--
--   PROJECTILE (imp)     a fireball is a PROP that travels. It needs a position
--                        every tick and something to stop it, so it is moved
--                        here and asked about the world each tick.
--

enemies = enemies or {}
local M = enemies

local SCALE = 48
local function du(n) return fx.frac(n, SCALE) end

local ZERO = fx.int(0)

-- Animation states, matching assets/props.txt. Named so a change to the
-- catalogue's numbering is one edit here rather than a hunt for magic numbers.
local IDLE, WALK, ATTACK, PAIN, DIE = 0, 1, 2, 3, 4

-- What each kind does. Everything about a monster's behaviour is one entry.
local KINDS = {
  zombieman = {
    speed = fx.frac(1, 1),      -- world units per second
    sight = du(1024),
    attack = "hitscan",
    range = du(2048),
    cooldown = 90,              -- ticks between shots
    windup = 8,                 -- ticks of attack animation before it fires
    tracer = 1,
  },
  imp = {
    speed = fx.frac(1, 1),
    sight = du(1024),
    attack = "projectile",
    melee = du(64),
    range = du(2048),
    cooldown = 100,
    windup = 10,
    projectile = "fireball",
    speed_shot = du(10 * 35),   -- Doom's fireball: 10 map units per tic
  },
}

-- Live monsters: entity index -> { e, def, cooldown, windup, target }
local live = {}
-- Fireballs in flight: prop index -> { p, dir, speed, born }
local shots = {}

-- The players, rebuilt ONCE per tick and shared by every monster.
--
-- This used to be a dnp.entities() call inside each monster's own update, which
-- allocated a table of handles per monster per tick: twenty-nine monsters on
-- MAP01, thirty entities each, sixty times a second, is fifty thousand userdata
-- allocations a second. It cost sixteen milliseconds of a sixteen-millisecond
-- frame with nobody even connected, and the collector chasing it is what took
-- the server down.
--
-- The list is small and the monsters all want the same one. Building it once is
-- not an optimisation, it is the difference between a level that runs and a
-- level that does not.
local players = {}
local players_tick = -1

local function refresh_players(tick)
  if players_tick == tick then return end
  players_tick = tick
  for i = #players, 1, -1 do players[i] = nil end
  for _, e in ipairs(dnp.entities()) do
    -- No catalogue kind means nobody put it there: a player.
    -- Kind 0 is a player. Asked directly rather than via the catalogue name,
    -- which was a longer way round to the same fact and depended on the
    -- catalogue having no entry for 0.
    if e:kind() == 0 then players[#players + 1] = { e = e, pos = e:pos() } end
  end
end

local function nearest_player(from)
  local best, bestd = nil, nil
  for i = 1, #players do
    local p = players[i].pos
    if p then
      local dx, dy = p.x - from.x, p.y - from.y
      local d = dx * dx + dy * dy
      if bestd == nil or d < bestd then best, bestd = players[i].e, d end
    end
  end
  return best, bestd
end

-- How often a monster reconsiders, in ticks.
--
-- Doom's own monsters think far more slowly than its frame rate, and so should
-- these: an input written with set_input PERSISTS until it is changed, so a
-- monster keeps walking between decisions without anything being called for it.
-- Staggered by index so twenty-nine of them do not all think on the same tick
-- and turn a smooth frame into a spike every fourth one.
local THINK = 4

-- Aim, as the engine's own angle unit. A turn is 2^32 and the engine turns at a
-- rate, so writing the aim is asking to face something rather than facing it.
local function aim_at(from, to)
  return dnp.atan2(to.y - from.y, to.x - from.x)
end

-- Place a list of monsters, as the converter emits them.
--
-- Capped at what the world has room for. MAX_AGENTS is 64 and includes players,
-- so a Doom level with ninety monsters in it does not fit -- and dropping the
-- overflow silently would make a level quietly easier than the one it was
-- converted from.
function M.place(list)
  local placed, dropped = 0, 0
  for _, e in ipairs(list) do
    if M.spawn(e.name, e.x, e.y, e.z) then placed = placed + 1 else dropped = dropped + 1 end
  end
  print(("[enemies] %d placed%s"):format(
    placed, dropped > 0 and (", " .. dropped .. " did not fit (MAX_AGENTS)") or ""))
end

function M.spawn(name, x, y, z)
  local def = KINDS[name]
  if def == nil then
    print("[enemies] no behaviour for '" .. tostring(name) .. "'")
    return nil
  end
  local e = dnp.spawn_enemy(name, vec3.new(du(x), du(y), du(z)))
  if e == nil then return nil end
  live[e:index()] = { e = e, def = def, cooldown = 0, windup = 0 }
  e:set_animation(IDLE)
  return e
end

local function fire_hitscan(m, from, aim)
  local target, at = dnp.hitscan(m.e, m.def.range)
  if at == nil then return nil end
  -- The tracer carries its START, its direction and its LENGTH -- not just the
  -- impact. A client given only the far end has no idea where the shot came
  -- from, and a client given only a direction draws a line to infinity through
  -- whatever it hit.
  --
  -- Length in 1/256 units, which is a hundredth of a Doom unit at this scale
  -- and comfortably inside the u32 the event carries.
  local muzzle = vec3.new(from.x, from.y, from.z + du(32))
  local dx, dy = at.x - muzzle.x, at.y - muzzle.y
  local len = dnp.length(vec3.new(dx, dy, ZERO))
  dnp.vfx(m.def.tracer, muzzle, aim, (len * fx.int(256)).int)
  return target
end

local function fire_projectile(m, from, aim)
  local dir = dnp.dir(aim)
  -- Launched clear of the shooter's own body, or it would be touching its owner
  -- on the tick it appears.
  local off = du(40)
  local p = dnp.spawn_prop(m.def.projectile,
    vec3.new(from.x + dir.x * off, from.y + dir.y * off, from.z + du(32)))
  if p == nil then return end
  shots[p:index()] = { p = p, dir = dir, speed = m.def.speed_shot, born = dnp.tick() }
end

-- No chaining needed, and none possible.
--
-- The engine collects each script's hooks after it loads and calls every one of
-- them in load order, so two scripts defining on_tick both run. This used to be
-- a plain global, where the second definition silently replaced the first --
-- and a map loading both this and doom.lua lost every door the moment it gained
-- a monster.
function on_tick()
  local tick = dnp.tick()


  refresh_players(tick)

  for idx, m in pairs(live) do
    if not m.e:valid() then
      live[idx] = nil
    elseif m.windup > 0 then
      -- Mid-attack: every tick, because the shot lands at the END of the windup
      -- and a monster that fired four ticks late would fire at where you were.
      m.windup = m.windup - 1
      if m.windup == 0 then
        local from = m.e:pos()
        local aim = m.aim or 0
        if m.def.attack == "hitscan" then fire_hitscan(m, from, aim)
        else fire_projectile(m, from, aim) end
        m.cooldown = m.def.cooldown
        m.e:set_animation(WALK)
      end
    else
      if m.cooldown > 0 then m.cooldown = m.cooldown - 1 end

      -- Staggered thinking. Between decisions the monster keeps doing whatever
      -- it was told, because an input persists.
      if (tick + idx) % THINK == 0 then
        local from = m.e:pos()
        local player, d2 = nearest_player(from)
        local sight = m.def.sight

        if player == nil or d2 >= sight * sight then
          m.e:set_animation(IDLE)
          m.e:set_input(ZERO, ZERO, m.e:facing() or 0)
        else
          local aim = aim_at(from, player:pos())
          m.aim = aim
          -- CAN IT SEE YOU. Distance alone put monsters through walls, shooting
          -- at people two rooms away -- "is it close" is not "is it visible",
          -- and the engine already knows the difference because the mover does.
          if not dnp.can_see(m.e, player) then
            m.e:set_animation(IDLE)
            m.e:set_input(ZERO, ZERO, aim)
          elseif m.cooldown == 0 then
            m.windup = m.def.windup
            m.e:set_animation(ATTACK)
            m.e:set_input(ZERO, ZERO, aim)
          else
            -- A WORLD direction, not "forward".
            --
            -- move_x/move_y are world axes: the client computes a
            -- screen-relative direction and sends the world vector, so the
            -- engine never has to know where a camera is. Sending (0, 1) meant
            -- "walk north" -- which every monster did, in unison, regardless of
            -- where the player was.
            local d = dnp.dir(aim)
            m.e:set_animation(WALK)
            m.e:set_input(d.x, d.y, aim)
          end
        end
      end
    end
  end

  -- Fireballs whose burst has finished playing.
  if M.expire then
    for idx, x in pairs(M.expire) do
      if tick >= x.at then
        if x.p:valid() then x.p:despawn() end
        M.expire[idx] = nil
      end
    end
  end

  -- Fireballs. Moved here rather than by the engine because a projectile is a
  -- gameplay object: what it does when it arrives is a decision, and the engine
  -- has no opinion about any of it.
  for idx, s in pairs(shots) do
    if not s.p:valid() then
      shots[idx] = nil
    elseif tick - s.born > 300 then
      s.p:despawn()
      shots[idx] = nil
    else
      local p = s.p:pos()
      local step = s.speed / fx.int(60)
      -- Does it reach? A projectile that ignored geometry sailed through every
      -- wall in the map, which is not a fireball, it is a rumour.
      local blocked, at = dnp.sweep(p, s.dir, step)
      if blocked then
        s.p:set_pos(at)
        s.p:set_animation(1)
        shots[idx] = nil
        M.expire = M.expire or {}
        M.expire[idx] = { p = s.p, at = tick + 20 }
      else
        s.p:set_pos(vec3.new(p.x + s.dir.x * step, p.y + s.dir.y * step, p.z))
      end
    end
  end
end

-- A fireball that touches something goes off. Reported by the engine's overlap
-- test, which is the same one a key uses -- a projectile is a pickup that hurts.
function on_touch(entity, prop)
  local s = shots[prop:index()]
  if s == nil then return end
  shots[prop:index()] = nil
  prop:set_animation(1)
  -- Left visible long enough for the burst to play, then removed. The client
  -- decides how fast it plays; this only says when it stops existing.
  M.expire = M.expire or {}
  M.expire[prop:index()] = { p = prop, at = dnp.tick() + 20 }
end
