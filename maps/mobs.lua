-- The mob runtime: a bounded interpreter over the tables in mob_vocab.lua.
--
-- WHY AN INTERPRETER AND NOT GENERATED LUA. The same three reasons the trigger
-- runtime gives, and they apply harder here:
--
--   1. The editor has to RENDER and RE-EDIT a mob's graph. Generated source
--      cannot be round-tripped once a person has touched it.
--   2. The script sandbox has a per-callback and per-tick instruction budget
--      (server/src/lua/vm.cpp). A bounded walk over a bounded table keeps that
--      enforceable; arbitrary generated code does not.
--   3. It already fits. The engine dispatches named hooks as LISTS of
--      handlers, so this registers its own and coexists with doom.lua,
--      triggers.lua and a map's own script.
--
-- WHAT THIS IS NOT. It is not Doom's actor model -- see the long comment at the
-- top of mob_vocab.lua. Doom is a test case for the vocabulary, not its shape.
--
-- COST. A Doom level is ~170 monsters and the budget is 16.6 ms a tick for
-- EVERYTHING. Three rules keep this affordable, and each was paid for by the
-- version of enemies.lua that did not have it:
--
--   think buckets   a mob decides every THINK-th tick, not every tick, and
--                   the bucket is a list rather than a filter over all of
--                   them. The version that walked all 170 to ask whether each
--                   was due threw away three quarters of the answers.
--   deadlines       every timer is an ABSOLUTE tick compared when the mob is
--                   already in hand, never a counter ticked down. A countdown
--                   has to be visited; a deadline is read.
--   sleep           with no players in the level, nothing here is defined --
--                   a mob chases a target, aims at one, decides whether it
--                   can see one. Levels keep ticking after everyone leaves.
--
-- ORDERING. Mobs step in spawn order within a bucket and buckets step in tick
-- order; transitions are FIRST MATCH in declaration order. Nothing here
-- iterates a hash with `pairs` to decide anything the simulation can see.

-- A GLOBAL, as maps/scripts/enemies.lua and maps/scripts/triggers.lua are: a
-- map's own script says `mobs.load{...}`, and the VM loads each declared script
-- into the same environment in the order the .map lists them.
--
-- `or {}` rather than `= {}` so loading it twice does not throw away the first
-- copy's live mobs -- which would leave them running with nothing referencing
-- them.
mobs = mobs or {}
local M = mobs

local vocab = mob_vocab
if vocab == nil then
  error("mobs.lua: mob_vocab.lua must be loaded first -- add it to the map's " ..
        "`script` lines ABOVE this one. Script order is load order.")
end

-- GLOBALS, as maps/scripts/enemies.lua reaches them. They are not fields of
-- `dnp`: the fx userdata and vec3 are installed at the top level by
-- server/src/lua/fx_binding.cpp. The test harness had them under dnp, which is
-- a shape the engine does not have -- so this loaded there and raised on the
-- first real server, which is the harness testing itself rather than the code.
local fx, vec3 = _G.fx, _G.vec3
local ZERO = fx.int(0)

-- Doom's map unit: 48 to the engine's one. Only used by definitions that say
-- so; nothing in the runtime assumes it.
local SCALE = 48
local function du(n) return fx.frac(n, SCALE) end
M.du = du

-- How many ticks a mob's decisions are spread over. Eight at 60 Hz is a
-- decision every 133 ms, which is well inside a monster's reaction time and
-- eight times cheaper than deciding every tick.
local THINK = 8

-- The ceiling on corpses kept in the world, oldest recycled first. Without one
-- a long fight fills the entity table and new spawns silently fail.
local MAX_CORPSES = 32

-- ---------------------------------------------------------------- diagnostics

local problems = {}
local function complain(fmt, ...)
  problems[#problems + 1] = string.format(fmt, ...)
end

-- ------------------------------------------------------------------ the spec
--
-- Indexed once, at load, so validation is a hash lookup rather than a scan of
-- the vocabulary per argument.

local function index_by_name(list)
  local by = {}
  for _, entry in ipairs(list) do by[entry.name] = entry end
  return by
end

local COND = index_by_name(vocab.conditions)
local ACT = index_by_name(vocab.actions)
local STATE_FIELD = index_by_name(vocab.state_fields)
local MOB_FIELD = index_by_name(vocab.mob_fields)
local MISSILE_FIELD = index_by_name(vocab.missile_fields)

-- THE CONTROL FOR THE VOCABULARY ITSELF.
--
-- A spec that lists an action the runtime does not implement is worse than no
-- spec: the editor offers it, the author uses it, and it silently does
-- nothing. So the runtime asserts at load that the two agree in BOTH
-- directions -- every name in the vocabulary has a handler, and every handler
-- is in the vocabulary. triggers.lua does the same thing and found two
-- mismatches the first time it ran.
local function check_vocabulary_matches_implementation(conds, acts)
  for name in pairs(COND) do
    if conds[name] == nil then
      complain("vocabulary lists condition '%s' and the runtime does not " ..
               "implement it", name)
    end
  end
  for name in pairs(conds) do
    if COND[name] == nil then
      complain("runtime implements condition '%s' and the vocabulary does " ..
               "not list it, so no editor will ever offer it", name)
    end
  end
  for name in pairs(ACT) do
    if acts[name] == nil then
      complain("vocabulary lists action '%s' and the runtime does not " ..
               "implement it", name)
    end
  end
  for name in pairs(acts) do
    if ACT[name] == nil then
      complain("runtime implements action '%s' and the vocabulary does not " ..
               "list it", name)
    end
  end
end

-- ---------------------------------------------------------------- arguments
--
-- Every argument is checked at LOAD, against the vocabulary, and an unknown
-- one is an error rather than a shrug. A silently ignored `dammage = 20` is an
-- attack that looks configured and does nothing, which is the failure this
-- whole file is arranged to avoid -- the same reason the trigger runtime
-- refuses an unknown trigger field.

local function is_int(v)
  return type(v) == "number" and v == math.floor(v)
end

-- fx arrives as a string ("0.25"), an integer, or an fx already. A
-- non-integral Lua NUMBER is refused by name: 0.1 is not 0.1 in a double, and
-- quantising it silently is how a mob ends up a hair slower on one build.
local function as_fx(v, where, what)
  -- fx.parse, not fx(...): the global `fx` is a plain table of constructors
  -- (int, frac, parse, raw, approx) with no __call. The harness had one, which
  -- is how this shipped as a call and raised on the first real server.
  if type(v) == "string" then return fx.parse(v) end
  if is_int(v) then return fx.int(v) end
  if type(v) == "number" then
    complain("%s: `%s` is %s, which is not exact. Write it as a string " ..
             "('%s') or a fraction, so it means the same on every build.",
             where, what, tostring(v), tostring(v))
    return ZERO
  end
  return v                                   -- already an fx userdata
end

-- One value, against one spec entry. Used for action/condition ARGUMENTS and
-- for mob/state/missile FIELDS, because they are the same question and having
-- asked it two ways is how `move = "charge!"` was accepted -- the field loops
-- checked only that a name was known and that a required one was present.
local function check_value(a, v, where, owner)
  if a.type == "int" and not is_int(v) then
    complain("%s: `%s` must be a whole number", where, a.name)
  elseif a.type == "bool" and type(v) ~= "boolean" then
    complain("%s: `%s` must be true or false", where, a.name)
  elseif a.type == "string" and type(v) ~= "string" then
    complain("%s: `%s` must be a string", where, a.name)
  elseif a.type == "table" and type(v) ~= "table" then
    complain("%s: `%s` must be a table", where, a.name)
  elseif a.type == "enum" then
    for _, allowed in ipairs(a.values) do
      if v == allowed then return v end
    end
    complain("%s: `%s` is '%s'; it must be one of %s", where, a.name,
             tostring(v), table.concat(a.values, ", "))
  elseif a.type == "fx" then
    return as_fx(v, where, a.name)
  end
  return v
end

local function check_args(spec, given, where)
  given = given or {}
  local by_name = {}
  for _, a in ipairs(spec.args or {}) do by_name[a.name] = a end

  for k in pairs(given) do
    -- `action`/`at` belong to a sequence entry, `cond`/`negate` to a
    -- condition; they are the entry's own fields rather than the spec's
    -- arguments, so they are not typos.
    if k ~= "action" and k ~= "at" and k ~= "cond" and k ~= "negate" and
       by_name[k] == nil then
      complain("%s: '%s' has no argument named `%s`", where, spec.name, k)
    end
  end

  -- one_of groups: exactly one member, and saying so beats "missing argument"
  -- when what is missing is a choice.
  local groups = {}
  for _, a in ipairs(spec.args or {}) do
    if a.one_of then
      groups[a.one_of] = groups[a.one_of] or { seen = 0, names = {} }
      local g = groups[a.one_of]
      g.names[#g.names + 1] = a.name
      if given[a.name] ~= nil then g.seen = g.seen + 1 end
    elseif not a.optional and given[a.name] == nil then
      complain("%s: '%s' needs `%s`", where, spec.name, a.name)
    end
  end
  for name, g in pairs(groups) do
    if g.seen ~= 1 then
      complain("%s: '%s' needs exactly one of %s (got %d)",
               where, spec.name, table.concat(g.names, ", "), g.seen)
    end
  end

  -- Types, and enums by value. An enum typo is the one that looks most like
  -- working code.
  for _, a in ipairs(spec.args or {}) do
    if given[a.name] ~= nil then
      given[a.name] = check_value(a, given[a.name],
                                  ("%s: '%s'"):format(where, spec.name))
    end
  end
end

-- ------------------------------------------------------------------- loading

local defs = {}            -- mob definitions, by name
local missiles = {}        -- missile definitions, by name
local compiled = {}        -- `lua` bodies, by the table that declared them

local function compile_lua(a, where)
  if a.fn ~= nil then compiled[a] = a.fn; return end
  if a.code == nil then return end          -- one_of has already said so
  if loadstring == nil then
    complain("%s: the 'lua' escape hatch's `code` needs loadstring, which " ..
             "this server removed (Trust::Strict). Use `fn = function(mob) " ..
             "... end`, which needs no compiler at run time.", where)
    return
  end
  -- `mob` named on the SAME line, so line 3 of an error is line 3 of what the
  -- author wrote, and the chunk is named for the state so a raise says where.
  local fn, err = loadstring("local mob = ... ; " .. a.code, "=" .. where)
  if fn == nil then
    complain("%s: the 'lua' body does not compile: %s", where, tostring(err))
    return
  end
  compiled[a] = fn
end

local function check_action_list(list, where, mob)
  for i, a in ipairs(list or {}) do
    local at = ("%s action %d"):format(where, i)
    local spec = ACT[a.action]
    if a.action == nil then
      complain("%s: needs an `action`", at)
    elseif spec == nil then
      complain("%s: there is no action called '%s'", at, tostring(a.action))
    else
      check_args(spec, a, at)
      if a.action == "lua" then compile_lua(a, at) end
      if a.action == "goto" and mob and mob.states[a.state] == nil then
        complain("%s: 'goto' names state '%s', which this mob does not have",
                 at, tostring(a.state))
      end
      if a.action == "missile" and a.kind and missiles[a.kind] == nil then
        complain("%s: 'missile' names '%s', which is not a defined missile",
                 at, tostring(a.kind))
      end
      if a.action == "spawn" and a.mob and defs[a.mob] == nil then
        complain("%s: 'spawn' names mob '%s', which is not defined", at,
                 tostring(a.mob))
      end
      if a.action == "set_var" or a.action == "add_var" or
         a.action == "set_cooldown" then
        if mob and a.var and (mob.vars == nil or mob.vars[a.var] == nil) then
          complain("%s: `var` is '%s', which this mob does not declare in " ..
                   "`vars`. Declaring it is what gives it a starting value " ..
                   "and what lets an editor list it.", at, tostring(a.var))
        end
      end
    end
  end
end

local function check_conditions(list, where, mob)
  for i, c in ipairs(list or {}) do
    local at = ("%s condition %d"):format(where, i)
    local spec = COND[c.cond]
    if c.cond == nil then
      complain("%s: needs a `cond`", at)
    elseif spec == nil then
      complain("%s: there is no condition called '%s'", at, tostring(c.cond))
    elseif c.negate ~= nil and type(c.negate) ~= "boolean" then
      complain("%s: `negate` must be true or false", at)
    else
      check_args(spec, c, at)
      if c.cond == "lua" then compile_lua(c, at) end
      if (c.cond == "var_at_least" or c.cond == "var_is" or
          c.cond == "cooldown_ready") and mob and c.var then
        if mob.vars == nil or mob.vars[c.var] == nil then
          complain("%s: `var` is '%s', which this mob does not declare in " ..
                   "`vars`", at, tostring(c.var))
        end
      end
    end
  end
end

-- M.load(data) -- data = { missiles = {...}, mobs = {...} }
--
-- Everything is checked here, at load, and a map with a broken mob table is
-- refused loudly rather than discovered when somebody walks into a room. The
-- server logs each problem on its own line; the editor shows them next to the
-- field.
function M.load(data)
  if type(data) ~= "table" then
    error("mobs.load: expected a table of { missiles = ..., mobs = ... }")
  end
  -- A SECOND load is refused rather than merged or silently preferred. Two
  -- scripts each defining the same mob is the bug the VM's hook LISTS exist to
  -- prevent one layer up, and merging would make which one won depend on
  -- script order, which is not something an author can see.
  if next(defs) ~= nil or next(missiles) ~= nil then
    error("mobs.load: called twice. One table per level -- if two scripts both " ..
          "have mobs, put them in one table, because otherwise which " ..
          "definition wins depends on the order the map lists its scripts.")
  end

  -- Missiles first: mobs reference them, and a reference to something not yet
  -- read would be reported as missing when it is only late.
  for name, m in pairs(data.missiles or {}) do
    local where = ("missile '%s'"):format(name)
    for k in pairs(m) do
      if MISSILE_FIELD[k] == nil then
        complain("%s: has no field named `%s`", where, k)
      end
    end
    for _, f in ipairs(vocab.missile_fields) do
      if m[f.name] == nil then
        if not f.optional then complain("%s: needs `%s`", where, f.name) end
      else
        m[f.name] = check_value(f, m[f.name], where)
      end
    end
    missiles[name] = m
  end

  -- The action lists hanging off a missile. Declared `type = "actions"` and,
  -- until this was written, never recursed into -- so a typo'd action in an
  -- `on_hit` was accepted silently and its `fx` arguments were never converted
  -- from the strings they are written as. Found by the behaviour test, not by
  -- the loader, which is the wrong way round and is why this is here now.
  for name, m in pairs(missiles) do
    local where = ("missile '%s'"):format(name)
    check_action_list(m.on_hit, where .. " on_hit", nil)
    check_action_list(m.on_expire, where .. " on_expire", nil)
  end

  for name, d in pairs(data.mobs or {}) do
    local where = ("mob '%s'"):format(name)
    d.name = name
    for k in pairs(d) do
      if MOB_FIELD[k] == nil and k ~= "name" then
        complain("%s: has no field named `%s`", where, k)
      end
    end
    for _, f in ipairs(vocab.mob_fields) do
      if d[f.name] == nil then
        if not f.optional then complain("%s: needs `%s`", where, f.name) end
      else
        d[f.name] = check_value(f, d[f.name], where)
      end
    end
    if type(d.states) ~= "table" or next(d.states) == nil then
      complain("%s: has no states", where)
      d.states = d.states or {}
    end
    defs[name] = d
  end

  -- Now the cross-references, with every definition in hand.
  for name, d in pairs(defs) do
    local where = ("mob '%s'"):format(name)

    -- Declaration order, recorded once. `pairs` over states is fine for
    -- CHECKING -- order does not change whether a thing is valid -- but the
    -- runtime must never let it decide behaviour, so transitions carry their
    -- own order in an array and `initial` is explicit.
    if d.initial == nil then
      complain("%s: needs `initial`. It could default to 'the first state', " ..
               "but state tables are keyed by name and Lua does not keep " ..
               "their order, so 'first' would mean whichever one the hash " ..
               "handed over -- different on another build.", where)
    elseif d.states[d.initial] == nil then
      complain("%s: `initial` is '%s', which is not one of its states",
               where, tostring(d.initial))
    end

    for sname, st in pairs(d.states) do
      local sw = ("%s state '%s'"):format(where, sname)
      for k in pairs(st) do
        if STATE_FIELD[k] == nil then
          complain("%s: has no field named `%s`", sw, k)
        end
      end
      for _, f in ipairs(vocab.state_fields) do
        if st[f.name] == nil then
          if not f.optional then complain("%s: needs `%s`", sw, f.name) end
        else
          st[f.name] = check_value(f, st[f.name], sw)
        end
      end
      if st.ticks ~= nil and st.next == nil then
        complain("%s: has `ticks` and no `next`, so it ends and stays there. " ..
                 "A state that runs out with nowhere to go is a mob frozen " ..
                 "mid-animation -- name where it goes, even if that is itself.",
                 sw)
      end
      if st.next ~= nil and d.states[st.next] == nil then
        complain("%s: `next` is '%s', which is not one of this mob's states",
                 sw, tostring(st.next))
      end

      for i, entry in ipairs(st.sequence or {}) do
        local ew = ("%s sequence %d"):format(sw, i)
        if not is_int(entry.at) or entry.at < 0 then
          complain("%s: needs `at`, a whole number of ticks after entering " ..
                   "the state", ew)
        end
        if st.ticks ~= nil and is_int(entry.at) and entry.at >= st.ticks then
          complain("%s: fires at tick %d of a state that lasts %d, so it " ..
                   "never runs", ew, entry.at, st.ticks)
        end
      end
      check_action_list(st.sequence, sw, d)

      for i, t in ipairs(st.transitions or {}) do
        local tw = ("%s transition %d"):format(sw, i)
        if t.to == nil then
          complain("%s: needs `to`", tw)
        elseif d.states[t.to] == nil then
          complain("%s: `to` is '%s', which is not one of this mob's states",
                   tw, tostring(t.to))
        end
        check_conditions(t.when, tw, d)
      end
    end

    -- A CORPSE IS THE DEATH STATE'S PRODUCT.
    --
    -- `corpse = true` with no `death` state is a mob that is meant to leave a
    -- body and cannot: `enter` does nothing when the state is missing, so it
    -- dies in whatever pose it was in, is added to the corpse list, and is
    -- then the thing `revive` finds and puts back. Silently. Found by writing
    -- a test that tried to resurrect one.
    if d.corpse and d.states.death == nil then
      complain("%s: has `corpse = true` and no `death` state. The corpse is " ..
               "what the death state leaves behind -- without one it dies in " ..
               "whatever pose it was already in.", where)
    end

    -- UNREACHABLE STATES. Not fatal -- a state reached only by `goto` from a
    -- sequence is legitimate and this walk sees those too -- but a state
    -- nothing can enter is almost always a renamed transition, and finding it
    -- at load beats finding it because a monster never plays its death
    -- animation.
    local reached = { [d.initial or ""] = true }
    local changed = true
    while changed do
      changed = false
      for sname, st in pairs(d.states) do
        if reached[sname] then
          local function mark(to)
            if to and d.states[to] and not reached[to] then
              reached[to] = true; changed = true
            end
          end
          mark(st.next)
          for _, t in ipairs(st.transitions or {}) do mark(t.to) end
          for _, a in ipairs(st.sequence or {}) do
            if a.action == "goto" then mark(a.state) end
          end
        end
      end
    end
    for sname in pairs(d.states) do
      if not reached[sname] and sname ~= "pain" and sname ~= "death" then
        complain("%s: state '%s' cannot be reached from '%s' by any " ..
                 "transition, `next` or `goto`", where, sname,
                 tostring(d.initial))
      end
    end
  end

  M.defs, M.missiles = defs, missiles
  return #problems == 0, problems
end


-- ============================================================== THE RUNTIME

local live = {}            -- mob record by entity index
local buckets = {}         -- THINK lists, round-robin by spawn order
for i = 0, THINK - 1 do buckets[i] = {} end
local spawned = 0
local corpses = {}
local flying = {}          -- missiles in flight, by prop index
local players = {}         -- refreshed once a tick, not once a mob
local asleep = false

-- ------------------------------------------------------------------ targets

-- Where each player FIRST appeared, which is where they go back to when they
-- die. Taken from the first sighting rather than from a spawn tag, because a
-- script that hardcoded a tag would be wrong on every map that named it
-- something else.
local home_of = {}

local function refresh_players()
  players = dnp.players() or {}
  for i = 1, #players do
    local e = players[i]
    if e:valid() then
      local idx = e:index()
      if home_of[idx] == nil then home_of[idx] = e:pos() end
    end
  end
end

-- Nearest player, by PLAN distance squared -- no square root, and no vertical
-- term. A cacodemon above you is still near you, and taking a root 170 times a
-- tick to compare two distances is work for an answer nobody reads.
local function nearest_player(from)
  local best, bd2, bpos = nil, nil, nil
  for i = 1, #players do
    local p = players[i]
    if p:valid() then
      local pos = p:pos()
      local dx, dy = pos.x - from.x, pos.y - from.y
      local d2 = dx * dx + dy * dy
      if bd2 == nil or d2 < bd2 then best, bd2, bpos = p, d2, pos end
    end
  end
  return best, bd2, bpos
end

local function aim_at(from, to)
  -- dnp.atan2, the engine's own CORDIC. `facing` is an ENTITY method -- where
  -- something is looking -- and there is no dnp.facing; a script computing an
  -- angle with a double-precision atan2 would produce one the simulation could
  -- not have produced.
  return dnp.atan2(to.y - from.y, to.x - from.x)
end

-- ------------------------------------------------------------------ routing

-- How often a mob may ask for a new route. A* over a Doom level is cheap but
-- not free, and the binding refuses past 64 calls a tick -- so a level's worth
-- of monsters all repathing on the same tick would have most of them silently
-- refused. Spread by index, and rare: a target that has not changed sector
-- does not need a new route at all.
local REPATH_TICKS = 30

-- Steer towards the next waypoint on the route, or straight at the target if
-- there is no route to follow.
--
-- STRAIGHT AT IT is the fallback, not the plan. It is what this file did for
-- every mob before pathfinding existed, and it is why monsters pressed into
-- corners: "walk towards the player" is only the right move when there is
-- nothing in the way, which on a real level is most of the time and never
-- when it matters.
local function steer_target(m, tick)
  local t = m.target
  if t == nil or m.tp == nil then return nil end
  local here = m.e:sector()
  if here == nil then return m.tp end

  -- The target's sector is only needed when a repath might happen. Asking for
  -- it unconditionally is a binding call per mob per think for an answer that
  -- is thrown away most of the time.
  local due = tick >= (m.repath_at or 0)
  local there = (due or m.route == nil) and t:sector() or m.route_to
  if there == nil then return m.tp end

  -- Same sector: no route needed, and asking for one is the storm the budget
  -- exists to stop.
  if here == there then m.route = nil; return m.tp end

  local stale = m.route == nil or m.route_to ~= there or due
  if stale then
    m.repath_at = tick + REPATH_TICKS + (m.idx % REPATH_TICKS)
    m.route_to = there
    -- The body it is planned for, so a wide monster is not routed through a
    -- gap it cannot fit -- the nav graph checks that, and only if it is told.
    local r = m.e:param("radius")
    local h = m.e:param("height")
    m.route = dnp.find_path(here, there, r, h, m.def.jump_height)
    m.route_i = 2                     -- [1] is the sector it is already in
  end

  if m.route == nil then return m.tp end

  -- Advance past waypoints already reached. Compared by SECTOR rather than by
  -- distance: a mob is at a waypoint when it is in that sector, and a radius
  -- test would have it circle a centroid it cannot stand on.
  while m.route[m.route_i] ~= nil and m.route[m.route_i].sector == here do
    m.route_i = m.route_i + 1
  end
  local wp = m.route[m.route_i]
  if wp == nil then return m.tp end
  -- A ROUTE THROUGH A JUMP IS A PROMISE. The graph planned this step on the
  -- understanding that the body would hop; walking it instead means pressing
  -- into the ledge forever, which is worse than never having routed here.
  m.want_jump = wp.jump == true
  return vec3.new(wp.x, wp.y, m.p.z)
end

-- ---------------------------------------------------------------- the state

-- set_state and set_move DO NOTHING when nothing would change.
--
-- For a monster nobody has walked near yet, that is every decision it will
-- ever make -- and each one it skips is a command that does not enter the
-- queue, does not replicate and does not appear in a snapshot.
local function set_anim(m, anim)
  if m.anim ~= anim then
    m.anim = anim
    if m.e:valid() then m.e:set_animation(anim) end
  end
end

local function set_move(m, mx, my, aim, buttons)
  buttons = buttons or 0
  if m.mx == mx and m.my == my and m.aim == aim and m.buttons == buttons then
    return
  end
  m.mx, m.my, m.aim, m.buttons = mx, my, aim, buttons
  -- POSITIONAL, because that is what the binding takes:
  -- entity:set_input(move_x, move_y, aim [, buttons]). Passing a table looked
  -- fine against the test harness -- which stored whatever it was handed --
  -- and would have raised on the real engine. Two programs, one call shape.
  if m.e:valid() then m.e:set_input(mx, my, aim, buttons) end
end

local function enter(m, name, tick)
  local st = m.def.states[name]
  if st == nil then return end
  m.state, m.st, m.entered = name, st, tick
  m.seq_i = 1
  set_anim(m, st.anim)
  if st.solid ~= nil and m.e:valid() then m.e:set_solid(st.solid) end
  if st.move == "stop" then set_move(m, ZERO, ZERO, m.aim) end
end

-- ------------------------------------------------------------- conditions
--
-- Keyed by name, and the table is checked against the vocabulary at load in
-- BOTH directions -- see check_vocabulary_matches_implementation. A condition
-- listed in the vocabulary and missing here would be offered by the editor and
-- silently do nothing.
--
-- Every one is a pure read except `chance`, which says so in the vocabulary.

local conds = {}

-- POSITIONS ARE FETCHED ONCE PER THINK, not once per condition.
--
-- Every condition wanted the mob's position and its target's, and each asked
-- the engine for them again: a zombieman's `chase` alone did it four times
-- before it had decided anything. Measured at 67 engine calls per think for
-- one monster, which on a Doom level of 170 is most of a frame.
--
-- `m.p` and `m.tp` are refreshed at the top of the mob's turn and are valid
-- only within it. Nothing outside a think reads them, and the tick they were
-- taken on is recorded so a stale pair cannot be used by accident.
local function refresh_pos(m, tick)
  m.p = m.e:pos()
  m.tp = (m.target ~= nil and m.target:valid()) and m.target:pos() or nil
  m.pos_tick = tick
end

function conds.has_target(m)     return m.tp ~= nil end
function conds.grounded(m)       return m.e:grounded() end

function conds.target_within(m, c)
  if m.tp == nil then return false end
  local dx, dy = m.tp.x - m.p.x, m.tp.y - m.p.y
  return dx * dx + dy * dy < c.distance * c.distance
end

function conds.target_visible(m)
  if m.tp == nil then return false end
  -- A RAYCAST, and the expensive thing in this file. `sight` gates it and a
  -- `target_within` before it in the same `when` gates it again: conditions
  -- evaluate in declaration order and stop at the first false, which is why
  -- the order in a transition is worth thinking about.
  return dnp.can_see(m.e, m.target)
end

function conds.target_in_front(m, c)
  if m.tp == nil then return false end
  local arc = c.arc or 90
  local want = aim_at(m.p, m.tp)
  -- THE BODY'S FACING, not the mob's aim.
  --
  -- `m.aim` is where it has DECIDED to look; facing is where it is actually
  -- pointing, and turning is a rate rather than a snap, so the two differ for
  -- as long as a monster is coming round. Comparing want against m.aim asks
  -- "have I decided to look at you", which is trivially true the instant a
  -- target is acquired -- so a monster fired while still turned away, the shot
  -- went wide, and it hit whatever was actually in front of it. Which is why
  -- monsters damaged each other and never the player.
  local have = m.e:facing()
  -- brad: a turn is 2^32, and the signed 32-bit difference is the shortest way
  -- round. Subtracting angles as plain numbers is what makes a monster
  -- refuse to shoot at anything just past due north.
  local d = want - have
  while d > 2147483647 do d = d - 4294967296 end
  while d < -2147483648 do d = d + 4294967296 end
  if d < 0 then d = -d end
  return d <= arc * 4294967296 / 360
end

function conds.hp_below(m, c)
  return m.hp * 100 < m.max_hp * c.percent
end

function conds.state_elapsed(m, c)
  return dnp.tick() - m.entered >= c.ticks
end

function conds.chance(m, c)
  return dnp.random(1, 100) <= c.percent
end

function conds.var_at_least(m, c) return (m.vars[c.var] or 0) >= c.value end
function conds.var_is(m, c)       return (m.vars[c.var] or 0) == c.value end
function conds.cooldown_ready(m, c) return dnp.tick() >= (m.vars[c.var] or 0) end

function conds.was_hurt(m, c)
  return m.hurt_at ~= nil and dnp.tick() - m.hurt_at < (c.within or 1)
end

function conds.hurt_by_mob(m) return m.hurt_by_mob == true end

function conds.lua(m, c)
  local fn = compiled[c]
  if fn == nil then return false end
  local ok, v = pcall(fn, m)
  if not ok then
    print("mob '" .. m.def.name .. "': lua condition raised: " .. tostring(v))
    return false
  end
  return v and true or false
end

-- AND, in declaration order, stopping at the first false. There is no OR and
-- no nesting: an OR is two transitions to the same state, which an editor can
-- draw as two edges and a boolean tree cannot.
local function holds(m, when)
  for i = 1, #(when or {}) do
    local c = when[i]
    local fn = conds[c.cond]
    local v = fn ~= nil and fn(m, c) or false
    if c.negate then v = not v end
    if not v then return false end
  end
  return true
end

-- ---------------------------------------------------------------- actions

local acts = {}
local hurt                 -- forward: damage anything, defined below
local launch               -- forward: fire a missile

function acts.face(m, a)
  if (a.towards or "target") == "target" then
    if m.target ~= nil and m.target:valid() then
      m.aim = aim_at(m.e:pos(), m.target:pos())
      m.e:set_input(m.mx or ZERO, m.my or ZERO, m.aim, m.buttons or 0)
    end
  end
end

function acts.hitscan(m, a)
  for _ = 1, (a.count or 1) do
    -- TWO returns, not four: dnp.hitscan gives (target_or_nil, impact) where
    -- the impact is a vec3. Reading it as x, y, z made `x` the vec3 and y and z
    -- nil, and vec3.new(vec3, nil, nil) RAISES -- which killed on_tick before
    -- the damage below ever ran, before update_missiles ran, and took the rest
    -- of the bucket with it. One typo presenting as four separate bugs: no
    -- damage, no tracer, monsters idle, and projectiles frozen in mid-air.
    local target, at = dnp.hitscan(m.e, a.range)
    if at == nil then return end

    -- The tracer carries its START, its DIRECTION and its LENGTH. A client
    -- given only the impact has no idea where the shot came from; one given
    -- only a direction draws a line to infinity through whatever it hit.
    --
    -- The muzzle height is `at.z` -- the height the ENGINE cast at -- rather
    -- than a guess, and the direction is the body's FACING rather than its
    -- aim: turning is a rate, so those differ while a monster is still coming
    -- round, and drawing the intent puts the line beside the thing it hit.
    if a.tracer then
      local from = m.p or m.e:pos()
      local muzzle = vec3.new(from.x, from.y, at.z)
      local len = dnp.length(vec3.new(at.x - muzzle.x, at.y - muzzle.y, ZERO))
      -- Length in 1/256 units, which the event carries as a u32.
      dnp.vfx(a.tracer, muzzle, m.e:facing(), (len * fx.int(256)).int)
    end

    -- An observability hook, for anything that wants to know what the shots
    -- are actually doing. `reached` is how far the bullet got -- which is the
    -- number a tracer draws, so a range test can measure the thing a player
    -- sees rather than infer it from a description.
    if M.on_shot then
      local dx, dy = at.x - (m.p or m.e:pos()).x, at.y - (m.p or m.e:pos()).y
      M.on_shot(m, target, dnp.length(vec3.new(dx, dy, ZERO)))
    end

    -- The shot ARRIVES. dnp.hitscan already answered what was in the way,
    -- walls included, so a target here is one the bullet actually reached.
    if target ~= nil and target:valid() then hurt(target, a.damage, m) end
  end
end

function acts.damage(m, a)
  if m == nil or m.target == nil or not m.target:valid() then return end
  hurt(m.target, a.amount, m)
end

function acts.melee(m, a)
  if m.target == nil or not m.target:valid() then return end
  local p, t = m.e:pos(), m.target:pos()
  local dx, dy = t.x - p.x, t.y - p.y
  if dx * dx + dy * dy <= a.range * a.range then hurt(m.target, a.damage, m) end
end

function acts.missile(m, a)
  local n = a.count or 1
  local arc = a.arc or 0
  for i = 1, n do
    -- Centred on the aim: three shots with an arc of 10 go -10, 0, +10.
    local off = (n == 1) and 0 or ((i - 1) - (n - 1) / 2) * arc
    launch(m, a.kind, (m.aim or 0) + off * 4294967296 / 360, a.offset)
  end
end

function acts.impulse(m, a)
  local towards = a.towards or "facing"
  local ang = m.aim or 0
  if towards == "target" and m.target ~= nil and m.target:valid() then
    ang = aim_at(m.e:pos(), m.target:pos())
  elseif towards == "away" then
    ang = ang + 2147483648
  end
  local d = dnp.dir(ang)
  m.e:add_impulse(vec3.new(d.x * a.speed, d.y * a.speed, a.vertical or ZERO))
end

function acts.spawn(m, a)
  local limit = a.limit
  if limit ~= nil then
    m.brood = m.brood or {}
    local alive = 0
    for i = #m.brood, 1, -1 do
      if m.brood[i]:valid() then alive = alive + 1 else table.remove(m.brood, i) end
    end
    -- A budget, not a rate. A pain elemental without one fills the entity
    -- table, and new spawns then fail silently everywhere else in the level.
    if alive >= limit then return end
  end
  local p = m.e:pos()
  local d = dnp.dir(m.aim or 0)
  local off = a.offset or ZERO
  for _ = 1, (a.count or 1) do
    local child = M.spawn(a.mob,
      vec3.new(p.x + d.x * off, p.y + d.y * off, p.z))
    if child ~= nil and limit ~= nil then
      m.brood = m.brood or {}
      m.brood[#m.brood + 1] = child
    end
  end
end

function acts.revive(m, a)
  for i = #corpses, 1, -1 do
    local c = corpses[i]
    if c.e:valid() and (a.mob == nil or c.def.name == a.mob) then
      local p, q = m.e:pos(), c.e:pos()
      local dx, dy = q.x - p.x, q.y - p.y
      if dx * dx + dy * dy <= a.radius * a.radius then
        table.remove(corpses, i)
        c.hp, c.dead = c.max_hp, false
        c.e:set_solid(true)
        enter(c, c.def.initial, dnp.tick())
        return
      end
    end
  end
end

function acts.damage_area(m, a)
  -- CENTRED WHERE IT WENT OFF, not on whoever set it off.
  --
  -- `m.impact` is set for the duration of a missile's on_hit and nil
  -- otherwise, so a rocket bursts where it landed and a mob that detonates
  -- itself bursts on itself. Reading m.e:pos() unconditionally put every
  -- rocket's blast back on the gunner that fired it, twenty units away from
  -- anything it could hurt -- the same mistake `melee` in an on_hit makes,
  -- one level up.
  local p = m.impact or m.e:pos()
  for _, e in ipairs(dnp.entities() or {}) do
    if e:valid() and (a.hurts_owner ~= false or e:index() ~= m.idx) then
      local q = e:pos()
      local dx, dy, dz = q.x - p.x, q.y - p.y, q.z - p.z
      local d2 = dx * dx + dy * dy + dz * dz
      if d2 <= a.radius * a.radius then
        local dmg = a.damage
        if a.falloff then
          -- Linear in distance, not in distance squared: a blast at full
          -- strength until the last inch and then nothing reads as a bug.
          --
          -- ALL IN fx, truncated ONCE at the end with `.int`. The first
          -- version wrote `1 - d / a.radius` in Lua arithmetic, mixing an fx
          -- with a number -- which the real fx userdata refuses outright, so
          -- it would have raised on the first rocket anybody fired.
          local d = dnp.length(vec3.new(dx, dy, dz))
          dmg = (fx.int(a.damage) * (a.radius - d) / a.radius).int
        end
        if dmg > 0 then hurt(e, dmg, m) end
      end
    end
  end
end

function acts.set_var(m, a)     m.vars[a.var] = a.value end
function acts.add_var(m, a)     m.vars[a.var] = (m.vars[a.var] or 0) + a.by end
function acts.set_cooldown(m, a) m.vars[a.var] = dnp.tick() + a.ticks end
function acts.set_anim(m, a)    set_anim(m, a.anim) end
function acts.despawn(m)        if m.e:valid() then m.e:despawn() end end

function acts.goto(m, a)
  enter(m, a.state, dnp.tick())
  return true                              -- stop the rest of the sequence
end

function acts.set_param(m, a)  m.e:param(a.param, a.value) end

function acts.effect(m, a)
  m.e:add_effect({ param = a.param, mul = a.mul, add = a.add,
                   ticks = a.ticks, tag = a.tag })
end

function acts.vfx(m, a)
  local at = a.at or "self"
  local p = m.e:pos()
  if at == "target" and m.target ~= nil and m.target:valid() then
    p = m.target:pos()
  elseif at == "impact" and m.impact ~= nil then
    p = m.impact
  end
  dnp.vfx(a.kind, p)
end

function acts.lua(m, a)
  local fn = compiled[a]
  if fn == nil then return end
  local ok, err = pcall(fn, m)
  if not ok then
    print("mob '" .. m.def.name .. "': lua action raised: " .. tostring(err))
  end
end

-- Run a sequence's due entries. Returns true if the mob left the state.
local function run_due(m, tick)
  local seq = m.st.sequence
  if seq == nil then return false end
  local age = tick - m.entered
  while m.seq_i <= #seq and seq[m.seq_i].at <= age do
    local a = seq[m.seq_i]
    m.seq_i = m.seq_i + 1
    local fn = acts[a.action]
    if fn ~= nil and fn(m, a) then return true end   -- `goto` said stop
  end
  return false
end

-- ------------------------------------------------------------------ spawning

-- M.spawn(kind, pos) -> entity | nil
function M.spawn(kind, pos, opts)
  local def = defs[kind]
  if def == nil then
    print("mobs: no definition for '" .. tostring(kind) .. "'")
    return nil
  end
  local e = dnp.spawn_enemy(def.prop, pos)
  if e == nil then return nil end

  local m = {
    e = e, def = def, idx = e:index(),
    hp = def.hp, max_hp = def.hp,
    vars = {}, target = nil, aim = 0,
    dormant = opts ~= nil and opts.dormant or false,
  }
  -- Each mob gets its OWN copy of the declared vars. Sharing the definition's
  -- table would have every zombieman on the level share one cooldown.
  for k, v in pairs(def.vars or {}) do m.vars[k] = v end

  if def.turn_speed then e:param("turn_speed", def.turn_speed) end

  if def.flying then
    -- No engine support needed: gravity_scale is a replicated parameter, so a
    -- flyer is flying on every client without a word of it crossing the wire.
    e:param("gravity_scale", ZERO)
  end

  live[m.idx] = m
  -- Round-robin by SPAWN ORDER, so a level's mobs are spread evenly across the
  -- think buckets whatever their entity indices turn out to be.
  local b = spawned % THINK
  spawned = spawned + 1
  local list = buckets[b]
  list[#list + 1] = m
  m.bucket = b

  enter(m, def.initial, dnp.tick())
  return e
end

-- Forget every mob, missile and corpse.
--
-- A level that is torn down and rebuilt -- a map change -- must not leave mobs
-- in the think buckets: `live` is keyed by entity index and the new level's
-- indices start again from the bottom, so a stale record is not merely dead
-- weight, it is a record that will be found for somebody ELSE'S entity.
--
-- `live` alone is not enough and that is the trap: the buckets hold their own
-- references, so clearing the one table leaves the mobs running. Found by the
-- behaviour tests, where two tests in a row shared a zombieman.
function M.reset()
  for k in pairs(home_of) do home_of[k] = nil end
  for k in pairs(live) do live[k] = nil end
  for k in pairs(flying) do flying[k] = nil end
  for i = 0, THINK - 1 do buckets[i] = {} end
  for i = #corpses, 1, -1 do corpses[i] = nil end
  spawned, asleep = 0, false
  players = {}
end

-- M.place{ scale = 48, {kind="imp", x=.., y=.., z=..}, ... }
--
-- What a map's own Lua calls. Coordinates are ENGINE units unless the list
-- carries a `scale`, in which case they are in units of 1/scale -- which is how
-- a converted Doom map states its own, since Doom's are integers and 656/48 is
-- not exactly representable as a decimal anybody should be writing by hand.
--
-- The scale lives in the DATA rather than in this runtime. A mob framework that
-- silently divided every coordinate by 48 would be a Doom framework, and the
-- next game's maps would be forty-eight times too small with nothing saying so.
function M.place(list)
  local scale = list ~= nil and list.scale or nil
  local function coord(v)
    if scale == nil then return v end
    return fx.frac(v, scale)
  end
  for _, it in ipairs(list or {}) do
    M.spawn(it.kind or it.name,
            vec3.new(coord(it.x), coord(it.y), coord(it.z)), it)
  end
end

-- --------------------------------------------------------------- damage

-- The one place anything loses health, players included.
--
-- The engine has no notion of health: it reports that a swing reached
-- something, that a ray ended on somebody. What that COSTS is here. `by` is
-- the mob that did it, or nil for a player, and it is what makes infighting
-- and pain attribution possible at all.
function hurt(target, amount, by)
  if target == nil or not target:valid() then return end
  local m = live[target:index()]

  if m == nil then
    -- Not one of ours: a player, or a prop somebody else owns.
    --
    -- A mob file does not get to decide a player's DEATH rules -- whether that
    -- is a respawn, a screen or a spectator -- so a map may install
    -- `on_player_hurt` and own it entirely. But the default is to take the
    -- health off rather than to do nothing: a hook nobody set should not mean
    -- an attack that silently misses, which is what it meant for exactly as
    -- long as it took to write a test.
    -- A PROP has no health. Barrels, keys and levers are the map script's
    -- business -- doom.lua decides that a barrel explodes -- and a prop handle
    -- has no `hp` method at all, so asking for one raised and took on_attack
    -- down with it. Checked rather than assumed: the two handle kinds are
    -- different userdata types and only one of them answers this.
    if target.hp == nil then return end

    if M.on_player_hurt then
      M.on_player_hurt(target, amount, by)
      return
    end

    local hp = (target:hp() or 100) - amount
    if hp > 0 then target:set_hp(hp); return end

    -- DEATH, and something has to happen. Writing zero and stopping leaves the
    -- player alive at no health forever, taking hits that change nothing --
    -- which reads exactly like damage having stopped working, and is how this
    -- was reported.
    --
    -- A respawn rather than anything more elaborate, and the same choice the
    -- runtime this replaced made: of a death screen, a spectator mode and this,
    -- it is the only one that does not need a whole feature behind it. A map
    -- that wants otherwise sets M.on_player_hurt and owns the rules entirely.
    target:set_hp(M.player_max_hp or 100)
    local home = home_of[target:index()]
    if home ~= nil then target:teleport(home) end
    if M.on_player_death then M.on_player_death(target, by) end
    return
  end
  if m.dead then return end

  m.hp = m.hp - amount
  m.hurt_at = dnp.tick()
  m.hurt_by_mob = by ~= nil

  if m.hp <= 0 then
    m.dead = true
    m.target = nil
    if m.e:valid() then m.e:set_solid(false) end
    enter(m, "death", dnp.tick())
    if m.def.corpse then
      corpses[#corpses + 1] = m
    elseif m.e:valid() then
      m.e:despawn()
    end
    if M.on_death then M.on_death(m, by) end
    return
  end

  -- INFIGHTING. Doom's monsters turn on whatever hurt them, and it is most of
  -- what makes a crowded room interesting. `by` is nil for a player, so this
  -- only ever retargets onto another mob.
  if by ~= nil and m.def.infighting and by.e:valid() and by.idx ~= m.idx then
    m.target = by.e
  end

  -- PAIN CHANCE, rolled. A monster that ALWAYS flinches can be stun-locked
  -- into never acting; one that never does gives no sign a hit landed.
  local chance = m.def.pain_chance
  if chance ~= nil and m.st and m.st.interruptible ~= false and
     m.def.states.pain ~= nil and dnp.random(0, 255) < chance then
    enter(m, "pain", dnp.tick())
  end
end
M.hurt = hurt

-- --------------------------------------------------------------- missiles

function launch(m, kind, ang, offset)
  local def = missiles[kind]
  if def == nil then return end
  local from = m.e:pos()
  local d = dnp.dir(ang)
  local off = offset or ZERO
  -- Clear of the shooter's own body, or it is touching its owner on the tick
  -- it appears -- which bursts it on the thing that fired it.
  local p = dnp.spawn_prop(def.prop,
    vec3.new(from.x + d.x * off, from.y + d.y * off,
             from.z + (def.z_offset or ZERO)))
  if p == nil then return end
  if def.anim_fly then p:set_animation(def.anim_fly) end
  flying[p:index()] = {
    p = p, def = def, dir = d, ang = ang,
    owner = m, target = m.target, born = dnp.tick(),
  }
end

-- Missiles in flight.
--
-- Lifted out of the think loop so the empty-level short circuit still runs it:
-- a fireball with nobody left to hit still has to expire. Skipping it leaks a
-- prop slot per shot, which is a slower and much more confusing failure than a
-- dropped frame.
local function update_missiles(tick)
  for idx, s in pairs(flying) do
    local def = s.def
    if not s.p:valid() then
      flying[idx] = nil
    elseif s.burst_at ~= nil then
      if tick >= s.burst_at then s.p:despawn(); flying[idx] = nil end
    elseif tick - s.born > (def.life or 300) then
      if def.on_expire then
        if s.owner then s.owner.impact = s.p:pos() end
        for _, a in ipairs(def.on_expire) do
          local fn = acts[a.action]
          if fn then fn(s.owner, a) end
        end
        if s.owner then s.owner.impact = nil end
      end
      s.p:despawn(); flying[idx] = nil
    else
      local p = s.p:pos()
      local step = def.speed / fx.int(60)

      -- HOMING, before the step: turn towards the target by at most `homing`
      -- degrees, then fly straight for one tick. Turning after moving would
      -- have it arrive at where the target was.
      if def.homing and s.target ~= nil and s.target:valid() then
        local want = aim_at(p, s.target:pos())
        local d = want - s.ang
        while d > 2147483647 do d = d - 4294967296 end
        while d < -2147483648 do d = d + 4294967296 end
        local cap = def.homing * 4294967296 / 360
        if d > cap then d = cap elseif d < -cap then d = -cap end
        s.ang = s.ang + d
        s.dir = dnp.dir(s.ang)
      end

      -- WHAT DOES IT REACH FIRST -- a wall, or somebody?
      --
      -- ONE call, because they are one question. This used to be dnp.sweep
      -- for the geometry and then a walk over dnp.entities() here for the
      -- bodies, which was O(missiles x entities) in Lua every tick AND could
      -- not be right: two separate answers cannot say which came first, so a
      -- fireball burst on a monster standing behind a door.
      --
      -- dnp.sweep_hit returns nil for nothing, `false` for a wall, and the
      -- entity for an entity, so the three cases are distinguishable without
      -- a flag.
      -- ITSELF and its owner. The missile is an entity sitting exactly at
      -- `p`, so skipping only the owner stops it at distance zero on the tick
      -- it is fired; skipping only itself bursts it on the mob that threw it.
      local what, at = dnp.sweep_hit(p, s.dir, step,
                                     s.radius or def.radius or du(11),
                                     s.p, s.owner and s.owner.e or nil)
      local hit = (what ~= nil and what ~= false) and what or nil
      local blocked = what ~= nil
      local nx, ny = p.x + s.dir.x * step, p.y + s.dir.y * step

      if hit ~= nil or blocked then
        if at ~= nil then s.p:set_pos(at) end
        if hit ~= nil and def.on_hit then
          -- The missile's OWNER is what runs on_hit, with the thing it reached
          -- as the target -- so `melee` and `damage_area` mean the same in a
          -- missile as they do in a state, and the runtime does not need a
          -- second copy of either.
          local saved = s.owner and s.owner.target
          if s.owner then
            s.owner.target = hit
            -- Where it landed, for the actions that are about a PLACE rather
            -- than about the mob: damage_area and vfx.
            s.owner.impact = at or s.p:pos()
          end
          for _, a in ipairs(def.on_hit) do
            local fn = acts[a.action]
            if fn then fn(s.owner, a) end
          end
          if s.owner then s.owner.target = saved; s.owner.impact = nil end
        end
        if def.anim_burst then
          s.p:set_animation(def.anim_burst)
          s.burst_at = tick + (def.burst_ticks or 20)
        else
          s.p:despawn(); flying[idx] = nil
        end
      else
        s.p:set_pos(vec3.new(nx, ny, p.z + (def.gravity or ZERO)))
      end
    end
  end
end

local function update_corpses()
  for i = #corpses, 1, -1 do
    if not corpses[i].e:valid() then table.remove(corpses, i) end
  end
  while #corpses > MAX_CORPSES do
    local oldest = table.remove(corpses, 1)
    live[oldest.idx] = nil
    if oldest.e:valid() then oldest.e:despawn() end
  end
end

-- ------------------------------------------------------------- the think loop

-- One mob's turn. Sequence first, then transitions, then movement.
--
-- THAT ORDER MATTERS. A sequence entry may `goto`, and a transition evaluated
-- before it would decide on a state the mob is about to leave. Movement last
-- so it is driven by the state the mob has actually ended up in.
local function think(m, tick)
  -- `ticks` running out is a transition too, and the LAST one considered: an
  -- explicit transition that matches beats the state simply expiring, which is
  -- what lets `pain` be cut short by death.
  if run_due(m, tick) then return end

  local st = m.st
  for i = 1, #(st.transitions or {}) do
    local t = st.transitions[i]
    if holds(m, t.when) then
      -- FIRST MATCH WINS, in declaration order. No scoring, no backtracking:
      -- a graph whose behaviour depends on a weighting nobody can see is not
      -- one an editor can show you.
      enter(m, t.to, tick)
      return
    end
  end

  if st.ticks ~= nil and tick - m.entered >= st.ticks then
    enter(m, st.next, tick)
    return
  end
end

-- How the body is driven. Called on the mob's THINK tick, not every tick.
--
-- This ran every tick at first, on the theory that a monster steered once in
-- eight would move in visible steps. It does not: set_input is STICKY -- the
-- engine keeps applying the last frame until a new one arrives -- so a mob
-- walks smoothly on an input set eight ticks ago, and only the DIRECTION
-- updates at 7.5 Hz, which is more often than Doom turned a monster.
--
-- The cost of getting that wrong was the whole budget. Driving 170 mobs a tick
-- meant two pos() calls, a CORDIC atan2, two sector() lookups and a dir() each
-- -- about a thousand binding calls a tick for a level of monsters -- and
-- MAP01 reported 20 ms in on_tick against a 16.6 ms frame.
local function drive(m)
  local st = m.st
  if st == nil or st.move == "free" then return end

  local face = st.face or "target"
  local aim = m.aim or 0
  if face == "target" and m.tp ~= nil then aim = aim_at(m.p, m.tp) end

  -- WHERE IT WALKS and WHERE IT LOOKS are different questions, and this is
  -- where they part. It faces the target -- so a shot goes at you, and a
  -- monster rounding a corner is still looking at you -- while its feet
  -- follow the route. Steering by the aim was what made monsters walk into
  -- the wall between you and them.
  local walk_aim = aim
  local mode = st.move or "stop"
  if mode == "chase" then
    local wp = steer_target(m, dnp.tick())
    if wp ~= nil then walk_aim = aim_at(m.p, wp) end
  end

  if mode == "stop" then
    set_move(m, ZERO, ZERO, aim)
    return
  end

  local ang = walk_aim
  if mode == "flee" then ang = aim + 2147483648
  elseif mode == "strafe" then ang = aim + 1073741824
  elseif mode == "wander" then ang = m.wander or aim
  end

  -- A WORLD direction, not "forward". move_x/move_y are world axes: the client
  -- computes a screen-relative direction and sends the world vector, so the
  -- engine never has to know where a camera is. Sending (0,1) once meant every
  -- monster on the level walked north in unison.
  -- HELD, not tapped. The engine's jump is edge-triggered off the button --
  -- max_jumps and the charge rules are its business -- so holding it while
  -- approaching the ledge means the hop happens on the tick the body is
  -- grounded and in reach, rather than on whichever tick the script guessed.
  local buttons = 0
  if m.want_jump and mode == "chase" then buttons = dnp.button.JUMP end

  local d = dnp.dir(ang)
  set_move(m, d.x, d.y, aim, buttons)

  -- A flyer holds a height above its target rather than falling. Costs no
  -- engine support: gravity is off and this is a velocity.
  if m.def.flying and m.tp ~= nil then
    local want = m.tp.z + (m.def.hover_z or ZERO)
    local dz = want - m.p.z
    local v = m.e:vel()
    m.e:set_velocity(vec3.new(v.x, v.y, dz))
  end
end

function on_tick()
  local tick = dnp.tick()
  refresh_players()

  -- NOBODY HERE: stop.
  --
  -- A level keeps ticking after its last player leaves, and every level runs
  -- its own mobs. On a server where clients have walked through several maps,
  -- most of the work each tick would be on behalf of empty rooms. Nothing a
  -- mob does is even defined without a target: it chases one, aims at one,
  -- decides whether it can see one.
  if #players == 0 then
    if not asleep then
      asleep = true
      for _, m in pairs(live) do
        if m.e:valid() and not m.dead then set_move(m, ZERO, ZERO, m.aim) end
      end
    end
    update_missiles(tick)
    update_corpses()
    return
  end
  asleep = false

  local bucket = buckets[tick % THINK]
  for i = #bucket, 1, -1 do
    local m = bucket[i]
    if not m.e:valid() then
      live[m.idx] = nil
      table.remove(bucket, i)
    elseif not m.dormant then
      -- TARGET ACQUISITION, on the think tick only. A raycast per mob per tick
      -- is the cost this file is arranged around; a distance gate comes first
      -- and `sight` is what makes it cheap.
      if not m.dead then
        refresh_pos(m, tick)
        if m.target == nil or not m.target:valid() then
          local p = m.p
          local player, d2 = nearest_player(p)
          local sight = m.def.sight
          if player ~= nil and (sight == nil or d2 < sight * sight) then
            m.target = player
          else
            m.target = nil
          end
          m.tp = (m.target ~= nil and m.target:valid()) and m.target:pos() or nil
        end
        think(m, tick)
        -- Steered here, in the bucket, rather than in a second pass over every
        -- mob. See the comment on drive().
        drive(m)
      end
    end
  end

  update_missiles(tick)
  update_corpses()
end

-- A swing reached something. The engine reports the contact and stops: it says
-- who swung and what the moving collider found, and has no opinion about
-- health or death. `attacker` is a player here, so `by` is nil and infighting
-- does not trigger.
function on_attack(attacker, target)
  if target == nil or not target:valid() then return end
  hurt(target, M.player_damage or 25, nil)
end

M.live = live
M.corpses = corpses
M.flying = flying
M.conds, M.acts = conds, acts
M.problems = problems
M.THINK = THINK
M.MAX_CORPSES = MAX_CORPSES

-- The vocabulary is only normative if something checks it. Runs at load, so a
-- mismatch is a startup line rather than a silently missing action.
check_vocabulary_matches_implementation(conds, acts)

-- No `return M`: the VM discards a script's return value, and a map reaches
-- this through the global above.
