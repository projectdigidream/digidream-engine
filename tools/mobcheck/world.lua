-- A fake world, enough to RUN mobs rather than only load them.
--
-- tools/mobcheck/stub.lua answers "is this table well formed". This answers
-- "does the graph behave", which is a different question and the one that
-- catches a runtime whose transitions never fire.
--
-- HONESTLY, AND THIS MATTERS: `fx` here is a plain Lua double, not the
-- engine's Q32.32. Doubles do not truncate where fx truncates, so this harness
-- can agree with the engine about a state machine and disagree about the
-- fourth decimal of a distance. That is fine for what it tests -- which state
-- a mob is in, and when -- and it is NOT a determinism test. The determinism
-- gate is scripts/check_determinism.sh and it does not run Lua at all.
--
-- Everything here is a stand-in a test drives directly: entities have a
-- position you set, `can_see` is a flag you set, and `random` is a sequence
-- you supply, so a test that depends on a roll says which roll it depends on.

-- ONLY CALLS FROM OUTSIDE THIS FILE COUNT. The fake's own hitscan walks every
-- entity to find the nearest, which the engine does in one call -- so counting
-- it made 160 monsters look like 670 position lookups a tick and sent me
-- optimising a loop that does not exist in the engine. A cost harness that
-- counts its own internals measures itself.
local MY_SRC = debug.getinfo(1, "S").short_src
local function from_outside()
  local i = debug.getinfo(3, "S")
  return i == nil or i.short_src ~= MY_SRC
end

local W = {}

W.calls = 0
W.tick_n = 0
W.entities = {}
W.next_index = 1
W.see = true                  -- what dnp.can_see returns
W.rolls = nil                 -- nil = always the low end; else a list, cycled
W.roll_i = 0
W.log = {}
W.vfx = {}
-- dnp.sound calls, as { name, at }: what a test can ask was heard.
W.sounds = {}

local function fxv(v)
  if type(v) == "table" then return v.n end
  if type(v) == "string" then return tonumber(v) end
  return v
end

-- fx as a number carrier with the operators the runtime uses. Real fx is a
-- userdata over an i64; this is a double, and the header says why that is
-- acceptable here and nowhere else.
local fxmt
local function mkfx(n) return setmetatable({ n = n }, fxmt) end
fxmt = {
  __add = function(a, b) return mkfx(fxv(a) + fxv(b)) end,
  __sub = function(a, b) return mkfx(fxv(a) - fxv(b)) end,
  __mul = function(a, b) return mkfx(fxv(a) * fxv(b)) end,
  __div = function(a, b) return mkfx(fxv(a) / fxv(b)) end,
  __unm = function(a) return mkfx(-fxv(a)) end,
  __lt = function(a, b) return fxv(a) < fxv(b) end,
  __le = function(a, b) return fxv(a) <= fxv(b) end,
  __eq = function(a, b) return fxv(a) == fxv(b) end,
  __tostring = function(a) return string.format("%.4f", a.n) end,
  -- The real fx userdata exposes `.int` (the integer part) and `.raw` (as a
  -- string). Scripts use `.int` to get back to a Lua number, so the fake has
  -- to have it or a runtime that truncates correctly fails only here.
  __index = function(a, k)
    if k == "int" then return math.floor(a.n) end
    if k == "raw" then return tostring(math.floor(a.n * 4294967296)) end
    return nil
  end,
}
W.mkfx = mkfx
W.fxv = fxv

-- A plain table of constructors, with NO __call -- the engine's `fx` is
-- exactly that, and giving the fake a __call let mobs.lua write fx("0.25"),
-- which loaded here and raised on the first real server.
local fx = {
  int   = function(n) return mkfx(n) end,
  frac  = function(n, d) return mkfx(n / d) end,
  raw   = function(s) return mkfx(tonumber(s)) end,
  parse = function(s) return mkfx(tonumber(s) or 0) end,
}
-- NO __call. The engine's `fx` is a plain table of constructors; giving the
-- fake one let mobs.lua write fx("0.25"), which loaded here and raised there.

-- An entity. `kind` is "player" or a prop name.
-- PROPS ARE NOT ENTITIES, and the engine is emphatic about it: they are
-- separate userdata types with separate metatables and different methods. A
-- prop has no hp, no set_input, no sector. This harness used to give every
-- object every method, and two bugs walked straight through the gap --
-- `hurt` calling :hp() on a barrel, and dnp.sweep_hit refusing the missile's
-- own prop handle. Both raised on a real server and neither could be
-- reproduced here.
local PROP_METHODS = {
  valid = true, index = true, pos = true, set_pos = true, kind = true,
  despawn = true, set_animation = true, set_solid = true, solid = true,
  -- A missile is the one prop with a heading worth replicating, so it is the
  -- one prop that sets its own.
  set_facing = true,
  facing = true,
}

local Ent = {}
W.by_name = {}
Ent.__index = function(self, k)
  -- A prop answers only what a prop answers. Returning nil rather than raising
  -- matches Lua's own behaviour for a missing method, and it is what
  -- `target.hp == nil` in mobs.lua tests for.
  if rawget(self, "is_prop") and not PROP_METHODS[k] then return nil end
  local v = rawget(Ent, k)
  if type(v) == "function" then
    return function(...)
      if from_outside() then
        W.calls = W.calls + 1
        W.by_name[k] = (W.by_name[k] or 0) + 1
      end
      return v(...)
    end
  end
  return v
end
function Ent:valid() return self.alive end
function Ent:index() return self.idx end
function Ent:pos() return self.p end
function Ent:vel() return self.v end
function Ent:set_pos(p) self.p = p end
-- The engine has this on entities (not props): move and re-derive the sector.
function Ent:teleport(p) self.p = p; self.teleports = (self.teleports or 0) + 1 end
function Ent:set_velocity(v) self.v = v end
function Ent:add_impulse(v)
  self.v = { x = self.v.x + v.x, y = self.v.y + v.y, z = self.v.z + v.z }
  self.impulses = (self.impulses or 0) + 1
end
function Ent:grounded() return self.on_ground ~= false end
function Ent:set_animation(a) self.anim = a; self.anims = (self.anims or 0) + 1 end
-- The SAME shape the binding takes: set_input(move_x, move_y, aim [, buttons]).
-- It used to accept a table, which let the runtime call it in a way the real
-- engine would have refused -- a fake that is more permissive than the thing
-- it stands in for tests nothing.
function Ent:set_input(mx, my, aim, buttons)
  -- Tests for the MISTAKE, not for "is a table": fx here is a table too, so a
  -- blunt type check rejects every legitimate call. An input table is the one
  -- carrying a `move_x` field.
  if type(mx) == "table" and mx.move_x ~= nil then
    error("set_input takes (move_x, move_y, aim [, buttons]), not a table")
  end
  self.input = { move_x = mx, move_y = my, facing = aim, buttons = buttons or 0 }
  -- The WANTED angle. The body turns towards it over several ticks; it does
  -- not snap. See W.advance().
  self.want_aim = aim
end
function Ent:set_solid(s) self.is_solid = s end
function Ent:despawn() self.alive = false end
function Ent:hp() return self.health end
function Ent:set_hp(h) self.health = h end
-- POSITIONAL, as the binding takes it: add_effect(name, op, value [, ticks]).
-- It used to accept a table, which let mobs.lua call it a way the engine does
-- not offer.
function Ent:add_effect(name, op, value, ticks)
  if type(name) == "table" then
    error("add_effect takes (param, op, value [, ticks]), not a table")
  end
  self.effects = self.effects or {}
  table.insert(self.effects, { param = name, op = op, value = value, ticks = ticks })
  -- Modelled well enough for `flying` and `turn_speed`, which are the two the
  -- runtime sets: a permanent `set` becomes the parameter.
  if op == "set" then
    self.params = self.params or {}
    self.params[name] = value
  end
  return #self.effects
end
-- The other half of the pair, which the engine has always had and this harness
-- did not: `tagged_effect` removes before it adds, so a mob declaring a `swing`
-- reached straight through the gap the moment one existed. An id the engine
-- returns and takes back is not an id if only one direction is modelled.
--
-- Freed rather than table.remove'd, because add_effect returns the INDEX as the
-- id and shifting the array would renumber every effect above the removed one.
function Ent:remove_effect(id)
  if self.effects == nil or id == nil then return false end
  local e = self.effects[id]
  if e == nil then return false end
  self.effects[id] = { param = e.param, freed = true }
  if e.op == "set" and self.params then self.params[e.param] = nil end
  return true
end
-- Write a parameter into the BODY, permanently, as l_entity_set_body_param does.
-- Distinct from add_effect on purpose: a monster's swing is a property of its
-- body and there is no room in the effect stack for one per monster.
function Ent:set_body_param(name, value)
  if type(name) == "table" then
    error("set_body_param takes (param, value), not a table")
  end
  self.params = self.params or {}
  self.params[name] = value
end
-- OR a mask into the input this entity already has, leaving the axes alone --
-- the shape l_entity_press takes, and the only way a script can start a swing
-- without also claiming to know where a monster was walking.
function Ent:press(buttons)
  self.input = self.input or {}
  self.input.buttons = bit.bor(self.input.buttons or 0, buttons or 0)
  self.pressed = self.input.buttons
end
-- And the half that ends a press. See l_entity_release: `press` is not
-- self-limiting, so the mob runtime releases on the following tick.
function Ent:release(buttons)
  self.input = self.input or {}
  self.input.buttons = bit.band(self.input.buttons or 0, bit.bnot(buttons or 0))
  self.pressed = self.input.buttons
end
-- Which way a PROP points. The engine has always had the field and nothing
-- could write it, so the harness never needed it either -- until a missile
-- started setting its own heading so its eight-view sprite picks a view.
function Ent:set_facing(a) self.aim = a end
function Ent:kind() return self.what end
-- Where the body is actually POINTING, which lags where it has been told to
-- point. `facing` used to return a constant 0 here, so every test happened to
-- pass whenever its target was due east -- and a condition that compared the
-- wanted angle against the body's real facing could not be tested at all.
function Ent:facing() return self.aim or 0 end
-- Which sector it is in. Settable, because routing is the thing under test and
-- a fake world with no geometry has no other way to say "these two are in
-- different rooms".
function Ent:sector() return self.sec or 0 end
-- READ ONLY, as the engine's is. It used to accept a value and store it, which
-- is a setter the engine does not have -- so mobs.lua "set" turn_speed and
-- gravity_scale for a whole phase without either ever changing.
function Ent:param(name) return (self.params or {})[name] end

function W.add(what, x, y, z, is_prop)
  local e = setmetatable({
    idx = W.next_index, alive = true, what = what, is_prop = is_prop or false,
    p = { x = mkfx(x), y = mkfx(y), z = mkfx(z or 0) },
    v = { x = mkfx(0), y = mkfx(0), z = mkfx(0) },
    health = 100, is_solid = true,
  }, Ent)
  W.next_index = W.next_index + 1
  W.entities[e.idx] = e
  return e
end

-- Turn every body a step towards the angle its input asked for.
--
-- The engine turns at `turn_speed`, a RATE: a monster told to look at you is
-- not looking at you on the same tick. Modelling that is what makes
-- `target_in_front` a real question -- without it a monster is always already
-- facing its target, fires the instant it acquires one, and every shot lands.
-- Which is exactly what this harness reported while monsters on a real server
-- were shooting each other.
W.turn_rate = 0.06                    -- turns per tick, ~3.6 a second
function W.advance()
  for _, e in pairs(W.entities) do
    if e.alive and e.want_aim ~= nil then
      local have = e.aim or 0
      local d = e.want_aim - have
      while d > 2147483647 do d = d - 4294967296 end
      while d < -2147483648 do d = d + 4294967296 end
      local cap = W.turn_rate * 4294967296
      if d > cap then d = cap elseif d < -cap then d = -cap end
      e.aim = have + d
    end
  end
end

function W.reset()
  W.calls = 0
  W.tick_n, W.entities, W.next_index = 0, {}, 1
  W.see, W.rolls, W.roll_i = true, nil, 0
  W.log, W.vfx, W.sounds = {}, {}, {}
end

-- Every call into the fake engine is counted, because the cost that matters on
-- a real level is the NUMBER OF BINDING CALLS per tick -- each one crosses the
-- Lua/C boundary and several do real work (a raycast, a sector lookup). Timing
-- this harness would measure the harness; counting the calls measures the thing
-- the harness stands in for.
--
-- ONLY CALLS FROM OUTSIDE THIS FILE COUNT. The fake's own hitscan walks every
-- entity to find the nearest, which the engine does in one call -- so counting
-- it made 160 monsters look like 670 position lookups a tick and sent me
-- optimising a loop that does not exist in the engine. A cost harness that
-- counts its own internals measures itself.
W.calls = 0
local function counted(t)
  local out = {}
  for k, v in pairs(t) do
    if type(v) == "function" then
      out[k] = function(...)
        if from_outside() then
          W.calls = W.calls + 1
          W.by_name[k] = (W.by_name[k] or 0) + 1
        end
        return v(...)
      end
    else
      out[k] = v
    end
  end
  return out
end
W.counted = counted

local function all_entities()
  local list = {}
  for _, e in pairs(W.entities) do if e.alive then list[#list + 1] = e end end
  table.sort(list, function(a, b) return a.idx < b.idx end)
  return list
end

-- TOP-LEVEL, exactly as server/src/lua/fx_binding.cpp installs them. They were
-- under `dnp` here, which let mobs.lua reach them a way the engine does not
-- offer -- it loaded against this harness and raised on the first real server.
-- `print` is the global the VM installs (server/src/lua/vm.cpp), rate-limited
-- there. There is no dnp.log, which is what mobs.lua used to call.
local real_print = print
_G.print = function(...)
  -- tostring each: table.concat refuses a boolean or a nil, and a harness that
  -- raises inside its own logging turns a passing test into a crash.
  local parts = {}
  for i = 1, select("#", ...) do parts[i] = tostring((select(i, ...))) end
  W.log[#W.log + 1] = table.concat(parts, " ")
  real_print(...)
end

_G.fx = fx
_G.vec3 = { new = function(x, y, z) return { x = x, y = y, z = z } end }

dnp = {
  tick = function() return W.tick_n end,

  random = function(lo, hi)
    if W.rolls == nil then return lo end
    W.roll_i = W.roll_i % #W.rolls + 1
    local v = W.rolls[W.roll_i]
    if v < lo then return lo elseif v > hi then return hi end
    return v
  end,

  players = function()
    local ps = {}
    for _, e in ipairs(all_entities()) do
      if e.what == "player" then ps[#ps + 1] = e end
    end
    return ps
  end,
  player_count = function() return #dnp.players() end,
  entities = all_entities,

  spawn_enemy = function(prop, p)
    local e = W.add(prop, fxv(p.x), fxv(p.y), fxv(p.z))
    return e
  end,
  spawn_prop = function(prop, p)
    return W.add(prop, fxv(p.x), fxv(p.y), fxv(p.z), true)
  end,

  can_see = function() return W.see end,

  -- The nearest entity ALONG THE SHOOTER'S FACING.
  --
  -- The facing part was missing, and it mattered: this returned the nearest
  -- entity in any direction, so a monster that had not finished turning still
  -- hit its target here and hit whatever it was actually pointing at on a real
  -- server. Monsters damaged each other and never the player, and 29 passing
  -- checks said the shot landed.
  hitscan = function(from, range)
    local best, bd = nil, nil
    local face = from:facing()
    local fx_dir = math.cos(face / 4294967296 * 2 * math.pi)
    local fy_dir = math.sin(face / 4294967296 * 2 * math.pi)
    for _, e in ipairs(all_entities()) do
      if e.idx ~= from:index() then
        local a, b = from:pos(), e:pos()
        local dx, dy = fxv(b.x) - fxv(a.x), fxv(b.y) - fxv(a.y)
        local d = math.sqrt(dx * dx + dy * dy)
        -- Along the ray, and within a body's width of it. Crude, and enough
        -- to make "was it pointing that way" a real question.
        local along = dx * fx_dir + dy * fy_dir
        local off = math.abs(dx * -fy_dir + dy * fx_dir)
        if along > 0 and off <= 0.6 and d <= fxv(range) and
           (bd == nil or d < bd) then best, bd = e, d end
      end
    end
    -- TWO returns, as the engine gives: (target_or_nil, impact_vec3). This
    -- used to return four -- target, x, y, z -- which is what the doc comment
    -- on the binding says and NOT what it does, and mobs.lua was written
    -- against the fake and raised on the first real shot.
    if best == nil then return nil, nil end
    local q = best:pos()
    return best, { x = q.x, y = q.y, z = q.z }
  end,

  -- Geometry only, like the real one: this fake world has no geometry, so
  -- nothing is ever blocked. A test that needs a wall sets W.blocked.
  sweep = function(from, dir, dist)
    if W.blocked then return true, from end
    return false, nil
  end,

  -- The engine's one-question sweep, faked: nearest entity along the segment
  -- whose cylinder the swept sphere touches, or `false` if W.blocked.
  sweep_hit = function(from, dir, dist, radius, ignore_a, ignore_b)
    if W.blocked then return false, from end
    local fx_, fy = fxv(from.x), fxv(from.y)
    local dx, dy = fxv(dir.x), fxv(dir.y)
    local r = fxv(radius)
    local best, bt = nil, nil
    for _, e in ipairs(all_entities()) do
      local skip = (ignore_a ~= nil and e:index() == ignore_a:index()) or
                   (ignore_b ~= nil and e:index() == ignore_b:index())
      if not skip then
        local q = e:pos()
        local mx, my = fxv(q.x) - fx_, fxv(q.y) - fy
        local tc = mx * dx + my * dy
        local reach = r + 0.35                    -- a default body radius
        if tc >= -reach and tc - reach <= fxv(dist) then
          local d2 = mx * mx + my * my - tc * tc
          if d2 <= reach * reach then
            local t = tc - math.sqrt(reach * reach - d2)
            if t < 0 then t = 0 end
            if t <= fxv(dist) and (bt == nil or t < bt) then best, bt = e, t end
          end
        end
      end
    end
    if best == nil then return nil end
    return best, { x = mkfx(fx_ + dx * bt), y = mkfx(fy + dy * bt),
                   z = from.z }
  end,

  dir = function(ang)
    local rad = ang / 4294967296 * 2 * math.pi
    return { x = mkfx(math.cos(rad)), y = mkfx(math.sin(rad)) }
  end,
  atan2 = function(y, x)
    local a = math.atan2(fxv(y), fxv(x))
    return math.floor(a / (2 * math.pi) * 4294967296)
  end,
  length = function(v)
    local x, y, z = fxv(v.x), fxv(v.y), fxv(v.z or 0)
    return mkfx(math.sqrt(x * x + y * y + z * z))
  end,

  -- A blast's reach and shove (core sim/blast.hpp), in floats: inside a
  -- sphere, the falloff by the PLANAR distance, outward in the plane.
  blast_reach = function(centre, at, radius, falloff)
    local dx, dy, dz = fxv(at.x) - fxv(centre.x), fxv(at.y) - fxv(centre.y), fxv(at.z) - fxv(centre.z)
    local r = fxv(radius)
    if dx * dx + dy * dy + dz * dz > r * r then return nil end
    if not falloff or r == 0 then return true end
    return true, mkfx((r - math.sqrt(dx * dx + dy * dy)) / r)
  end,
  knock_impulse = function(origin, at, knockback, lift, k)
    local dx, dy = fxv(at.x) - fxv(origin.x), fxv(at.y) - fxv(origin.y)
    local a = (dx == 0 and dy == 0) and 0 or math.atan2(dy, dx)
    local kb = fxv(knockback) * fxv(k)
    return { x = mkfx(math.cos(a) * kb), y = mkfx(math.sin(a) * kb), z = mkfx(fxv(lift) * fxv(k)) }
  end,

  -- The engine's dnp.sound (docs/SOUNDS.md): from a place or a thing. Recorded,
  -- never played -- what a sound is is the client's business.
  sound = function(name, at, opts)
    W.sounds[#W.sounds + 1] = { name = name, at = at, opts = opts }
    return true
  end,
  vfx = function(kind, at, dir, param)
    W.vfx[#W.vfx + 1] = { kind = kind, at = at, dir = dir, param = param }
  end,

  -- The protocol's button bits, matching proto::button in
  -- core/include/dnp/proto/messages.hpp.
  button = {
    JUMP = 0x0001, DASH = 0x0002, CLIMB = 0x0004, HOVER = 0x0008,
    FLIGHT = 0x0010, GRAPPLE = 0x0020, INTERACT = 0x0040,
    ATTACK_PRIMARY = 0x0080, ATTACK_SECONDARY = 0x0100, USE = 0x0200,
    CROUCH = 0x0400,
  },

  -- A route, from W.routes[from][to] if a test set one, else nil. Deliberately
  -- NOT a pathfinder: this harness has no geometry, so inventing one would
  -- test the fake rather than the runtime. A test that cares about routing
  -- states the route it expects to be followed.
  find_path = function(from, to, radius, height, jump_height)
    W.path_calls = (W.path_calls or 0) + 1
    W.last_jump_arg = jump_height
    local by_from = W.routes and W.routes[from]
    if by_from == nil then return nil, "unreachable" end
    return by_from[to]
  end,
}

return W
