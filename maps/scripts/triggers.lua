-- The trigger runtime: a small interpreter over a table of triggers.
--
-- A map's behaviour, authored rather than programmed. A trigger is
--
--     event  ->  conditions (all must hold)  ->  actions (in order)
--
-- and all three come from maps/scripts/trigger_vocab.lua, which is the
-- normative vocabulary. Read docs/TRIGGERS.md first if you are writing one;
-- this file is how it runs.
--
-- WHY DATA AND NOT GENERATED LUA. Three reasons, in order of weight:
--
--   1. The editor has to render and re-edit triggers. Generated source cannot
--      be round-tripped once a human has touched it.
--   2. The sandbox has a per-callback and per-tick instruction budget
--      (server/src/lua/vm.cpp). A bounded interpreter over a bounded table
--      keeps that enforceable; arbitrary generated code does not.
--   3. It already fits. The engine dispatches named hooks as LISTS of
--      handlers, so this file registers one handler per event and dispatches
--      from the table. Nothing in the engine changed to make this work.
--
-- THERE IS NO `wait`. Nothing here suspends, so nothing can be resumed:
-- `coroutine` is absent from the sandbox by design, because Lua 5.1's hooks are
-- per-thread and a coroutine runs with the instruction budget disarmed --
-- a runaway inside one is unkillable. Time is a tick deadline: `run_after`
-- schedules a trigger for tick + n and on_tick drains what is due. An author
-- who expects `wait` will write a deadlock, so the vocabulary does not offer
-- the word at all.
--
-- A map declares this runtime, the vocabulary it validates against, and its own
-- trigger table, in that order:
--
--     script scripts/trigger_vocab.lua
--     script scripts/triggers.lua
--     script mymap_triggers.lua          -- calls triggers.load{...}
--

triggers = triggers or {}

local M = triggers

if trigger_vocab == nil then
  -- Named as the missing DECLARATION, not as a missing global: the map file is
  -- where the fix goes, and "attempt to index a nil value" points at this line
  -- instead.
  error("triggers.lua needs the vocabulary -- add `script scripts/trigger_vocab.lua` " ..
        "to the map, BEFORE scripts/triggers.lua")
end

M.name = "?"
M.list = {}            -- every trigger, in declaration order
M.panels = {}          -- panel layouts, by name

local by_event = {}    -- event name -> { trigger, ... }, declaration order
local by_name = {}     -- trigger name -> trigger
-- Named state this trigger table keeps. ONE table, not two: `counter_add` and
-- `set_property` are two verbs over the same store, so a counter and a property
-- of the same name are the same thing rather than two that silently shadow each
-- other. Per map, never per player -- see docs/TRIGGERS.md.
local state = {}
local sectors_by_tag = {}   -- tag -> { sector, ... }, built once at load
local busy = {}        -- sector index -> the tick its motion settles
-- One shared empty list, so the hot path allocates nothing for a trigger that
-- has no sequence -- which is nearly all of them.
local EMPTY = {}
local timers = {}      -- pending run_after: { tick, trigger, ev }
local open_panels = {} -- agent index -> panel name -> ui_id
-- Custom code, compiled once at LOAD. Keyed by the action table itself, so
-- nothing is written back into the data an editor round-trips.
local compiled = {}
local loaded = false

-- How deep `run` may nest before the runtime refuses.
--
-- Not a stack-overflow guard -- the instruction budget already catches runaway
-- work. It is a DIAGNOSTIC: two triggers that run each other are a mistake an
-- author makes in the editor, and "trigger A ran trigger B ran trigger A" with
-- names in it is an answer, where a budget kill is just a dead callback.
local MAX_RUN_DEPTH = 8
local depth = 0

-- How many complaints to print before falling silent. A few, because the useful
-- case is an author saying "this switch does nothing" and the log answering
-- with a trigger name -- and a line per press would bury it.
local MAX_REPORTS = 8
local reported = 0

local ZERO = fx.int(0)
local ZERO3 = vec3.new(ZERO, ZERO, ZERO)

local function say(fmt, ...)
  print(("[triggers] %s: " .. fmt):format(M.name, ...))
end

local function complain(fmt, ...)
  if reported >= MAX_REPORTS then return end
  reported = reported + 1
  say(fmt, ...)
end

-- The event currently being dispatched.
--
-- One table, reused, rather than one per dispatch: on_tick and on_post_tick run
-- sixty times a second per level, and a fresh table each time is sixty
-- allocations a second per level with nobody connected. That is the shape of
-- the cost that produced 39 ms in a tick on empty levels.
-- `entity` is who DID it; `target` is what it was done to, when the event
-- carries one -- the prop you walked into, the thing your swing reached. An
-- action says which of the two it addresses with its `who` argument, because
-- "set the animation" on an attack is ambiguous otherwise and guessing wrong is
-- silent.
local EV = { entity = nil, target = nil, side = nil, sector = nil,
             crush_kind = nil, ui_id = nil, widget_id = nil, value = nil,
             running = nil }

local function clear_event()
  EV.entity, EV.target, EV.side, EV.sector = nil, nil, nil, nil
  EV.crush_kind, EV.ui_id, EV.widget_id, EV.value = nil, nil, nil, nil
  EV.running = nil
end

-- ------------------------------------------------------------------ arguments

-- An integer, an exact decimal string, or an exact fraction {n, d}.
--
-- Never a Lua float. fx.parse refuses anything not exactly representable, which
-- is the point: a speed written as 0.1 in a double is not the number the
-- simulation would use, and a door running at a slightly different speed on two
-- machines is a desync with no obvious cause.
--
-- The FRACTION form is not decoration. A Doom door opens four map units below
-- the ceiling above it, and at 48 units to ours that is 4/48 -- a number with no
-- finite decimal. Without {4, 48} an author would have to write 0.083333 and get
-- a door that is a hair off the one every converted map already has.
local function as_fx(v)
  if type(v) == "string" then return fx.parse(v) end
  if type(v) == "table" then return fx.frac(v[1], v[2]) end
  return fx.int(v)
end

local function as_int(v) return v end

-- Which sector an argument names: an index, "event" (the sector the event
-- carried) or "behind" (the neighbour of the side the event carried).
--
-- "behind" is how a Doom door is found: the switch is a side in the room, and
-- the thing that moves is the sector on the other side of it.
local function sector_of(v)
  if v == nil then return nil end
  if type(v) == "number" then return dnp.sector(v) end
  if v == "event" then return EV.sector end
  if v == "behind" then
    if EV.side == nil then return nil end
    local sd = dnp.side(EV.side)
    return sd and sd.neighbor or nil
  end
  return nil
end

-- Every sector an action addresses: one, or all of a tag.
--
-- The tag index is built ONCE at load. dnp.sectors() allocates a handle per
-- sector, and a converted Doom map has eight hundred of them -- scanning that
-- per switch press is the same cost mistake, moved from the tick loop into the
-- door.
-- A FRESH table per call, deliberately.
--
-- A shared scratch would be one allocation cheaper and a re-entrancy trap: the
-- moment an action list nests -- `run` inside a trigger whose first action
-- addressed a tag -- the inner call would clobber the list the outer loop is
-- walking, and the symptom would be one sector of a tagged set quietly not
-- moving. The expensive part is the tag INDEX, and that is built once at load.
local function targets_of(args)
  local out = {}
  if args.tag ~= nil then
    local list = sectors_by_tag[args.tag]
    if list then
      for i = 1, #list do out[i] = list[i] end
    end
  else
    local s = sector_of(args.sector)
    if s then out[1] = s end
  end
  return out
end

-- --------------------------------------------------------------- sector maths
--
-- DUPLICATED FROM maps/scripts/doom.lua, knowingly, and the duplication is
-- controlled rather than denied.
--
-- doom.lua computes the same neighbour extremes for converted Doom linedefs.
-- Making one call the other would either drag the whole Doom line-action
-- machinery into every trigger map or move both into a third file that the 32
-- converted maps would then have to declare. What is actually load-bearing is
-- that the two agree on the NUMBER, so the end-to-end test asserts the
-- trigger-driven door opens to exactly the height doom.lua's door opens to on
-- the same geometry. A drift here fails that, loudly.

-- A sector's UNMOVED heights. floor()/ceil() report base + offset, and
-- everything below works in offsets, so the base has to be recovered rather
-- than assumed: a lift asked to return "to its floor" after an earlier trigger
-- left it down would otherwise return to where it already is.
local function base_of(s)
  local off = s:offset()
  if off == nil then return s:floor(), s:ceil(), ZERO3 end
  return s:floor() - off.z, s:ceil() - off.z, off
end

local function lower(a, b) return a < b end
local function higher(a, b) return a > b end

local function neighbour_extreme(s, field, pick)
  local best = nil
  for _, n in ipairs(s:neighbors()) do
    local v = (field == "floor") and n:floor() or n:ceil()
    if v ~= nil and (best == nil or pick(v, best)) then best = v end
  end
  return best
end

-- Resolved AT TRIGGER TIME, never baked: a staircase raised by an earlier
-- trigger changes what "the next higher floor" means, and Doom's own actions
-- are defined that way.
local function resolve_height(s, to, height)
  local floor, ceil = base_of(s)
  if to == nil or to == "base_floor" then return floor end
  if to == "base_ceiling" then return ceil end
  if to == "absolute" then return height end
  if to == "lowest_neighbour_floor" then
    return neighbour_extreme(s, "floor", lower) or floor
  end
  if to == "highest_neighbour_floor" then
    return neighbour_extreme(s, "floor", higher) or floor
  end
  if to == "lowest_neighbour_ceiling" then
    return neighbour_extreme(s, "ceil", lower) or ceil
  end
  if to == "highest_neighbour_ceiling" then
    return neighbour_extreme(s, "ceil", higher) or ceil
  end
  if to == "next_higher_floor" then
    -- The LOWEST neighbour floor still above ours; if none is, the floor stays
    -- where it is rather than jumping to an extreme.
    local best = nil
    for _, n in ipairs(s:neighbors()) do
      local v = n:floor()
      if v ~= nil and v > floor and (best == nil or v < best) then best = v end
    end
    return best or floor
  end
  return floor
end

local function abs_fx(v) if v < ZERO then return -v end return v end

-- Ticks to travel `dist` at `speed`, never zero.
--
-- Zero would be a teleport, and a door that teleports shut crushes whoever is
-- under it with no warning. Less dramatically, a duration of 0 makes the
-- parametric evaluation return the endpoint immediately, so the motion is
-- invisible and the door merely "is" open.
local function ticks_for(dist, speed)
  local n = (abs_fx(dist) / speed).int
  if n < 1 then n = 1 end
  return n
end

-- ------------------------------------------------------------------- entities

-- Which of the event's two handles an action addresses.
--
-- No type test anywhere in this file, and that is not an oversight. An entity
-- handle and a prop handle answer the same verbs -- :name(), :index(),
-- :kind(), :set_animation(), :set_solid(), :despawn() -- so an action that
-- addresses "the target" works whether the swing reached a barrel or a
-- zombieman without asking which. Where the two genuinely differ the binding
-- says so: entity:despawn() raises for a player, because a script deleting a
-- person is a mistake worth a stack trace rather than a silent nothing.
local function who_of(args)
  if args.who == "target" then return EV.target end
  return EV.entity
end

-- ----------------------------------------------------------------- conditions
--
-- Every one of these is a pure read. None allocates an entity, none mutates,
-- and none touches dnp.random -- see docs/TRIGGERS.md for why `chance` is not
-- a condition.

local COND = {}

COND.side_is = function(args)
  return EV.side ~= nil and EV.side == args.side
end

COND.sector_is = function(args)
  local want = sector_of(args.sector)
  return EV.sector ~= nil and want ~= nil and EV.sector == want
end

COND.sector_tag_is = function(args)
  local s = args.sector ~= nil and sector_of(args.sector) or EV.sector
  if s == nil then return false end
  return s:tag() == args.tag
end

COND.entity_is_player = function()
  -- A player is an agent with no catalogue kind: nobody placed it, so it walked
  -- in. The same test the monster scripts make, and the one the binding makes
  -- for dnp.players().
  if EV.entity == nil then return false end
  local k = EV.entity:kind()
  return k ~= nil and k == 0
end

COND.entity_kind_is = function(args)
  if EV.entity == nil then return false end
  return EV.entity:name() == args.name
end

COND.entity_in_sector_tag = function(args)
  if EV.entity == nil then return false end
  local s = EV.entity:sector()
  return s ~= nil and s:tag() == args.tag
end

COND.target_name_is = function(args)
  local t = EV.target
  if t == nil then return false end
  -- The catalogue's NAME, so renumbering assets/props.txt cannot quietly point
  -- a trigger at something else. A player has no catalogue entry and answers
  -- nil here, which is the correct false.
  return t:name() == args.name
end

COND.crush_kind_is = function(args)
  return EV.crush_kind ~= nil and EV.crush_kind == args.kind
end

COND.widget_is = function(args)
  if EV.widget_id == nil or EV.widget_id ~= args.widget then return false end
  if args.value == nil then return true end
  return EV.value == args.value
end

COND.sector_at_rest = function(args)
  local s = sector_of(args.sector)
  if s == nil then return false end
  local until_tick = busy[s:index()]
  return until_tick == nil or until_tick <= dnp.tick()
end

COND.sector_offset_at_least = function(args)
  local s = sector_of(args.sector)
  if s == nil then return false end
  local off = s:offset()
  local z = off and off.z or ZERO
  return z >= as_fx(args.z)
end

COND.property_is = function(args)
  return state[args.name] == args.value
end

COND.counter_at_least = function(args)
  local v = state[args.counter]
  -- A counter that is holding a string is not "less than" anything: comparing
  -- them raises in Lua, and a raise here would take the whole callback with it.
  -- False is the honest answer, and set_property is where that value came from.
  if type(v) ~= "number" then return v == nil and 0 >= args.value or false end
  return v >= args.value
end

COND.trigger_enabled = function(args)
  local t = by_name[args.trigger]
  return t ~= nil and t.enabled ~= false
end

COND.tick_at_least = function(args)
  return dnp.tick() >= args.tick
end

-- -------------------------------------------------------------------- actions
--
-- Every action returns whether it DID something. That answer is what `once`
-- spends itself on: a one-shot that fired while its door was already moving is
-- not spent. Doom behaves the same way, and for a reason worth keeping -- a
-- player who walked over a lift trigger while the lift was still travelling
-- would otherwise be locked out of the room permanently.

local ACT = {}

-- One parametric move, on one sector. Returns true if it started.
local function move_one(s, args)
  local idx = s:index()
  if not args.interrupt and busy[idx] ~= nil and busy[idx] > dnp.tick() then
    return false
  end

  local surface = args.surface or "both"
  local floor, ceil, off = base_of(s)
  local base = (surface == "ceiling") and ceil or floor

  local height = resolve_height(s, args.to, args.height ~= nil and as_fx(args.height) or nil)
  if args.offset ~= nil then height = height + as_fx(args.offset) end

  -- AND SIDEWAYS. `dx`/`dy` move the footprint; the engine has carried riders
  -- and shoved bodies horizontally since sector motion existed, and this
  -- vocabulary had no way to ask for it -- so every sliding platform in the
  -- repository was raw Lua.
  local dx = args.dx ~= nil and as_fx(args.dx) or ZERO
  local dy = args.dy ~= nil and as_fx(args.dy) or ZERO

  local from = off
  local to = vec3.new(off.x + dx, off.y + dy, height - base)

  -- A SLIDE ALONE DOES NOT ALSO COME HOME. `to` and `height` are both absent
  -- for a pure sideways move, and resolve_height then answers "the base
  -- floor" -- which is the right default for a lift with nothing else said
  -- and quite wrong here: a platform already raised would drop back as it
  -- travelled, which nobody asked for and no argument would have prevented.
  if args.to == nil and args.height == nil and (args.dx ~= nil or args.dy ~= nil) then
    to = vec3.new(to.x, to.y, from.z)
  end

  -- ALONG THE LINE ACTUALLY TRAVELLED, so `speed` means the same thing for a
  -- lift, a slider and a diagonal. dnp.length is the engine's own 2D length --
  -- it ignores z, deliberately, because almost every caller wants plan
  -- distance -- so the third axis comes from a second call on the result.
  -- Two exact fixed-point roots rather than one approximation nobody can
  -- reproduce.
  local plan = dnp.length(vec3.new(to.x - from.x, to.y - from.y, ZERO))
  local n = ticks_for(dnp.length(vec3.new(plan, to.z - from.z, ZERO)), as_fx(args.speed))

  -- AND A TURN, on the same phase.
  --
  -- Its own speed, in degrees per tick, because "units per tick" and "degrees
  -- per tick" are not the same quantity and one argument meaning both is how
  -- an author ends up tuning a number that does two things. When a move does
  -- both, the LONGER of the two durations wins: they share one phase, so the
  -- alternative is one of them arriving and then waiting.
  local spin = args.spin ~= nil and as_fx(args.spin) or nil
  local pivot = nil
  if spin ~= nil then
    local sn = ticks_for(spin, as_fx(args.spin_speed or 1))
    if sn > n then n = sn end
    if args.pivot_x ~= nil or args.pivot_y ~= nil then
      pivot = vec3.new(as_fx(args.pivot_x or 0), as_fx(args.pivot_y or 0), ZERO)
    end
  end

  local loop = args.loop or "once"

  if not s:move_parametric(from, to, n, loop, surface, spin, pivot) then return false end

  -- A looping motion never settles, so it is never "busy" -- otherwise a
  -- pingpong crusher would refuse every later trigger aimed at it forever.
  if loop == "once" then busy[idx] = dnp.tick() + n else busy[idx] = nil end
  return true
end

ACT.sector_move = function(args)
  local any = false
  local list = targets_of(args)
  for i = 1, #list do
    if move_one(list[i], args) then any = true end
  end
  return any
end

ACT.sector_stop = function(args)
  local any = false
  local list = targets_of(args)
  for i = 1, #list do
    local s = list[i]
    local _, _, off = base_of(s)
    -- Freeze where it is: a zero-length parametric move starting now.
    if s:move_parametric(off, off, 1, "once", "both") then
      busy[s:index()] = nil
      any = true
    end
  end
  return any
end

ACT.sector_offset = function(args)
  local any = false
  local list = targets_of(args)
  local z = as_fx(args.z)
  for i = 1, #list do
    local s = list[i]
    if s:set_offset(vec3.new(ZERO, ZERO, z)) then any = true end
  end
  return any
end

ACT.teleport = function(args)
  local e = who_of(args)
  if e == nil then return false end
  e:teleport(vec3.new(as_fx(args.x), as_fx(args.y), as_fx(args.z)))
  return true
end

ACT.set_hp = function(args)
  local e = who_of(args)
  if e == nil then return false end
  if args.add ~= nil then
    local hp = e:hp()
    if hp == nil then return false end
    e:set_hp(hp + args.add)
  else
    e:set_hp(args.value)
  end
  return true
end

ACT.set_solid = function(args)
  local e = who_of(args)
  if e == nil then return false end
  e:set_solid(args.solid)
  return true
end

ACT.set_animation = function(args)
  local e = who_of(args)
  if e == nil then return false end
  e:set_animation(args.state)
  return true
end

ACT.despawn = function(args)
  local e = who_of(args)
  if e == nil then return false end
  return e:despawn()
end

ACT.spawn_prop = function(args)
  return dnp.spawn_prop(args.name,
                        vec3.new(as_fx(args.x), as_fx(args.y), as_fx(args.z))) ~= nil
end

ACT.spawn_enemy = function(args)
  return dnp.spawn_enemy(args.name,
                         vec3.new(as_fx(args.x), as_fx(args.y), as_fx(args.z))) ~= nil
end

ACT.add_effect = function(args)
  local e = who_of(args)
  if e == nil then return false end
  return e:add_effect(args.param, args.op, as_fx(args.value), args.ticks) ~= 0
end

ACT.change_map = function(args)
  if EV.entity == nil then return false end
  return EV.entity:change_map(args.level) == true
end

ACT.music = function(args)
  -- The event's player, or everyone in the level. A cue for a player-less
  -- event (a timer, a monster's death with nobody near) is nothing to send,
  -- and saying so is what `false` is for.
  local who = nil
  if not args.everyone then
    if EV.entity == nil then return false end
    who = EV.entity
  end
  dnp.music(who, args.cue, args.fade, args.at)
  return true
end

ACT.screen = function(args)
  -- The event's player, or everyone in the level -- the same rule as `music`,
  -- and for the same reason: a fade for a player-less event is nothing to send.
  local who = nil
  if not args.everyone then
    if EV.entity == nil then return false end
    who = EV.entity
  end
  -- Only the fields the author gave. A table built from every field would send
  -- a `letterbox = 0` with every fade and raise the bars nobody asked for.
  local t = {}
  -- The author's numbers, unscaled. dnp.screen multiplies by a thousand on the
  -- way to the wire and the client divides by it again, so what arrives is what
  -- was written: an opacity in 0..1 and a duration in milliseconds. Same
  -- convention as `camera` beside it -- pre-scaling here would have been a
  -- second one, and the two would have disagreed by a factor of a thousand in
  -- whichever field somebody read from the other's example.
  if args.fade ~= nil then t.fade = (as_fx(args.fade) * fx.int(1000)).int / 1000 end
  if args.fade_ms ~= nil then t.fade_ms = args.fade_ms end
  if args.letterbox ~= nil then t.letterbox = (as_fx(args.letterbox) * fx.int(1000)).int / 1000 end
  if args.letterbox_ms ~= nil then t.letterbox_ms = args.letterbox_ms end
  if next(t) == nil then return false end
  dnp.screen(who, t)
  return true
end

ACT.ui_open = function(args)
  if EV.entity == nil then return false end
  local panel = M.panels[args.panel]
  if panel == nil then return false end
  local who = EV.entity:index()
  if who == nil then return false end
  local id = dnp.ui_open(EV.entity, panel)
  if id == 0 then return false end
  open_panels[who] = open_panels[who] or {}
  open_panels[who][args.panel] = id
  return true
end

ACT.ui_close = function(args)
  if EV.entity == nil then return false end
  local who = EV.entity:index()
  if who == nil then return false end
  local mine = open_panels[who]
  local id = mine and mine[args.panel] or nil
  if id == nil then return false end
  dnp.ui_close(EV.entity, id)
  mine[args.panel] = nil
  return true
end

ACT.camera = function(args)
  if EV.entity == nil then return false end
  -- The binding takes THOUSANDTHS-scaled plain numbers, not fx: a camera is not
  -- simulation state. It still needs a fixed scale, so the vocabulary types the
  -- value as fx and it is converted here rather than letting an author write a
  -- double that means something different on two machines.
  local v = as_fx(args.value)
  local milli = (v * fx.int(1000)).int
  local t = {}
  t[args.field] = milli / 1000
  dnp.camera(EV.entity, t)
  return true
end

ACT.vfx = function(args)
  return dnp.vfx(args.kind, vec3.new(as_fx(args.x), as_fx(args.y), as_fx(args.z)),
                 0, args.param or 0)
end

ACT.counter_add = function(args)
  local v = state[args.counter]
  state[args.counter] = (type(v) == "number" and v or 0) + (args.by or 1)
  return true
end

ACT.set_property = function(args)
  state[args.name] = args.value
  return true
end

-- Custom code: the escape hatch, and the only place a trigger stops being a
-- bounded vocabulary.
--
-- COMPILED AT LOAD, never here. A `loadstring` per firing would be a compile
-- per button press, and -- much worse -- a syntax error would surface the first
-- time somebody pressed the switch rather than when the map was loaded. Now it
-- is a load-time problem naming the trigger, like every other mistake in the
-- table.
--
-- NOT WRAPPED IN pcall, deliberately. The VM already runs every callback under
-- one and logs what raised; wrapping again here would also swallow the
-- INSTRUCTION BUDGET's kill, which is re-armed at 1 precisely so that a script
-- catching its own error cannot carry on burning the frame. An escape hatch
-- that could defeat the budget would not be an escape hatch, it would be a way
-- to stall the tick.
--
-- Returning `false` means "did nothing", which is what a `once` spends itself
-- on. Anything else -- including returning nothing at all -- counts as having
-- acted.
ACT.lua = function(args)
  local fn = compiled[args]
  if fn == nil then return false end
  return fn(EV) ~= false
end

ACT.counter_set = function(args)
  state[args.counter] = args.value
  return true
end

ACT.enable = function(args)
  local t = by_name[args.trigger]
  if t == nil then return false end
  t.enabled = true
  return true
end

ACT.disable = function(args)
  local t = by_name[args.trigger]
  if t == nil then return false end
  t.enabled = false
  return true
end

-- Forward declaration: run_one and ACT.run are mutually recursive.
local run_one

ACT.run = function(args)
  local t = by_name[args.trigger]
  if t == nil then return false end
  if depth >= MAX_RUN_DEPTH then
    -- Named on both sides, because a cycle is what this usually is and one name
    -- does not identify a cycle.
    complain("'%s' ran '%s' %d deep -- refusing; is there a cycle?",
             EV.running or "?", args.trigger, depth)
    return false
  end
  depth = depth + 1
  local did = run_one(t)
  depth = depth - 1
  return did
end

ACT.run_after = function(args)
  local t = by_name[args.trigger]
  if t == nil then return false end
  -- FROM WHEN. A door waits ninety ticks after it has finished opening, not
  -- ninety ticks after the button; measuring from the press would start the
  -- close while the ceiling was still on its way up, and the faster the door
  -- the wronger it would be. `after` names the sector whose settle tick the
  -- wait runs from -- still a tick deadline, still nothing suspended.
  local base = dnp.tick()
  if args.after ~= nil then
    local s = sector_of(args.after)
    local settles = s and busy[s:index()] or nil
    if settles ~= nil and settles > base then base = settles end
  end
  -- The event is CAPTURED, not referenced: EV is reused, so by the time this
  -- fires it describes some other event entirely. The entity is captured as a
  -- handle, which is generational -- if it died in the meantime the actions
  -- that need it become defined no-ops rather than touching a reused slot.
  timers[#timers + 1] = {
    tick = base + args.ticks,
    trigger = t,
    entity = EV.entity, target = EV.target, side = EV.side, sector = EV.sector,
    crush_kind = EV.crush_kind,
    ui_id = EV.ui_id, widget_id = EV.widget_id, value = EV.value,
  }
  return true
end

ACT.log = function(args)
  say("%s: %s", EV.running or "?", args.text)
  return true
end

-- ------------------------------------------------------------------- dispatch

local function conditions_hold(t)
  local list = t.conditions
  if list == nil then return true end
  for i = 1, #list do
    local c = list[i]
    local fn = COND[c.c]
    local ok = fn(c)
    if c.negate then ok = not ok end
    if not ok then return false end
  end
  return true
end

-- One trigger, conditions and all. Returns whether any action did something.
run_one = function(t)
  if t.enabled == false then return false end
  if t.once and t.spent then return false end
  if not conditions_hold(t) then return false end

  local previous = EV.running
  EV.running = t.name

  local any = false
  local list = t.actions or EMPTY
  for i = 1, #list do
    local a = list[i]
    if ACT[a.a](a) then any = true end
  end

  -- The sequence. `when` is a DEADLINE measured from this firing, not a
  -- countdown -- the mob vocabulary's rule, under a different word because
  -- `music` already owns `at` here.
  --
  -- A beat due now runs NOW rather than next tick: when = 0 means "with the rest
  -- of this trigger", and pushing it through the queue would put the first
  -- frame of every cutscene one tick after the thing that started it.
  local seq = t.sequence or EMPTY
  if #seq > 0 then
    local now = dnp.tick()
    for i = 1, #seq do
      local b = seq[i]
      if b.when <= 0 then
        if ACT[b.a](b) then any = true end
      else
        -- The event is CAPTURED, exactly as run_after captures it and for the
        -- same reason: EV is reused, so by the time a beat fires it describes
        -- some other event entirely.
        timers[#timers + 1] = {
          tick = now + b.when,
          action = b,
          entity = EV.entity, target = EV.target, side = EV.side, sector = EV.sector,
          crush_kind = EV.crush_kind,
          ui_id = EV.ui_id, widget_id = EV.widget_id, value = EV.value,
        }
        -- Queuing IS doing something. A `once` cutscene whose first beat is at
        -- tick 30 must still be spent, or it restarts on every event until one
        -- of its beats happens to report true.
        any = true
      end
    end
  end

  EV.running = previous

  -- Spent only if it actually did something. A one-shot that fired into a door
  -- that was already open has not been used up, and from inside the game a
  -- trigger burnt that way is indistinguishable from one that never worked.
  if any and t.once then t.spent = true end
  return any
end

local function dispatch(event)
  if not loaded then return end
  local list = by_event[event]
  if list == nil then return end
  for i = 1, #list do
    run_one(list[i])
  end
end

-- -------------------------------------------------------------------- timers

-- Due deadlines, drained on on_tick.
--
-- The queue is SWAPPED OUT before anything runs, because a timer's actions
-- queue more timers: opening a door schedules its close. Iterating the live
-- table and assigning the survivors back over it drops every entry a callback
-- added -- `for i = 1, #timers` fixes the limit once, so the new entry is never
-- visited and the reassignment throws it away. The symptom is a door that opens
-- and stays open, with no error anywhere.
local function drain_timers(tick)
  if #timers == 0 then return end
  local due = timers
  timers = {}
  for i = 1, #due do
    local d = due[i]
    if d.tick <= tick then
      clear_event()
      EV.entity, EV.target, EV.side, EV.sector = d.entity, d.target, d.side, d.sector
      EV.crush_kind = d.crush_kind
      EV.ui_id, EV.widget_id, EV.value = d.ui_id, d.widget_id, d.value
      -- A sequence beat is ONE action, already validated and with no conditions
      -- of its own: the trigger's conditions were checked when it fired, and
      -- re-checking them here would mean a cutscene that stops halfway because
      -- the player walked out of the sector that started it.
      if d.action ~= nil then ACT[d.action.a](d.action)
      else run_one(d.trigger) end
      clear_event()
    else
      timers[#timers + 1] = d
    end
  end
end

-- ---------------------------------------------------------------------- hooks
--
-- Every event gets a handler, whether the loaded table uses it or not.
--
-- Not laziness: the VM harvests a chunk's hook globals as soon as THAT CHUNK
-- finishes, and this file runs before the map's data file has called load().
-- At the moment the handlers must exist, nothing here knows which events the
-- map uses. So all nine are registered and the unused ones return on a nil
-- lookup -- one table index per event, which is less than the cost of finding
-- out any other way.

function on_tick()
  drain_timers(dnp.tick())
  dispatch("on_tick")
end

function on_post_tick() dispatch("on_post_tick") end

function on_interact(entity, side, sector)
  clear_event()
  EV.entity, EV.side, EV.sector = entity, side, sector
  dispatch("on_interact")
  clear_event()
end

function on_cross(entity, side, sector)
  clear_event()
  EV.entity, EV.side, EV.sector = entity, side, sector
  dispatch("on_cross")
  clear_event()
end

function on_touch(entity, prop)
  clear_event()
  EV.entity, EV.target = entity, prop
  dispatch("on_touch")
  clear_event()
end

function on_prop_use(entity, prop)
  clear_event()
  EV.entity, EV.target = entity, prop
  dispatch("on_prop_use")
  clear_event()
end

function on_attack(entity, target)
  clear_event()
  -- `target` is a prop or an agent, and nothing here needs to know which; see
  -- who_of.
  EV.entity, EV.target = entity, target
  dispatch("on_attack")
  clear_event()
end

function on_crush(entity, sector, kind)
  clear_event()
  EV.entity, EV.sector, EV.crush_kind = entity, sector, kind
  dispatch("on_crush")
  clear_event()
end

function on_ui_event(entity, ui_id, widget_id, value)
  clear_event()
  EV.entity, EV.ui_id, EV.widget_id, EV.value = entity, ui_id, widget_id, value
  dispatch("on_ui_event")
  clear_event()
end

-- ----------------------------------------------------------------- validation
--
-- Fail CLOSED, and say everything that is wrong rather than the first thing.
--
-- A trigger table with a typo in one action name is a map that half works, and
-- a map that half works is harder to diagnose than one that refuses to load and
-- names the line. This is the same decision doom.lua makes when a map's line
-- table disagrees with its geometry.

local function index_vocab()
  local ev, cond, act = {}, {}, {}
  for _, e in ipairs(trigger_vocab.events) do ev[e.name] = e end
  for _, c in ipairs(trigger_vocab.conditions) do cond[c.name] = c end
  for _, a in ipairs(trigger_vocab.actions) do act[a.name] = a end
  return ev, cond, act
end

-- Every vocabulary entry has an implementation, and every implementation is in
-- the vocabulary.
--
-- THE control for the one-implementation rule. The document is written from the
-- vocabulary and the editor's UI is generated from it, so a handler this file
-- grew quietly would be invisible to both -- and a vocabulary entry with no
-- handler is an action the editor offers and the runtime cannot perform. Both
-- directions are checked, because only checking one leaves the other free.
local function check_vocabulary_matches_implementation(cond_v, act_v)
  local bad = 0
  -- `when` belongs to a sequence beat and to nothing else. An action declaring
  -- an argument by that name would declare one no author could reach inside a
  -- sequence -- a Lua table cannot hold the key twice -- and the failure would
  -- be a silently ignored argument rather than an error. Checked here, at load,
  -- because this is the file that knows the reservation exists.
  for name, spec in pairs(act_v) do
    for _, arg in ipairs(spec.args or {}) do
      if arg.name == "when" then
        bad = bad + 1
        say("action '%s' declares an argument 'when', which a sequence beat "
            .. "reserves for its own tick", name)
      end
    end
  end
  for name in pairs(cond_v) do
    if COND[name] == nil then
      bad = bad + 1
      say("the vocabulary offers condition '%s' and this runtime cannot evaluate it", name)
    end
  end
  for name in pairs(COND) do
    if cond_v[name] == nil then
      bad = bad + 1
      say("this runtime implements condition '%s' and the vocabulary does not list it", name)
    end
  end
  for name in pairs(act_v) do
    if ACT[name] == nil then
      bad = bad + 1
      say("the vocabulary offers action '%s' and this runtime cannot perform it", name)
    end
  end
  for name in pairs(ACT) do
    if act_v[name] == nil then
      bad = bad + 1
      say("this runtime implements action '%s' and the vocabulary does not list it", name)
    end
  end
  return bad
end

-- One condition's or action's arguments against its vocabulary entry.
local function check_args(spec, given, where, problems)
  local seen_group = {}
  for _, arg in ipairs(spec.args) do
    local v = given[arg.name]
    if v == nil then
      if arg.one_of == nil and not arg.optional then
        problems[#problems + 1] =
          ("%s: '%s' needs an argument '%s'"):format(where, spec.name, arg.name)
      end
    else
      if arg.one_of ~= nil then
        if seen_group[arg.one_of] ~= nil then
          problems[#problems + 1] =
            ("%s: '%s' was given both '%s' and '%s'; they are alternatives")
            :format(where, spec.name, seen_group[arg.one_of], arg.name)
        end
        seen_group[arg.one_of] = arg.name
      end
      if arg.type == "enum" then
        local ok = false
        for _, allowed in ipairs(arg.values) do if v == allowed then ok = true end end
        if not ok then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' is '%s', which is not one of its values")
            :format(where, spec.name, arg.name, tostring(v))
        end
      elseif arg.type == "fx" then
        -- Caught HERE rather than at fire time. An unparseable speed inside a
        -- door is an error at midnight in a callback nobody is watching; the
        -- same mistake at load time names the trigger.
        if type(v) == "string" then
          fx.parse(v)
        elseif type(v) == "table" then
          if type(v[1]) ~= "number" or type(v[2]) ~= "number" or v[2] == 0 then
            problems[#problems + 1] =
              ("%s: '%s' argument '%s' is a fraction and must be {numerator, denominator}")
              :format(where, spec.name, arg.name)
          end
        elseif type(v) == "number" then
          -- A NON-INTEGRAL Lua number is the loud failure the fx type exists
          -- for: 0.1 is not 0.1 in a double, so a door written at that speed
          -- runs at a speed the simulation could not have produced. The exact
          -- decimal string "0.1" is what the author meant and is accepted.
          if v ~= math.floor(v) then
            problems[#problems + 1] =
              ("%s: '%s' argument '%s' is %s -- doubles are not exact in Q32.32; " ..
               "write it as the string \"%s\"")
              :format(where, spec.name, arg.name, tostring(v), tostring(v))
          end
        else
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be an integer, an exact decimal string, " ..
             "or a fraction {n, d}"):format(where, spec.name, arg.name)
        end
      elseif arg.type == "int" then
        if type(v) ~= "number" then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be a number"):format(where, spec.name, arg.name)
        end
      elseif arg.type == "string" then
        if type(v) ~= "string" then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be a string"):format(where, spec.name, arg.name)
        end
      elseif arg.type == "string_list" then
        if type(v) ~= "table" then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be a list of strings")
            :format(where, spec.name, arg.name)
        else
          for _, o in ipairs(v) do
            if type(o) ~= "string" then
              problems[#problems + 1] =
                ("%s: '%s' argument '%s' contains something that is not a string")
                :format(where, spec.name, arg.name)
            end
          end
        end
      elseif arg.type == "value" then
        -- Anything a property may hold. Not "anything at all": a table or a
        -- function here would be state an editor cannot round-trip and a
        -- condition cannot compare.
        if not (type(v) == "number" or type(v) == "string" or type(v) == "boolean") then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be a number, a string or a boolean")
            :format(where, spec.name, arg.name)
        end
      elseif arg.type == "function" then
        if type(v) ~= "function" then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be a function"):format(where, spec.name, arg.name)
        end
      elseif arg.type == "bool" then
        if type(v) ~= "boolean" then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be true or false"):format(where, spec.name, arg.name)
        end
      elseif arg.type == "trigger" then
        if type(v) ~= "string" then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be the name of another trigger")
            :format(where, spec.name, arg.name)
        end
      elseif arg.type == "sector" then
        if not (type(v) == "number" or v == "event" or v == "behind") then
          problems[#problems + 1] =
            ("%s: '%s' argument '%s' must be a sector index, \"event\" or \"behind\"")
            :format(where, spec.name, arg.name)
        end
      end
    end
  end
  -- Arguments the vocabulary does not know about. Almost always a typo, and a
  -- silently ignored one is a trigger that looks configured and is not.
  for k in pairs(given) do
    -- `when` is RESERVED: on a sequence beat it is the tick the beat fires on,
    -- so no action may declare an argument by that name -- one would be
    -- unreachable inside a sequence, because a Lua table cannot hold the key
    -- twice. Enforced below rather than trusted, since "free today" is not the
    -- same as safe.
    --
    -- NOT `at`, which the mob vocabulary uses for this and which would have been
    -- the consistent choice: `music` already has an `at` argument -- next_bar,
    -- loop_end, now -- and music is exactly what a cutscene wants to change.
    -- A colliding name is a correctness problem and an inconsistent one is a
    -- learning cost, so this takes the learning cost.
    if k ~= "c" and k ~= "a" and k ~= "negate" and k ~= "kind" and k ~= "when" then
      local known = false
      for _, arg in ipairs(spec.args) do if arg.name == k then known = true end end
      if not known then
        problems[#problems + 1] =
          ("%s: '%s' has no argument '%s'"):format(where, spec.name, k)
      end
    end
  end
  -- Exactly one member of each one_of group.
  local groups = {}
  for _, arg in ipairs(spec.args) do
    if arg.one_of ~= nil then groups[arg.one_of] = true end
  end
  for g in pairs(groups) do
    if seen_group[g] == nil then
      local names = {}
      for _, arg in ipairs(spec.args) do
        if arg.one_of == g then names[#names + 1] = arg.name end
      end
      problems[#problems + 1] =
        ("%s: '%s' needs one of: %s"):format(where, spec.name, table.concat(names, ", "))
    end
  end
end

-- Panels, against the vocabulary's description of them.
--
-- Checked at LOAD for the same reason everything else is: dnp.ui_open RAISES on
-- a bad widget, and a raise inside a callback takes the rest of the trigger
-- with it. So the first time anybody presses the switch, the panel does not
-- open AND whatever came after it in the action list silently did not happen.
-- Here it is a load-time problem naming the panel and the widget.
local function check_panels(panels, problems)
  local spec = trigger_vocab.panels
  local kinds = {}
  for _, w in ipairs(spec.widgets) do kinds[w.name] = w end

  for name, panel in pairs(panels) do
    local where = ("panel '%s'"):format(tostring(name))
    if type(panel) ~= "table" then
      problems[#problems + 1] = ("%s: is not a table"):format(where)
    else
      if panel.title ~= nil and type(panel.title) ~= "string" then
        problems[#problems + 1] = ("%s: its title is not a string"):format(where)
      end
      local widgets = panel.widgets or {}
      if #widgets > spec.max_widgets then
        -- The protocol's cap, not this file's opinion. A panel over it is
        -- refused by the binding, and a refused panel is one that never opens.
        problems[#problems + 1] =
          ("%s: %d widgets, the protocol's limit is %d")
          :format(where, #widgets, spec.max_widgets)
      end
      for i, w in ipairs(widgets) do
        local wwhere = ("%s widget %d"):format(where, i)
        local kind = kinds[w.kind]
        if kind == nil then
          problems[#problems + 1] =
            ("%s: no widget kind '%s'"):format(wwhere, tostring(w.kind))
        else
          check_args(kind, w, wwhere, problems)
          if w.options ~= nil and #w.options > spec.max_options then
            problems[#problems + 1] =
              ("%s: %d options, the protocol's limit is %d")
              :format(wwhere, #w.options, spec.max_options)
          end
        end
      end
    end
  end
end

-- The geometry a trigger was written against, checked against the map that
-- actually loaded.
--
-- THIS IS THE GUARD THAT MATTERS MOST, and it was added after measuring its
-- absence. Side indices come from the order mapc walks the .map, so inserting
-- one vertex renumbers every side after it. With no guard, a table of stale
-- indices loads perfectly and watches the wrong walls: one added vertex in
-- sector 0 of maps/doors_trig.map pointed the door, both trip-wires and the
-- exit at blank wall, and the runtime cheerfully reported "11 triggers on 4
-- events". From inside the game that is a map where nothing works and nothing
-- says why -- which is precisely the failure this whole file is arranged to
-- avoid, arriving through the back door.
--
-- doom.lua carries the same guard on every converted line and it is how the
-- back-side orientation bug was found. Checked at LOAD and fatal for the table,
-- because a map whose switches point at nothing is not a map that should start.
-- Set at load: false when there is no map to check against.
--
-- A NARROW bypass, and it is loud. tools/triggercheck lints a trigger table
-- with no map attached, so every expect_neighbour would report "side N is not a
-- side of this map" and the useful problems would be buried in noise. On a
-- server this is never false -- the map loads before its scripts do -- and the
-- one line it prints says plainly that the strongest guard here did not run.
local has_map = true

local function check_geometry(spec, given, where, problems)
  if not has_map then return end
  if given.expect_neighbour ~= nil then
    local sd = dnp.side(given.side)
    if sd == nil then
      problems[#problems + 1] =
        ("%s: side %s is not a side of this map"):format(where, tostring(given.side))
    else
      local nb = sd.neighbor and sd.neighbor:index() or -1
      if nb ~= given.expect_neighbour then
        -- Both numbers, always. "the geometry moved" says a rebuild is needed;
        -- WHICH side and what is behind it now says whether the map was edited
        -- or the table was written against a different map entirely.
        problems[#problems + 1] =
          ("%s: side %d has neighbour %d, this trigger was written against %d " ..
           "-- the map's sides have been renumbered")
          :format(where, given.side, nb, given.expect_neighbour)
      end
    end
  end

  if given.expect_tag ~= nil then
    for _, arg in ipairs(spec.args) do
      if arg.type == "sector" and type(given[arg.name]) == "number" then
        local sec = dnp.sector(given[arg.name])
        if sec == nil then
          problems[#problems + 1] =
            ("%s: sector %d is not a sector of this map"):format(where, given[arg.name])
        elseif sec:tag() ~= given.expect_tag then
          problems[#problems + 1] =
            ("%s: sector %d is tagged %s, this trigger was written against tag %d " ..
             "-- the map's sectors have been renumbered")
            :format(where, given[arg.name], tostring(sec:tag()), given.expect_tag)
        end
      end
    end
  end
end

-- Custom code, compiled once, here.
--
-- Two forms, and the difference is not stylistic:
--
--   fn = function(ev) ... end   a Lua function in the table. Compiled by the
--                               ordinary loader when the map's file loaded, so
--                               it works under every trust profile -- but it is
--                               a VALUE, and an editor cannot round-trip a
--                               value it did not write.
--   code = "..."                source as a string. An editor can show it in a
--                               box, keep it in the sidecar and write it back
--                               unchanged -- but compiling a string needs
--                               `loadstring`, which Trust::Strict removes.
--
-- So a hand-written map wants `fn` and an editor-written one wants `code`, and
-- a map that used `code` will refuse to load on a Strict server rather than
-- starting with one action quietly missing.
local function compile_custom(a, where, problems)
  if a.fn ~= nil then
    compiled[a] = a.fn
    return
  end
  if a.code == nil then return end        -- the one_of check has already said so
  if loadstring == nil then
    problems[#problems + 1] =
      ("%s: the 'lua' action's `code` needs loadstring, which this server removed " ..
       "(Trust::Strict). Use `fn = function(ev) ... end` instead, which needs no " ..
       "compiler at run time."):format(where)
    return
  end
  -- `ev` as a named parameter, on the SAME line, so the author writes
  -- `ev.entity` rather than `local ev = ...` and line 3 of an error is still
  -- line 3 of what they wrote. The chunk name is the trigger, so a raise names
  -- it instead of naming "[string ...]".
  local fn, err = loadstring("local ev = ... ; " .. a.code, "=" .. where)
  if fn == nil then
    problems[#problems + 1] = ("%s: the 'lua' action does not compile: %s")
                              :format(where, tostring(err))
    return
  end
  compiled[a] = fn
end

-- ------------------------------------------------------------------------ load

-- Fields a trigger itself may carry. Anything else is a typo, and a silently
-- ignored `evnt = "on_tick"` is a trigger that looks configured and never
-- fires -- the same failure an unknown action argument would be, one level up.
local TRIGGER_FIELDS = {
  name = true, event = true, conditions = true, actions = true,
  once = true, enabled = true, sequence = true,
}

-- Both lists as one, for the checks that apply to every action a trigger holds.
-- Written out rather than reached for twice, so a check added later cannot be
-- added to one and forgotten on the other.
local function concat_lists(a, b)
  if b == nil or #b == 0 then return a or {} end
  if a == nil or #a == 0 then return b end
  local out = {}
  for i = 1, #a do out[#out + 1] = a[i] end
  for i = 1, #b do out[#out + 1] = b[i] end
  return out
end

function M.load(data)
  -- A SECOND table is refused, not merged and not silently preferred.
  --
  -- This is the bug the VM's hook lists exist to prevent, one layer up: two
  -- scripts that each wrote `function on_tick` used to lose the first
  -- silently. Two trigger tables in one map would lose every trigger in the
  -- first the moment the second loaded, with nothing anywhere reporting it.
  if loaded then
    say("a trigger table is already loaded (%s); refusing to replace it. " ..
        "One map, one table -- put both sets of triggers in it.", M.name)
    return false
  end

  M.name = data.name or "?"
  M.panels = data.panels or {}
  local list = data.triggers or {}

  local ev_v, cond_v, act_v = index_vocab()

  local wrong = check_vocabulary_matches_implementation(cond_v, act_v)
  if wrong > 0 then
    say("%d vocabulary/implementation mismatches -- no triggers loaded", wrong)
    return false
  end

  local problems = {}

  -- Before anything that reads geometry.
  has_map = #dnp.sectors() > 0
  if not has_map then
    say("no map is loaded, so expect_neighbour/expect_tag were NOT checked -- " ..
        "this table's fit to a map is still unverified")
  end

  check_panels(M.panels, problems)

  -- Names first: a `run` or a condition may refer forward to a trigger declared
  -- later, so every name has to exist before any reference is checked.
  by_name = {}
  for i, t in ipairs(list) do
    local name = t.name
    if type(name) ~= "string" or name == "" then
      problems[#problems + 1] = ("trigger %d has no name"):format(i)
    elseif by_name[name] ~= nil then
      -- Names are how triggers refer to each other, so two of them is not a
      -- cosmetic problem: `run` would silently pick one.
      problems[#problems + 1] = ("two triggers are named '%s'"):format(name)
    else
      by_name[name] = t
    end
  end

  for i, t in ipairs(list) do
    local where = ("trigger %d (%s)"):format(i, tostring(t.name))

    if t.event ~= nil and ev_v[t.event] == nil then
      -- The list is not a suggestion: an event the engine does not report
      -- cannot be a trigger event, and adding one means adding an engine
      -- report.
      problems[#problems + 1] =
        ("%s: '%s' is not an event the engine reports"):format(where, tostring(t.event))
    end

    for _, c in ipairs(t.conditions or {}) do
      local spec = cond_v[c.c]
      if spec == nil then
        problems[#problems + 1] =
          ("%s: no condition named '%s'"):format(where, tostring(c.c))
      else
        check_args(spec, c, where, problems)
        check_geometry(spec, c, where, problems)
        -- A condition that reads the event's side is meaningless on a hook that
        -- carries no side. Checked at load, because at fire time it is simply
        -- false forever and looks like a trigger that never matches.
        if spec.events ~= nil and t.event ~= nil then
          local ok = false
          for _, e in ipairs(spec.events) do if e == t.event then ok = true end end
          if not ok then
            problems[#problems + 1] =
              ("%s: condition '%s' reads something %s does not carry")
              :format(where, spec.name, t.event)
          end
        end
      end
    end

    -- A sequence's beats ARE actions -- same vocabulary, same checks -- plus a
    -- tick to fire on. Validated in the same loop so a beat cannot be the one
    -- kind of action nobody checked.
    for _, b in ipairs(t.sequence or {}) do
      if type(b.when) ~= "number" or b.when ~= math.floor(b.when) or b.when < 0 then
        problems[#problems + 1] =
          ("%s: a sequence beat needs `when`, a whole number of ticks from the "
           .. "trigger firing, not '%s'"):format(where, tostring(b.when))
      end
    end

    for _, a in ipairs(concat_lists(t.actions, t.sequence)) do
      local spec = act_v[a.a]
      if spec == nil then
        problems[#problems + 1] = ("%s: no action named '%s'"):format(where, tostring(a.a))
      else
        check_args(spec, a, where, problems)
        check_geometry(spec, a, where, problems)
        if spec.name == "lua" then compile_custom(a, where, problems) end
        -- A panel name that resolves to nothing is the same class of mistake as
        -- a `run` naming a trigger that is not there: at fire time it is a
        -- silent false forever.
        if (spec.name == "ui_open" or spec.name == "ui_close")
           and a.panel ~= nil and M.panels[a.panel] == nil then
          problems[#problems + 1] =
            ("%s: '%s' names panel '%s', which this table does not define")
            :format(where, spec.name, tostring(a.panel))
        end
        for _, arg in ipairs(spec.args) do
          if arg.type == "trigger" and a[arg.name] ~= nil
             and by_name[a[arg.name]] == nil then
            problems[#problems + 1] =
              ("%s: '%s' names trigger '%s', which does not exist")
              :format(where, spec.name, tostring(a[arg.name]))
          end
        end
      end
    end

    if (t.actions == nil or #t.actions == 0)
       and (t.sequence == nil or #t.sequence == 0) then
      problems[#problems + 1] = ("%s: has no actions"):format(where)
    end

    for k in pairs(t) do
      if not TRIGGER_FIELDS[k] then
        problems[#problems + 1] = ("%s: a trigger has no field '%s'"):format(where, tostring(k))
      end
    end
  end

  if #problems > 0 then
    -- Named, not just counted. "4 problems" says a rebuild is needed; WHICH
    -- four says whether the editor emitted something wrong or the author did.
    for i = 1, #problems do
      if i > MAX_REPORTS then
        say("...and %d more", #problems - MAX_REPORTS)
        break
      end
      say("%s", problems[i])
    end
    say("%d problems in the trigger table -- nothing loaded, the map has no triggers",
        #problems)
    by_name = {}
    return false
  end

  -- Only now is any of it live.
  M.list = list
  by_event = {}
  state = {}
  busy = {}
  timers = {}
  open_panels = {}
  for _, t in ipairs(list) do
    t.spent = false
    if t.enabled == nil then t.enabled = true end
    if t.event ~= nil then
      by_event[t.event] = by_event[t.event] or {}
      local q = by_event[t.event]
      q[#q + 1] = t
    end
  end

  -- The tag index, built once. See targets_of.
  sectors_by_tag = {}
  for _, s in ipairs(dnp.sectors()) do
    local tag = s:tag()
    if tag ~= nil then
      sectors_by_tag[tag] = sectors_by_tag[tag] or {}
      local q = sectors_by_tag[tag]
      q[#q + 1] = s
    end
  end

  loaded = true
  local named = 0
  for _ in pairs(by_event) do named = named + 1 end
  say("%d triggers on %d events", #list, named)
  return true
end

-- Read-only accessors, for tests and for anything that wants to see the
-- runtime's own bookkeeping without reaching into its locals.
function M.counter(name)
  local v = state[name]
  return type(v) == "number" and v or 0
end

-- The general store, for custom code and for tests. Reading is always safe;
-- writing from `lua` code is how a trigger keeps something a counter cannot
-- hold.
function M.property(name) return state[name] end
function M.set_property(name, value) state[name] = value end
function M.pending() return #timers end
function M.is_enabled(name)
  local t = by_name[name]
  return t ~= nil and t.enabled ~= false
end
function M.is_spent(name)
  local t = by_name[name]
  return t ~= nil and t.spent == true
end

-- --------------------------------------------------- THE VERBS, FOR GRAPHS
--
-- docs/GRAPHS.md: a graph's conditions and actions ARE this file's, run by the
-- same implementations so a door a graph opens and a door a trigger opens are
-- one `sector_move`. Additive and read-only with respect to everything above:
-- it checks an entry with the loader's own checker, and runs one against an
-- event the caller supplies -- saving and restoring the event in flight, so a
-- graph run from inside a trigger's `lua` action does not clobber the trigger's.
--
-- An entry is shaped as a trigger's is: { c = "side_is", side = 4, ... } or
-- { a = "sector_move", ... }, its arguments as literal fields.
local function index_sectors_once()
  if loaded or next(sectors_by_tag) ~= nil then return end
  for _, s in ipairs(dnp.sectors()) do
    local tag = s:tag()
    if tag ~= nil then
      sectors_by_tag[tag] = sectors_by_tag[tag] or {}
      local q = sectors_by_tag[tag]
      q[#q + 1] = s:index()
    end
  end
end

local function with_event(ev, fn, entry)
  local saved = {}
  for k, v in pairs(EV) do saved[k] = v end
  clear_event()
  if ev ~= nil then
    EV.entity, EV.target, EV.side, EV.sector = ev.entity, ev.target, ev.side, ev.sector
    EV.crush_kind, EV.ui_id, EV.widget_id, EV.value = ev.crush_kind, ev.ui_id, ev.widget_id, ev.value
    EV.running = ev.running
  end
  local ok, result = pcall(fn, entry)
  clear_event()
  for k, v in pairs(saved) do EV[k] = v end
  if not ok then error(result, 0) end
  return result
end

M.verbs = {}

--- Problems with one entry, as the loader would report them: unknown name,
--- arguments, geometry, and a condition used on an event it reads nothing
--- from. `kind` is "condition" or "action"; `event` may be nil.
function M.verbs.check(kind, entry, event, where)
  local _, cond_v, act_v = index_vocab()
  local problems = {}
  has_map = #dnp.sectors() > 0
  if kind == "condition" then
    local spec = cond_v[entry.c]
    if spec == nil then
      problems[#problems + 1] = ("%s: no condition named '%s'"):format(where, tostring(entry.c))
      return problems
    end
    check_args(spec, entry, where, problems)
    check_geometry(spec, entry, where, problems)
    if spec.events ~= nil and event ~= nil then
      local ok = false
      for _, e in ipairs(spec.events) do if e == event then ok = true end end
      if not ok then
        problems[#problems + 1] = ("%s: condition '%s' reads something %s does not carry")
                                  :format(where, spec.name, event)
      end
    end
  elseif kind == "action" then
    local spec = act_v[entry.a]
    if spec == nil then
      problems[#problems + 1] = ("%s: no action named '%s'"):format(where, tostring(entry.a))
      return problems
    end
    check_args(spec, entry, where, problems)
    check_geometry(spec, entry, where, problems)
    if spec.name == "lua" then compile_custom(entry, where, problems) end
  else
    problems[#problems + 1] = ("%s: '%s' is not a kind of verb"):format(where, tostring(kind))
  end
  return problems
end

--- Does condition `entry` hold for event `ev` ({entity, target, side, ...})?
function M.verbs.condition(entry, ev)
  index_sectors_once()
  local ok = with_event(ev, COND[entry.c], entry)
  if entry.negate then ok = not ok end
  return ok and true or false
end

--- Run action `entry` for event `ev`. Returns whether it did something.
function M.verbs.action(entry, ev)
  index_sectors_once()
  return with_event(ev, ACT[entry.a], entry) and true or false
end
