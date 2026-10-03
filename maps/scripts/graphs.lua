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

local function say(fmt, ...) print(("[graphs] " .. fmt):format(...)) end

-- ------------------------------------------------------------ the vocabulary

local SPEC = {}
for _, n in ipairs(V.nodes) do SPEC[n.name] = n end
local GRAPH_FIELD, NODE_FIELD = {}, {}
for _, f in ipairs(V.graph_fields) do GRAPH_FIELD[f.name] = f end
for _, f in ipairs(V.node_fields) do NODE_FIELD[f.name] = f end

local function set_of(list) local s = {} for _, v in ipairs(list or {}) do s[v] = true end return s end

-- The events a graph may start from: the triggers', and in a module's graph the
-- two the game VM is told about.
local function event_names()
  local s = {}
  if trigger_vocab ~= nil then for _, e in ipairs(trigger_vocab.events) do s[e.name] = true end end
  s.on_player_enter = true
  s.on_player_leave = true
  return s
end

-- ------------------------------------------------------------- the runtime

local loaded = {}      -- graphs in load order
local timers = {}      -- { tick, graph, node, pin, ev }

-- Pure nodes: a value from their inputs. `pull(pin)` reads an input.
local PURE = {}
function PURE.event(g, n, ev, pull, pin) return ev[pin] end
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
function EXEC.condition(g, n, ev) return triggers.verbs.condition(n.entry, ev) and "true" or "false" end
function EXEC.action(g, n, ev) triggers.verbs.action(n.entry, ev) return "then" end
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
          bad("%s: '%s' is not an event a graph can start from", where, n.event)
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
        for _, e in ipairs(evs) do
          for _, p in ipairs(triggers.verbs.check(kind, n.entry, e or nil, ("%s node '%s'"):format(label, n.id))) do
            problems[#problems + 1] = p
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
  local g = { name = data.name, label = label, vars = vars, by_id = by_id, exec_next = exec_next,
              data_src = data_src, by_event = by_event, spent = {} }
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
