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
-- The attack, which is a collider with a shape, a sweep and a rate rather than
-- a single number. Each of these is a replicated parameter, so changing one
-- changes what the CLIENT predicts too -- the swing on screen follows.
local W_REACH  = 8
local W_INNER  = 9
local W_ARC    = 10
local W_FROM   = 11
local W_TO     = 12
local W_DUR    = 13
local W_COOL   = 14
-- The camera. Client-side (design §4.1), configured from here: the server never
-- says where the camera IS, only how it should follow.
local W_CDIST  = 15
local W_CPITCH = 16
local W_CSMOOTH= 17
local W_CACCEL = 18
local W_CLEAD  = 19
local W_CLEADS = 20

-- Effect ids, per agent, so a change REPLACES the previous one rather than
-- stacking on it. Without this, dragging a slider five times leaves five
-- effects on the same parameter and the value depends on the order they are
-- combined in.
local effects = {}   -- agent index -> { speed = id, turn = id }
-- The panel's own copy of the monster damage.
--
-- Kept here as well as pushed into the monster script, because that script is
-- not always loaded -- a map with no monsters does not load it -- and a slider
-- that snapped back to 20 every time the panel opened would look broken when it
-- was merely talking to nobody.
local damage = nil

-- The camera settings this panel is showing, per player.
--
-- Held here because nothing reports them back: the camera is the client's and
-- the server has no copy, so the panel is the authority on what it last sent.
-- Defaults chosen to match the client's own, so opening the panel does not
-- silently change the view.
local cam = {}
local function cam_of(idx)
  cam[idx] = cam[idx] or {
    distance = 45, pitch = 125, smoothing = 8, accel = 0, lead = 0, lead_speed = 4
  }
  return cam[idx]
end

-- Everything the panel knows, sent at once. One message rather than one per
-- slider: they are read together and a half-applied camera is a worse thing to
-- look at than either the old one or the new one.
local function push_camera(e)
  local c = cam_of(e:index())
  dnp.camera(e, {
    distance = c.distance,
    pitch = c.pitch / 1000,
    smoothing = c.smoothing,
    max_accel = c.accel,
    lead = c.lead,
    lead_smoothing = c.lead_speed
  })
end
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

-- An fx as an integer number of hundredths, for a slider to carry.
--
-- fx has no `to number` that is not lossy, which is the point of it -- so the
-- conversion is done by multiplying in fx and taking the integer part, and the
-- rounding happens once, here, rather than somewhere it would be a surprise.
local function hundredths(v)
  if v == nil then return 0 end
  return (v * fx.int(100)).int
end

local function build(e)
  local maps = level_names()
  local idx = e:index()
  cam_of(idx)
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
        -- Through the monster runtime's own published table. `mobs` may be
        -- nil -- a map that declares no monsters declares no mobs.lua -- so
        -- the panel falls back to a number rather than going quiet.
        value = damage or (mobs and mobs.player_damage) or 25, lo = 1, hi = 200 },
      { kind = "slider", id = W_HP,     label = "Health",
        value = e:hp() or 100, lo = 1, hi = 200 },
      { kind = "button", id = W_HEAL,   label = "Full health" },
      { kind = "label",  id = 0, label = "Attack" },
      -- Distances in HUNDREDTHS of a world unit, angles in hundredths of a
      -- turn. A slider carries an integer, and rounding a reach of 2.0 to the
      -- nearest unit would give a weapon three usable lengths -- so the
      -- fraction lives in the scale rather than being lost.
      { kind = "slider", id = W_REACH,  label = "Reach /100",
        value = hundredths(e:param("attack_reach")), lo = 25, hi = 800 },
      { kind = "slider", id = W_INNER,  label = "Min reach /100",
        value = hundredths(e:param("attack_min_reach")), lo = 0, hi = 600 },
      { kind = "slider", id = W_ARC,    label = "Half arc /100 turn",
        value = hundredths(e:param("attack_half_arc")), lo = 1, hi = 50 },
      { kind = "slider", id = W_FROM,   label = "Sweep from /100",
        value = hundredths(e:param("attack_sweep_from")), lo = -50, hi = 50 },
      { kind = "slider", id = W_TO,     label = "Sweep to /100",
        value = hundredths(e:param("attack_sweep_to")), lo = -50, hi = 50 },
      { kind = "slider", id = W_DUR,    label = "Swing ticks",
        value = (e:param("attack_ticks")).int, lo = 1, hi = 60 },
      { kind = "slider", id = W_COOL,   label = "Cooldown ticks",
        value = (e:param("attack_cooldown")).int, lo = 0, hi = 180 },
      { kind = "label",  id = 0, label = "Camera" },
      { kind = "slider", id = W_CDIST,  label = "Distance",
        value = cam[idx].distance, lo = 5, hi = 120 },
      -- Milli-turns, like everything angular that has to fit in a slider. An
      -- eighth of a turn is 125 and is roughly the isometric default.
      { kind = "slider", id = W_CPITCH, label = "Pitch /1000 turn",
        value = cam[idx].pitch, lo = 10, hi = 240 },
      { kind = "slider", id = W_CSMOOTH,label = "Follow speed",
        value = cam[idx].smoothing, lo = 1, hi = 40 },
      -- Zero is unlimited, which is the weightless camera. Anything else gives
      -- it mass: it takes time to get moving and time to stop.
      { kind = "slider", id = W_CACCEL, label = "Accel (0=off)",
        value = cam[idx].accel, lo = 0, hi = 400 },
      { kind = "slider", id = W_CLEAD,  label = "Look ahead",
        value = cam[idx].lead, lo = 0, hi = 20 },
      { kind = "slider", id = W_CLEADS, label = "Look-ahead speed",
        value = cam[idx].lead_speed, lo = 1, hi = 30 },
      { kind = "select", id = W_MAP,    label = "Map",
        value = chosen_map[idx] or 0, options = maps },
      { kind = "button", id = W_GO,     label = "Go to map" },
    }
  }
end

local function show(e)
  local idx = e:index()
  -- CLOSE THE OLD ONE FIRST.
  --
  -- Every open allocates a fresh ui_id -- deliberately, so an event for a panel
  -- that has closed names an id that no longer exists rather than one recycled
  -- into a different panel. The consequence is that reopening does not replace:
  -- the client keys panels by id, sees a new one, and draws it beside the old.
  -- That is what "multiple panels are created" looks like, and the fix belongs
  -- here rather than in the client, because a script opening two panels at once
  -- is a legitimate thing to want.
  local old = panel_of[idx]
  if old then dnp.ui_close(e, old) end
  -- Cleared BEFORE the open, so a failed open leaves no id behind. It used to
  -- keep the old one on failure, which is an id naming a panel that has already
  -- been closed -- so every later ui_set went to a window that was not there,
  -- and the panel was simply gone with nothing pointing at the reason.
  panel_of[idx] = nil

  local id = dnp.ui_open(e, build(e))
  if id ~= 0 then panel_of[idx] = id end
end

-- Opened when somebody arrives, and not before: dnp.ui_open needs a session to
-- send to, and on_tick is the first place this script can know there is one.
local seen = {}
local shown_hp = {}

function on_tick()
  -- FORGET PLAYERS WHO ARE NOT HERE.
  --
  -- Every level runs its own copy of this script with its own `seen`, and a
  -- player who leaves for another map leaves their entry behind as true. Come
  -- back later -- to the same map, or into the same agent slot as somebody else
  -- -- and this script believes it has already opened a panel for them, so it
  -- never opens one. Which is a debug panel that disappears and does not come
  -- back.
  --
  -- Rebuilt from who is actually here rather than hooked to a departure: there
  -- is no "player left" event, and inventing one to serve a bookkeeping table
  -- would be the wrong shape.
  local here = {}
  for _, e in ipairs(dnp.players()) do here[e:index()] = true end
  for idx in pairs(seen) do
    if not here[idx] then
      seen[idx] = nil
      panel_of[idx] = nil
      shown_hp[idx] = nil
    end
  end

  for _, e in ipairs(dnp.players()) do
    local idx = e:index()
    if not seen[idx] then
      seen[idx] = true
      show(e)
      shown_hp[idx] = nil
    end

    -- HEALTH FOLLOWS. A panel is built from state at the moment it opens and
    -- nothing pushes changes into one, so a health slider sat at whatever it
    -- was when the panel appeared while monsters shot the number down behind
    -- it. dnp.ui_set changes the widget in place -- no reopen, no new id, no
    -- window flickering every time somebody is hit.
    --
    -- Only when it CHANGES: this runs sixty times a second, and a reliable
    -- control message per tick per player to say the same number again is a
    -- waste of the channel that carries map data.
    local hp = e:hp()
    if hp ~= nil and hp ~= shown_hp[idx] and panel_of[idx] ~= nil then
      shown_hp[idx] = hp
      dnp.ui_set(e, panel_of[idx], W_HP, hp)
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
    damage = value
    if mobs then mobs.player_damage = value end
  elseif widget == W_REACH then
    set_param(e, "reach", "attack_reach", fx.frac(value, 100))
  elseif widget == W_INNER then
    set_param(e, "inner", "attack_min_reach", fx.frac(value, 100))
  elseif widget == W_ARC then
    set_param(e, "arc", "attack_half_arc", fx.frac(value, 100))
  elseif widget == W_FROM then
    set_param(e, "from", "attack_sweep_from", fx.frac(value, 100))
  elseif widget == W_TO then
    set_param(e, "to", "attack_sweep_to", fx.frac(value, 100))
  elseif widget == W_DUR then
    set_param(e, "dur", "attack_ticks", fx.int(value))
  elseif widget == W_COOL then
    set_param(e, "cool", "attack_cooldown", fx.int(value))
  elseif widget == W_CDIST  then cam_of(idx).distance = value;   push_camera(e)
  elseif widget == W_CPITCH then cam_of(idx).pitch = value;      push_camera(e)
  elseif widget == W_CSMOOTH then cam_of(idx).smoothing = value;  push_camera(e)
  elseif widget == W_CACCEL then cam_of(idx).accel = value;      push_camera(e)
  elseif widget == W_CLEAD  then cam_of(idx).lead = value;       push_camera(e)
  elseif widget == W_CLEADS then cam_of(idx).lead_speed = value; push_camera(e)
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
