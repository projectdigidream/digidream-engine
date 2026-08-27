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

-- The nearest player, or nil. Players are the agents a session is sitting at,
-- which is exactly the ones dnp.entities() returns that are not ours.
local function nearest_player(from)
  local best, bestd = nil, nil
  for _, e in ipairs(dnp.entities()) do
    if e:name() == nil then                 -- no catalogue kind: a player
      local p = e:pos()
      if p then
        local dx, dy = p.x - from.x, p.y - from.y
        local d = dx * dx + dy * dy
        if bestd == nil or d < bestd then best, bestd = e, d end
      end
    end
  end
  return best, bestd
end

-- Aim, as the engine's own angle unit. A turn is 2^32 and the engine turns at a
-- rate, so writing the aim is asking to face something rather than facing it.
local function aim_at(from, to)
  return dnp.atan2(to.y - from.y, to.x - from.x)
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
  -- The tracer is drawn whether or not anything was hit: a shot that goes wide
  -- is a shot you should be able to see going wide.
  if at then dnp.vfx(m.def.tracer, at, aim, 0) end
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


  for idx, m in pairs(live) do
    if not m.e:valid() then
      live[idx] = nil
    else
      local from = m.e:pos()
      local player, d2 = nearest_player(from)

      if player == nil then
        m.e:set_animation(IDLE)
        m.e:set_input(ZERO, ZERO, m.e:facing() or 0)
      else
        local to = player:pos()
        local aim = aim_at(from, to)
        local sight = m.def.sight

        if m.windup > 0 then
          -- Mid-attack: standing still, animation playing, and the shot lands
          -- at the END of it. A monster that fired on the frame it decided to
          -- is a monster you cannot react to.
          m.windup = m.windup - 1
          m.e:set_input(ZERO, ZERO, aim)
          if m.windup == 0 then
            if m.def.attack == "hitscan" then fire_hitscan(m, from, aim)
            else fire_projectile(m, from, aim) end
            m.cooldown = m.def.cooldown
            m.e:set_animation(WALK)
          end
        elseif d2 < sight * sight and m.cooldown == 0 then
          m.windup = m.def.windup
          m.e:set_animation(ATTACK)
          m.e:set_input(ZERO, ZERO, aim)
        else
          if m.cooldown > 0 then m.cooldown = m.cooldown - 1 end
          if d2 < sight * sight then
            -- Walk toward the player. move_y is "forward" in the engine's own
            -- frame, so a monster that is facing you walks at you -- and since
            -- turning is a rate, it leans into corners rather than sliding
            -- round them sideways.
            m.e:set_animation(WALK)
            m.e:set_input(ZERO, fx.int(1), aim)
          else
            m.e:set_animation(IDLE)
            m.e:set_input(ZERO, ZERO, aim)
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
      s.p:set_pos(vec3.new(p.x + s.dir.x * step, p.y + s.dir.y * step, p.z))
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
