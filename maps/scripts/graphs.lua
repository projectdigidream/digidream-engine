--
-- GRAPHS: node scripting alongside Lua (docs/GRAPHS.md).
--
-- A graph is data -- nodes and links, decoded from a .graph.json by the server
-- and handed to graphs.load -- and this walks it. Its events, conditions and
-- actions are the trigger vocabulary's and run through triggers.verbs, the one
-- implementation of each; the graph adds flow and data (graph_vocab.lua).
--
-- Loaded after trigger_vocab.lua, triggers.lua and graph_vocab.lua. Several
-- graphs may load into one VM: a map's own, or a module's in the game VM.
--
-- BOUNDED. A run -- one event, or one delay coming due -- takes at most
-- MAX_STEPS nodes; a loop drawn by mistake stops there and says so once,
-- rather than holding the tick. Data is pulled, not pushed, and each pure node
-- is evaluated once per run.

graphs = graphs or {}
local M = graphs
assert(graph_vocab ~= nil, "graphs.lua: load graph_vocab.lua first")
local V = graph_vocab

local MAX_STEPS = 256
local MAX_REPORTS = 20
local MAX_CALL_DEPTH = 4

local function say(fmt, ...) print(("[graphs] " .. fmt):format(...)) end

-- ------------------------------------------------------------ the vocabulary

local SPEC = {}
for _, n in ipairs(V.nodes) do SPEC[n.name] = n end
local GRAPH_FIELD, NODE_FIELD = {}, {}
for _, f in ipairs(V.graph_fields) do GRAPH_FIELD[f.name] = f end
for _, f in ipairs(V.node_fields) do NODE_FIELD[f.name] = f end

local function set_of(list) local s = {} for _, v in ipairs(list or {}) do s[v] = true end return s end

-- In the game VM (a module's graph) or a level's.
local function in_game() return dnp ~= nil and dnp.is_game ~= nil and dnp.is_game() end
local function has_map() return dnp ~= nil and dnp.sectors ~= nil and #(dnp.sectors() or {}) > 0 end

-- The events a graph may start from: in a level, the triggers'; in the game,
-- the hooks the game VM is given (graph_vocab.game.events).
local function event_names()
  local s = {}
  if in_game() then
    for _, e in ipairs(V.game.events) do s[e] = true end
  elseif trigger_vocab ~= nil then
    for _, e in ipairs(trigger_vocab.events) do s[e.name] = true end
  end
  return s
end
local GAME_OK = { condition = {}, action = {} }
for _, c in ipairs(V.game.conditions) do GAME_OK.condition[c] = true end
for _, a in ipairs(V.game.actions) do GAME_OK.action[a] = true end

-- ------------------------------------------------------------- the runtime

local loaded = {}      -- graphs in load order
local timers = {}      -- { tick, graph, node, pin, ev }
-- PANELS (docs/PANELS.md), per VM: the definitions the loaded graphs carry, by
-- name (the first graph to carry one wins); which player has which open, by
-- "<player>/<panel>" -> ui_id; and the way back, ui_id -> what it is.
local panel_defs = {}  -- name -> { def, label }
local open_panels = {} -- "<player>/<panel>" -> ui_id
local by_ui = {}       -- ui_id -> { key, panel, player }
local call_depth = 0

-- A verb's argument specs, by kind and name, from the trigger vocabulary.
local verb_specs = nil
local function verb_spec(kind, name)
  if trigger_vocab == nil then return nil end
  if verb_specs == nil then
    verb_specs = { condition = {}, action = {} }
    for _, c in ipairs(trigger_vocab.conditions or {}) do verb_specs.condition[c.name] = c end
    for _, a in ipairs(trigger_vocab.actions or {}) do verb_specs.action[a.name] = a end
  end
  return verb_specs[kind][name]
end
local PINNABLE = {}
for _, t in ipairs(V.pinnable_types or {}) do PINNABLE[t] = true end

-- Does a value linked at run time fit a verb argument? The literal checks the
-- trigger loader makes, for a value nobody could check at load.
local function fits(arg, v)
  local t = arg.type
  if t == "int" or t == "sector" then
    return type(v) == "number" and v == math.floor(v) and (t ~= "sector" or v >= 0)
  end
  if t == "fx" then
    -- Never a fractional double: 0.1 is not 0.1 in Q32.32. A whole number, an
    -- exact decimal string, or {numerator, denominator}.
    if type(v) == "number" then return v == math.floor(v) end
    if type(v) == "string" then return (pcall(fx.parse, v)) end
    return type(v) == "table" and type(v[1]) == "number" and type(v[2]) == "number" and v[2] ~= 0
  end
  if t == "string" then return type(v) == "string" end
  if t == "bool" then return type(v) == "boolean" end
  if t == "enum" then for _, x in ipairs(arg.values) do if v == x then return true end end return false end
  if t == "value" then return type(v) == "number" or type(v) == "string" or type(v) == "boolean" end
  return false
end

-- The verb's entry for this run: its literal fields, and the linked arguments'
-- values. Nil (and said once) when a linked value does not fit.
local function verb_entry(g, n, pull)
  if n.linked == nil then return n.entry end
  local e = {}
  for k, v in pairs(n.entry) do e[k] = v end
  for name, arg in pairs(n.linked) do
    local v = pull(name)
    if not fits(arg, v) then
      g.misfit = g.misfit or {}
      if not g.misfit[n.id] then
        g.misfit[n.id] = true
        say("%s: node '%s' was given %s for '%s', which is not a %s -- skipped",
            g.name, n.id, tostring(v), name, arg.type)
      end
      return nil
    end
    e[name] = v
  end
  return e
end

-- Pure nodes: a value from their inputs. `pull(pin)` reads an input.
-- ENTITY PROPERTIES (graph_vocab.properties): what get_var, set_var and
-- add_var reach on an entity when `var` is not one of the graph's own.
local PROPERTY = {}
for _, p in ipairs(V.properties or {}) do PROPERTY[p.name] = p end
local entity_of, prop_get, prop_set   -- defined with the world's nodes below

local PURE = {}
function PURE.event(g, n, ev, pull, pin) return ev[pin] end
-- An on_call's outputs are what the call sent, carried as its run's event.
PURE.on_call = PURE.event
function PURE.get_var(g, n, ev, pull)
  if g.vars[n.var] == nil and PROPERTY[n.var] ~= nil then
    return prop_get(entity_of(pull, ev, nil, g.data_src[n.id]), n.var)
  end
  return g.vars[n.var]
end
function PURE.literal(g, n) return n.value end
function PURE.compare(g, n, ev, pull)
  local a, b, op = pull("a"), pull("b"), n.op
  if op == "==" then return a == b end
  if op == "~=" then return a ~= b end
  if type(a) ~= "number" or type(b) ~= "number" then return false end
  if op == "<" then return a < b end
  if op == "<=" then return a <= b end
  if op == ">" then return a > b end
  return a >= b
end
function PURE.math(g, n, ev, pull)
  local a, b, op = tonumber(pull("a")) or 0, tonumber(pull("b")) or 0, n.op
  if op == "+" then return a + b end
  if op == "-" then return a - b end
  if op == "*" then return a * b end
  if op == "/" then return b == 0 and 0 or a / b end
  if op == "min" then return math.min(a, b) end
  return math.max(a, b)
end
function PURE.logic(g, n, ev, pull)
  local a = pull("a")
  if n.op == "not" then return not a end
  local b = pull("b")
  if n.op == "and" then return (a and b) and true or false end
  return (a or b) and true or false
end
function PURE.random(g, n) return dnp.random(n.lo, n.hi) end

-- Exec nodes: do something and say which exit to take, or nil to stop this
-- path. `go(pin)` continues later through an exit (sequence, delay).
local EXEC = {}
function EXEC.condition(g, n, ev, pull)
  local e = verb_entry(g, n, pull)
  if e == nil then return "false" end
  return triggers.verbs.condition(e, ev) and "true" or "false"
end
function EXEC.action(g, n, ev, pull)
  local e = verb_entry(g, n, pull)
  if e ~= nil then triggers.verbs.action(e, ev) end
  return "then"
end
-- on_call starts runs; it is never stepped INTO.
function EXEC.on_call() return "then" end
local fire_call
function EXEC.call(g, n, ev, pull)
  local entity = pull("entity")
  if entity == nil then entity = ev.entity end
  local value = pull("value")
  local game = dnp ~= nil and dnp.is_game ~= nil and dnp.is_game()
  if n.to == "game" then
    if game or dnp == nil or dnp.call_game == nil then fire_call(n.name, entity, value)
    elseif dnp.game_has("graph_call") then dnp.call_game("graph_call", n.name, value, entity) end
  else
    if not game then fire_call(n.name, entity, value)
    elseif entity ~= nil and dnp.call_level ~= nil then dnp.call_level(entity, "graph_call", n.name, value)
    else
      g.misfit = g.misfit or {}
      if not g.misfit[n.id] then
        g.misfit[n.id] = true
        say("%s: node '%s' calls a level with no entity to say which -- skipped", g.name, n.id)
      end
    end
  end
  return "then"
end
function EXEC.branch(g, n, ev, pull) local v = pull("cond") return (v ~= nil and v ~= false) and "true" or "false" end
function EXEC.sequence(g, n, ev, pull, go)
  for i = n.count - 1, 1, -1 do go(tostring(i)) end   -- run after 0, in order
  return "0"
end
function EXEC.delay(g, n, ev)
  local copy = {}
  for k, v in pairs(ev) do copy[k] = v end
  timers[#timers + 1] = { tick = dnp.tick() + n.ticks, graph = g, node = n.id, pin = "then", ev = copy }
  return nil
end
function EXEC.once(g, n) if g.spent[n.id] then return nil end g.spent[n.id] = true return "then" end
function EXEC.set_var(g, n, ev, pull)
  if g.vars[n.var] == nil and PROPERTY[n.var] ~= nil then
    prop_set(entity_of(pull, ev, nil, g.data_src[n.id]), n.var, pull("value"))
    return "then"
  end
  g.vars[n.var] = pull("value")
  return "then"
end
function EXEC.add_var(g, n, ev, pull)
  if g.vars[n.var] == nil and PROPERTY[n.var] ~= nil then
    local e = entity_of(pull, ev, nil, g.data_src[n.id])
    prop_set(e, n.var, (tonumber(prop_get(e, n.var)) or 0) + (tonumber(pull("value")) or 0))
    return "then"
  end
  g.vars[n.var] = (tonumber(g.vars[n.var]) or 0) + (tonumber(pull("value")) or 0)
  return "then"
end

-- Declared here, defined below: nodes that continue a run later (a fade's
-- `done`, a subtitle's) call it from their timers.
local run
local view_of, start_view   -- a node's linked numbers; see run
local any_start, fire_start -- the start nodes' dispatch, defined with them below

-- ------------------------------------------------------- THE WORLD'S NODES
--
-- Plain verbs on things (graph_vocab.lua, "THE WORLD"). Sector moves are the
-- trigger runtime's own sector_move / sector_stop, so they replicate and
-- predict exactly as a trigger's door does; hurting goes through the mob
-- runtime when it is loaded, so a monster feels pain, dies and drops.

local SPEED = { slow = { 1, 80 }, normal = { 1, 40 }, fast = { 1, 10 } }   -- units a tick
local SPIN = { slow = { 1, 2 }, normal = { 1, 1 }, fast = { 3, 1 } }        -- degrees a tick
local DOOR_GAP = { -1, 12 }    -- DOOM's four units under the lowest ceiling beside it

local jobs = {}                -- { tick, fn }: a door's close, a lift's return
local function later(ticks, fn) jobs[#jobs + 1] = { tick = dnp.tick() + math.max(1, ticks), fn = fn } end
local function drain_jobs(now)
  if #jobs == 0 then return end
  local due, keep = {}, {}
  for _, j in ipairs(jobs) do if j.tick <= now then due[#due + 1] = j else keep[#keep + 1] = j end end
  jobs = keep
  for _, j in ipairs(due) do j.fn() end
end

-- A plain number as an exact fx, never through a double's error.
local function fxn(n)
  n = tonumber(n) or 0
  if n == math.floor(n) then return fx.int(n) end
  return fx.parse(("%.6f"):format(n))
end
local function unfx(f) return type(f) == "number" and f or tonumber(f.raw) / 4294967296 end
-- What a trigger entry takes for an fx argument (an integer or an exact
-- decimal string -- never a float): a plain number as itself or its decimal,
-- an fx as its EXACT decimal. Every fx is a fraction over 2^32, so its decimal
-- is finite; nothing is rounded on the way to the trigger runtime.
local function entry_fx(v)
  if type(v) == "number" then
    if v == math.floor(v) then return v end
    return ("%.6f"):format(v)
  end
  local raw = tonumber(v.raw)      -- a 64-bit raw arrives as a string
  local neg = raw < 0
  if neg then raw = -raw end
  local ip = math.floor(raw / 4294967296)
  local fr = raw - ip * 4294967296
  local digits = {}
  while fr > 0 and #digits < 40 do
    fr = fr * 10
    local d = math.floor(fr / 4294967296)
    digits[#digits + 1] = d
    fr = fr - d * 4294967296
  end
  return (neg and "-" or "") .. ip .. (#digits > 0 and ("." .. table.concat(digits)) or "")
end
local function ticks_of(seconds) return math.floor((tonumber(seconds) or 0) * 60 + 0.5) end

-- The entity a node acts on: what is linked -- and if the link yields
-- nothing, NOTHING, never the event's entity in its place (a `spawn` that
-- could not spawn, feeding a `kill`, must not kill the player who set it
-- off). Unlinked, the `entity` input is the event's own.
entity_of = function(pull, ev, pin, linked)
  pin = pin or "entity"
  local e
  if linked ~= nil and linked[pin] ~= nil then e = pull(pin)
  elseif pin == "entity" then e = ev.entity end
  if e ~= nil and type(e) == "userdata" and e.valid ~= nil and e:valid() then return e end
  return nil
end
local function index_of(sec)
  if type(sec) == "number" then return sec end
  if sec ~= nil and type(sec) == "userdata" and sec.index ~= nil then return sec:index() end
  return nil
end
-- The sectors a node acts on: the linked one, the picked one, or every one
-- carrying its tag.
local function sectors_for(n, pull)
  local linked = index_of(pull("sector"))
  if linked ~= nil then return { linked } end
  if n.tag ~= nil then
    local out = {}
    for _, sec in ipairs(dnp.sectors() or {}) do if sec:tag() == n.tag then out[#out + 1] = sec:index() end end
    return out
  end
  return {}
end
local function move(entry, ev)
  entry.a = entry.a or "sector_move"
  entry.interrupt = true
  triggers.verbs.action(entry, ev)
end
local function hurt(e, amount)
  if mobs ~= nil and mobs.hurt ~= nil then mobs.hurt(e, amount, nil)
  elseif e.set_hp ~= nil and e.hp ~= nil then e:set_hp((e:hp() or 0) - amount) end
end

-- Reading and setting an entity's properties. Movement parameters on a
-- PLAYER go as a lasting "set" effect, one per (player, parameter), a later
-- set replacing it: the engine will not let a script rewrite a player's body,
-- whose rules the client predicts. On anything else, into the body itself.
local prop_effects = {}   -- "<entity>/<param>" -> effect id
local function raw_num(f) return f == nil and nil or unfx(f) end
prop_get = function(e, name)
  local p = PROPERTY[name]
  if p == nil or e == nil or (e.valid ~= nil and not e:valid()) then
    if name == "alive" then return false end
    return nil
  end
  if name == "hp" or name == "alive" then
    local m = mobs ~= nil and mobs.health ~= nil and mobs.health(e) or nil
    local hp = m ~= nil and m or (e.hp ~= nil and e:hp() or nil)
    if name == "alive" then return hp ~= nil and hp > 0 end
    return hp
  end
  if name == "pain_ticks" then
    return mobs ~= nil and mobs.pain_ticks ~= nil and mobs.pain_ticks(e) or nil
  end
  if name == "x" or name == "y" or name == "z" then return raw_num(e:pos()[name]) end
  if name == "speed" then
    local v = e:vel()
    local vx, vy = raw_num(v.x), raw_num(v.y)
    return math.sqrt(vx * vx + vy * vy)
  end
  if name == "vertical_speed" then return raw_num(e:vel().z) end
  if name == "grounded" then return e:grounded() == true end
  if name == "facing" then return ((e:facing() or 0) % 4294967296) / 4294967296 * 360 end
  if name == "sector" then return index_of(e:sector()) end
  if name == "kind" then return e:kind() end
  if name == "name" then return e.name ~= nil and e:name() or nil end
  if name == "is_player" then return e:kind() == 0 end
  -- A movement parameter: what the physics uses now.
  local v = e:param(name)
  if v == nil then return nil end
  local x = unfx(v)
  if p.type == "bool" then return x ~= 0 end
  if p.type == "int" then return math.floor(x + 0.5) end
  return x
end
prop_set = function(e, name, value)
  local p = PROPERTY[name]
  if p == nil or p.access ~= "read_write" or e == nil or (e.valid ~= nil and not e:valid()) then return end
  if name == "hp" then
    local v = math.floor((tonumber(value) or 0) + 0.5)
    if mobs ~= nil and mobs.set_health ~= nil and mobs.health ~= nil and mobs.health(e) ~= nil then mobs.set_health(e, v)
    elseif e.set_hp ~= nil then e:set_hp(v) end
    return
  end
  if name == "pain_ticks" then
    if mobs ~= nil and mobs.set_pain_ticks ~= nil then mobs.set_pain_ticks(e, tonumber(value) or 1) end
    return
  end
  local v
  if p.type == "bool" then v = (value ~= nil and value ~= false and value ~= 0) and 1 or 0
  else v = tonumber(value); if v == nil then return end end
  -- An exact decimal for the engine (check_fx refuses a fractional double).
  local text = (p.type == "number") and string.format("%.6f", v):gsub("0+$", ""):gsub("%.$", "") or tostring(math.floor(v + 0.5))
  if e:kind() == 0 then
    local key = (e.player ~= nil and e:player() or e:index()) .. "/" .. name
    if prop_effects[key] ~= nil then e:remove_effect(prop_effects[key]) end
    local id = e:add_effect(name, "set", text)
    prop_effects[key] = (id ~= nil and id ~= 0) and id or nil
  else
    e:set_body_param(name, text)
  end
end

function EXEC.teleport(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e == nil then return "then" end
  local p = e:pos()
  local onto = entity_of(pull, ev, "to", g.data_src[n.id])
  local spawn = n.spawn ~= nil and dnp.spawn_point ~= nil and dnp.spawn_point(n.spawn) or nil
  if spawn ~= nil then p = spawn
  elseif onto ~= nil then p = onto:pos()
  else
    local x, y, z = pull("x"), pull("y"), pull("z")
    p = vec3.new(x ~= nil and fxn(x) or p.x, y ~= nil and fxn(y) or p.y, z ~= nil and fxn(z) or p.z)
  end
  e:teleport(p)
  return "then"
end
function EXEC.push(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e == nil or e.add_impulse == nil then return "then" end
  local strength = fxn(pull("strength") or 10)
  local d = { x = fx.int(0), y = fx.int(0) }
  local how = n.direction
  if how == "forward" or how == "backward" then
    d = dnp.dir(e:facing())
    if how == "backward" then d = { x = -d.x, y = -d.y } end
  elseif how == "away" or how == "toward" then
    local from = entity_of(pull, ev, "from", g.data_src[n.id])
    if from ~= nil then
      local a, b = e:pos(), from:pos()
      local ang = dnp.atan2(a.y - b.y, a.x - b.x)
      d = dnp.dir(ang)
      if how == "toward" then d = { x = -d.x, y = -d.y } end
    end
  elseif how == "angle" then
    d = dnp.dir(math.floor((tonumber(pull("angle")) or 0) / 360 * 4294967296) % 4294967296)
  end
  local lift = fxn(pull("up") or 0)
  -- STRAIGHT UP: the strength is the vertical (a spring), with any `up` on top.
  if how == "up" then lift = lift + strength end
  e:add_impulse(vec3.new(d.x * strength, d.y * strength, lift))
  return "then"
end
function EXEC.stop(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e ~= nil and e.set_velocity ~= nil then e:set_velocity(vec3.new(fx.int(0), fx.int(0), fx.int(0))) end
  return "then"
end
function EXEC.damage(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e ~= nil then hurt(e, math.floor(tonumber(pull("amount")) or 10)) end
  return "then"
end
function EXEC.heal(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e ~= nil then
    local add = math.floor(tonumber(pull("amount")) or 25)
    local m = mobs ~= nil and mobs.health ~= nil and mobs.health(e) or nil
    if m ~= nil and m > 0 then mobs.set_health(e, math.min(n.max or math.huge, m + add))
    elseif e.set_hp ~= nil and e.hp ~= nil then e:set_hp(math.min(n.max or 100, (e:hp() or 0) + add)) end
  end
  return "then"
end
function EXEC.kill(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e ~= nil then hurt(e, 1000000) end
  return "then"
end
function EXEC.remove(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e ~= nil then e:despawn() end
  return "then"
end
function EXEC.spawn(g, n, ev, pull)
  local at = entity_of(pull, ev, "at", g.data_src[n.id])
  local p
  if at ~= nil then p = at:pos()
  elseif n.x ~= nil and n.y ~= nil then p = vec3.new(fxn(n.x), fxn(n.y), fxn(n.z or 0))
  elseif ev.entity ~= nil and ev.entity:valid() then p = ev.entity:pos() end
  local made = nil
  if p ~= nil then
    if mobs ~= nil and mobs.has ~= nil and mobs.has(n.what) then made = mobs.spawn(n.what, p)
    else made = dnp.spawn_prop(n.what, p) end
  end
  ev._out = ev._out or {}
  ev._out[n.id] = { entity = made }
  return "then"
end
function PURE.spawn(g, n, ev, pull, pin) return ev._out and ev._out[n.id] and ev._out[n.id][pin] end
function EXEC.speed_up(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e == nil then return "then" end
  local factor, ticks = fxn(pull("factor") or 2), ticks_of(n.seconds or 5)
  if mobs ~= nil and mobs.tagged_effect ~= nil then mobs.tagged_effect(e, "graph_speed", "move_speed", "mul", factor, ticks)
  elseif e.add_effect ~= nil then e:add_effect("move_speed", "mul", factor, ticks) end
  return "then"
end
function EXEC.play_sound(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e ~= nil and dnp.sound ~= nil then dnp.sound(n.sound, e) end
  return "then"
end

-- A door shut: its ceiling down onto its own floor, wherever that is now.
local function shut(idx, speed, ev)
  local sec = dnp.sector(idx)
  if sec == nil then return end
  move({ sector = idx, surface = "ceiling", to = "absolute", height = entry_fx(sec:floor()), speed = speed }, ev)
end

function EXEC.open_door(g, n, ev, pull)
  local speed = SPEED[n.speed or "normal"]
  for _, idx in ipairs(sectors_for(n, pull)) do
    move({ sector = idx, surface = "ceiling", to = "lowest_neighbour_ceiling", offset = DOOR_GAP, speed = speed }, ev)
    if n.close_after ~= nil and n.close_after > 0 then
      later(ticks_of(n.close_after), function() shut(idx, speed, ev) end)
    end
  end
  return "then"
end
function EXEC.close_door(g, n, ev, pull)
  for _, idx in ipairs(sectors_for(n, pull)) do shut(idx, SPEED[n.speed or "normal"], ev) end
  return "then"
end
local FLOOR_TO = { next_higher = "next_higher_floor", lowest_nearby = "lowest_neighbour_floor",
                   highest_nearby = "highest_neighbour_floor", original = "base_floor" }
local CEIL_TO = { lowest_nearby = "lowest_neighbour_ceiling", highest_nearby = "highest_neighbour_ceiling",
                  original = "base_ceiling" }
local function move_surface(n, ev, pull, surface, named)
  local speed = SPEED[n.speed or "normal"]
  for _, idx in ipairs(sectors_for(n, pull)) do
    if n.to == "up_by" or n.to == "down_by" then
      local sec = dnp.sector(idx)
      if sec ~= nil then
        local now = surface == "floor" and sec:floor() or sec:ceil()
        local by = fxn(pull("amount") or 1)
        move({ sector = idx, surface = surface, to = "absolute",
               height = entry_fx(n.to == "up_by" and now + by or now - by), speed = speed }, ev)
      end
    else
      move({ sector = idx, surface = surface, to = named[n.to], speed = speed }, ev)
    end
  end
  return "then"
end
function EXEC.move_floor(g, n, ev, pull) return move_surface(n, ev, pull, "floor", FLOOR_TO) end
function EXEC.move_ceiling(g, n, ev, pull) return move_surface(n, ev, pull, "ceiling", CEIL_TO) end
function EXEC.lift(g, n, ev, pull)
  local speed = SPEED[n.speed or "normal"]
  for _, idx in ipairs(sectors_for(n, pull)) do
    local sec = dnp.sector(idx)
    if sec ~= nil then
      local lowest = sec:floor()
      for _, nb in ipairs(sec:neighbors() or {}) do if nb:floor() < lowest then lowest = nb:floor() end end
      local drop = unfx(sec:floor() - lowest)
      local travel = math.ceil(drop / (speed[1] / speed[2]))
      move({ sector = idx, surface = "floor", to = "lowest_neighbour_floor", speed = speed }, ev)
      later(travel + ticks_of(n.wait or 3), function()
        move({ sector = idx, surface = "floor", to = "base_floor", speed = speed }, ev)
      end)
    end
  end
  return "then"
end
function EXEC.slide_sector(g, n, ev, pull)
  for _, idx in ipairs(sectors_for(n, pull)) do
    move({ sector = idx, surface = "both", dx = entry_fx(n.dx or 0), dy = entry_fx(n.dy or 0),
           speed = SPEED[n.speed or "normal"], loop = n.back_and_forth and "pingpong" or nil }, ev)
  end
  return "then"
end
function EXEC.spin_sector(g, n, ev, pull)
  for _, idx in ipairs(sectors_for(n, pull)) do
    move({ sector = idx, surface = "both", spin = entry_fx(n.degrees or 360), spin_speed = SPIN[n.speed or "normal"],
           speed = SPEED.normal, loop = n.forever and "loop" or nil }, ev)
  end
  return "then"
end
function EXEC.stop_sector(g, n, ev, pull)
  for _, idx in ipairs(sectors_for(n, pull)) do triggers.verbs.action({ a = "sector_stop", sector = idx }, ev) end
  return "then"
end
-- Sector looks (SECTOR_SET): kept by the server, so a player arriving later
-- sees the lights already out.
function EXEC.set_floor_texture(g, n, ev, pull)
  if dnp.set_sector_texture == nil then return "then" end
  for _, idx in ipairs(sectors_for(n, pull)) do dnp.set_sector_texture(idx, "floor", n.texture) end
  return "then"
end
function EXEC.set_ceiling_texture(g, n, ev, pull)
  if dnp.set_sector_texture == nil then return "then" end
  for _, idx in ipairs(sectors_for(n, pull)) do dnp.set_sector_texture(idx, "ceiling", n.texture) end
  return "then"
end
function EXEC.set_sector_light(g, n, ev, pull)
  if dnp.set_sector_light == nil then return "then" end
  for _, idx in ipairs(sectors_for(n, pull)) do dnp.set_sector_light(idx, n.light, n.seconds or 0) end
  return "then"
end
function EXEC.set_sector_effect(g, n, ev, pull)
  if dnp.set_sector_effect == nil then return "then" end
  local settings = false
  if n.on ~= false then
    settings = {}
    if n.density ~= nil then settings.density = n.density end
    if n.color ~= nil then settings.color = n.color end
  end
  for _, idx in ipairs(sectors_for(n, pull)) do dnp.set_sector_effect(idx, n.effect, settings) end
  return "then"
end

function EXEC.set_wall_texture(g, n, ev, pull)
  if dnp.set_side_texture ~= nil then dnp.set_side_texture(n.side, n.part or "middle", n.texture) end
  return "then"
end

-- ------------------------------------------------------------------ PANELS
--
-- A panel by name from the graph's own `panels` (docs/PANELS.md). Keyed by the
-- player's session (e:player()), as game scripts key players, so the game's
-- panel follows its player into the next level.
local function player_key(e)
  local p = e.player ~= nil and e:player() or nil
  return p ~= nil and ("p" .. p) or ("e" .. e:index())
end
local function panel_player(n, g, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e == nil or e.kind == nil or e:kind() ~= 0 then return nil end   -- players only
  return e
end
local function forget_panel(key)
  local id = open_panels[key]
  if id ~= nil then by_ui[id] = nil end
  open_panels[key] = nil
  return id
end
function EXEC.open_panel(g, n, ev, pull)
  local e = panel_player(n, g, ev, pull)
  local def = g.panels[n.panel]
  if e == nil or def == nil or dnp.ui_open == nil then return "then" end
  local key = player_key(e) .. "/" .. n.panel
  -- Never twice: an open copy is closed first.
  local old = forget_panel(key)
  if old ~= nil then dnp.ui_close(e, old) end
  local id = dnp.ui_open(e, { title = def.title, template = def.template, widgets = def.widgets, pinned = def.pinned })
  if id ~= nil and id ~= 0 then
    open_panels[key] = id
    by_ui[id] = { key = key, panel = n.panel, player = e:player() }
    -- Widgets declared `hidden = true` start hidden (show_element shows them).
    if dnp.ui_show ~= nil then
      for _, w in ipairs(def.widgets) do if w.hidden then dnp.ui_show(e, id, w.id, false) end end
    end
  end
  return "then"
end
-- SAVED ON DISK. A store is one table, { [variable] = value }, under a name;
-- graphs naming the same store share it, so a save writes beside what is
-- there rather than over it. Plain values only: a number, text, true/false.
local function store_names(g, n)
  if type(n.vars) == "string" then
    local out = {}
    for name in n.vars:gmatch("[^,%s]+") do out[#out + 1] = name end
    return out
  end
  local out = {}
  for k in pairs(g.vars) do out[#out + 1] = k end
  table.sort(out)
  return out
end
local store_said = {}
local function store_say(key, fmt, ...)
  if store_said[key] then return end
  store_said[key] = true
  say(fmt, ...)
end
function EXEC.save_vars(g, n)
  local store = n.store or "game"
  if dnp.store_save == nil then return "failed" end
  local have = dnp.store_load(store)
  if type(have) ~= "table" then have = {} end
  for _, k in ipairs(store_names(g, n)) do
    local v = g.vars[k]
    local t = type(v)
    if t == "number" or t == "string" or t == "boolean" then have[k] = v
    elseif v ~= nil then store_say(g.name .. "/" .. k, "%s: '%s' holds a %s, which cannot be kept on disk", g.name, k, t) end
  end
  local ok, why = dnp.store_save(store, have)
  if not ok then
    store_say(g.name .. "/" .. store, "%s: could not save store '%s': %s", g.name, store, tostring(why))
    return "failed"
  end
  return "then"
end
function EXEC.load_vars(g, n)
  local store = n.store or "game"
  if dnp.store_load == nil then return "none" end
  local have, why = dnp.store_load(store)
  if type(have) ~= "table" then
    if why ~= nil then store_say(g.name .. "/" .. store, "%s: store '%s': %s", g.name, store, tostring(why)) end
    return "none"
  end
  for _, k in ipairs(store_names(g, n)) do
    local v = have[k]
    if v ~= nil and g.vars[k] ~= nil and type(v) == type(g.vars[k]) then g.vars[k] = v end
  end
  return "then"
end
function EXEC.forget_saved(g, n)
  if dnp.store_delete ~= nil then dnp.store_delete(n.store or "game") end
  return "then"
end

function EXEC.close_panel(g, n, ev, pull)
  local e = panel_player(n, g, ev, pull)
  if e == nil then return "then" end
  local id = forget_panel(player_key(e) .. "/" .. n.panel)
  if id ~= nil and dnp.ui_close ~= nil then dnp.ui_close(e, id) end
  return "then"
end
function EXEC.set_widget(g, n, ev, pull)
  local e = panel_player(n, g, ev, pull)
  if e == nil or dnp.ui_set == nil then return "then" end
  local id = open_panels[player_key(e) .. "/" .. n.panel]
  local w = g.panels[n.panel] and g.panels[n.panel].by_name[n.widget]
  local v = tonumber(pull("value"))
  if id ~= nil and w ~= nil and v ~= nil then dnp.ui_set(e, id, w.id, math.floor(v + 0.5)) end
  return "then"
end
local function show_element(g, n, ev, pull, shown)
  local e = panel_player(n, g, ev, pull)
  if e == nil or dnp.ui_show == nil then return "then" end
  local id = open_panels[player_key(e) .. "/" .. n.panel]
  if id == nil then return "then" end
  if n.decor ~= nil then dnp.ui_show(e, id, n.decor, shown); return "then" end
  local w = g.panels[n.panel] and g.panels[n.panel].by_name[n.widget]
  if w ~= nil then dnp.ui_show(e, id, w.id, shown) end
  return "then"
end
function EXEC.show_element(g, n, ev, pull) return show_element(g, n, ev, pull, true) end
function EXEC.hide_element(g, n, ev, pull) return show_element(g, n, ev, pull, false) end
function PURE.panel_is_open(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  return e ~= nil and open_panels[player_key(e) .. "/" .. n.panel] ~= nil
end
function EXEC.set_hud(g, n, ev, pull)
  if dnp.hud == nil then return "then" end
  local v = pull("value")
  if v == nil then v = false end
  if n.to == "everyone" then
    dnp.hud(nil, { [n.name] = v })
  else
    local e = entity_of(pull, ev, nil, g.data_src[n.id])
    if e ~= nil then dnp.hud(e, { [n.name] = v }) end
  end
  return "then"
end

-- ---------------------------------------------------------------- MARKERS
--
-- A small UI hung on something in the world (docs/MARKERS.md), through
-- dnp.mark. Known in the graph by NAME and by what it hangs on, so one `mark`
-- node can hang a bar over every monster that walks in, each its own: the key
-- is "<name>/<entity index>", or "<name>/place" (per player when only one
-- player sees it).
local function marker_number(v)
  if type(v) == "number" then return math.floor(v) end
  if type(v) == "boolean" then return v and 1 or 0 end
  if type(v) == "userdata" and v.raw ~= nil then return math.floor(unfx(v)) end
  return nil
end
local function marker_values(n, pull)
  local values = {}
  if n.text ~= nil then values.text = tostring(n.text) end
  local v = pull("value")
  if v ~= nil then values.value = marker_number(v) or tostring(v) end
  local mx = pull("max")
  if mx ~= nil then values.max = marker_number(mx) or tostring(mx) end
  return values
end
-- The marker of this name the node means: on the entity, else this player's
-- place one, else the place one.
local function marker_key(g, name, e)
  g.marks = g.marks or {}
  if e ~= nil then
    local k = name .. "/" .. e:index()
    if g.marks[k] ~= nil then return k end
    k = name .. "/place/" .. e:index()
    if g.marks[k] ~= nil then return k end
  end
  return name .. "/place"
end
function EXEC.mark(g, n, ev, pull)
  if dnp.mark == nil then return "then" end
  g.marks = g.marks or {}
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  local place = n.at == "place"
  local on, key
  if place then
    -- A coordinate as written (a number) or linked (a number, or an fx).
    local coord = function(v)
      if type(v) == "userdata" and v.raw ~= nil then return v end
      return fxn(tonumber(v) or 0)
    end
    on = vec3.new(coord(pull("x")), coord(pull("y")), coord(pull("z")))
    key = n.marker .. ((n.to == "player" and e ~= nil) and ("/place/" .. e:index()) or "/place")
  else
    if e == nil then return "then" end
    on, key = e, n.marker .. "/" .. e:index()
  end
  if g.marks[key] ~= nil then dnp.unmark(g.marks[key]) end
  local opts = { through_walls = n.through_walls or nil, debug = n.debug or nil }
  if n.height ~= nil then opts.height = fxn(n.height) end
  if n.range ~= nil then opts.range = fxn(n.range) end
  if n.to == "player" then
    if e == nil then return "then" end
    opts.to = e
  end
  g.marks[key] = dnp.mark(on, n.template, marker_values(n, pull), opts)
  return "then"
end
function EXEC.set_marker(g, n, ev, pull)
  if dnp.mark_set == nil then return "then" end
  local key = marker_key(g, n.marker, entity_of(pull, ev, nil, g.data_src[n.id]))
  local id = g.marks[key]
  -- Gone (its entity with it): forgotten here too.
  if id ~= nil and not dnp.mark_set(id, marker_values(n, pull)) then g.marks[key] = nil end
  return "then"
end
function EXEC.remove_marker(g, n, ev, pull)
  if dnp.unmark == nil then return "then" end
  local key = marker_key(g, n.marker, entity_of(pull, ev, nil, g.data_src[n.id]))
  if g.marks[key] ~= nil then dnp.unmark(g.marks[key]); g.marks[key] = nil end
  return "then"
end

-- ------------------------------------------------------------------ INPUT
--
-- A button by name: the engine's own (proto::button), or one of the module's
-- own keys (dnp.actions(), docs/KEYS.md), whose bits are e:actions()'.
local BUTTON = { jump = 0x0001, dash = 0x0002, grab = 0x0004, hover = 0x0008, fly = 0x0010,
                 grapple = 0x0020, use = 0x0040, attack = 0x0080, alt_attack = 0x0100, crouch = 0x0400 }
M.BUTTON = BUTTON
local function own_keys() return dnp.actions ~= nil and dnp.actions() or {} end
-- { engine = bit } or { own = bit }, or nil for a name nobody has.
local function button_of(name)
  if BUTTON[name] ~= nil then return { engine = BUTTON[name] } end
  for i, k in ipairs(own_keys()) do if k == name then return { own = 2 ^ (i - 1) } end end
  return nil
end
local function bit_set(v, b) return v ~= nil and math.floor(v / b) % 2 == 1 end
local function holding(e, b)
  if b.engine then return bit_set(e:buttons(), b.engine) end
  return e.actions ~= nil and bit_set(e:actions(), b.own)
end
function PURE.button_held(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  local b = button_of(n.button)
  return e ~= nil and b ~= nil and holding(e, b) or false
end
-- Each player's buttons and keys last tick, so a press is told on the tick
-- it went down and a release on the tick it came up.
local last_input = {}
local function tick_buttons()
  if not any_start("when_button_pressed") and not any_start("when_button_released") then return end
  for _, e in ipairs(dnp.players() or {}) do
    local key = player_key(e)
    local now = { b = e:buttons() or 0, a = e.actions ~= nil and e:actions() or 0 }
    local was = last_input[key] or { b = 0, a = 0 }
    last_input[key] = now
    if now.b ~= was.b or now.a ~= was.a then
      for _, kind in ipairs({ "when_button_pressed", "when_button_released" }) do
        fire_start(kind, function(n)
          local b = button_of(n.button)
          if b == nil then return false end
          local is, wasdown
          if b.engine then is, wasdown = bit_set(now.b, b.engine), bit_set(was.b, b.engine)
          else is, wasdown = bit_set(now.a, b.own), bit_set(was.a, b.own) end
          if kind == "when_button_pressed" then return is and not wasdown end
          return wasdown and not is
        end, { entity = e })
      end
    end
  end
end

-- ----------------------------------------------------------------- WEAPONS
--
-- The weapon runtime's one entry point: in this VM, or the game's.
local function wapi(op, e, a, b)
  if e == nil then return nil end
  if weapons ~= nil and weapons.api ~= nil then return weapons.api(op, e, a, b) end
  if dnp.game_has ~= nil and dnp.game_has("weapon_api") then return dnp.call_game("weapon_api", op, e, a, b) end
  return nil
end
local function weapon_player(n, g, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e == nil or e.kind == nil or e:kind() ~= 0 then return nil end
  return e
end
function EXEC.give_weapon(g, n, ev, pull) wapi("give", weapon_player(n, g, ev, pull), n.weapon, n.select == true) return "then" end
function EXEC.take_weapon(g, n, ev, pull) wapi("take", weapon_player(n, g, ev, pull), n.weapon) return "then" end
function EXEC.select_weapon(g, n, ev, pull) wapi("select", weapon_player(n, g, ev, pull), n.weapon) return "then" end
function EXEC.give_ammo(g, n, ev, pull) wapi("give_ammo", weapon_player(n, g, ev, pull), n.ammo, n.amount) return "then" end
function EXEC.set_ammo(g, n, ev, pull) wapi("set_ammo", weapon_player(n, g, ev, pull), n.ammo, n.amount) return "then" end
function PURE.held_weapon(g, n, ev, pull)
  local s = wapi("state", weapon_player(n, g, ev, pull))
  return s and s.held or nil
end
function PURE.has_weapon(g, n, ev, pull)
  local s = wapi("state", weapon_player(n, g, ev, pull))
  if s == nil then return false end
  for _, w in ipairs(s.have or {}) do if w == n.weapon then return true end end
  return false
end
function PURE.ammo_of(g, n, ev, pull)
  local s = wapi("state", weapon_player(n, g, ev, pull))
  if s == nil then return nil end
  local pool = n.ammo or s.pool
  return pool and (s.ammo[pool] or 0) or nil
end
-- Told by the weapon runtime (weapons.lua tell_graphs).
function M.weapon_event(kind, e, weapon)
  local ev = { entity = e, weapon = weapon }
  if kind == "changed" then fire_start("when_weapon_changed", nil, ev)
  elseif kind == "fired" then fire_start("when_fired", function(n) return n.weapon == nil or n.weapon == weapon end, ev)
  elseif kind == "out_of_ammo" then fire_start("when_out_of_ammo", nil, ev) end
end
-- From the game, when the weapons are game-wide and this graph is a level's.
if dnp ~= nil and dnp.export ~= nil and not in_game() then
  dnp.export("graph_weapon_event", function(e, kind, weapon) M.weapon_event(kind, e, weapon) end)
end

-- The level's sky (docs/SKY.md). `yaw` in degrees here, turns on the wire.
function EXEC.set_sky(g, n, ev, pull)
  if dnp.sky == nil then return "then" end
  if n.kind == "default" then dnp.sky(nil); return "then" end
  local words = { n.kind, n.image or "" }
  if n["repeat"] ~= nil then words[#words + 1] = "repeat " .. tostring(n["repeat"]) end
  if n.yaw ~= nil then words[#words + 1] = "yaw " .. tostring(n.yaw / 360) end
  dnp.sky(table.concat(words, " "))
  return "then"
end

-- ---------------------------------------------------- CINEMATIC AND CAMERA
--
-- The players a cutscene node acts on: every one in the level with
-- `everyone`, else the linked entity or the event's -- if it is a player.
-- Said ONCE per node when it acted on nobody: "my camera does nothing" is
-- otherwise silent, and the Play log is where a phone shows it.
local nobody_said = {}
local function audience_of(n, g, ev, pull)
  if n.everyone then return dnp.players() or {} end
  -- A run with NO player in it -- a timer, the level starting -- and nothing
  -- linked to `entity`: there is no "the player" to mean, so the node means
  -- everyone here. Before, it meant nobody, silently: an `every` turning the
  -- camera turned no camera. A link that yields nobody still means nobody.
  local linked = g.data_src[n.id] and g.data_src[n.id].entity
  if not linked and ev.entity == nil then return dnp.players() or {} end
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e ~= nil and e.kind ~= nil and e:kind() == 0 then return { e } end
  return {}
end
local function audience(n, g, ev, pull)
  local list = audience_of(n, g, ev, pull)
  if #list == 0 then
    local key = g.name .. "/" .. n.id
    if not nobody_said[key] then
      nobody_said[key] = true
      local why = (g.data_src[n.id] and g.data_src[n.id].entity) and "its linked `entity` is not a player"
                  or (#(dnp.players() or {}) == 0 and "no player is in this level yet")
                  or "the event's entity is not a player"
      say("%s: node '%s' (%s) acted on no player -- %s; tick `everyone` to reach them all",
          g.name, n.id, n.type, why)
    end
  end
  return list
end
local function turns(deg) return (tonumber(deg) or 0) / 360 end

local frozen = {}        -- player index -> the effect ids that took their controls
local spoken = {}        -- player index -> the subtitle line now shown (a counter)

function EXEC.start_cinematic(g, n, ev, pull)
  local ms = math.floor((n.seconds or 0.5) * 1000)
  for _, p in ipairs(audience(n, g, ev, pull)) do
    local i = p:index()
    if frozen[i] == nil and p.add_effect ~= nil then
      -- Rules, not instructions: replicated parameters the client predicts.
      frozen[i] = { p:add_effect("input_scale", "set", fx.int(0)), p:add_effect("attack_enabled", "set", fx.int(0)) }
    end
    dnp.screen(p, { letterbox = n.letterbox or 0.12, letterbox_ms = ms })
  end
  return "then"
end
function EXEC.end_cinematic(g, n, ev, pull)
  local ms = math.floor((n.seconds or 0.5) * 1000)
  for _, p in ipairs(audience(n, g, ev, pull)) do
    local i = p:index()
    for _, id in ipairs(frozen[i] or {}) do if id ~= 0 then p:remove_effect(id) end end
    frozen[i] = nil
    dnp.screen(p, { letterbox = 0, letterbox_ms = ms })
    dnp.camera(p, { cam_mode = 0, cam_path = -1 })
    spoken[i] = (spoken[i] or 0) + 1
    dnp.hud(p, { subtitle = false, subtitle_speaker = false,
                 subtitle_typing = false, subtitle_pack = false, subtitle_sound = false })
  end
  return "then"
end
function EXEC.fade(g, n, ev, pull, go)
  local secs = n.seconds or 1
  for _, p in ipairs(audience(n, g, ev, pull)) do
    dnp.screen(p, { fade = n.to == "black" and 1 or 0, fade_ms = math.floor(secs * 1000) })
  end
  later(ticks_of(secs), function() run(g, n.id, "done", ev) end)
  return "then"
end
-- Every subtitle field, cleared: the line, its speaker and how it is typed.
local NO_SUBTITLE = { subtitle = false, subtitle_speaker = false,
                      subtitle_typing = false, subtitle_pack = false, subtitle_sound = false }

function EXEC.subtitle(g, n, ev, pull)
  local text = tostring(pull("text") or "")
  -- TYPED: the client shows it `pack` letters at a time, `speed` letters a
  -- second, playing `sound` with each step that shows a letter. It stays long
  -- enough to be typed AND read.
  local typed = n.mode == "typed"
  local speed = typed and math.max(1, n.speed or 30) or nil
  local pack = typed and math.max(1, math.floor(n.pack or 2)) or nil
  local secs = n.seconds or ((typed and #text / speed or 0) + math.min(8, 1.5 + #text * 0.05))
  local mine = {}
  for _, p in ipairs(audience(n, g, ev, pull)) do
    local i = p:index()
    spoken[i] = (spoken[i] or 0) + 1
    mine[i] = spoken[i]
    dnp.hud(p, { subtitle = text, subtitle_speaker = n.speaker or false,
                 subtitle_typing = speed or false, subtitle_pack = pack or false,
                 subtitle_sound = (typed and n.sound) or false })
  end
  later(ticks_of(secs), function()
    -- Taken off only if it is still THIS line: a newer one replaced it.
    for i, line in pairs(mine) do
      if spoken[i] == line then
        local p = dnp.entity(i)
        if p ~= nil and p:valid() then dnp.hud(p, NO_SUBTITLE) end
      end
    end
    run(g, n.id, "done", ev)
  end)
  return "then"
end
function EXEC.clear_subtitle(g, n, ev, pull)
  for _, p in ipairs(audience(n, g, ev, pull)) do
    spoken[p:index()] = (spoken[p:index()] or 0) + 1
    dnp.hud(p, NO_SUBTITLE)
  end
  return "then"
end

-- A map by what the author calls it: the name the server lists (dnp.levels),
-- forgiving the extension and the case, or its number.
local function level_index(want)
  if type(want) == "number" then return want end
  if type(want) ~= "string" or dnp.levels == nil then return nil end
  local list = dnp.levels() or {}
  local base = want:gsub("%.dnpm$", ""):gsub("%.map$", "")
  for i, name in ipairs(list) do if name == base then return i - 1 end end
  for i, name in ipairs(list) do if name:lower() == base:lower() then return i - 1 end end
  return nil
end
M.level_index = level_index

-- A request, not a setting: the client asks its shell to close the app
-- (docs/protocol.md SCREEN_SET field 4). The server does nothing else -- the
-- player's leaving is the connection closing, as any leaving is.
function EXEC.quit_game(g, n, ev, pull)
  for _, p in ipairs(audience(n, g, ev, pull)) do dnp.screen(p, { quit = 1 }) end
  return "then"
end
function EXEC.change_map(g, n, ev, pull)
  local to = level_index(n.level)
  local players = audience(n, g, ev, pull)
  if to == nil or #players == 0 then return "then" end
  local function go()
    for _, p in ipairs(players) do
      if p:valid() then p:change_map(to, n.spawn) end
    end
  end
  if n.fade ~= nil and n.fade > 0 then
    for _, p in ipairs(players) do dnp.screen(p, { fade = 1, fade_ms = math.floor(n.fade * 1000) }) end
    later(ticks_of(n.fade), go)
  else
    go()
  end
  return "then"
end

function EXEC.camera_follow(g, n, ev, pull)
  local f = { cam_mode = 0, cam_path = -1 }
  if n.distance ~= nil then f.distance = n.distance end
  if n.height ~= nil then f.pitch = turns(n.height) end
  if n.angle ~= nil then f.yaw = turns(n.angle) end
  if n.lock ~= nil then f.lock_yaw = n.lock and 1 or 0; f.lock_zoom = n.lock and 1 or 0 end
  if n.lead ~= nil then f.lead = n.lead end
  if n.lead_velocity ~= nil then f.lead_velocity = n.lead_velocity end
  for _, p in ipairs(audience(n, g, ev, pull)) do dnp.camera(p, f) end
  return "then"
end
function EXEC.camera_flythrough(g, n, ev, pull)
  local speed = n.speed or 6
  for _, p in ipairs(audience(n, g, ev, pull)) do
    dnp.camera(p, { cam_mode = 2, cam_path = n.path, cam_path_speed = speed, cam_path_blend = n.blend or 1 })
  end
  -- The engine has no completion event: the shot's end is arithmetic, from
  -- the path's baked length (dnp.path_length) and the speed it is flown at.
  local len = dnp.path_length ~= nil and dnp.path_length(n.path) or nil
  if len ~= nil then
    later(math.max(1, math.ceil(unfx(len) / speed * 60)), function() run(g, n.id, "done", ev) end)
  end
  return "then"
end
function EXEC.camera_track(g, n, ev, pull)
  for _, p in ipairs(audience(n, g, ev, pull)) do
    dnp.camera(p, { cam_mode = 1, cam_path = n.path, cam_path_blend = n.blend or 1 })
  end
  return "then"
end
function EXEC.camera_shake(g, n, ev, pull)
  for _, p in ipairs(audience(n, g, ev, pull)) do
    dnp.camera(p, { cam_shake = n.strength or 0.3, cam_shake_time = math.floor((n.seconds or 0.5) * 1000) })
  end
  return "then"
end

function PURE.position_of(g, n, ev, pull, pin)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e == nil then return nil end
  return unfx(e:pos()[pin])
end
function PURE.health_of(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  if e == nil then return nil end
  -- A monster's health is its record's; the engine's slot is a player's.
  local m = mobs ~= nil and mobs.health ~= nil and mobs.health(e) or nil
  if m ~= nil then return m end
  return e.hp ~= nil and e:hp() or nil
end
function PURE.distance(g, n, ev, pull)
  local a, b = entity_of(pull, ev, "a", g.data_src[n.id]), entity_of(pull, ev, "b", g.data_src[n.id])
  if a == nil or b == nil then return nil end
  local p, q = a:pos(), b:pos()
  local dx, dy, dz = unfx(p.x - q.x), unfx(p.y - q.y), unfx(p.z - q.z)
  return math.sqrt(dx * dx + dy * dy + dz * dz)
end
function PURE.nearest_player(g, n, ev, pull)
  local to = entity_of(pull, ev, "to", g.data_src[n.id]) or ev.entity
  local best, bd = nil, math.huge
  for _, p in ipairs(dnp.players() or {}) do
    local d = 0
    if to ~= nil and to:valid() then
      local a, b = p:pos(), to:pos()
      local dx, dy = unfx(a.x - b.x), unfx(a.y - b.y)
      d = dx * dx + dy * dy
    end
    if d < bd then best, bd = p, d end
  end
  return best
end
function PURE.sector_of(g, n, ev, pull)
  local e = entity_of(pull, ev, nil, g.data_src[n.id])
  return e ~= nil and index_of(e:sector()) or nil
end

-- A NODE'S NUMBERS, LINKED (docs/GRAPH_NUMBERS.md): a node whose numeric
-- fields are data inputs with a link is handed to its function as a view in
-- which those fields hold the linked values -- so every `n.seconds` in this
-- file reads the link when there is one and the literal when there is not,
-- with nothing in the nodes themselves to change.
view_of = function(n, pull)
  if n.linked_nums == nil then return n end
  local v = setmetatable({}, { __index = n })
  for _, a in ipairs(n.linked_nums) do
    local x = pull(a)
    if x ~= nil then v[a] = x end
  end
  return v
end

-- The same for a START node, checked outside any run: its linked numbers are
-- worked out afresh, from pure nodes, each time it is asked.
local function eval_out(g, id, out, ev, memo)
  local key = id .. "." .. out
  if memo[key] ~= nil then return memo[key][1] end
  local m = g.by_id[id]
  if m == nil or PURE[m.type] == nil then return nil end
  local function pull(input)
    local src = g.data_src[m.id] and g.data_src[m.id][input]
    if src then return eval_out(g, src[1], src[2], ev, memo) end
    return m[input]
  end
  local v = PURE[m.type](g, view_of(m, pull), ev, pull, out)
  memo[key] = { v }
  return v
end
start_view = function(g, n)
  if n.linked_nums == nil then return n end
  local memo = {}
  return view_of(n, function(input)
    local src = g.data_src[n.id] and g.data_src[n.id][input]
    if src then return eval_out(g, src[1], src[2], {}, memo) end
    return nil
  end)
end

-- One run, from exit `pin` of node `start`.
run = function(g, start, pin, ev)
  local memo = {}
  local function value_of(id, out)
    local key = id .. "." .. out
    local n = g.by_id[id]
    -- An exec node's output (a `spawn`'s entity) is what it last produced in
    -- this run: read fresh, never remembered from before it ran.
    local live = EXEC[n.type] ~= nil
    local hit = not live and memo[key] or nil
    if hit ~= nil then return hit[1] end
    local function pull(input)
      local src = g.data_src[n.id] and g.data_src[n.id][input]
      if src then return value_of(src[1], src[2]) end
      return n[input]
    end
    local v = PURE[n.type](g, view_of(n, pull), ev, pull, out)
    if not live then memo[key] = { v } end   -- boxed, so a nil or false is remembered too
    return v
  end
  local stack = { { start, pin } }
  local steps = 0
  while #stack > 0 do
    local top = table.remove(stack)
    local nxt = g.exec_next[top[1]] and g.exec_next[top[1]][top[2]]
    while nxt ~= nil do
      steps = steps + 1
      if steps > MAX_STEPS then
        if not g.overran then
          g.overran = true
          say("%s: a run went past %d steps at node '%s' and was stopped -- a loop?",
              g.name, MAX_STEPS, nxt)
        end
        return
      end
      local n = g.by_id[nxt]
      local function pull(input)
        local src = g.data_src[n.id] and g.data_src[n.id][input]
        if src then return value_of(src[1], src[2]) end
        return n[input]
      end
      local function go(exit) stack[#stack + 1] = { n.id, exit } end
      local exit = EXEC[n.type](g, view_of(n, pull), ev, pull, go)
      nxt = exit and g.exec_next[n.id] and g.exec_next[n.id][exit] or nil
    end
  end
end

local function fire(event, ev)
  for _, g in ipairs(loaded) do
    for _, id in ipairs(g.by_event[event] or {}) do run(g, id, "then", ev) end
  end
end
M.fire = fire

-- ------------------------------------------------------ WHEN THINGS HAPPEN
--
-- The start nodes a designer reaches for (graph_vocab.lua): each fires once
-- for the moment it names -- a prop reached, a sector entered, a floor stepped
-- on -- found by watching for the CHANGE, where the engine reports the state.
local STARTS = {}
for _, n in ipairs(V.nodes) do if n.starts then STARTS[n.name] = true end end
for name in pairs(STARTS) do PURE[name] = PURE.event end   -- outputs: what the moment carried

any_start = function(kind)
  for _, g in ipairs(loaded) do if g.by_start[kind] ~= nil then return true end end
  return false
end
fire_start = function(kind, test, ev, exit)
  for _, g in ipairs(loaded) do
    for _, id in ipairs(g.by_start[kind] or {}) do
      local n = start_view(g, g.by_id[id])
      if test == nil or test(n) then run(g, id, exit or "then", ev) end
    end
  end
end
local function is_player(e) return e ~= nil and e.kind ~= nil and e:kind() == 0 end
local function who_ok(who, e)
  who = who or "players"
  if who == "anyone" then return true end
  if who == "players" then return is_player(e) end
  return e ~= nil and not is_player(e)
end
local function sector_ok(n, idx)
  -- -1: a sector of another level of the region, which this one cannot name.
  if idx == nil or idx < 0 then return false end
  if n.sector ~= nil then return idx == n.sector end
  if n.tag ~= nil then
    local sec = dnp.sector(idx)
    return sec ~= nil and sec:tag() == n.tag
  end
  return true
end

-- A death or a hurt, told by the mob runtime (mobs.lua, hurt).
function M.notify(what, target, by, amount, kind)
  local ev = { entity = target, by = by, amount = amount, target = by }
  if what == "dies" then
    fire_start("when_dies", function(n) return who_ok(n.what or "monsters", target) and (n.kind == nil or n.kind == kind) end, ev)
  elseif what == "hurt" then
    fire_start("when_hurt", function(n) return who_ok(n.what or "anyone", target) and (n.kind == nil or n.kind == kind) end, ev)
  end
end

-- Touches arrive every tick a body stays in a prop; a touch STARTS when the
-- pair was not touching the tick before.
local touch_seen = {}
local function touched(entity, prop)
  if entity == nil or prop == nil or not any_start("when_prop_touched") then return end
  local now = dnp.tick()
  local key = entity:index() .. "/" .. prop:index()
  local last = touch_seen[key]
  touch_seen[key] = now
  if last ~= nil and last >= now - 1 then return end
  local name = prop.name ~= nil and prop:name() or nil
  fire_start("when_prop_touched", function(n)
    return who_ok(n.who, entity) and (n.prop == nil or n.prop == name)
  end, { entity = entity, prop = prop, target = prop })
end

-- What on_touch feeds the start nodes: reachable for a script relaying a
-- touch, and for a test (the hooks themselves are harvested by the VM).
M.touched = function(entity, prop) touched(entity, prop) end

-- What each watched body was doing last tick: its sector, and the floor it
-- stood on (its sector, when grounded).
local track = {}
local t0 = nil
local function watch_everyone()
  for _, kind in ipairs({ "when_sector_entered", "when_sector_left", "when_floor_touched", "when_near" }) do
    for _, g in ipairs(loaded) do
      for _, id in ipairs(g.by_start[kind] or {}) do
        if (g.by_id[id].who or "players") ~= "players" then return true end
      end
    end
  end
  return false
end
local function tick_starts(now)
  if t0 == nil then
    t0 = now
    fire_start("when_level_starts", nil, {})
  end
  local el = now - t0
  fire_start("every", function(n) local p = math.max(1, ticks_of(n.seconds)); return el > 0 and el % p == 0 end, {})
  fire_start("after", function(n) return el == ticks_of(n.seconds) end, {})

  local spatial = any_start("when_sector_entered") or any_start("when_sector_left")
               or any_start("when_floor_touched") or any_start("when_near")
  local roster = not in_game() and (any_start("when_player_arrives") or any_start("when_player_leaves"))
  if not spatial and not roster then return end
  local list = (spatial and watch_everyone()) and dnp.entities() or dnp.players()
  local seen = {}
  for _, e in ipairs(list or {}) do
    if e:valid() then
      local i = e:index()
      seen[i] = true
      local sec = index_of(e:sector())
      local floor = (e.grounded ~= nil and e:grounded()) and sec or nil
      local t = track[i]
      -- WHO, and WHEN LAST. A level with nobody in it does not tick, so a
      -- player who left was never seen leaving; the one who arrives next --
      -- or the same one, back -- would find the slot taken and never be
      -- seen arriving (a title screen's panel, once, then never). A gap in
      -- this level's clock, or another player in the slot, is a departure
      -- and a new arrival.
      local who = (e.player ~= nil and is_player(e)) and e:player() or nil
      -- IN THIS LEVEL, not merely in its world: in a region every resident
      -- level sees every player, and one standing in another level's geometry
      -- has a sector this level cannot name (-1). Arriving is coming into this
      -- level's own sectors, and leaving is going out of them -- which is the
      -- server's own test for a player's home (docs/REGION_GAPS.md §2).
      local here = sec ~= nil and sec >= 0
      if t ~= nil and (t.seen < now - 1 or t.who ~= who) then
        track[i] = nil
        if roster and t.player and t.here then fire_start("when_player_leaves", nil, {}) end
        t = nil
      end
      if t == nil then
        track[i] = { sector = sec, floor = floor, since = now, player = is_player(e), who = who, seen = now, here = here }
        if roster and here and is_player(e) then fire_start("when_player_arrives", nil, { entity = e }) end
      else
        t.seen = now
        if here ~= t.here then
          t.here = here
          if roster and t.player then
            if here then fire_start("when_player_arrives", nil, { entity = e })
            else fire_start("when_player_leaves", nil, {}) end
          end
        end
        if sec ~= t.sector then
          local was = t.sector
          t.sector = sec
          if was ~= nil then
            fire_start("when_sector_left", function(n) return who_ok(n.who, e) and sector_ok(n, was) end,
                       { entity = e, sector = was })
          end
          if sec ~= nil then
            fire_start("when_sector_entered", function(n) return who_ok(n.who, e) and sector_ok(n, sec) end,
                       { entity = e, sector = sec })
          end
        end
        if floor ~= t.floor then
          t.floor = floor
          t.since = now
          if floor ~= nil then
            fire_start("when_floor_touched", function(n) return who_ok(n.who, e) and sector_ok(n, floor) end,
                       { entity = e, sector = floor })
          end
        elseif floor ~= nil then
          local held = now - t.since
          fire_start("when_floor_touched", function(n)
            if n.repeat_every == nil then return false end
            local p = math.max(1, ticks_of(n.repeat_every))
            return held > 0 and held % p == 0 and who_ok(n.who, e) and sector_ok(n, floor)
          end, { entity = e, sector = floor })
        end
      end
      if spatial then
        for _, g in ipairs(loaded) do
          for _, id in ipairs(g.by_start.when_near or {}) do
            local n = start_view(g, g.by_id[id])
            if who_ok(n.who, e) then
              local p = e:pos()
              local dx, dy = unfx(p.x) - n.x, unfx(p.y) - n.y
              local r = n.radius or 2
              local inside = dx * dx + dy * dy <= r * r
              g.near = g.near or {}
              local key = id .. "/" .. i
              if inside and not g.near[key] then
                g.near[key] = true
                run(g, id, "near", { entity = e })
              elseif not inside and g.near[key] then
                g.near[key] = nil
                run(g, id, "away", { entity = e })
              end
            end
          end
        end
      end
    end
  end
  for i, t in pairs(track) do
    if not seen[i] then
      track[i] = nil
      if roster and t.player and t.here then fire_start("when_player_leaves", nil, {}) end
    end
  end
  -- Touches not renewed are over; forget them now and then.
  if now % 600 == 0 then
    for k, last in pairs(touch_seen) do if last < now - 2 then touch_seen[k] = nil end end
  end
end

-- The on_call nodes of `name`, in every graph of this VM, in load order.
-- Bounded in depth: a call that leads back to itself stops, and says so.
fire_call = function(name, entity, value)
  if call_depth >= MAX_CALL_DEPTH then
    say("a call to '%s' is %d deep -- stopped (calls that call each other?)", tostring(name), call_depth)
    return
  end
  call_depth = call_depth + 1
  local ok, err = pcall(function()
    for _, g in ipairs(loaded) do
      for _, id in ipairs(g.by_call[name] or {}) do run(g, id, "then", { entity = entity, value = value }) end
    end
  end)
  call_depth = call_depth - 1
  if not ok then error(err, 0) end
end
M.call = function(name, entity, value) fire_call(name, entity, value) end

-- Reachable from the other side: the game calls a level's graphs through
-- dnp.call_level(e, "graph_call", name, value), which hands the export (e,
-- name, value); a level calls the game's through dnp.call_game("graph_call",
-- name, value, entity).
if dnp ~= nil and dnp.export ~= nil then
  if dnp.is_game ~= nil and dnp.is_game() then
    dnp.export("graph_call", function(name, value, entity) fire_call(name, entity, value) end)
  else
    dnp.export("graph_call", function(e, name, value) fire_call(name, e, value) end)
  end
end

-- SAVED with the game (docs/SAVING.md): each graph's variables and which of
-- its `once` nodes have been passed, by the graph's NAME -- its identity, so a
-- file renamed or moved keeps its state. Not its pending
-- delays -- a save keeps state, not what was in the middle of happening. A
-- variable a later version of the graph no longer has, or now holds as a
-- different type, is dropped rather than forced in.
if dnp ~= nil and dnp.persist ~= nil then
  dnp.persist("graphs", function()
    local out = {}
    for _, g in ipairs(loaded) do
      local spent = {}
      for id in pairs(g.spent) do spent[#spent + 1] = id end
      table.sort(spent)
      out[g.name] = { vars = g.vars, spent = spent }
    end
    return out
  end, function(t)
    for _, g in ipairs(loaded) do
      local s = t and (t[g.name] or t[g.label])   -- by name; by file before 2026-10-04
      if type(s) == "table" then
        for k, v in pairs(s.vars or {}) do
          if g.vars[k] ~= nil and type(g.vars[k]) == type(v) then g.vars[k] = v end
        end
        for _, id in ipairs(s.spent or {}) do
          if g.by_id[id] ~= nil and g.by_id[id].type == "once" then g.spent[id] = true end
        end
      end
    end
  end)
end

local function drain_timers(now)
  if #timers == 0 then return end
  local due, keep = {}, {}
  for _, t in ipairs(timers) do
    if t.tick <= now then due[#due + 1] = t else keep[#keep + 1] = t end
  end
  timers = keep
  for _, t in ipairs(due) do run(t.graph, t.node, t.pin, t.ev) end
end

-- The hooks, one per event: harvested by the VM into its lists beside every
-- other script's, so a map's triggers and its graphs both hear everything.
function on_tick()
  tick_buttons()
  drain_jobs(dnp.tick())
  drain_timers(dnp.tick())
  if #loaded > 0 then
    tick_starts(dnp.tick())
    fire("on_tick", {})
  end
end
function on_post_tick() if #loaded > 0 then fire("on_post_tick", {}) end end
function on_interact(entity, side, sector)
  fire("on_interact", { entity = entity, side = side, sector = sector })
  local si = index_of(side)
  fire_start("when_wall_used", function(n) return n.side == nil or n.side == si end,
             { entity = entity, side = side, sector = index_of(sector) })
end
function on_cross(entity, side, sector) fire("on_cross", { entity = entity, side = side, sector = sector }) end
function on_touch(entity, prop) fire("on_touch", { entity = entity, target = prop }); touched(entity, prop) end
function on_prop_use(entity, prop)
  fire("on_prop_use", { entity = entity, target = prop })
  local name = prop ~= nil and prop.name ~= nil and prop:name() or nil
  fire_start("when_prop_used", function(n) return n.prop == nil or n.prop == name end,
             { entity = entity, prop = prop, target = prop })
end
function on_attack(entity, target) fire("on_attack", { entity = entity, target = target }) end
function on_crush(entity, sector, kind) fire("on_crush", { entity = entity, sector = sector, crush_kind = kind }) end
local function ui_event(entity, ui_id, widget_id, value)
  -- A panel this VM's graphs opened: its press, or its × (widget 0), by name.
  -- A trigger table's panel is not in `by_ui`, so it fires nothing here.
  local open = by_ui[ui_id]
  if open ~= nil then
    if widget_id == 0 then
      forget_panel(open.key)
      fire_start("when_panel_closed", function(n) return n.panel == open.panel end, { entity = entity })
    else
      local def = panel_defs[open.panel] and panel_defs[open.panel].def
      local w = def and def.by_id[widget_id]
      if w ~= nil then
        fire_start("when_panel_used", function(n)
          return n.panel == open.panel and (n.widget == nil or n.widget == w.name)
        end, { entity = entity, widget = w.name, value = value })
      end
    end
  end
  fire("on_ui_event", { entity = entity, ui_id = ui_id, widget_id = widget_id, value = value })
end
on_ui_event = ui_event
-- Reachable for a test, as touched is: the VM harvests the hook itself.
M.ui_event = ui_event
function on_player_enter(entity)
  fire("on_player_enter", { entity = entity })
  fire_start("when_player_arrives", nil, { entity = entity })
end
function on_player_leave(player)
  -- The panels it had from this VM went with it.
  for id, o in pairs(by_ui) do
    if o.player == player then open_panels[o.key] = nil; by_ui[id] = nil end
  end
  fire("on_player_leave", { value = player })
  fire_start("when_player_leaves", nil, { value = player })
end

-- ------------------------------------------------------------- validation

-- Every node type in the vocabulary is implemented, and nothing is implemented
-- that the vocabulary does not list -- the control every vocabulary here has.
--
-- RE-READ on every call rather than using SPEC, so a caller that changed the
-- vocabulary is compared against the changed one -- otherwise the control for
-- this check would pass without looking at anything.
function M.check_vocabulary()
  local wrong, listed = {}, {}
  for _, n in ipairs(V.nodes) do
    listed[n.name] = true
    if PURE[n.name] == nil and EXEC[n.name] == nil then wrong[#wrong + 1] = "no implementation of '" .. n.name .. "'" end
  end
  for name in pairs(PURE) do if not listed[name] then wrong[#wrong + 1] = "'" .. name .. "' is not in the vocabulary" end end
  for name in pairs(EXEC) do if not listed[name] then wrong[#wrong + 1] = "'" .. name .. "' is not in the vocabulary" end end
  -- The game's lists name only what exists: a typo there would refuse a verb
  -- that works, or let one through by a name nothing has.
  if trigger_vocab ~= nil and V.game ~= nil then
    local have = { event = {}, condition = {}, action = {} }
    for _, e in ipairs(trigger_vocab.events) do have.event[e.name] = true end
    have.event.on_player_enter = true
    have.event.on_player_leave = true
    for _, c in ipairs(trigger_vocab.conditions) do have.condition[c.name] = true end
    for _, a in ipairs(trigger_vocab.actions) do have.action[a.name] = true end
    for kind, list in pairs({ event = V.game.events, condition = V.game.conditions, action = V.game.actions }) do
      for _, name in ipairs(list) do
        if not have[kind][name] then wrong[#wrong + 1] = ("graph_vocab.game names %s '%s', which does not exist"):format(kind, name) end
      end
    end
  end
  return wrong
end

local function check_type(arg, v)
  local t = arg.type
  if t == "int" then return type(v) == "number" and v == math.floor(v) end
  if t == "number" then return type(v) == "number" end
  if t == "string" then return type(v) == "string" end
  if t == "bool" then return type(v) == "boolean" end
  if t == "table" then return type(v) == "table" end
  if t == "enum" then for _, x in ipairs(arg.values) do if v == x then return true end end return false end
  if t == "value" then return type(v) == "number" or type(v) == "string" or type(v) == "boolean" end
  return false
end

local function parse_pin(s)
  if type(s) ~= "string" then return nil end
  local id, pin = s:match("^(.+)%.([^%.]+)$")
  return id, pin
end

--- Load one graph. Returns true, or false and the problems -- and loads nothing
--- of a graph that has any.
function M.load(data, label)
  label = label or (type(data) == "table" and data.name) or "graph"
  local problems = {}
  local function bad(fmt, ...) problems[#problems + 1] = ("%s: " .. fmt):format(label, ...) end

  for _, w in ipairs(M.check_vocabulary()) do bad("vocabulary: %s", w) end
  if type(data) ~= "table" then bad("is not a table"); say("%s", problems[1]); return false, problems end
  for k in pairs(data) do if GRAPH_FIELD[k] == nil then bad("has no field '%s'", tostring(k)) end end
  for _, f in ipairs(V.graph_fields) do
    if data[f.name] == nil then
      if not f.optional then bad("needs '%s'", f.name) end
    elseif not check_type(f, data[f.name]) then
      bad("'%s' should be a %s", f.name, f.type)
    end
  end
  if data.graph ~= nil and data.graph ~= 1 then bad("is format %s; this runtime reads 1", tostring(data.graph)) end
  if #problems > 0 then
    for i = 1, math.min(#problems, MAX_REPORTS) do say("%s", problems[i]) end
    say("%s: %d problems -- the graph is not loaded", label, #problems)
    return false, problems
  end

  local vars = {}
  for k, v in pairs(data.vars or {}) do
    if type(k) ~= "string" then bad("a variable name must be a string")
    elseif PROPERTY[k] ~= nil then
      bad("variable '%s' has an entity property's name -- get_var and set_var would not know which you meant", k)
    elseif type(v) ~= "number" and type(v) ~= "string" and type(v) ~= "boolean" then
      bad("variable '%s' starts as a %s; a number, string or boolean only", k, type(v))
    else vars[k] = v end
  end

  -- PANELS (docs/PANELS.md 2): each checked as a trigger table's panel is --
  -- the same widget kinds and limits, from trigger_vocab when it is loaded --
  -- plus names a node can say. A problem refuses the graph: dnp.ui_open
  -- raises on a bad widget, inside the callback that opened it.
  local panels = {}
  local NAME = "^[a-z0-9_]+$"
  local pv = trigger_vocab ~= nil and trigger_vocab.panels or nil
  local kinds = {}
  if pv ~= nil then for _, k in ipairs(pv.widgets) do kinds[k.name] = k end
  else for _, k in ipairs({ "label", "button", "toggle", "slider", "select" }) do kinds[k] = true end end
  local max_widgets = pv and pv.max_widgets or 32
  local max_options = pv and pv.max_options or 32
  for pname, p in pairs(data.panels or {}) do
    local where = ("panel '%s'"):format(tostring(pname))
    if type(pname) ~= "string" or #pname > 24 or not pname:match(NAME) then
      bad("%s: a panel's name is 1..24 of a-z, 0-9 and _", where)
    elseif type(p) ~= "table" then bad("%s: is not a table", where)
    else
      if p.title ~= nil and type(p.title) ~= "string" then bad("%s: its title is not a string", where) end
      if p.template ~= nil and (type(p.template) ~= "string" or #p.template > 24 or not p.template:match(NAME)) then
        bad("%s: its template is 1..24 of a-z, 0-9 and _", where)
      end
      local widgets = p.widgets or {}
      if type(widgets) ~= "table" then widgets = {}; bad("%s: its widgets are a list", where) end
      if #widgets > max_widgets then bad("%s: %d widgets, the protocol's limit is %d", where, #widgets, max_widgets) end
      if p.pinned ~= nil and type(p.pinned) ~= "boolean" then bad("%s: `pinned` is true or false", where) end
      local def = { title = p.title, template = p.template, pinned = p.pinned == true, widgets = {}, by_name = {}, by_id = {} }
      for i, w in ipairs(widgets) do
        local ww = ("%s widget %d"):format(where, i)
        if type(w) ~= "table" then bad("%s: is not a table", ww)
        elseif kinds[w.kind] == nil then bad("%s: no widget kind '%s'", ww, tostring(w.kind))
        else
          if type(w.name) ~= "string" or #w.name > 24 or not w.name:match(NAME) then
            bad("%s: needs a `name`, 1..24 of a-z, 0-9 and _", ww)
          elseif def.by_name[w.name] ~= nil then bad("%s: two widgets are named '%s'", where, w.name) end
          if type(w.id) ~= "number" or w.id ~= math.floor(w.id) or w.id < 1 or w.id > 65535 then
            bad("%s: `id` is a whole number from 1 (0 means the panel was closed)", ww)
          elseif def.by_id[w.id] ~= nil then bad("%s: two widgets are id %d", where, w.id) end
          if type(w.label) ~= "string" then bad("%s: needs a `label`", ww) end
          if w.hidden ~= nil and type(w.hidden) ~= "boolean" then bad("%s: `hidden` is true or false", ww) end
          if w.kind == "slider" and (type(w.lo) ~= "number" or type(w.hi) ~= "number") then
            bad("%s: a slider needs `lo` and `hi`", ww)
          end
          if w.kind == "select" then
            if type(w.options) ~= "table" or #w.options == 0 then bad("%s: a select needs `options`", ww)
            elseif #w.options > max_options then bad("%s: %d options, the protocol's limit is %d", ww, #w.options, max_options) end
          end
          if type(w.name) == "string" then def.by_name[w.name] = w end
          if type(w.id) == "number" then def.by_id[w.id] = w end
          -- `name` rides along: dnp.ui_open reads the keys it knows and no others.
          def.widgets[#def.widgets + 1] = w
        end
      end
      panels[pname] = def
    end
  end

  local events = event_names()
  local by_id, order = {}, {}
  for i, n in ipairs(data.nodes) do
    local where = ("node %d"):format(i)
    if type(n) ~= "table" then bad("%s is not a table", where)
    elseif type(n.id) ~= "string" or n.id == "" or n.id:find("%.") then
      bad("%s needs an `id`: a non-empty name without dots", where)
    elseif by_id[n.id] ~= nil then bad("two nodes are '%s'", n.id)
    else
      where = ("node '%s'"):format(n.id)
      local spec = SPEC[n.type]
      if spec == nil then bad("%s: no node type '%s'", where, tostring(n.type))
      else
        local fields = {}
        for k in pairs(NODE_FIELD) do fields[k] = true end
        for _, a in ipairs(spec.args) do fields[a.name] = true end
        local verb = n.type == "condition" or n.type == "action"
        if n.type == "on_call" and type(n.name) == "string" and n.name == "" then
          bad("%s: an on_call needs a `name`", where)
        end
        if not verb then
          for k in pairs(n) do if not fields[k] then bad("%s: a %s has no field '%s'", where, n.type, tostring(k)) end end
          for _, a in ipairs(spec.args) do
            local v = n[a.name]
            if v == nil then
              -- Asked again after the links: a required field fed by a link is given.
              if not a.optional then n._missing = n._missing or {}; n._missing[#n._missing + 1] = a.name end
            elseif not check_type(a, v) then
              bad("%s: '%s' should be a %s", where, a.name, a.type)
            end
          end
        end
        if n.type == "event" and type(n.event) == "string" and not events[n.event] then
          if in_game() then
            bad("%s: '%s' does not reach the game -- a game graph starts from %s",
                where, n.event, table.concat(V.game.events, ", "))
          else
            bad("%s: '%s' is not an event a graph can start from", where, n.event)
          end
        end
        -- A level's node in a game graph: the game has no level of its own.
        if spec.level_only and in_game() then
          bad("%s: a %s acts on a level, and a game graph has none -- `call` a level's graph", where, n.type)
        end
        -- A map named must be one the server has, and a spawn point picked on
        -- this map must be on it.
        if n.type == "change_map" and dnp.levels ~= nil and #(dnp.levels() or {}) > 0 then
          local to = level_index(n.level)
          if to == nil or to < 0 or to >= #dnp.levels() then
            bad("%s: there is no map '%s' -- the server has %s", where, tostring(n.level),
                table.concat(dnp.levels(), ", "))
          end
        end
        if n.type == "teleport" and n.spawn ~= nil and has_map() and dnp.spawn_point ~= nil
           and dnp.spawn_point(n.spawn) == nil then
          bad("%s: this map has no spawn point tagged '%s'", where, n.spawn)
        end
        -- A camera path picked on the map must be there.
        if (n.type == "camera_flythrough" or n.type == "camera_track") and type(n.path) == "number"
           and has_map() and dnp.path_length ~= nil and dnp.path_length(n.path) == nil then
          bad("%s: the map has no camera path tagged %d", where, n.path)
        end
        -- A sector or wall picked on the map must still be there.
        if has_map() and type(n.sector) == "number" and dnp.sector(n.sector) == nil then
          bad("%s: sector %d is not in this map", where, n.sector)
        end
        if has_map() and (n.type == "set_wall_texture" or n.type == "when_wall_used")
           and type(n.side) == "number" and dnp.side(n.side) == nil then
          bad("%s: side %d is not in this map", where, n.side)
        end
        if (n.type == "move_floor" or n.type == "move_ceiling") and n.to ~= nil and spec ~= nil then
          local ok = false
          for _, a in ipairs(spec.args) do if a.name == "to" then for _, v in ipairs(a.values) do ok = ok or v == n.to end end end
          if not ok then bad("%s: `to` is '%s'", where, tostring(n.to)) end
        end
        -- A level's verb in a game graph: the game has no level of its own.
        if verb and in_game() then
          local name = n.type == "condition" and n.c or n.a
          if type(name) == "string" and not GAME_OK[n.type][name] then
            bad("%s: %s '%s' acts on a level, and a game graph has none -- " ..
                "`call` a level's graph to do it there", where, n.type, name)
          end
        end
        if n.type == "sequence" and type(n.count) == "number" and (n.count < 2 or n.count > 8) then
          bad("%s: `count` is 2 to 8", where)
        end
        if n.type == "delay" and type(n.ticks) == "number" and n.ticks < 1 then
          bad("%s: `ticks` is at least 1", where)
        end
        if n.type == "save_vars" or n.type == "load_vars" or n.type == "forget_saved" then
          if n.store ~= nil and (type(n.store) ~= "string" or not n.store:match("^[%w_%-]+$") or #n.store > 48) then
            bad("%s: `store` '%s' is not a store name: 1-48 letters, digits, _ or -", where, tostring(n.store))
          end
          if type(n.vars) == "string" then
            for name in n.vars:gmatch("[^,%s]+") do
              if vars[name] == nil then bad("%s: no variable '%s' in `vars` to keep", where, name) end
            end
          end
        end
        if (n.type == "get_var" or n.type == "set_var" or n.type == "add_var") and type(n.var) == "string" then
          local prop = vars[n.var] == nil and PROPERTY[n.var] or nil
          if vars[n.var] == nil and prop == nil then
            bad("%s: no variable '%s' in `vars`, and no entity property by that name", where, n.var)
          elseif prop ~= nil and n.type ~= "get_var" and prop.access ~= "read_write" then
            bad("%s: an entity's '%s' can be read, not set", where, n.var)
          elseif prop ~= nil and n.type == "add_var" and prop.type ~= "int" and prop.type ~= "number" then
            bad("%s: '%s' is not a number to add to", where, n.var)
          end
        end
        if verb then
          if triggers == nil or triggers.verbs == nil then
            bad("%s: a %s needs triggers.lua loaded before graphs.lua", where, n.type)
          else
            -- The verb's own entry: the node's fields less the graph's.
            local entry = {}
            for k, v in pairs(n) do if k ~= "id" and k ~= "type" and k ~= "meta" then entry[k] = v end end
            n.entry = entry
          end
        end
        by_id[n.id] = n
        order[#order + 1] = n
      end
    end
  end

  -- Links: output pin to input pin, exec to exec and data to data.
  local exec_next, data_src = {}, {}
  for i, l in ipairs(data.links) do
    local where = ("link %d"):format(i)
    if type(l) ~= "table" or #l ~= 2 then bad("%s: is [\"<node>.<pin>\", \"<node>.<pin>\"]", where)
    else
      local fid, fpin = parse_pin(l[1])
      local tid, tpin = parse_pin(l[2])
      local f, t = fid and by_id[fid], tid and by_id[tid]
      if not f or not t then bad("%s: '%s' -> '%s' names a node that does not exist", where, tostring(l[1]), tostring(l[2]))
      else
        local fs, ts = SPEC[f.type], SPEC[t.type]
        local f_exec, f_data = set_of(fs.exec_out), set_of(fs.data_out)
        local t_exec, t_data = set_of(ts.exec_in), set_of(ts.data_in)
        -- A verb's arguments of a pinnable type are data inputs too.
        if ts.data_in_args then
          local vs = verb_spec(t.type, t.type == "condition" and t.c or t.a)
          for _, a in ipairs(vs and vs.args or {}) do
            if PINNABLE[a.type] then t_data[a.name] = true end
          end
        end
        if f.type == "sequence" and f_exec[fpin] and tonumber(fpin) >= (f.count or 0) then f_exec[fpin] = nil end
        if f_exec[fpin] and t_exec[tpin] then
          exec_next[fid] = exec_next[fid] or {}
          if exec_next[fid][fpin] ~= nil then bad("%s: '%s' already leads somewhere; an exec output has one link", where, l[1])
          else exec_next[fid][fpin] = tid end
        elseif f_data[fpin] and t_data[tpin] then
          data_src[tid] = data_src[tid] or {}
          if data_src[tid][tpin] ~= nil then bad("%s: '%s' already has an input; a data input takes one link", where, l[2])
          else data_src[tid][tpin] = { fid, fpin } end
        else
          bad("%s: '%s' -> '%s' does not join an output to an input of the same kind", where, l[1], l[2])
        end
      end
    end
  end

  -- A NODE'S NUMBERS FROM LINKS (docs/GRAPH_NUMBERS.md): which numeric
  -- fields are data inputs with a link, for view_of. And a required field
  -- that is linked needs no literal.
  for _, n in ipairs(order) do
    local spec = SPEC[n.type]
    if spec ~= nil and n.entry == nil then
      local ins = set_of(spec.data_in)
      for _, a in ipairs(spec.args) do
        if (a.type == "int" or a.type == "number") and ins[a.name] and data_src[n.id] and data_src[n.id][a.name] then
          n.linked_nums = n.linked_nums or {}
          n.linked_nums[#n.linked_nums + 1] = a.name
        end
      end
    end
    for _, name in ipairs(n._missing or {}) do
      if not (data_src[n.id] and data_src[n.id][name]) then bad("node '%s': needs '%s'", n.id, name) end
    end
    n._missing = nil
  end

  -- A BUTTON NODE NAMES A BUTTON: the engine's, or one the module declared
  -- (checked where the server is there to say which).
  for _, n in ipairs(order) do
    if (n.type == "when_button_pressed" or n.type == "when_button_released" or n.type == "button_held")
       and type(n.button) == "string" and button_of(n.button) == nil and dnp.actions ~= nil then
      bad("node '%s': no button '%s' -- the engine's are %s; the module's own are %s", n.id, n.button,
          "jump, dash, grab, hover, fly, grapple, use, attack, alt_attack, crouch",
          #own_keys() > 0 and table.concat(own_keys(), ", ") or "none (module.txt `key` lines)")
    end
  end

  -- A MARKER NODE NAMES ITS TEMPLATE as dnp.mark will take it, so a typo is
  -- said at load rather than raised by the run that reaches it.
  for _, n in ipairs(order) do
    if n.type == "mark" and (type(n.template) ~= "string" or not n.template:match("^[a-z0-9_]+$") or #n.template > 24) then
      bad("node '%s': marker template '%s' must be 1..24 of [a-z0-9_]", n.id, tostring(n.template))
    end
  end

  -- A PANEL NODE NAMES ONE OF THIS GRAPH'S PANELS, and a widget it has.
  for _, n in ipairs(order) do
    local spec = SPEC[n.type]
    if spec ~= nil and spec.category == "panel" and n.type ~= "set_hud" then
      local def = panels[n.panel]
      if type(n.panel) == "string" and def == nil then
        bad("node '%s': this graph carries no panel '%s'", n.id, n.panel)
      elseif def ~= nil and n.widget ~= nil and def.by_name[n.widget] == nil then
        bad("node '%s': panel '%s' has no widget '%s'", n.id, n.panel, tostring(n.widget))
      elseif def ~= nil and (n.type == "show_element" or n.type == "hide_element")
             and (n.widget == nil) == (n.decor == nil) then
        bad("node '%s': name a `widget` or a `decor` element -- one of them", n.id)
      elseif n.decor ~= nil and (type(n.decor) ~= "string" or #n.decor > 32 or not n.decor:match("^[%w_%-]+$")) then
        bad("node '%s': `decor` is an element's id, 1..32 letters, digits, _ or -", n.id)
      elseif def ~= nil and n.type == "set_widget" then
        local w = def.by_name[n.widget]
        if w ~= nil and (w.kind == "label" or w.kind == "button") then
          bad("node '%s': a %s has no value to set -- a toggle, slider or select does", n.id, w.kind)
        end
      end
    end
  end

  -- A SECTOR NODE NEEDS A SECTOR: picked, linked, or a tag.
  for _, n in ipairs(order) do
    local spec = SPEC[n.type]
    if spec ~= nil and spec.category == "sector" and #spec.exec_out > 0
       and n.sector == nil and n.tag == nil and not (data_src[n.id] and data_src[n.id].sector) then
      bad("node '%s': a %s needs a sector -- picked, linked from `sector_of`, or a `tag`", n.id, n.type)
    end
  end

  -- WHICH VERB ARGUMENTS ARE LINKED: those take their value at run time, so
  -- the loader checks the rest and does not ask for a literal they replace.
  for _, n in ipairs(order) do
    if n.entry ~= nil and data_src[n.id] ~= nil then
      local vs = verb_spec(n.type, n.type == "condition" and n.c or n.a)
      for _, a in ipairs(vs and vs.args or {}) do
        if data_src[n.id][a.name] ~= nil then
          n.linked = n.linked or {}
          n.linked[a.name] = a
        end
      end
    end
  end

  -- No data cycles: a value cannot be computed from itself.
  local state = {}
  local function visit(id)
    if state[id] == 1 then return true end
    if state[id] == 2 then return false end
    state[id] = 1
    for _, src in pairs(data_src[id] or {}) do
      if visit(src[1]) then return true end
    end
    state[id] = 2
    return false
  end
  for _, n in ipairs(order) do
    state = {}
    if visit(n.id) then bad("node '%s': its data inputs lead back to itself", n.id) break end
  end

  -- Which events reach each verb, so a condition used after an event it reads
  -- nothing from is refused, as the trigger loader refuses it.
  local reached = {}
  for _, n in ipairs(order) do
    if n.type == "event" then
      local seen, stack = {}, { n.id }
      while #stack > 0 do
        local id = table.remove(stack)
        if not seen[id] then
          seen[id] = true
          reached[id] = reached[id] or {}
          reached[id][n.event] = true
          for _, to in pairs(exec_next[id] or {}) do stack[#stack + 1] = to end
        end
      end
    end
  end
  if triggers ~= nil and triggers.verbs ~= nil then
    for _, n in ipairs(order) do
      if n.entry ~= nil then
        local kind = n.type
        local evs = {}
        for e in pairs(reached[n.id] or {}) do evs[#evs + 1] = e end
        table.sort(evs)
        if #evs == 0 then evs = { false } end
        -- Checked without the linked arguments, and not asked for them.
        local entry = n.entry
        if n.linked ~= nil then
          entry = {}
          for k, v in pairs(n.entry) do if n.linked[k] == nil then entry[k] = v end end
        end
        for _, e in ipairs(evs) do
          for _, p in ipairs(triggers.verbs.check(kind, entry, e or nil, ("%s node '%s'"):format(label, n.id))) do
            local missing = p:match("needs an argument '([^']+)'")
            -- "needs one of: value, add" is answered by a link into any of them.
            local group = p:match("needs one of: (.+)$")
            local linked_member = false
            if group and n.linked then
              for name in group:gmatch("[^, ]+") do linked_member = linked_member or n.linked[name] ~= nil end
            end
            if not (missing and n.linked and n.linked[missing]) and not linked_member then problems[#problems + 1] = p end
          end
        end
      end
    end
  end

  if #problems > 0 then
    for i = 1, math.min(#problems, MAX_REPORTS) do say("%s", problems[i]) end
    if #problems > MAX_REPORTS then say("...and %d more", #problems - MAX_REPORTS) end
    say("%s: %d problems -- the graph is not loaded", label, #problems)
    return false, problems
  end

  local by_event = {}
  for _, n in ipairs(order) do
    if n.type == "event" then
      by_event[n.event] = by_event[n.event] or {}
      table.insert(by_event[n.event], n.id)
    end
  end
  -- EACH GRAPH HAS ITS OWN NAME, in its VM: a map or a module may carry many
  -- graphs, and the name is how a message, graphs.var and a save tell them
  -- apart. A second graph under a name already loaded is refused, naming both.
  for _, other in ipairs(loaded) do
    if other.name == data.name then
      bad("a graph named '%s' is already loaded here (%s) -- each graph needs its own name",
          tostring(data.name), other.label)
    end
  end
  if #problems > 0 then
    for i = 1, math.min(#problems, MAX_REPORTS) do say("%s", problems[i]) end
    say("%s: %d problems -- the graph is not loaded", label, #problems)
    return false, problems
  end

  local by_start = {}
  for _, n in ipairs(order) do
    if STARTS[n.type] then
      by_start[n.type] = by_start[n.type] or {}
      table.insert(by_start[n.type], n.id)
    end
  end
  local by_call = {}
  for _, n in ipairs(order) do
    if n.type == "on_call" then
      by_call[n.name] = by_call[n.name] or {}
      table.insert(by_call[n.name], n.id)
    end
  end
  -- THE FIRST GRAPH TO CARRY A PANEL DEFINES IT in this VM; a second that
  -- carries a different one is said, and uses the first's.
  for pname, def in pairs(panels) do
    local have = panel_defs[pname]
    if have == nil then
      panel_defs[pname] = { def = def, label = label }
    else
      local same = #have.def.widgets == #def.widgets and have.def.title == def.title and have.def.template == def.template
      for i, w in ipairs(def.widgets) do
        local o = have.def.widgets[i]
        same = same and o ~= nil and o.name == w.name and o.id == w.id and o.kind == w.kind and o.label == w.label
      end
      if not same then
        say("%s: panel '%s' differs from %s's; %s's is the one used", label, pname, have.label, have.label)
      end
      panels[pname] = have.def
    end
  end
  local g = { name = data.name, label = label, vars = vars, by_id = by_id, exec_next = exec_next,
              data_src = data_src, by_event = by_event, by_call = by_call, by_start = by_start, spent = {},
              panels = panels }
  loaded[#loaded + 1] = g
  say("%s: %d nodes, %d links", label, #order, #data.links)
  return true
end

-- ------------------------------------------------------------- inspection

--- A loaded graph's variable, for tests and for a debug panel.
function M.var(name, var)
  for _, g in ipairs(loaded) do if g.name == name then return g.vars[var] end end
  return nil
end
function M.count() return #loaded end
function M.pending() return #timers end
--- Forget every graph and timer. For a test.
function M.reset() loaded = {}; timers = {}; jobs = {}; track = {}; touch_seen = {}; t0 = nil; frozen = {}; spoken = {}
  panel_defs = {}; open_panels = {}; by_ui = {}; last_input = {} end
