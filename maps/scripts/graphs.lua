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
local PURE = {}
function PURE.event(g, n, ev, pull, pin) return ev[pin] end
-- An on_call's outputs are what the call sent, carried as its run's event.
PURE.on_call = PURE.event
function PURE.get_var(g, n) return g.vars[n.var] end
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
function EXEC.set_var(g, n, ev, pull) g.vars[n.var] = pull("value") return "then" end
function EXEC.add_var(g, n, ev, pull) g.vars[n.var] = (tonumber(g.vars[n.var]) or 0) + (tonumber(pull("value")) or 0) return "then" end

-- One run, from exit `pin` of node `start`.
local function run(g, start, pin, ev)
  local memo = {}
  local function value_of(id, out)
    local key = id .. "." .. out
    local hit = memo[key]
    if hit ~= nil then return hit[1] end
    local n = g.by_id[id]
    local function pull(input)
      local src = g.data_src[n.id] and g.data_src[n.id][input]
      if src then return value_of(src[1], src[2]) end
      return n[input]
    end
    local v = PURE[n.type](g, n, ev, pull, out)
    memo[key] = { v }   -- boxed, so a nil or false result is remembered too
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
      local exit = EXEC[n.type](g, n, ev, pull, go)
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
-- its `once` nodes have been passed, by the graph's file. Not its pending
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
      out[g.label] = { vars = g.vars, spent = spent }
    end
    return out
  end, function(t)
    for _, g in ipairs(loaded) do
      local s = t and t[g.label]
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
function on_tick() drain_timers(dnp.tick()); if #loaded > 0 then fire("on_tick", {}) end end
function on_post_tick() if #loaded > 0 then fire("on_post_tick", {}) end end
function on_interact(entity, side, sector) fire("on_interact", { entity = entity, side = side, sector = sector }) end
function on_cross(entity, side, sector) fire("on_cross", { entity = entity, side = side, sector = sector }) end
function on_touch(entity, prop) fire("on_touch", { entity = entity, target = prop }) end
function on_prop_use(entity, prop) fire("on_prop_use", { entity = entity, target = prop }) end
function on_attack(entity, target) fire("on_attack", { entity = entity, target = target }) end
function on_crush(entity, sector, kind) fire("on_crush", { entity = entity, sector = sector, crush_kind = kind }) end
function on_ui_event(entity, ui_id, widget_id, value)
  fire("on_ui_event", { entity = entity, ui_id = ui_id, widget_id = widget_id, value = value })
end
function on_player_enter(entity) fire("on_player_enter", { entity = entity }) end
function on_player_leave(player) fire("on_player_leave", { value = player }) end

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
    elseif type(v) ~= "number" and type(v) ~= "string" and type(v) ~= "boolean" then
      bad("variable '%s' starts as a %s; a number, string or boolean only", k, type(v))
    else vars[k] = v end
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
              if not a.optional then bad("%s: needs '%s'", where, a.name) end
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
        if (n.type == "get_var" or n.type == "set_var" or n.type == "add_var")
           and type(n.var) == "string" and vars[n.var] == nil then
          bad("%s: no variable '%s' in `vars`", where, n.var)
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
            if not (missing and n.linked and n.linked[missing]) then problems[#problems + 1] = p end
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
  local by_call = {}
  for _, n in ipairs(order) do
    if n.type == "on_call" then
      by_call[n.name] = by_call[n.name] or {}
      table.insert(by_call[n.name], n.id)
    end
  end
  local g = { name = data.name, label = label, vars = vars, by_id = by_id, exec_next = exec_next,
              data_src = data_src, by_event = by_event, by_call = by_call, spent = {} }
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
function M.reset() loaded = {}; timers = {} end
