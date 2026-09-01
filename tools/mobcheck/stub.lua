-- The smallest world a mob table can be validated against.
--
-- Answers one question: is this TABLE well formed -- do its actions exist, do
-- its arguments type-check, do its references resolve, does its custom code
-- compile? That is what an editor wants answered while somebody is typing, and
-- answering it needs no map and no server.
--
-- It deliberately does NOT answer whether the mobs behave well, or whether the
-- props they name are in assets/props.txt -- that is checked by the server at
-- load, against the catalogue that actually arrived. Saying so here keeps a
-- clean run from looking like more of a bill of health than it is.
--
-- Nothing below runs. The loader reads the table and exits; no mob is ever
-- spawned, so no action is ever performed.

local function fxnum(v)
  -- Enough of an fx to satisfy the loader's `as_fx`: it only ever stores what
  -- it is given. Arithmetic on these is not exercised by loading.
  return setmetatable({ v = v }, { __tostring = function(s) return "fx(" .. tostring(s.v) .. ")" end })
end

-- A plain table of constructors, with NO __call: the engine's `fx` is exactly
-- that.
local fx = {
  int   = function(n) return fxnum(n) end,
  frac  = function(n, d) return fxnum(n .. "/" .. d) end,
  raw   = function(s) return fxnum(s) end,
  parse = function(s) return fxnum(s) end,
}
-- NO __call: the engine's `fx` is a plain table of constructors.

_G.fx = fx
_G.vec3 = { new = function(x, y, z) return { x = x, y = y, z = z } end }

dnp = {
  tick = function() return 0 end,
  random = function(lo, hi) return lo end,
  players = function() return {} end,
  player_count = function() return 0 end,
  entities = function() return {} end,
  spawn_prop = function() return nil end,
  spawn_enemy = function() return nil end,
  can_see = function() return false end,
  hitscan = function() return nil end,
  sweep = function() return false, nil end,
  sweep_hit = function() return nil end,
  find_path = function() return nil end,
  button = { JUMP = 0x0001, DASH = 0x0002, CLIMB = 0x0004, HOVER = 0x0008,
             FLIGHT = 0x0010, GRAPPLE = 0x0020, INTERACT = 0x0040,
             ATTACK_PRIMARY = 0x0080, ATTACK_SECONDARY = 0x0100,
             USE = 0x0200, CROUCH = 0x0400 },
  dir = function() return { x = fxnum(0), y = fxnum(0) } end,
  vfx = function() end,
}
