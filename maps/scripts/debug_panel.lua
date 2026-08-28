-- A panel for changing things while the game is running (docs/protocol.md §11).
--
-- Loaded on purpose, never by a map:
--
--   dnp-server --map ... --script maps/scripts/debug_panel.lua
--
-- It opens on whoever joins and stays open. Everything it does is something a
-- script could already do -- an effect on a movement parameter, a health value,
-- a map change -- so this adds no capability, only a way to reach them without
-- editing a file and restarting.
--
-- WIDGET IDS ARE THIS FILE'S OWN. The engine passes them back untouched, so
-- they are how a handler is found; they mean nothing outside here.
local W_SPEED  = 1
local W_DAMAGE = 2
local W_HP     = 3
local W_MAP    = 4
local W_GO     = 5
local W_HEAL   = 6
local W_TURN   = 7

-- Effect ids, per agent, so a change REPLACES the previous one rather than
-- stacking on it. Without this, dragging a slider five times leaves five
-- effects on the same parameter and the value depends on the order they are
-- combined in.
local effects = {}   -- agent index -> { speed = id, turn = id }
local chosen_map = {}
local panel_of = {}

local function set_param(e, key, name, value)
  local idx = e:index()
  effects[idx] = effects[idx] or {}
  local old = effects[idx][key]
  if old then e:remove_effect(old) end
  -- `set`, not `mul`: the panel shows an absolute number and the player expects
  -- the parameter to become it. A multiplier would compound with whatever the
  -- map or an item had already done and the slider would stop meaning anything.
  effects[idx][key] = e:add_effect(name, "set", value)
end

-- The maps this server was started with, by name, for the map selector.
--
-- dnp.levels() reports what the operator actually loaded rather than a list
-- written here, which would go stale the first time somebody ran the server
-- with different arguments.
local function level_names()
  local out = {}
  for _, name in ipairs(dnp.levels()) do out[#out + 1] = name end
  return out
end

local function build(e)
  local maps = level_names()
  local idx = e:index()
  return {
    title = "Debug",
    widgets = {
      { kind = "label",  id = 0, label = "Player " .. tostring(idx) },
      -- Ranges are the slider's contract and the server clamps to them
      -- (§11.3), so they are chosen to be survivable rather than merely wide:
      -- a speed of zero is a player who cannot move and has no way to open a
      -- panel to fix it.
      { kind = "slider", id = W_SPEED,  label = "Speed",
        value = (e:param("move_speed")).int, lo = 1, hi = 40 },
      { kind = "slider", id = W_TURN,   label = "Turn/s",
        value = (e:param("turn_speed")).int, lo = 1, hi = 20 },
      { kind = "slider", id = W_DAMAGE, label = "Damage",
        value = (enemies and enemies.melee_damage) or 20, lo = 1, hi = 200 },
      { kind = "slider", id = W_HP,     label = "Health",
        value = e:hp() or 100, lo = 1, hi = 200 },
      { kind = "button", id = W_HEAL,   label = "Full health" },
      { kind = "select", id = W_MAP,    label = "Map",
        value = chosen_map[idx] or 0, options = maps },
      { kind = "button", id = W_GO,     label = "Go to map" },
    }
  }
end

local function show(e)
  local id = dnp.ui_open(e, build(e))
  if id ~= 0 then panel_of[e:index()] = id end
end

-- Opened when somebody arrives, and not before: dnp.ui_open needs a session to
-- send to, and on_tick is the first place this script can know there is one.
local seen = {}
function on_tick()
  for _, e in ipairs(dnp.players()) do
    local idx = e:index()
    if not seen[idx] then
      seen[idx] = true
      show(e)
    end
  end
end

function on_ui_event(e, ui_id, widget, value)
  if panel_of[e:index()] ~= ui_id then return end
  local idx = e:index()

  if widget == W_SPEED then
    set_param(e, "speed", "move_speed", fx.int(value))
  elseif widget == W_TURN then
    set_param(e, "turn", "turn_speed", fx.int(value))
  elseif widget == W_DAMAGE then
    -- Reaches the monster script through the table it publishes. Both are
    -- ordinary globals in one VM, which is the whole extension mechanism here:
    -- a script cooperates with another by name, not through the engine.
    if enemies then enemies.melee_damage = value end
  elseif widget == W_HP then
    e:set_hp(value)
  elseif widget == W_HEAL then
    e:set_hp(100)
    -- Reopened so the health slider shows the new value. A panel is a snapshot
    -- of state at the moment it was built, and nothing pushes changes into one.
    show(e)
  elseif widget == W_MAP then
    -- Remembered, not acted on. A select changes as you scroll it, and a map
    -- change on every keystroke would make the list impossible to use.
    chosen_map[idx] = value
  elseif widget == W_GO then
    local target = chosen_map[idx]
    if target ~= nil then
      seen[idx] = nil       -- reopen on the far side; it is a new level
      panel_of[idx] = nil
      e:change_map(target)
    end
  end
end
