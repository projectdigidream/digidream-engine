--
-- Weapons, ammunition and what a player is carrying.
--
-- PER-PLAYER INVENTORY, held here rather than in the engine. What a body can do
-- is physics and belongs to the parameter table; what a person is CARRYING is a
-- gameplay question, and the engine has no opinion about shotguns. Same
-- reasoning that keeps doom.lua's key ring in Lua.
--
-- Keyed by AGENT INDEX, and forgotten when that index is reused. A slot is
-- handed to whoever arrives next, so an inventory that outlived its owner would
-- arm a new player with the last one's shotgun -- which is the bug the health
-- reset in place_in_level exists to prevent, one level up.
--
-- WHY THIS IS A SEPARATE SCRIPT. The VM harvests hooks as a LIST of handlers,
-- one per script, so this file's on_tick and on_touch run alongside doom.lua's
-- rather than replacing them. That is what lets weapons be added to the DOOM II
-- maps without editing the file that knows about doors.
--
weapons = weapons or {}
local W = weapons

local SCALE = 48
local function du(n) return fx.frac(n, SCALE) end
local function tics(n) return math.floor(n * 60 / 35 + 0.5) end

-- Degrees to brad, for the spread offsets below. A turn is 2^32.
local function deg(n) return math.floor(n * 4294967296 / 360) end

local ZERO = fx.int(0)

-- --- what a weapon IS --------------------------------------------------------
--
-- Data, in the shape the mob vocabulary uses and for the same reason: a table
-- is something an editor can render and re-edit, and a weapon defined by a
-- function is one only this file can read.
--
-- EVERY WEAPON HERE IS A HITSCAN, melee included. That is not a shortcut --
-- Doom's own fist is a line attack with a short range, and modelling a punch as
-- a very short bullet means the two share every line of the code that fires
-- them. `melee` only suppresses the tracer.
--
-- `ammo = nil` means it never runs out, which is what the fists and chainsaw
-- are. `per` is what one pull of the trigger costs. `auto` keeps firing while
-- the button is held; everything else wants the trigger released first.
W.defs = {
  fists = {
    label = "Fists", order = 1, melee = true,
    damage = 10, range = du(64), count = 1, spread = 0, cooldown = tics(12)
  },
  chainsaw = {
    label = "Chainsaw", order = 2, melee = true, auto = true,
    damage = 12, range = du(80), count = 1, spread = 0, cooldown = tics(4)
  },
  pistol = {
    label = "Pistol", order = 3,
    damage = 10, range = du(2048), count = 1, spread = 3,
    ammo = "bullets", per = 1, cooldown = tics(14), tracer = 1
  },
  shotgun = {
    label = "Shotgun", order = 4,
    -- Seven pellets, Doom's number, each its own cast with its own spread roll.
    -- That is the entire difference between this and the pistol, and it is why
    -- dnp.hitscan grew an angular offset: without one all seven travel the
    -- identical ray, and a shotgun is a pistol that hits seven times as hard at
    -- any range whatsoever.
    damage = 10, range = du(2048), count = 7, spread = 14,
    ammo = "shells", per = 1, cooldown = tics(30), tracer = 1
  },
  supershotgun = {
    label = "Super Shotgun", order = 5,
    damage = 10, range = du(2048), count = 20, spread = 26,
    ammo = "shells", per = 2, cooldown = tics(50), tracer = 1
  },
  chaingun = {
    label = "Chaingun", order = 6, auto = true,
    damage = 10, range = du(2048), count = 1, spread = 8,
    ammo = "bullets", per = 1, cooldown = tics(5), tracer = 1
  }
}

-- The most of each kind a player may hold. Doom's numbers.
W.ammo_max = { bullets = 200, shells = 50, rockets = 50, cells = 300 }

-- --- what the pickups do -----------------------------------------------------
--
-- By catalogue NAME, so renumbering modules/doom2/assets/props.txt cannot make
-- a medikit grant cells. Same rule doom.lua's key ring follows.
local HEAL = { stimpack = 10, medikit = 25, health_bonus = 1 }
local ARMOUR = { armour_green = 100, armour_blue = 200 }

local GIVES_WEAPON = {
  w_chainsaw = "chainsaw", w_shotgun = "shotgun",
  w_supershotgun = "supershotgun", w_chaingun = "chaingun"
}

-- NOT the rocket launcher, plasma rifle or BFG.
--
-- Those fire projectiles, and the only launcher in this codebase is a local
-- inside maps/scripts/mobs.lua that takes a mob record rather than a player.
-- Exposing it is a change to the mob runtime and a decision about whose
-- framework owns player projectiles -- worth making deliberately rather than as
-- a side effect of adding a shotgun. The pickups exist in the catalogue and are
-- placed on the maps; they grant nothing yet and say so when walked over,
-- because a pickup that silently does nothing is indistinguishable from a bug.
local NOT_YET = {
  w_rocket = "rocket launcher", w_plasma = "plasma rifle", w_bfg = "BFG"
}

-- A weapon pickup comes with a little of what it eats, as Doom's do.
local WEAPON_AMMO = {
  shotgun = { shells = 8 }, supershotgun = { shells = 8 },
  chaingun = { bullets = 20 }
}

local GIVES_AMMO = {
  ammo_clip = { bullets = 10 }, ammo_box = { bullets = 50 },
  ammo_shells = { shells = 4 }, ammo_shellbox = { shells = 20 },
  ammo_rocket = { rockets = 1 }, ammo_rocketbox = { rockets = 5 },
  ammo_cell = { cells = 20 }, ammo_cellpack = { cells = 100 }
}

-- Health has a ceiling, and the bonus is the one thing that goes past it --
-- which is why it is a number here rather than a flag.
local HEALTH_MAX = 100
local HEALTH_MAX_BONUS = 200

-- --- inventory ---------------------------------------------------------------

W.of = {}
local ready_at = {}     -- agent index -> the tick its weapon may fire again
local was_holding = {}  -- agent index -> last tick's buttons

local function fresh()
  -- Everyone starts with these, as Doom does: something that always works and
  -- something with a little ammunition.
  return {
    have = { fists = true, pistol = true },
    ammo = { bullets = 50, shells = 0, rockets = 0, cells = 0 },
    armour = 0,
    current = "pistol"
  }
end

--- The inventory for an agent, created on demand.
function W.get(idx)
  if idx == nil then return nil end
  W.of[idx] = W.of[idx] or fresh()
  return W.of[idx]
end

--- Forget an agent's inventory. For when a slot is reused.
function W.forget(idx)
  W.of[idx] = nil
  ready_at[idx] = nil
  was_holding[idx] = nil
end

--- Every weapon a player holds, in display order.
function W.list(idx)
  local inv = W.get(idx)
  if inv == nil then return {} end
  local out = {}
  for name, def in pairs(W.defs) do
    if inv.have[name] then out[#out + 1] = { name = name, def = def } end
  end
  table.sort(out, function(a, b) return a.def.order < b.def.order end)
  return out
end

--- Switch to a weapon, if it is held. True when the selection changed.
function W.select(idx, name)
  local inv = W.get(idx)
  if inv == nil or not inv.have[name] or inv.current == name then return false end
  inv.current = name
  return true
end

-- --- picking things up -------------------------------------------------------

local function give_ammo(inv, what)
  local took = false
  for kind, n in pairs(what or {}) do
    local max = W.ammo_max[kind] or 0
    local before = inv.ammo[kind] or 0
    if before < max then
      inv.ammo[kind] = math.min(max, before + n)
      took = true
    end
  end
  return took
end

function on_touch(entity, prop)
  -- PLAYERS ONLY. Every other agent carries the catalogue id of what it was
  -- spawned as, and a level of monsters walking over the medikits would clear a
  -- Doom map of pickups in seconds -- which is exactly what happened to the keys
  -- before doom.lua grew the same guard.
  if entity:kind() ~= 0 then return end
  local idx = entity:index()
  if idx == nil then return end

  local name = prop:name()
  if name == nil then return end
  local inv = W.get(idx)
  local took = false

  local heal = HEAL[name]
  if heal then
    local hp = entity:hp() or 0
    -- The bonus trickles past the normal ceiling; everything else stops at it.
    local ceiling = (name == "health_bonus") and HEALTH_MAX_BONUS or HEALTH_MAX
    if hp < ceiling then
      entity:set_hp(math.min(ceiling, hp + heal))
      took = true
    end
  end

  local armour = ARMOUR[name]
  if armour and inv.armour < armour then
    inv.armour = armour
    took = true
  end

  local weapon = GIVES_WEAPON[name]
  if weapon then
    -- The weapon is taken once; its ammunition is taken every time -- which is
    -- why a second shotgun on a map is still worth walking over, and why `took`
    -- is an OR of both.
    if not inv.have[weapon] then
      inv.have[weapon] = true
      inv.current = weapon
      took = true
    end
    if give_ammo(inv, WEAPON_AMMO[weapon]) then took = true end
  end

  if NOT_YET[name] then
    print(("[weapons] the %s is not implemented yet; leaving it on the floor")
          :format(NOT_YET[name]))
  end

  if give_ammo(inv, GIVES_AMMO[name]) then took = true end

  -- REFUSED PICKUPS STAY ON THE FLOOR. Walking over a medikit at full health
  -- must not consume it, or a player who is topped up destroys the health they
  -- will need in a minute. Doom does the same.
  if took then prop:despawn() end
end

-- --- firing ------------------------------------------------------------------

--- Can this weapon be fired right now?
local function has_ammo(inv, def)
  if def.ammo == nil then return true end
  return (inv.ammo[def.ammo] or 0) >= (def.per or 1)
end

--- Fall back to the best weapon that still has ammunition.
--
-- Doom does this and it is not a nicety: a player who empties a shotgun while
-- something is chasing them is otherwise holding an inert object, pressing fire
-- and getting nothing, with fifty bullets in their pocket. Seen exactly that
-- way -- "weapon=supershotgun shells=0" for the rest of a run while the pistol
-- sat there loaded.
--
-- BEST, not first: highest order wins, so running a shotgun dry drops to the
-- pistol rather than to the fists.
local function switch_to_usable(inv)
  local best = nil
  for name, def in pairs(W.defs) do
    if inv.have[name] and has_ammo(inv, def) then
      if best == nil or def.order > W.defs[best].order then best = name end
    end
  end
  if best then inv.current = best end
  return best
end

local function fire(e, idx, inv, def, now)
  -- Ammunition first: a weapon that fires and then discovers it was empty has
  -- already drawn a tracer and spent a cooldown.
  if not has_ammo(inv, def) then
    switch_to_usable(inv)
    return false
  end
  if def.ammo then
    inv.ammo[def.ammo] = (inv.ammo[def.ammo] or 0) - (def.per or 1)
  end

  local from = e:pos()
  for _ = 1, (def.count or 1) do
    -- One roll per pellet, centred on the facing. dnp.random is the seeded
    -- generator, so this is reproducible with the rest of the tick.
    local off = 0
    if (def.spread or 0) > 0 then
      off = deg(def.spread) * (dnp.random(-1000, 1000) / 1000)
    end

    -- TWO returns, not four: (target_or_nil, impact). Reading it as x, y, z is
    -- the mistake that once presented as four separate bugs at once, and the
    -- comment in mobs.lua's acts.hitscan says so at length.
    local target, at = dnp.hitscan(e, def.range, math.floor(off))
    if at ~= nil then
      if def.tracer then
        -- The tracer carries its START, its DIRECTION and its LENGTH. Muzzle
        -- height is the height the ENGINE cast at rather than a guess.
        local muzzle = vec3.new(from.x, from.y, at.z)
        local len = dnp.length(vec3.new(at.x - muzzle.x, at.y - muzzle.y, ZERO))
        dnp.vfx(def.tracer, muzzle, e:facing(), (len * fx.int(256)).int)
      end
      -- Through the mob framework, not set_hp: it is what knows about dying,
      -- corpses, infighting and not damaging a prop that has no health.
      if target ~= nil and target:valid() and mobs and mobs.hurt then
        mobs.hurt(target, def.damage, nil)
      end
    end
  end

  ready_at[idx] = now + (def.cooldown or tics(12))
  return true
end


-- --- the weapon panel --------------------------------------------------------
--
-- A `select` listing what you are carrying, and four read-outs for the
-- ammunition. The declarative UI protocol (docs/protocol.md §11) is the only
-- way a script can put anything on screen, and it is enough for this: the
-- client renders the widgets and never runs a line of script.
--
-- REBUILT ONLY WHEN THE SET CHANGES. A panel is built from state at the moment
-- it opens and nothing pushes changes into one, so a weapon picked up has to
-- reopen it -- but ammunition changes several times a second and reopening for
-- that would flicker the window on every shot. dnp.ui_set changes one widget in
-- place, which is what the ammunition read-outs use.
--
-- WHY SLIDERS FOR AMMUNITION. A label's text is fixed when the panel opens, so
-- it cannot count down; a slider carries a value that ui_set can change. Its
-- events are ignored below -- dragging one is a request to have more bullets,
-- and the server is not obliged to grant it.
local W_SELECT = 1
local AMMO_WIDGET = { bullets = 10, shells = 11, rockets = 12, cells = 13 }

local panel_of = {}    -- agent index -> ui_id
local shown = {}       -- agent index -> { list = "csv of weapon names", ammo = {} }

local function build_panel(idx)
  local inv = W.get(idx)
  local list = W.list(idx)
  local options, value = {}, 1
  for i, w in ipairs(list) do
    options[i] = w.def.label
    if w.name == inv.current then value = i end
  end
  -- A panel with no options is one the client cannot render sensibly, and it
  -- cannot happen -- everyone holds the fists -- but saying so beats finding out.
  if #options == 0 then options[1] = "(none)" end

  local widgets = {
    { kind = "label",  id = 0, label = "Weapon" },
    { kind = "select", id = W_SELECT, label = "Held", value = value,
      lo = 1, hi = #options, options = options },
    { kind = "label",  id = 0, label = "Ammunition" }
  }
  for _, kind in ipairs({ "bullets", "shells", "rockets", "cells" }) do
    widgets[#widgets + 1] = {
      kind = "slider", id = AMMO_WIDGET[kind], label = kind,
      value = inv.ammo[kind] or 0, lo = 0, hi = W.ammo_max[kind] or 0
    }
  end
  return { title = "Weapons", widgets = widgets }
end

--- The weapons a player holds, as one comparable string.
local function signature(idx)
  local names = {}
  for _, w in ipairs(W.list(idx)) do names[#names + 1] = w.name end
  return table.concat(names, ",")
end

local function open_panel(e, idx)
  -- CLOSE THE OLD ONE FIRST. Every open allocates a fresh ui_id, so reopening
  -- does not replace: the client keys panels by id, sees a new one, and draws
  -- it beside the old. Same trap debug_panel.lua documents at length.
  if panel_of[idx] then dnp.ui_close(e, panel_of[idx]) end
  panel_of[idx] = nil
  local id = dnp.ui_open(e, build_panel(idx))
  if id ~= 0 then
    panel_of[idx] = id
    shown[idx] = { list = signature(idx), ammo = {} }
  end
end

--- Keep an open panel in step, reopening only when the weapon list changed.
local function refresh_panel(e, idx)
  local inv = W.get(idx)
  if panel_of[idx] == nil or shown[idx] == nil then open_panel(e, idx); return end

  if signature(idx) ~= shown[idx].list then open_panel(e, idx); return end

  -- The selection, when something else changed it -- running a weapon dry and
  -- falling back is the common case, and a panel still highlighting the empty
  -- gun is worse than none.
  local list = W.list(idx)
  for i, w in ipairs(list) do
    if w.name == inv.current and shown[idx].sel ~= i then
      shown[idx].sel = i
      dnp.ui_set(e, panel_of[idx], W_SELECT, i)
    end
  end

  -- Ammunition, only where it moved. This runs sixty times a second and a
  -- reliable control message per counter per tick is a waste of the channel
  -- that carries map data.
  for kind, wid in pairs(AMMO_WIDGET) do
    local n = inv.ammo[kind] or 0
    if shown[idx].ammo[kind] ~= n then
      shown[idx].ammo[kind] = n
      dnp.ui_set(e, panel_of[idx], wid, n)
    end
  end
end

function on_ui_event(e, ui_id, widget, value)
  local idx = e:index()
  if idx == nil or panel_of[idx] ~= ui_id then return end
  if widget ~= W_SELECT then
    -- An ammunition read-out. Dragging one is a request for more bullets, and
    -- the answer is no -- but the widget has already moved on the client, so
    -- it is put back rather than left showing a number nobody has.
    local inv = W.get(idx)
    for kind, wid in pairs(AMMO_WIDGET) do
      if wid == widget then dnp.ui_set(e, ui_id, wid, inv.ammo[kind] or 0) end
    end
    return
  end

  local list = W.list(idx)
  local chosen = list[value]
  if chosen then
    W.select(idx, chosen.name)
    if shown[idx] then shown[idx].sel = value end
  end
end

function on_tick()
  -- NO ARGUMENT. Vm::call_hook takes a tick but uses it only for error
  -- accounting -- it pushes the handler with zero arguments -- so a hook
  -- declared as on_tick(tick) gets nil and every comparison against it raises.
  -- Every other script in the tree reads the clock this way; this one assumed
  -- the tick was a parameter and failed on its first firing tick.
  local now = dnp.tick()
  -- Players who have left. A slot is handed to whoever arrives next, so an
  -- inventory or a panel id that outlived its owner would arm a new player with
  -- the last one's shotgun -- the same reasoning as doom.lua's key ring.
  local here = {}
  for _, e in ipairs(dnp.players()) do here[e:index()] = true end
  for idx in pairs(W.of) do
    if not here[idx] then W.forget(idx); panel_of[idx] = nil; shown[idx] = nil end
  end

  for _, e in ipairs(dnp.players()) do
    local idx = e:index()
    if idx ~= nil then
      local inv = W.get(idx)
      refresh_panel(e, idx)
      local def = W.defs[inv.current]
      local held = e:buttons() or 0
      local before = was_holding[idx] or 0
      was_holding[idx] = held

      -- Lua 5.1 has no bitwise operators; LuaJIT ships the `bit` library, and
      -- the button set is a mask.
      local firing = bit.band(held, dnp.button.ATTACK_PRIMARY) ~= 0
      local pressed = bit.band(before, dnp.button.ATTACK_PRIMARY) == 0

      if def and firing and (def.auto or pressed) and now >= (ready_at[idx] or 0) then
        fire(e, idx, inv, def, now)
      end
    end
  end
end
