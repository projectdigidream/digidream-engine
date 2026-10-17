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
-- A state's `next` that means "back to the state it interrupted" (see resume).
local PREVIOUS = "@previous"
local ZERO = fx.int(0)
local ONE  = fx.int(1)

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
local SWING_FIELD = index_by_name(vocab.swing_fields)
local PAIN_FIELD = index_by_name(vocab.pain_time_fields)
local DROP_FIELD = index_by_name(vocab.drop_fields or {})
local DROP_CHOICE_FIELD = index_by_name(vocab.drop_choice_fields or {})
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
  elseif a.type == "prop" then
    -- A NAME IN assets/props.txt, and this file cannot check that it is one.
    --
    -- The catalogue is what the SERVER and the CLIENT read; the mob runtime
    -- never opens it, and dnp.spawn_enemy resolves a name against it at spawn
    -- time. So the loader checks the only thing it can -- that it is a name at
    -- all -- and whether the record exists is the editor's to answer, because
    -- the editor is the one holding the catalogue. It already does: a body
    -- with no catalogue record is reported beside the mob that names it.
    --
    -- Deliberately NOT the same as `mob` and `missile`, which ARE checked,
    -- because those are defined in this very table and a wrong name there is
    -- knowable here.
    if type(v) ~= "string" then
      complain("%s: `%s` must be the name of a prop in assets/props.txt",
               where, a.name)
    end
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
      -- A SWING with nothing to swing. The action reads the mob's `swing`
      -- declaration for its shape and on_attack reads it for the damage, so
      -- without one the press would start a collider with the engine's
      -- defaults -- a two-unit reach and a 90-degree arc no author wrote -- and
      -- then do no damage with it. Silently.
      if a.action == "swing" and mob and mob.swing == nil then
        complain("%s: 'swing' needs a `swing` table on the mob, which declares " ..
                 "the arc, the reach, how long the collider lives and what a " ..
                 "hit costs", at)
      end
      if a.action == "missile" and a.kind and missiles[a.kind] == nil then
        complain("%s: 'missile' names '%s', which is not a defined missile",
                 at, tostring(a.kind))
      end
      if a.action == "spawn" and a.mob and defs[a.mob] == nil then
        complain("%s: 'spawn' names mob '%s', which is not defined", at,
                 tostring(a.mob))
      end
      -- Every action that NAMES a variable, not just the three that write an
      -- integer into one. `set_angle` writes a heading and `face` reads one,
      -- and both were introduced without being added here -- so a typo'd var
      -- name in either was accepted silently and the mob then turned to a
      -- heading that was always zero.
      if a.action == "set_var" or a.action == "add_var" or
         a.action == "set_cooldown" or a.action == "set_angle" or
         a.action == "face" then
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
-- A mob's `drops` (mob_vocab.lua drop_fields): a list of entries, each exactly
-- one of an `item` and a `one_of` list of weighted choices. Items are checked
-- against the catalogue when there is one to ask (a test harness may not).
local function check_drops(d, where)
  local dwhere = where .. " drops"
  if type(d.drops) ~= "table" or #d.drops == 0 then
    complain("%s: must be a list of entries, each {item = ...} or {one_of = {...}}", dwhere)
    d.drops = nil
    return
  end
  local function known(item, at)
    if type(item) ~= "string" then
      complain("%s: `item` must be a catalogue name", at)
      return false
    end
    if dnp ~= nil and dnp.prop_kind ~= nil and dnp.prop_kind(item) == nil then
      complain("%s: `item` '%s' is not in the catalogue", at, item)
      return false
    end
    return true
  end
  local kept = {}
  for i, e in ipairs(d.drops) do
    local ewhere = ("%s entry %d"):format(dwhere, i)
    local ok = type(e) == "table"
    if not ok then complain("%s: must be a table", ewhere) end
    if ok then
      for k in pairs(e) do
        if DROP_FIELD[k] == nil then complain("%s: has no field named `%s`", ewhere, k); ok = false end
      end
    end
    if ok and (e.item ~= nil) == (e.one_of ~= nil) then
      complain("%s: needs exactly one of `item` and `one_of`", ewhere)
      ok = false
    end
    if ok and e.count ~= nil and (type(e.count) ~= "number" or e.count < 1 or e.count ~= math.floor(e.count)) then
      complain("%s: `count` must be a whole number, at least 1", ewhere)
      ok = false
    end
    if ok and e.item ~= nil then ok = known(e.item, ewhere) end
    if ok and e.one_of ~= nil then
      if type(e.one_of) ~= "table" or #e.one_of == 0 then
        complain("%s: `one_of` must be a list of {item = ..., weight = n}", ewhere)
        ok = false
      else
        for j, c in ipairs(e.one_of) do
          local cwhere = ("%s choice %d"):format(ewhere, j)
          if type(c) ~= "table" then complain("%s: must be a table", cwhere); ok = false
          else
            for k in pairs(c) do
              if DROP_CHOICE_FIELD[k] == nil then complain("%s: has no field named `%s`", cwhere, k); ok = false end
            end
            if type(c.weight) ~= "number" or c.weight < 1 or c.weight ~= math.floor(c.weight) then
              complain("%s: `weight` must be a whole number, at least 1", cwhere)
              ok = false
            end
            if c.item ~= nil and not known(c.item, cwhere) then ok = false end
          end
        end
      end
    end
    -- A broken entry is dropped from the table, not half-run at a death.
    if ok then kept[#kept + 1] = e end
  end
  d.drops = (#kept > 0) and kept or nil
end

-- THE MISSILES, told to the game VM when the weapons run game-wide
-- (docs/WEAPON_PREDICTION.md): its compiled weapon table carries each launch's
-- missile -- speed, size, burst -- so a predicting client can fly its own
-- rocket, and the definitions live here, in the level, where monsters fire them
-- too. A game without weapons there has no such export and hears nothing.
--- How heavy each monster is, by the prop it is drawn as: the table carries
--- it, so a predicting client shows its own blast shoving them by the rule
--- apply_hit uses (100 when a mob gives none, as there).
function M.mob_masses()
  local out = {}
  for name, d in pairs(defs) do out[d.prop or name] = d.mass or 100 end
  return out
end

local function publish_missiles()
  if dnp == nil or dnp.is_game == nil or dnp.is_game() then return end
  if dnp.game_has == nil or not dnp.game_has("weapon_missiles") then return end
  pcall(dnp.call_game, "weapon_missiles", missiles)
  if dnp.game_has("weapon_masses") then pcall(dnp.call_game, "weapon_masses", M.mob_masses()) end
end
--- The missile definitions by name, for weapons.lua's compiler in this VM.
function M.missile_defs() return missiles end

-- ONE MISSILE'S FIELDS, against the vocabulary: names and values. Shared by
-- mobs.load and mobs.add_missiles, which used to accept anything at all.
-- A missile's `sequence`: the actions as any list's, and each `at` a whole
-- number of ticks. SORTED by it here, stably, so the flight can walk it once
-- in order the way a state's sequence is walked.
local function check_missile_sequence(m, where)
  if m.sequence == nil then return end
  check_action_list(m.sequence, where .. " sequence", nil)
  for i, a in ipairs(m.sequence) do
    if type(a) == "table" and (type(a.at) ~= "number" or a.at < 0 or a.at ~= math.floor(a.at)) then
      complain("%s sequence %d: `at` is a whole number of ticks since launch", where, i)
    end
  end
  local order = {}
  for i, a in ipairs(m.sequence) do order[i] = { i = i, a = a } end
  table.sort(order, function(x, y)
    local ax, ay = (type(x.a) == "table" and x.a.at) or 0, (type(y.a) == "table" and y.a.at) or 0
    if ax ~= ay then return ax < ay end
    return x.i < y.i
  end)
  for i, o in ipairs(order) do m.sequence[i] = o.a end
end

local function check_missile_fields(name, m)
  local where = ("missile '%s'"):format(name)
  -- Its body, defaulted to its name. Same rule as a mob's, below.
  m.prop = m.prop or name
  for k in pairs(m) do
    if MISSILE_FIELD[k] == nil then
      -- The one a missile is most often given by mistake: its damage is what
      -- it DOES on arrival, so it is an action -- and a field that was
      -- accepted and never read let a rocket do none.
      if k == "damage" then
        complain("%s: has no field named `damage` -- what it does on arrival goes in " ..
                 "`on_hit`, e.g. on_hit = { { action = \"damage\", amount = %s } }",
                 where, tostring(m.damage))
      else
        complain("%s: has no field named `%s`", where, k)
      end
    end
  end
  for _, f in ipairs(vocab.missile_fields) do
    if m[f.name] == nil then
      if not f.optional then complain("%s: needs `%s`", where, f.name) end
    else
      m[f.name] = check_value(f, m[f.name], where)
    end
  end
end

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
    check_missile_fields(name, m)
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
    check_action_list(m.on_bounce, where .. " on_bounce", nil)
    check_missile_sequence(m, where)
  end

  for name, d in pairs(data.mobs or {}) do
    local where = ("mob '%s'"):format(name)
    d.name = name

    -- A MOB IS A PROP WITH A BRAIN, and the name is the join.
    --
    -- This file says what a monster decides; assets/props.txt says what it is
    -- -- how wide, how tall, how fast, what it looks like -- because the
    -- client reads that file and must never read this one. Two files, one
    -- thing, and the thing has one name.
    --
    -- `prop` used to be required, and every definition that has ever been
    -- written restated its own key into it: nineteen in the editor's sidecar,
    -- nineteen in doom_monsters.lua, thirty-eight for thirty-eight, and not
    -- one naming anything else. A field whose only observed value is derivable
    -- is not a link, it is a chance to typo one -- and a typo here spawns a
    -- monster with somebody else's body, or nothing at all.
    --
    -- So it defaults, and stays accepted: naming a record deliberately is how
    -- two mobs share one body, which is a real thing to want (a spectre is a
    -- demon that renders differently) even though nothing here does it yet.
    d.prop = d.prop or name

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

    -- THE SWING, field by field. Validated here rather than left to the engine
    -- because these go straight into the replicated parameter table: a
    -- misspelled `raech` would be a monster with the engine's default two-unit
    -- reach and nothing anywhere saying why, which is the failure this whole
    -- file is arranged against.
    if d.swing ~= nil then
      if type(d.swing) ~= "table" then
        complain("%s: `swing` must be a table of swing fields", where)
        d.swing = nil
      else
        local swhere = where .. " swing"
        for k in pairs(d.swing) do
          if SWING_FIELD[k] == nil then
            local known = {}
            for _, f in ipairs(vocab.swing_fields) do known[#known + 1] = f.name end
            table.sort(known)
            complain("%s: has no field named `%s`. Known: %s", swhere, k,
                     table.concat(known, ", "))
          end
        end
        for _, f in ipairs(vocab.swing_fields) do
          if d.swing[f.name] == nil then
            if not f.optional then complain("%s: needs `%s`", swhere, f.name) end
          else
            d.swing[f.name] = check_value(f, d.swing[f.name], swhere)
          end
        end
      end
    end

    -- HOW LONG A FLINCH LASTS, field by field, like `swing` above.
    if d.pain_time ~= nil then
      local pwhere = where .. " pain_time"
      for k in pairs(d.pain_time) do
        if PAIN_FIELD[k] == nil then complain("%s: has no field named `%s`", pwhere, k) end
      end
      for _, f in ipairs(vocab.pain_time_fields) do
        if d.pain_time[f.name] ~= nil then
          d.pain_time[f.name] = check_value(f, d.pain_time[f.name], pwhere)
        end
      end
      local mx = d.pain_time.max
      if mx ~= nil and is_int(mx) and mx < 1 then complain("%s: `max` must be at least 1", pwhere) end
      if d.states == nil or d.states.pain == nil then
        complain("%s: there is no `pain` state for it to lengthen", pwhere)
      end
    end

    -- THE DROPS: a list of entries, each an item or a weighted choice. Checked
    -- here, at load, so a misspelt item is a complaint now rather than a
    -- monster that silently leaves nothing -- or a raise in the middle of
    -- somebody's death, which is worse.
    if d.drops ~= nil then check_drops(d, where) end

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
      if st.next ~= nil and st.next ~= PREVIOUS and d.states[st.next] == nil then
        complain("%s: `next` is '%s', which is not one of this mob's states " ..
                 "(or \"%s\", back to the state it interrupted)",
                 sw, tostring(st.next), PREVIOUS)
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
    -- Seeded with the states the RUNTIME enters, as well as the initial one:
    -- `pain` and `death` are reached by being hurt, and what they lead to by
    -- `next` is as reachable as they are.
    local reached = { [d.initial or ""] = true, pain = true, death = true }
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
      if not reached[sname] then
        complain("%s: state '%s' cannot be reached from '%s' by any " ..
                 "transition, `next` or `goto`", where, sname,
                 tostring(d.initial))
      end
    end
  end

  M.defs, M.missiles = defs, missiles

  -- THE GRAPH GOES TO C++, once, here.
  --
  -- After validation and not before: that is the whole arrangement. This file
  -- and mob_vocab.lua remain the only definition of what a condition IS and
  -- the only thing that checks a graph against it, and what crosses is a graph
  -- already known to be well formed. server/src/mobs/ reads it and has no
  -- opinion about whether it is legal -- a second validator would be a second
  -- answer to the same question.
  --
  -- NOT AN ERROR PATH. A kind using a condition the C++ loop does not
  -- implement keeps running here, exactly as it always has, and says which
  -- condition kept it. `problems` above is about graphs that are WRONG; this
  -- is about graphs that are merely expensive, and conflating the two would
  -- make "your mob is broken" and "your mob is slow" the same message.
  --
  -- Guarded because the Lua-only test harness (scripts/check_mobs.sh) has no
  -- engine behind it, and a mob framework that required one to load would be
  -- untestable without a server.
  M.compiled, M.on_lua_path, M.lua_path_why = 0, 0, {}
  if dnp ~= nil and dnp.mobs_compile ~= nil then
    M.compiled, M.on_lua_path, M.lua_path_why = dnp.mobs_compile(defs)
    -- SAID OUT LOUD, EVERY LOAD, including when the answer is "all of them".
    -- "800 of your 1000 mobs are on the slow path because their graph uses
    -- `spawn`" has to be findable, and a line that only appears when something
    -- is wrong makes the normal case indistinguishable from the binding never
    -- having run at all.
    print(("mobs: %d kinds compiled, %d on the Lua path")
          :format(M.compiled, M.on_lua_path))
    for i = 1, #M.lua_path_why do print("mobs: " .. M.lua_path_why[i]) end
  end

  publish_missiles()
  return #problems == 0, problems
end


-- ============================================================== THE RUNTIME

local live = {}            -- mob record by entity index
local buckets = {}         -- THINK lists, round-robin by spawn order
for i = 0, THINK - 1 do buckets[i] = {} end
local spawned = 0
local corpses = {}
local flying = {}          -- missiles in flight, by prop index
-- A player's own rocket shoves waiting for the input they were meant for (see
-- damage_area): { e, origin, a, share, due }.
local owed_shoves = {}
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

-- ------------------------------------------------------------------ auto-aim
--
-- THERE IS NO VERTICAL AIM IN THIS ENGINE, and that is not an oversight: a
-- command carries one brad and a brad is a heading. So every shot this file
-- has ever fired travelled dead level, and anything standing on a step, a
-- balcony or in a pit was unhittable by bullets and by rockets alike -- while
-- `can_see` said plainly that it was visible, because sight learned to slope
-- and shooting never did.
--
-- The vertical answer is therefore ENTIRELY automatic; there is no input for
-- it to assist. The horizontal one assists, and only slightly.

-- How far off the nose a shot corrects itself, in degrees.
--
-- Doom's own number: it tried the aim, then the aim plus and minus 2^26 in a
-- 2^32 turn, which is 5.625 degrees each way. Small enough that aiming is
-- still the player's job and large enough that a monster at the edge of a
-- corridor is not missed by a pixel.
M.autoaim_h = 5.625

-- How far it looks for something to correct towards, when the shot itself
-- names no range -- a missile has a lifetime rather than a reach. Doom's
-- MISSILERANGE, the same 2048 units its hitscans use.
M.autoaim_range = du(2048)

-- Where a shot leaves a body, and where it is aimed AT: the middle.
--
-- The same height dnp.hitscan casts from and dnp.can_see looks between, said
-- here for the third time -- which is a third place to disagree, so it reads
-- the body's own replicated `height` rather than any constant. `or ZERO` is
-- for the offline harnesses, whose entities carry only the parameters a test
-- set.
local function eye_of(e)
  local p = e:pos()
  -- A PROP HAS NO PARAMETERS. It is a different userdata type with a different
  -- method table, and asking it for one raises rather than answering nil -- so
  -- a missile, which is a prop, is its own middle.
  local h = e.param and e:param("height") or nil
  return p.z + (h or ZERO) / fx.int(2)
end
M.eye_of = eye_of

-- Rise per unit of horizontal run, from a shooter to a body's middle.
--
-- The one number both consumers want: dnp.hitscan multiplies it by how far
-- along the ray is, and a missile adds it to its z once per step. Nil when
-- the two are in the same place, where the question has no answer and a
-- division would invent one.
local function slope_to(from_e, to_e)
  local a, b = from_e:pos(), to_e:pos()
  local d = dnp.length(vec3.new(b.x - a.x, b.y - a.y, ZERO))
  if d <= ZERO then return nil end
  return (eye_of(to_e) - eye_of(from_e)) / d
end
M.slope_to = slope_to

--- What a shot should actually be pointed at.
---
--- Returns the target, the exact heading to it, and the slope to its middle --
--- or nil when nothing inside the cone is worth correcting towards, in which
--- case the caller fires where it was already pointing.
---
--- NEAREST WINS, not straightest: inside a cone this narrow the two almost
--- always agree, and "the thing in front of you" is the rule `target_nearest`
--- already uses, so a player and a monster do not disagree about what they are
--- looking at.
---
--- The ray is cast LAST, for the candidate that is currently winning, for the
--- same reason acts.target_nearest casts it last: it is the expensive part and
--- a nearer candidate found afterwards would have wasted it.
function M.autoaim(e, range, slop)
  if e == nil or not e:valid() then return nil end
  local from = e:pos()
  local face = e:facing()
  local reach = range or M.autoaim_range
  local cap = math.floor((slop or M.autoaim_h or 0) * 4294967296 / 360)
  if cap <= 0 then return nil end
  local best, bd, bang

  local function consider(o)
    if o == nil or not o:valid() or o:index() == e:index() then return end
    -- A SOLID body only: what a predicting client can see to aim at, since it
    -- mirrors exactly the solid ones (World::autoaim asks the same). A corpse
    -- is not solid, and neither is anything a script made a ghost.
    if o.solid ~= nil and not o:solid() then return end
    local q = o:pos()
    local d = dnp.length(vec3.new(q.x - from.x, q.y - from.y, ZERO))
    if d > reach or d <= ZERO then return end
    if bd ~= nil and d >= bd then return end
    -- Signed, wrapped, in brad. The turn wraps, so a target a degree to the
    -- left of north is 359 degrees away by subtraction and one degree away in
    -- fact; folding the difference into the half-turn is what tells them
    -- apart.
    local ang = aim_at(from, q)
    local off = (ang - face) % 4294967296
    if off > 2147483648 then off = off - 4294967296 end
    if off > cap or off < -cap then return end
    if not dnp.can_see(e, o) then return end
    best, bd, bang = o, d, ang
  end

  for i = 1, #players do consider(players[i]) end
  for _, o in pairs(live) do
    if not o.dead then consider(o.e) end
  end

  if best == nil then return nil end
  return best, bang, slope_to(e, best)
end

-- The tracer's `param`, which carries its LENGTH and now its RISE.
--
-- A vfx event has one angle and it is a heading, so a tracer drawn from the
-- event alone is flat -- and a sloped shot with a flat tracer is the failure
-- this file keeps finding: the damage lands correctly and the picture says it
-- did not, which is the worse way round. Both halves are 1/256 of a unit in
-- sixteen bits: lengths run to 256 units where the longest weapon in the game
-- reaches 43, and the rise is offset by half its range so it can be negative.
-- Clamped rather than wrapped, because a wrapped length draws a tracer
-- pointing backwards.
local function tracer_param(len, rise)
  local n = (len * fx.int(256)).int
  if n > 65535 then n = 65535 elseif n < 0 then n = 0 end
  local r = 32768 + ((rise or ZERO) * fx.int(256)).int
  if r > 65535 then r = 65535 elseif r < 0 then r = 0 end
  return r * 65536 + n
end

-- ------------------------------------------------------------------ routing

-- How often a mob may ask for a new route. A* over a Doom level is cheap but
-- not free, and the binding refuses past 64 calls a tick -- so a level's worth
-- of monsters all repathing on the same tick would have most of them silently
-- refused. Spread by index, and rare: a target that has not changed sector
-- does not need a new route at all.
local REPATH_TICKS = 30

-- How far counts as having got somewhere, for `stuck`: a quarter unit.
--
-- Written SQUARED, as the fraction it is, rather than as a quarter multiplied
-- by itself -- the comparison is against a squared distance and this file is
-- loaded by scripts/check_mobs.sh against a stub `fx` with no arithmetic on
-- it, where a multiply at load time raises before anything can be checked.
-- Not an argument: see the condition's own entry in mob_vocab.lua for why one
-- number is the whole of it.
local STUCK_EPS2 = fx.frac(1, 16)

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
  -- Against what was last SENT, as the C++ loop's set_move compares, not
  -- against `m.aim`: an action may turn the steering aim without sending it
  -- (teleport does, so both runtimes send it at the same moment), and
  -- comparing against the field it had just written skipped the send, so a
  -- monster that flinched straight after a teleport faced the old way in Lua.
  if m.mx == mx and m.my == my and m.sent_aim == aim and m.buttons == buttons then
    m.aim = aim
    return
  end
  m.mx, m.my, m.aim, m.sent_aim, m.buttons = mx, my, aim, aim, buttons
  -- POSITIONAL, because that is what the binding takes:
  -- entity:set_input(move_x, move_y, aim [, buttons]). Passing a table looked
  -- fine against the test harness -- which stored whatever it was handed --
  -- and would have raised on the real engine. Two programs, one call shape.
  if m.e:valid() then m.e:set_input(mx, my, aim, buttons) end
end

-- DNP_MOB_TRACE=1 prints every state change, from whichever runtime made it.
--
-- For comparing the two against each other: a world hash says they differ and
-- never says how, and the difference between two mob runtimes is always a
-- decision one of them took. `os` is absent under Trust::Strict, which is why
-- this is guarded rather than assumed.
local TRACE = (os ~= nil and os.getenv ~= nil and os.getenv("DNP_MOB_TRACE") == "1") or false
M.TRACE = TRACE

-- `next = "@previous"`: A REACTION, which goes back to what it interrupted.
--
-- `pain` is the one the runtime enters itself, and with a fixed `next` every
-- hit sent a boss with phases back to the first of them. So every way into a
-- state keeps where the mob was in the one it is leaving -- which state, how
-- far in, how far through its sequence, how long it was rolled to last, its
-- animation -- and a state whose `next` is "@previous" goes back to exactly
-- that when it runs out: RESUMED, not re-entered, so a summon a hit cut into
-- does not summon twice and a barrage does not start again. Leaving one
-- reaction for another (a second hit during pain) keeps the first snapshot,
-- so it still returns to what it was doing before either.
local function is_reaction(st) return st ~= nil and st.next == PREVIOUS end

local function resume(m, tick)
  local p = m.prev
  if p == nil or m.def.states[p.name] == nil then
    -- Nothing to go back to (a reaction on the very first state): start where
    -- the mob starts.
    return false
  end
  if TRACE then
    print(("[mobenter] %d %d %s %s"):format(dnp.tick(), m.idx, p.name, m.cpp and "cpp" or "lua"))
  end
  local st = m.def.states[p.name]
  m.prev = nil
  m.state, m.st, m.entered = p.name, st, tick - p.elapsed
  m.state_ticks = p.state_ticks
  m.seq_i = p.seq_i
  if p.anim ~= nil then set_anim(m, p.anim) end
  if st.solid ~= nil and m.e:valid() then m.e:set_solid(st.solid) end
  if st.move == "stop" then set_move(m, ZERO, ZERO, m.aim) end
  return true
end

local function enter(m, name, tick)
  if TRACE then
    print(("[mobenter] %d %d %s %s"):format(
      dnp.tick(), m.idx, name, m.cpp and "cpp" or "lua"))
  end
  -- A MOB THE C++ LOOP OWNS IS ENTERED THERE, and this returns.
  --
  -- `hurt` and `revive` both call this from outside a think -- pain, death, a
  -- corpse put back on its feet -- and they are the only callers that can
  -- reach a tracked mob. Doing it here as well would leave the two runtimes
  -- disagreeing about which state the mob is in, with the C++ one winning on
  -- the next think and the animation this set already sent.
  if m.cpp then
    local _, ticks = dnp.mob_enter(m.idx, name)
    return ticks
  end
  local st = m.def.states[name]
  if st == nil then return end
  -- Where it was, for a reaction to come back to (see PREVIOUS).
  if m.st ~= nil and not is_reaction(m.st) then
    m.prev = { name = m.state, elapsed = tick - m.entered, seq_i = m.seq_i,
               state_ticks = m.state_ticks, anim = m.anim }
  end
  m.state, m.st, m.entered = name, st, tick
  -- HOW LONG THIS TIME. Rolled here rather than read in `think`, because
  -- `enter` is the one place every way into a state passes through -- a
  -- transition, a `goto`, `hurt` sending it to pain, `revive` putting it back
  -- -- and a roll made anywhere else would be made a different number of times
  -- in the two runtimes.
  m.state_ticks = st.ticks
  if st.ticks ~= nil and st.ticks_jitter then
    m.state_ticks = st.ticks + dnp.random(0, st.ticks_jitter)
  end
  m.seq_i = 1
  set_anim(m, st.anim)
  if st.solid ~= nil and m.e:valid() then m.e:set_solid(st.solid) end
  if st.move == "stop" then set_move(m, ZERO, ZERO, m.aim) end
  return m.state_ticks
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
-- IS THIS TARGET STILL WORTH HAVING?
--
-- Not the same question as "does the entity exist". A mob that dies keeps its
-- entity -- that is what a corpse IS -- and it stays in `live` so it can be
-- resurrected, so `valid()` goes on saying yes long after it has stopped being
-- an opponent. Anything that retaliated against it during infighting therefore
-- kept it as a target and went on shooting the body, which is both useless and
-- unmistakable to watch.
--
-- A player is never in `live`, so this is only ever the mob case.
local function target_gone(t)
  if t == nil or not t:valid() then return true end
  local other = live[t:index()]
  return other ~= nil and other.dead == true
end

local function refresh_pos(m, tick)
  m.p = m.e:pos()
  m.tp = (not target_gone(m.target)) and m.target:pos() or nil
  m.pos_tick = tick
end

function conds.has_target(m)     return m.tp ~= nil end
function conds.grounded(m)       return m.e:grounded() end

-- ------------------------------------------------------------- proximity
--
-- PLAN distance squared throughout, like target_within: no root, no vertical
-- term. All four walk `live` or `dnp.props()` -- there is no spatial index
-- over entities on the script side, and building one here would be a second
-- answer to a question core/ already has a grid for. They are bounded by a
-- distance the author writes, and the vocabulary says to put one on anything
-- that also raycasts.

local function near_mobs(m, dist, kind, alive, fn)
  local d2 = dist * dist
  for idx, o in pairs(live) do
    if idx ~= m.idx and o.e:valid() and
       (kind == nil or o.def.name == kind) and
       (alive == false or not o.dead) then
      local q = o.e:pos()
      local dx, dy = q.x - m.p.x, q.y - m.p.y
      if dx * dx + dy * dy <= d2 then
        if fn(o, dx * dx + dy * dy) then return true end
      end
    end
  end
  return false
end

function conds.mob_within(m, c)
  -- `alive` defaults TRUE: a corpse is still an entity and still in `live`,
  -- because that is what lets `revive` find it -- so a condition that counted
  -- them by default would have a monster reacting to a room of bodies.
  local alive = c.alive
  if alive == nil then alive = true end
  return near_mobs(m, c.distance, c.mob, alive, function() return true end)
end

function conds.count_within(m, c)
  local n = 0
  local want = c.at_least or 1
  near_mobs(m, c.distance, c.mob, true, function()
    n = n + 1
    return n >= want          -- stop as soon as the answer cannot change
  end)
  return n >= want
end

function conds.prop_within(m, c)
  local d2 = c.distance * c.distance
  for _, p in ipairs(dnp.props() or {}) do
    if p:valid() and (c.prop == nil or p:name() == c.prop) then
      local q = p:pos()
      local dx, dy = q.x - m.p.x, q.y - m.p.y
      if dx * dx + dy * dy <= d2 then return true end
    end
  end
  return false
end

-- IT HAS BEEN TRYING AND HAS NOT GOT ANYWHERE.
--
-- Against where it was on a PREVIOUS decision, not against last tick: a mob
-- decides every THINK-th tick and its position between decisions is the
-- mover's business. `m.was` and `m.was_at` are written by the runtime on each
-- think, so this is a comparison rather than a query.
-- THE COSTLY THREE. Each asks the engine rather than reading a number.

function conds.can_reach(m)
  if m.tp == nil or m.target == nil or not m.target:valid() then return false end
  local here = m.e:sector()
  local there = m.target:sector()
  if here == nil or there == nil then return false end
  if here == there then return true end
  local r = m.e:param("radius")
  local h = m.e:param("height")
  -- find_path answers nil when it is refused as well as when there is no
  -- route, and the two are different facts. Refused is reported rather than
  -- mistaken for unreachable: a monster that gave up because the budget was
  -- full would look exactly like one that gave up because a door was shut.
  local route, why = dnp.find_path(here, there, r, h, m.def.jump_height)
  if route ~= nil then return true end
  if why == "path budget" and M.on_path_refused then M.on_path_refused(m) end
  return false
end

function conds.ally_in_front(m, c)
  local arc = c.arc or 12
  local reach = c.distance or m.def.sight
  if reach == nil then return false end
  local d2max = reach * reach
  local have = m.e:facing()
  local half = arc * 4294967296 / 360
  for idx, o in pairs(live) do
    if idx ~= m.idx and not o.dead and o.e:valid() and
       (c.mob == nil or o.def.name == c.mob) then
      local q = o.e:pos()
      local dx, dy = q.x - m.p.x, q.y - m.p.y
      if dx * dx + dy * dy <= d2max then
        local want = dnp.atan2(dy, dx)
        local d = want - have
        while d > 2147483647 do d = d - 4294967296 end
        while d < -2147483648 do d = d + 4294967296 end
        if d < 0 then d = -d end
        if d <= half then return true end
      end
    end
  end
  return false
end

function conds.target_above(m, c)
  if m.tp == nil then return false end
  return m.tp.z - m.p.z >= (c.by or ZERO)
end

function conds.target_below(m, c)
  if m.tp == nil then return false end
  return m.p.z - m.tp.z >= (c.by or ZERO)
end

function conds.stuck(m, c)
  if m.still_since == nil then return false end
  return dnp.tick() - m.still_since >= (c.ticks or 16)
end

function conds.target_is(m, c)
  if m.target == nil or not m.target:valid() then return false end
  local kind = c.kind
  if kind == "prop" then return m.target_prop == true end
  if m.target_prop then return false end
  local other = live[m.target:index()]
  if kind == "mob" then return other ~= nil end
  return other == nil        -- "player": an agent that is not one of ours
end

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
local hang_markers         -- forward: a mob's markers (docs/MARKERS.md), below
local health_changed
local drop_markers
local launch               -- forward: fire a missile

-- A TURN IS 2^32, and that is exact in a double, which is why every angle in
-- this file is a plain Lua number rather than an fx. Degrees come in as ints
-- and are converted here, in that order -- `deg * 4294967296 / 360` -- so the
-- rounding is the same one the C++ loop does with the same integers.
local function brad_of_degrees(deg)
  return math.floor((deg or 0) * 4294967296 / 360)
end

-- Wrap to the 32-bit turn. Lua numbers are doubles and an angle that has had a
-- few offsets added to it is outside the range the engine takes, which the
-- binding would then truncate differently from the way C++ wraps.
local function wrap_brad(a)
  return a % 4294967296
end

-- The heading an action asks for. Shared by `face` and `set_angle` because
-- they ask the same question and having asked it twice is how two spellings of
-- "towards" drift apart.
local function heading_for(m, towards, a)
  if towards == "angle" then
    return brad_of_degrees(a.angle)
  elseif towards == "var" then
    return m.vars[a.var] or 0
  elseif towards == "random" then
    -- Off the world's own stream, exactly as `chance` is: "random" in a
    -- simulation means reproducibly arbitrary, not unpredictable to us.
    return dnp.random(0, 359) * 4294967296 / 360
  elseif towards == "facing" then
    return m.e:facing()
  elseif towards == "away" then
    if m.target ~= nil and m.target:valid() then
      return wrap_brad(aim_at(m.e:pos(), m.target:pos()) + 2147483648)
    end
    return wrap_brad((m.aim or 0) + 2147483648)
  elseif towards == "movement" then
    return m.aim or 0
  end
  -- "target", and the default.
  if m.target ~= nil and m.target:valid() then
    return aim_at(m.e:pos(), m.target:pos())
  end
  return nil
end

function acts.face(m, a)
  local h = heading_for(m, a.towards or "target", a)
  if h == nil then return end
  h = h + brad_of_degrees(a.offset)
  if a.jitter then
    -- ONE DRAW, whether or not the jitter is zero degrees -- the two runtimes
    -- share an RNG stream and a draw one makes and the other skips
    -- desynchronises every later one. Guarded by `a.jitter` being present at
    -- all, which is a property of the GRAPH and so is the same on both sides.
    h = h + brad_of_degrees(dnp.random(-a.jitter, a.jitter))
  end
  -- Through set_move, as the C++ loop's `face` is, so what was sent is known.
  set_move(m, m.mx or ZERO, m.my or ZERO, wrap_brad(h), m.buttons or 0)
end

function acts.set_angle(m, a)
  local h = heading_for(m, a.towards or "target", a)
  if h == nil then return end
  if a.jitter then h = h + brad_of_degrees(dnp.random(-a.jitter, a.jitter)) end
  m.vars[a.var] = math.floor(wrap_brad(h))
end

function acts.hitscan(m, a)
  -- WHERE THE SHOT ACTUALLY POINTS, decided once and then scattered from.
  --
  -- Once, not once per pellet, because a shotgun whose seven pellets each ran
  -- their own auto-aim would have them converge on one body instead of
  -- spreading across it -- and because the search costs a ray.
  --
  -- A monster needs no search: it has a target and has been turning towards it
  -- for the last half second, so only the VERTICAL is missing. A player has no
  -- target and no vertical aim at all, so both halves come from the cone.
  local aim_off, slope = 0, nil
  if a.offsets ~= nil then
    -- A PLAYER'S SHOT THE CORE AIMED (docs/WEAPON_PREDICTION.md): the cone and
    -- the view were already applied, identically on the shooter's client.
    aim_off, slope = a.aim_off or 0, a.slope
  elseif m.target ~= nil and not m.target_prop and m.target:valid() then
    slope = slope_to(m.e, m.target)
  elseif m.player and M.free_aim() then
    -- FIRST PERSON: where they look is where it goes, up and down included,
    -- and nothing corrects it -- the view is the aim.
    slope = M.pitch_slope(m.e)
  elseif m.player then
    local t, ang = M.autoaim(m.e, a.range)
    if t ~= nil then
      local d = (ang - m.e:facing()) % 4294967296
      if d > 2147483648 then d = d - 4294967296 end
      aim_off, slope = d, slope_to(m.e, t)
    end
  end
  for pellet = 1, (a.count or 1) do
    -- TWO returns, not four: dnp.hitscan gives (target_or_nil, impact) where
    -- the impact is a vec3. Reading it as x, y, z made `x` the vec3 and y and z
    -- nil, and vec3.new(vec3, nil, nil) RAISES -- which killed on_tick before
    -- the damage below ever ran, before update_missiles ran, and took the rest
    -- of the bucket with it. One typo presenting as four separate bugs: no
    -- damage, no tracer, monsters idle, and projectiles frozen in mid-air.
    -- THE SPREAD, which every monster declares and none had ever used.
    --
    -- The vocabulary has carried `spread` on this action since it existed, and
    -- this line never passed it -- so every monster in every converted level
    -- fired down the exact centre of its aim, and a shotgun guy's four pellets
    -- travelled one ray. It read as monsters being accurate rather than as an
    -- argument being dropped.
    --
    -- Degrees to brad, floored, exactly as weapons.lua does it: the two are the
    -- same calculation and a difference between them would be a difference
    -- between how a player's shotgun and a monster's scatter.
    -- M.spread_scale multiplies every declared spread, and exists for the same
    -- reason M.player_damage does: a server may want its monsters to shoot
    -- differently from how the catalogue describes them. Zero is perfect
    -- accuracy, which is what this engine did before this line existed and what
    -- a range test wants when it is measuring AIM rather than scatter.
    -- Per-OWNER first, so a knob meant for monsters cannot silently flatten a
    -- player's shotgun. mobs.launch_from's synthetic record sets its own, and a
    -- test that turns monster scatter off to measure aim would otherwise also
    -- turn off the seven-pellet spread it was about to assert -- a vacuous pass
    -- of exactly the kind this file keeps finding.
    local scale = m.spread_scale or M.spread_scale or 1
    local off = 0
    -- OR THE CORE'S, when a player's weapon ran in it (docs/WEAPON_PREDICTION.md):
    -- the same floor(r / 1000 * spread) per pellet, drawn from the per-player
    -- stream a predicting client draws from too, so its tracers go where these
    -- rays do. The spread knobs do not apply: a client cannot see them.
    if a.offsets ~= nil then
      off = a.offsets[pellet] or 0
    elseif a.spread and a.spread ~= 0 and scale ~= 0 then
      off = math.floor(dnp.random(-1000, 1000) / 1000
                       * (a.spread * scale * 4294967296 / 360))
    end
    off = off + aim_off
    local target, at = dnp.hitscan(m.e, a.range, off, slope)
    -- A shot that reached nothing is still a SHOT. It draws no tracer and does
    -- no damage, but it is reported, because the observability hook is how a
    -- test tells "the state machine never fired" from "the aim was off" -- and
    -- those have opposite fixes.
    --
    -- AND THE REST OF THE PELLETS STILL GO. This used to `return`, which leaves
    -- the whole function and not the iteration, so ONE pellet finding nothing
    -- cancelled the other six -- a shotgun fired at open sky put out a single
    -- ray. It was invisible in a room, because a ray that reaches a wall has an
    -- impact point and every pellet then flew.
    if at == nil then
      if M.on_shot then M.on_shot(m, nil, a.range) end
      goto next_pellet
    end

    -- The tracer carries its START, its DIRECTION and its LENGTH. A client
    -- given only the impact has no idea where the shot came from; one given
    -- only a direction draws a line to infinity through whatever it hit.
    --
    -- The muzzle height is `at.z` -- the height the ENGINE cast at -- rather
    -- than a guess, and the direction is the body's FACING rather than its
    -- aim: turning is a rate, so those differ while a monster is still coming
    -- round, and drawing the intent puts the line beside the thing it hit.
    --
    -- PLUS THIS PELLET'S OWN OFFSET, which is the whole of what makes a shotgun
    -- look like one. Without it every pellet's tracer is drawn down the centre
    -- line: seven rays are cast, seven scatter, seven land in different places
    -- -- and seven tracers are stacked on top of each other, so the player sees
    -- ONE. The damage was right and the picture was a lie, which is the worst
    -- way round.
    if a.tracer then
      local from = m.p or m.e:pos()
      local len = dnp.length(vec3.new(at.x - from.x, at.y - from.y, ZERO))
      -- AND ITS RISE, so the line is drawn where the bullet went.
      --
      -- `at.z` is where the ray ENDED, so the muzzle is that height less
      -- whatever the slope climbed on the way there -- the engine's own
      -- arithmetic read backwards rather than a second guess at eye height,
      -- which is how the two would come to disagree. A flat shot climbs
      -- nothing and this is the line it always was.
      local rise = slope ~= nil and slope * len or ZERO
      local muzzle = vec3.new(from.x, from.y, at.z - rise)
      -- Length and rise in 1/256 units, which the event carries as one u32.
      -- A shot the core aimed was drawn by its shooter's own client the
      -- moment it was predicted (World::present_weapon): sent to everybody
      -- but that client.
      dnp.vfx(a.tracer, muzzle, wrap_brad(m.e:facing() + off),
              tracer_param(len, rise), a.offsets ~= nil and m.e or nil)
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
    if target ~= nil and target:valid() then
      hurt(target, a.damage, m)
      M.apply_hit(target, m.p or m.e:pos(), a)
    end

    ::next_pellet::
  end
end

function acts.damage(m, a)
  if m == nil or m.target == nil or not m.target:valid() then return end
  hurt(m.target, a.amount, m)
end

-- Entities that pressed a button this tick, released at the start of the next.
-- See acts.swing for why this is not the engine's job.
local pressing = {}

local function release_presses()
  if next(pressing) == nil then return end
  local now = dnp.tick()
  for idx, p in pairs(pressing) do
    -- ONLY A PRESS FROM AN EARLIER TICK.
    --
    -- The tick is recorded rather than assumed, because a press can be made
    -- from inside this hook (a state's sequence reaching `swing`) or from
    -- outside it (a trigger, a test). Releasing everything at the top of the
    -- hook would clear a press made moments earlier from outside before
    -- World::step ever saw the button, and the swing would never start --
    -- silently, and only for that caller.
    if p.tick < now then
      if p.e:valid() then p.e:release(dnp.button.ATTACK_PRIMARY) end
      pressing[idx] = nil
    end
  end
end

-- ANNOUNCE AN INCOMING ATTACK, so a client can draw the warning.
--
-- The engine has no notion of a telegraph and should not: what a wind-up LOOKS
-- like is a rendering decision, and the catalogue already owns that -- a vfx
-- `kind` is exactly "which visual", the same way a tracer style is.
--
-- So this rides the existing vfx event and adds no wire format. `pos` is where
-- it is centred, `dir` the heading it covers, and `param` PACKS the two numbers
-- a warning needs and a tracer does not: the radius in 1/256 units in the low
-- sixteen bits, and how many ticks until it lands in the high sixteen. A
-- client that does not know the kind ignores it, which is what an unknown
-- effect id already does.
function M.telegraph(e, a, origin, ang)
  if a.effect == nil then return end
  local r = math.min(65535, math.floor(((a.radius or ZERO) * fx.int(256)).int))
  local t = math.min(65535, a.ticks or 0)
  dnp.vfx(a.effect, origin, wrap_brad(ang), r + t * 65536)
end

function acts.telegraph(m, a)
  M.telegraph(m.e, a, m.p or m.e:pos(), m.aim or m.e:facing())
end

-- START THE SWEPT COLLIDER. One tick, and one tick is all it takes.
--
-- The engine's attack is edge-triggered, so a single tick with the button down
-- starts a swing that runs for `attack_ticks` on its own and hits each target at
-- most once however long it lasts. Nothing here holds the button, and nothing
-- here decides what a hit costs -- on_attack does, from the same declaration
-- these parameters came from.
--
-- `press` rather than `set_input`, because a monster's movement axes belong to
-- the native loop: writing a whole input frame here would mean passing movement
-- this action does not know, and stopping the monster dead for a tick to do it.
function acts.swing(m, a)
  if m.def == nil or m.def.swing == nil then
    -- Refused at LOAD as well, so this is the belt to that braces. Swinging
    -- with the engine's defaults would be a two-unit reach and a 90-degree arc
    -- that no author wrote down.
    return
  end
  m.e:press(dnp.button.ATTACK_PRIMARY)
  -- RELEASED NEXT TICK, by the loop below, because `press` is not
  -- self-limiting. The native mob loop only writes an input frame when
  -- something IT knows about changed, and an attack state usually sets
  -- `move = "stop"` -- so a monster's axes are constant, nothing writes the
  -- button back to zero, `attack_held` stays true, and the second swing never
  -- starts. It would have worked for a chasing monster and failed for a
  -- standing one, which is the worst way for it to fail.
  pressing[m.idx] = { e = m.e, tick = dnp.tick() }
end

-- HEIGHT TOO, not only across the floor. A claw measured in plan alone
-- reached a player on a ledge two storeys up, or a cacodemon over a pit bit
-- whoever walked under it. The rule is the engine's own swing's
-- (core/src/sim/world.cpp, the attack sweep): the two bodies' height spans,
-- each stretched by the reach, must meet -- so a hit lands whenever the gap
-- between them, above or below, is within `range`. A body with no height (a
-- prop, or a test's bare entity) is a point at its feet.
local function height_of(e)
  local h = e.param and e:param("height") or nil
  return h or ZERO
end

function acts.melee(m, a)
  if m.target == nil or not m.target:valid() then return end
  local p, t = m.e:pos(), m.target:pos()
  local dx, dy = t.x - p.x, t.y - p.y
  if p.z + height_of(m.e) + a.range < t.z then return end         -- it is above the reach
  if p.z > t.z + height_of(m.target) + a.range then return end    -- or below it
  if dx * dx + dy * dy <= a.range * a.range then
    hurt(m.target, a.damage, m)
    -- The same hit effects every other damage path gets. Without this the
    -- knockback, the slow and the blood would all validate at load and do
    -- nothing at runtime -- which is the exact shape of failure the loader
    -- exists to prevent, arriving through the one action that forgot to ask.
    M.apply_hit(m.target, p, a)
  end
end

-- WHERE A MOVING TARGET WILL BE when a missile of horizontal `speed` from
-- `from` can meet it, or nil when it cannot (faster than the missile, going
-- away). In SECONDS OF FLIGHT rather than units, so the quadratic's terms stay
-- small: at a few hundred units the squares of distances overflow Q32.32.
local function intercept(from, target, speed)
  if speed <= ZERO or target.vel == nil then return nil end
  local q, v = target:pos(), target:vel()
  local dx, dy = (q.x - from.x) / speed, (q.y - from.y) / speed
  local vx, vy = v.x / speed, v.y / speed
  -- |D + V t| = t, in those units: (V.V - 1) t^2 + 2 (D.V) t + D.D = 0.
  local a = vx * vx + vy * vy - fx.int(1)
  local b = (dx * vx + dy * vy) * fx.int(2)
  local c = dx * dx + dy * dy
  local t
  if a == ZERO then
    if b >= ZERO then return nil end
    t = -c / b
  else
    local disc = b * b - a * c * fx.int(4)
    if disc < ZERO then return nil end
    local r = fx.sqrt(disc)
    local t1, t2 = (-b - r) / (a * fx.int(2)), (-b + r) / (a * fx.int(2))
    if t1 > t2 then t1, t2 = t2, t1 end
    t = (t1 > ZERO) and t1 or t2
    if t <= ZERO then return nil end
  end
  return vec3.new(q.x + v.x * t, q.y + v.y * t, q.z)
end
M.intercept = intercept

function acts.missile(m, a)
  local n = a.count or 1
  local arc = a.arc or 0
  local base, slope, origin = m.aim or 0, nil, nil
  local parent = m.parent
  local tgt = (m.target ~= nil and not m.target_prop and m.target:valid()) and m.target or nil
  if parent ~= nil then
    -- FROM A MISSILE (its on_hit, on_expire or sequence): from where it is,
    -- heading on its way unless told otherwise. Flat: a cluster spreads in
    -- the plane it burst in, and a child with gravity falls from there.
    origin = m.impact or (parent.p:valid() and parent.p:pos()) or nil
    if origin == nil then return end
    local towards = a.towards or "on"
    if towards == "back" then
      base = wrap_brad(parent.ang + 2147483648)
    elseif towards == "target" and tgt ~= nil then
      base = aim_at(origin, tgt:pos())
      local q = tgt:pos()
      local run = dnp.length(vec3.new(q.x - origin.x, q.y - origin.y, ZERO))
      if run > ZERO then slope = (eye_of(tgt) - origin.z) / run end
    else
      base = parent.ang
    end
  elseif tgt ~= nil then
    -- THE HEIGHT IT IS THROWN AT. A monster is already turned towards its
    -- target, so the horizontal half is done; this is the half that was
    -- missing, and without it an imp on a balcony threw fireballs over the
    -- floor below it forever. One slope for the whole volley, so an arc of
    -- three stays an arc.
    slope = slope_to(m.e, tgt)
    -- LEADING: at where the target will be when this missile gets there.
    local def = missiles[a.kind]
    if a.lead and def ~= nil then
      local from = m.e:pos()
      local at = intercept(from, tgt, def.speed)
      if at ~= nil then
        base = aim_at(from, at)
        local run = dnp.length(vec3.new(at.x - from.x, at.y - from.y, ZERO))
        if run > ZERO then slope = (at.z + (eye_of(tgt) - tgt:pos().z) - eye_of(m.e)) / run end
        m.lead_at = at
      end
    end
  end
  for i = 1, n do
    -- Centred on the aim: three shots with an arc of 10 go -10, 0, +10.
    local off = (n == 1) and 0 or ((i - 1) - (n - 1) / 2) * arc
    launch(m, a.kind, base + off * 4294967296 / 360, a.offset, slope, origin)
  end
  m.lead_at = nil
end

function acts.impulse(m, a)
  local towards = a.towards or "facing"
  local ang = m.aim or 0
  if towards == "target" and m.target ~= nil and m.target:valid() then
    ang = aim_at(m.e:pos(), m.target:pos())
  elseif towards == "away" then
    ang = ang + 2147483648
  elseif towards == "random" then
    ang = dnp.random(0, 359) * 4294967296 / 360
  end
  if a.jitter then
    ang = ang + math.floor(dnp.random(-a.jitter, a.jitter) * 4294967296 / 360)
  end
  ang = wrap_brad(ang)
  local d = dnp.dir(ang)
  m.e:add_impulse(vec3.new(d.x * a.speed, d.y * a.speed, a.vertical or ZERO))
end

-- TELEPORT: gone from here and somewhere near the target, with an effect at
-- both ends. `behind` is `distance` behind the target's back (then a little
-- either side, so a target with its back to a wall still gets something);
-- `near`, the default, a random spot between `min` and `distance` from it, up
-- to `tries` of them. Every candidate must be somewhere this body can STAND
-- (dnp.place: a floor, headroom, no wall or body within its radius), and a
-- `near` one somewhere it could WALK to the target from: a spot behind a wall
-- it could see through a window is not near in any sense a player feels. With
-- nowhere that qualifies it stays put, and nothing is drawn.
--
-- With no target, `near` is around itself (a blink) and `behind` does nothing.
--
-- `near` draws twice per try and stops at the first spot that qualifies. Both
-- runtimes run this same function, so they draw alike.
local DEG = 4294967296 / 360
local BEHIND = { 0, 30, -30, 60, -60 }
function acts.teleport(m, a)
  local to = a.to or "near"
  local tgt = nil
  if m.target ~= nil and not m.target_prop and m.target:valid() then tgt = m.target end
  if to == "behind" and tgt == nil then return end
  local centre = tgt and tgt:pos() or m.e:pos()
  local r, h = m.e:param("radius"), m.e:param("height")
  local far = a.distance or fx.int(3)
  local close = a.min or ZERO
  local there = tgt and tgt:sector() or nil
  local function try(ang, d)
    local dir = dnp.dir(wrap_brad(ang))
    local at, sector = dnp.place(vec3.new(centre.x + dir.x * d, centre.y + dir.y * d, centre.z),
                                 r, h, m.e)
    if at == nil then return nil end
    if to == "near" and there ~= nil and sector ~= there and
       dnp.find_path(sector, there, r, h, m.def.jump_height) == nil then
      return nil
    end
    return at
  end
  local at = nil
  if to == "behind" then
    local back = tgt:facing() + 2147483648
    for _, off in ipairs(BEHIND) do
      at = try(back + math.floor(off * DEG), far)
      if at ~= nil then break end
    end
  else
    for _ = 1, a.tries or 8 do
      local ang = math.floor(dnp.random(0, 359) * DEG)
      local d = close + (far - close) * fx.frac(dnp.random(0, 100), 100)
      at = try(ang, d)
      if at ~= nil then break end
    end
  end
  if at == nil then return end
  local from = m.e:pos()
  if a.effect then dnp.vfx(a.effect, from, m.aim or 0, 0) end
  m.e:teleport(at)
  -- Facing what it came for, which is the point of arriving behind somebody.
  -- The AIM only, which both runtimes take from here (M.run_action hands it
  -- back to the C++ loop). Not the input, the route or the position cache:
  -- those are each runtime's own, and setting them here from the Lua record
  -- -- stale for a mob the C++ loop owns -- made the two disagree the tick
  -- after every teleport. The move itself is a command, applied after the
  -- scripts, and both see the new place on the next tick.
  if tgt ~= nil then m.aim = wrap_brad(aim_at(at, tgt:pos())) end
  if a.effect then dnp.vfx(a.effect, at, m.aim or 0, 0) end
  if a.sound then dnp.sound(a.sound, at) end
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
    -- SCATTERED, per child, when asked. An angle and a distance, which is two
    -- draws -- stated because the count is the thing that has to match if this
    -- ever moves into the C++ loop.
    local jx, jy = ZERO, ZERO
    if a.jitter then
      local jd = dnp.dir(dnp.random(0, 359) * 4294967296 / 360)
      local frac = fx.frac(dnp.random(0, 100), 100)
      jx, jy = jd.x * a.jitter * frac, jd.y * a.jitter * frac
    end
    local child = M.spawn(a.mob,
      vec3.new(p.x + d.x * off + jx, p.y + d.y * off + jy, p.z))
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
        hang_markers(c)
        if c.cpp then
          dnp.mob_set(c.idx, "hp", c.hp)
          dnp.mob_set(c.idx, "dead", false)
        end
        c.e:set_solid(true)
        enter(c, c.def.initial, dnp.tick())
        return
      end
    end
  end
end

-- WHAT A HIT DOES TO THE THING IT HIT, beyond taking health off.
--
-- One helper, called from every place damage lands -- the swept collider's
-- on_attack, a hitscan ray, a blast -- because knockback that only works for
-- one of the three is the kind of gap nobody notices until a level designer
-- asks why the shotgun shoves and the rocket does not.
--
-- `knockback` is world units per second, AWAY from `origin`: the muzzle for a
-- bullet, the blast centre for an explosion, the swinger for a swing. Reusing
-- `origin` rather than the attacker's position is what makes a rocket throw
-- people outward from where it landed instead of away from the gunner.
--
-- `slow` is a multiplier on move_speed for `slow_ticks`, through the effect
-- stack, which already recomputes from base every tick and so cannot drift.
-- Tagged by the attacker, so hitting the same target twice REPLACES rather than
-- stacking to a standstill.
--
-- `share` scales the shove: for a blast, the fraction of its damage this target
-- took (fx; whole when absent). And it is divided by the target's MASS, as
-- Doom's thrust is (damage * 12.5 / mass): a baron is not moved by what throws
-- an imp across the room.
-- How fast a shoved body comes to rest, in units/s^2, and the least shove
-- worth a slide. About Doom's friction: a 100-mass body thrown at a rocket's
-- 11.7 units/s slides some seven units before it has its feet again.
local KNOCK_DECEL = fx.int(10)
local KNOCK_MIN = fx.frac(1, 2)
local KNOCK_MAX_TICKS = 90

-- `bare`: the shove only, none of the slide that follows it -- for the
-- owner of a blast (a rocket jump), whose client predicts the shove and can
-- predict nothing else about it.
function M.apply_hit(target, origin, a, share, bare)
  if target == nil or not target:valid() then return end
  -- A PROP is not shoved: it has no velocity to shove (and no add_impulse --
  -- a barrel in a blast raised here once knockback became common).
  if a.knockback and a.knockback ~= 0 and target.add_impulse ~= nil then
    local who = live[target:index()]
    local mass = (who and who.def and who.def.mass) or 100
    local k = (share or fx.int(1)) * fx.frac(100, mass)
    local kb = a.knockback * k
    -- Outward in the plane, and up by `lift`: the core's rule
    -- (sim/blast.hpp), the one a client predicting its own shove runs.
    target:add_impulse(dnp.knock_impulse(origin, target:pos(), a.knockback, a.lift or ZERO, k))
    -- AND THE SHOVE IS KEPT, as Doom keeps it: momentum that friction wears
    -- down, not something the body's own walking cancels in a tenth of a
    -- second. Steering at 80 units/s^2 ate an 8 units/s blast within six
    -- ticks, so a zombie hit by a rocket barely moved. While shoved, the body
    -- steers and brakes at KNOCK_DECEL instead -- for as long as that takes to
    -- bring the shove to rest, and no longer.
    if kb > KNOCK_MIN and not bare then
      local t = math.min(KNOCK_MAX_TICKS, math.ceil((kb / KNOCK_DECEL).raw / 4294967296 * 60))
      M.tagged_effect(target, "knock_accel", "ground_accel", "set", KNOCK_DECEL, t)
      M.tagged_effect(target, "knock_friction", "friction", "set", KNOCK_DECEL, t)
    end
  end
  if a.slow and a.slow_ticks then
    M.tagged_effect(target, "hitslow", "move_speed", "mul", a.slow, a.slow_ticks)
  end
  -- PARTICLES, from the one place every damage path already funnels through.
  --
  -- A swing, a bullet and a blast all reach here, so `hit_vfx` on any of them
  -- sprays where it landed without a second wiring -- and the client draws the
  -- burst from the kind alone, so nothing about it crosses the wire or enters a
  -- hash. Thrown BACK along the blow, away from whatever dealt it, which is the
  -- direction blood goes.
  -- WHAT BLEEDS IS THE TARGET'S BUSINESS, not the weapon's.
  --
  -- The attack names a default -- red, for most things -- and a mob that
  -- declares `blood` overrides it, because an imp bleeds green whether it was
  -- shot, punched or blown up. Putting the colour on the attack would mean
  -- every weapon in every module listing every exception.
  --
  -- A player is not in `live`, so they take the attack's default, which is what
  -- "red for enemies and the player" is.
  local mine = live[target:index()]
  local kind = (mine and mine.def and mine.def.blood) or a.hit_vfx
  if kind then
    local p = target:pos()
    local ang = (p.x.raw == origin.x.raw and p.y.raw == origin.y.raw)
        and 0 or aim_at(origin, p)
    dnp.vfx(kind, p, wrap_brad(ang), a.hit_vfx_radius and
            math.floor((a.hit_vfx_radius * fx.int(256)).int) or 0)
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
    -- The owner is in it unless `hurts_owner = false`; with `shoves_owner`
    -- too, it is SHOVED and never hurt -- a rocket jump.
    local own = e:valid() and e:index() == m.idx
    local spared = own and a.hurts_owner == false
    if e:valid() and (not spared or a.shoves_owner) then
      -- In reach, and by what share: the CORE's rule (sim/blast.hpp), since
      -- a predicting client runs it too for its own rocket jump. Inside a
      -- sphere; the falloff linear in the PLANAR distance, as it always was.
      local inside, share = dnp.blast_reach(p, e:pos(), a.radius, a.falloff)
      if inside then
        local dmg = a.damage
        -- ALL IN fx, truncated ONCE at the end with `.int`.
        if share ~= nil then dmg = (fx.int(a.damage) * share).int end
        if dmg > 0 and not spared then hurt(e, dmg, m) end
        -- Knockback and slow apply whether or not the damage rounded to zero at
        -- the edge of the falloff: being on the fringe of a blast still shoves
        -- you, and a wave that pushes without hurting is a real thing to write.
        --
        -- THE OWNER'S OWN SHOVE lands on the input it was meant for. Their
        -- client applies it in the step of the input as many ticks after the
        -- launch as the rocket flew; here the rocket flew in server ticks, and
        -- each tick the server ran on a repeated input in between left the
        -- player that much behind it -- so the shove landed early, and every
        -- snapshot until the next correction mispredicted. Held until the
        -- player's input reaches it; on a steady link that is now.
        local due = own and spared and m.launch_input ~= nil and e.input_tick ~= nil
                    and m.launch_input + (dnp.tick() - m.launch_tick) or nil
        local at = due and e:input_tick() or nil
        if due ~= nil and at ~= nil and at < due then
          owed_shoves[#owed_shoves + 1] = { e = e, origin = p, a = a, share = share, due = due }
        else
          M.apply_hit(e, p, a, share, spared)
        end
      end
    end
  end
end

function acts.set_var(m, a)     m.vars[a.var] = a.value end
function acts.add_var(m, a)     m.vars[a.var] = (m.vars[a.var] or 0) + a.by end
function acts.set_cooldown(m, a)
  local t = a.ticks
  -- ONE DRAW whenever the graph carries a jitter at all, even for zero -- the
  -- two runtimes share a stream and a draw one makes and the other skips moves
  -- every later one.
  if a.jitter then t = t + dnp.random(0, a.jitter) end
  m.vars[a.var] = dnp.tick() + t
end

function acts.var_random(m, a)
  m.vars[a.var] = dnp.random(a.min, a.max)
end
function acts.set_anim(m, a)    set_anim(m, a.anim) end
function acts.despawn(m)        if m.e:valid() then m.e:despawn() end end

-- ------------------------------------------------------------- targeting

-- A TARGET IS WHATEVER IT CHOSE. The runtime's own acquisition still finds the
-- nearest player when a mob has none -- which is what every Doom monster wants
-- -- and these are how a graph says otherwise.
--
-- `m.target_prop` is what tells the two apart afterwards. A prop handle and an
-- entity handle are different userdata types and only one of them answers
-- `hp`, which is exactly the distinction `hurt` already has to make; recording
-- it here means nothing downstream has to guess from the handle.
function acts.target_nearest(m, a)
  local of = a.of or "player"
  local reach = a.distance or m.def.sight
  local d2max = reach ~= nil and (reach * reach) or nil
  local best, bd2, is_prop = nil, nil, false

  local function consider(e, prop)
    if not e:valid() then return end
    local q = e:pos()
    local dx, dy = q.x - m.p.x, q.y - m.p.y
    local d2 = dx * dx + dy * dy
    if d2max ~= nil and d2 > d2max then return end
    if bd2 ~= nil and d2 >= bd2 then return end
    -- THE RAY LAST, because it is the expensive one and a nearer candidate
    -- found later would have wasted it. Only ever cast for the thing that is
    -- currently winning.
    if a.visible and not prop and not dnp.can_see(m.e, e) then return end
    best, bd2, is_prop = e, d2, prop
  end

  if of == "player" or of == "any" then
    for i = 1, #players do consider(players[i], false) end
  end
  if of == "mob" or of == "any" then
    for idx, o in pairs(live) do
      if idx ~= m.idx and not o.dead and (a.mob == nil or o.def.name == a.mob) then
        consider(o.e, false)
      end
    end
  end
  if of == "prop" or of == "any" then
    for _, p in ipairs(dnp.props() or {}) do
      if a.prop == nil or p:name() == a.prop then consider(p, true) end
    end
  end

  m.target = best
  m.target_prop = is_prop
  m.tp = best ~= nil and best:pos() or nil
  if m.cpp then dnp.mob_set(m.idx, "target", best ~= nil and best:index() or nil) end
end

function acts.target_attacker(m)
  -- `hurt` records it; this is what makes retaliating a DECISION rather than
  -- the mob-level `infighting` field's unconditional reflex.
  local by = m.hurt_by
  if by == nil or not by.e:valid() or by.dead then return end
  m.target, m.target_prop = by.e, false
  m.tp = by.e:pos()
  if m.cpp then dnp.mob_set(m.idx, "target", by.idx) end
end

function acts.wake(m, a)
  m.dormant = false
  if m.cpp then dnp.mob_set(m.idx, "dormant", false) end
  if a.radius == nil then return end
  local d2 = a.radius * a.radius
  for idx, o in pairs(live) do
    if idx ~= m.idx and o.dormant and not o.dead and o.e:valid() and
       (a.mob == nil or o.def.name == a.mob) then
      local q = o.e:pos()
      local dx, dy = q.x - m.p.x, q.y - m.p.y
      if dx * dx + dy * dy <= d2 then
        o.dormant = false
        if o.cpp then dnp.mob_set(o.idx, "dormant", false) end
      end
    end
  end
end

function acts.sleep(m)
  m.dormant = true
  if m.cpp then dnp.mob_set(m.idx, "dormant", true) end
end

-- DOOM'S NOISE PROPAGATION, which is most of why a room reacts as a room.
-- Without it a mob only ever learns about you by seeing you itself, so a
-- corridor of monsters wakes one at a time as you round each corner.
function acts.alert_nearby(m, a)
  if m.target == nil or not m.target:valid() or m.target_prop then return end
  local d2 = a.radius * a.radius
  for idx, o in pairs(live) do
    if idx ~= m.idx and not o.dead and o.e:valid() and
       (a.mob == nil or o.def.name == a.mob) then
      local q = o.e:pos()
      local dx, dy = q.x - m.p.x, q.y - m.p.y
      if dx * dx + dy * dy <= d2 then
        -- Woken AND pointed. Waking something with nothing to hunt just costs
        -- it a think a second until it happens to see you.
        o.dormant = false
        o.target, o.target_prop = m.target, false
        if o.cpp then
          dnp.mob_set(o.idx, "dormant", false)
          dnp.mob_set(o.idx, "target", m.target:index())
        end
      end
    end
  end
end

function acts.target_none(m)
  m.target, m.target_prop, m.tp = nil, false, nil
  if m.cpp then dnp.mob_set(m.idx, "target", nil) end
end

function acts.goto(m, a)
  enter(m, a.state, dnp.tick())
  return true                              -- stop the rest of the sequence
end

-- A PARAMETER IS CHANGED THROUGH THE EFFECT STACK, not by writing it.
--
-- `entity:param(name)` is a READ. It takes no value and ignores one, so
-- `e:param("turn_speed", 12)` compiled, ran, and did nothing at all -- which is
-- why `flying` never floated anything either: its gravity_scale was set the
-- same non-existent way. The stack is the mechanism because it recomputes from
-- base every tick and therefore cannot drift (design §6).
--
-- add_effect(name, op, value [, ticks]); ticks 0 or absent is permanent.
-- REPLACES its own previous value, by tag. It used to add an effect per call
-- and never take one away, and the effect stack holds 256 for the whole world:
-- a boss that set its speed on entering each of its states used the stack up in
-- a few minutes, and from then on every effect anywhere -- a weapon's swing, a
-- slow -- silently failed to apply. Defined after M.tagged_effect, below.
local set_param_tagged
function acts.set_param(m, a)
  set_param_tagged(m.e, "set_param/" .. a.param, a.param, "set", a.value)
end

-- Effects that replace rather than stack, by (entity, tag).
--
-- `tag` has been in the vocabulary and in the docs since effects existed and was
-- silently ignored -- an argument an author could write, that read as configured
-- and did nothing. It matters now because a WEAPON writes its swing shape into
-- these parameters when it is selected: without a tag, switching weapons ten
-- times leaves ten effects per parameter, each recomputed every tick, and the
-- tenth reach is the product of ten.
--
-- Entirely in Lua, because the binding already returns an effect id and already
-- takes one back. Nothing in C++ needed to change.
local tagged = {}

function M.tagged_effect(e, tag, param, op, value, ticks)
  local key = e:index() .. "/" .. tag
  local old = tagged[key]
  -- Removed FIRST, so the replacement is never briefly stacked on the thing it
  -- replaces. A stale id from a reused slot removes an effect that should have
  -- gone anyway, and remove_effect on an id that is not there is a no-op.
  if old ~= nil then e:remove_effect(old) end
  local eid = e:add_effect(param, op, value, ticks)
  tagged[key] = (eid ~= 0) and eid or nil
  return eid
end

set_param_tagged = M.tagged_effect

-- Drop every tagged effect this entity holds. For a weapon being put away, and
-- for a slot about to be reused by somebody else.
function M.clear_tagged(e, tag)
  local prefix = e:index() .. "/"
  -- A tag matches itself AND anything beneath it, because acts.effect stores one
  -- key per op: tagging an `effect` "reach" writes "reach/mul" and "reach/add",
  -- and a clear of "reach" that matched only the bare name would leave both
  -- behind. Matching the segment rather than the string is also what makes
  -- clearing a whole weapon's worth of parameters one call.
  local want = tag and (prefix .. tag) or nil
  for key, eid in pairs(tagged) do
    local mine = key:sub(1, #prefix) == prefix
    local hit = (want == nil)
        or key == want
        or key:sub(1, #want + 1) == want .. "/"
    if mine and hit then
      e:remove_effect(eid)
      tagged[key] = nil
    end
  end
end

-- The swing parameters, for a mob or anything else that declares the same shape.
--
-- Deliberately the same field names and the same parameters weapons.lua writes
-- for a player, through the same effect tag. Two implementations of "write a
-- swing into the parameter table" would be two places for the turn-fraction
-- conversion to be wrong in, and only one of them would get a test.
local SWING_PARAM = {
  reach = "attack_reach", half_arc = "attack_half_arc",
  sweep_from = "attack_sweep_from", sweep_to = "attack_sweep_to",
  min_reach = "attack_min_reach",
}
local SWING_U8 = { ticks = "attack_ticks", cooldown = "attack_cooldown" }

-- Written into the BODY, not stacked as an effect.
--
-- A weapon writes a player's swing through the effect stack, because a player's
-- swing is temporary -- it changes with every weapon switch -- and there are a
-- handful of players. A monster's claw is neither: it is a property of the body,
-- spent once at spawn and never removed.
--
-- And the stack could not carry it anyway. MAX_EFFECTS is 256 FOR THE WHOLE
-- WORLD (server/src/lua/effects.hpp), every monster already spends one slot on
-- `turn_speed`, and a Doom level has more than a hundred monsters. Eight slots
-- apiece for a swing exhausts it, and `add_effect` answers a full stack with a
-- zero -- so the monsters past the limit would silently get the engine's
-- defaults: a two-unit reach and a 90-degree arc nobody wrote down.
local function apply_swing(e, sw)
  if sw == nil then
    -- NOT written. `attack_enabled` defaults to true and a mob without a swing
    -- is stopped three other ways -- the action refuses, the loader refuses, and
    -- on_attack answers no damage -- none of which costs a parameter write per
    -- monster. Nothing in the engine presses a monster's attack button.
    return
  end
  e:set_body_param("attack_enabled", ONE)
  for field, param in pairs(SWING_PARAM) do
    if sw[field] ~= nil then e:set_body_param(param, sw[field]) end
  end
  for field, param in pairs(SWING_U8) do
    if sw[field] ~= nil then e:set_body_param(param, fx.int(sw[field])) end
  end
end
M.apply_swing = apply_swing

function acts.effect(m, a)
  -- One op per effect, so a modifier that both scales and offsets is two.
  if a.tag ~= nil then
    if a.mul ~= nil then M.tagged_effect(m.e, a.tag .. "/mul", a.param, "mul", a.mul, a.ticks) end
    if a.add ~= nil then M.tagged_effect(m.e, a.tag .. "/add", a.param, "add", a.add, a.ticks) end
    return
  end
  if a.mul ~= nil then m.e:add_effect(a.param, "mul", a.mul, a.ticks) end
  if a.add ~= nil then m.e:add_effect(a.param, "add", a.add, a.ticks) end
end

function acts.vfx(m, a)
  local at = a.at or "self"
  local p = m.e:pos()
  if at == "target" and m.target ~= nil and m.target:valid() then
    p = m.target:pos()
  elseif at == "impact" and m.impact ~= nil then
    p = m.impact
  end
  -- The RADIUS the effect comes out of, for a particle kind: a blast whose
  -- specks all start at one point reads as a firecracker however many there
  -- are. Carried in the event's `param`, in 1/256 units, which is the same slot
  -- a tracer uses for its length.
  -- AN IMPACT IS THROWN BACK OUT, the way the missile came. The heading it
  -- flew is into whatever it hit, and a blast cast along it went into the
  -- wall: half its fireball and most of its smoke drawn on the far side,
  -- where nobody standing on this side could see them.
  local ang = m.aim or 0
  if at == "impact" and m.impact ~= nil then ang = wrap_brad(ang + 2147483648) end
  dnp.vfx(a.kind, p, ang,
          a.radius and math.floor((a.radius * fx.int(256)).int) or 0)
end

-- A SOUND (docs/protocol.md §5.7). From the mob, following it, unless it is
-- the target's or an impact's -- a missile's burst is heard where it burst,
-- not from the monster that threw it.
function acts.sound(m, a)
  local from = a.from or "self"
  if from == "target" and m.target ~= nil and m.target:valid() then
    dnp.sound(a.name, m.target)
  elseif from == "impact" and m.impact ~= nil then
    dnp.sound(a.name, m.impact)
  elseif m.e ~= nil and m.e:valid() then
    dnp.sound(a.name, m.e)
  end
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
  local st = m.st
  local seq = st.sequence
  if seq == nil then return false end
  local entered = m.entered
  local age = tick - entered
  while m.seq_i <= #seq and seq[m.seq_i].at <= age do
    local a = seq[m.seq_i]
    m.seq_i = m.seq_i + 1
    local fn = acts[a.action]
    if fn ~= nil and fn(m, a) then return true end   -- `goto` said stop
    -- WATCHED, as the C++ loop watches: an action that sends this mob to
    -- another state, or into the same one again -- its own blast making it
    -- flinch, a death -- leaves a sequence that no longer belongs to it. Going
    -- on walked the OLD table from step 1 of the new entry, so a monster hurt
    -- by its own slam slammed again, and the runtimes parted there.
    if m.st ~= st or m.entered ~= entered or not m.e:valid() then return true end
  end
  return false
end

-- ------------------------------------------------------------------ spawning

-- ITS MARKERS (docs/MARKERS.md): the mob's own `health_bar`, and, when the
-- module launches with the debug overlay, the overlay's health counter over
-- every monster. Hung while it lives, kept current as its health changes, and
-- taken down when it dies -- a corpse has no bar.
function hang_markers(m)
  if dnp == nil or dnp.mark == nil or not m.e:valid() then return end
  local values = { value = math.max(0, math.floor(m.hp or 0)), max = math.floor(m.max_hp or m.hp or 0) }
  if m.def.health_bar and m.marker == nil then m.marker = dnp.mark(m.e, m.def.health_bar, values) end
  if m.debug_marker == nil and dnp.debug_overlay ~= nil and dnp.debug_overlay() then
    m.debug_marker = dnp.mark(m.e, "debug_health", values, { debug = true, height = fx.frac(1, 8) })
  end
end
function health_changed(m)
  if dnp == nil or dnp.mark_set == nil then return end
  local values = { value = math.max(0, math.floor(m.hp or 0)), max = math.floor(m.max_hp or m.hp or 0) }
  if m.marker then dnp.mark_set(m.marker, values) end
  if m.debug_marker then dnp.mark_set(m.debug_marker, values) end
end
function drop_markers(m)
  if dnp == nil or dnp.unmark == nil then return end
  if m.marker then dnp.unmark(m.marker); m.marker = nil end
  if m.debug_marker then dnp.unmark(m.debug_marker); m.debug_marker = nil end
end
M.hang_markers, M.drop_markers = hang_markers, drop_markers

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
    e = e, def = def, idx = e:index(), kind = kind,
    hp = def.hp, max_hp = def.hp,
    vars = {}, target = nil, aim = 0,
    dormant = opts ~= nil and opts.dormant or false,
  }
  -- Each mob gets its OWN copy of the declared vars. Sharing the definition's
  -- table would have every zombieman on the level share one cooldown.
  for k, v in pairs(def.vars or {}) do m.vars[k] = v end

  -- Through the effect stack, permanently. `param` is a read-only accessor and
  -- silently ignored the value it was handed, so neither of these did anything
  -- until the shooting range made it obvious that a monster's turn rate was
  -- never changing.
  if def.turn_speed then e:add_effect("turn_speed", "set", def.turn_speed) end

  if def.flying then
    -- No engine support needed: gravity_scale is a replicated parameter, so a
    -- flyer is flying on every client without a word of it crossing the wire.
    e:add_effect("gravity_scale", "set", ZERO)
  end

  -- THE SWEPT COLLIDER, if this mob declares one.
  --
  -- Written into the replicated parameter table exactly as a weapon writes a
  -- player's, through the same tag, so the shape of a monster's claw is
  -- predicted by the same machinery and neither side learns anything new. The
  -- monster then swings by pressing the attack button for one tick, which the
  -- shared controller turns into a volume moving through an arc -- so a
  -- monster's attack can be READ AND DODGED, which an instant distance check
  -- can never be.
  --
  -- `attack_enabled` is set BOTH WAYS on purpose. Its default is true, so a mob
  -- with no `swing` that somehow pressed the button would swing with the
  -- engine's defaults: a two-unit reach and a 90-degree arc nobody asked for.
  apply_swing(e, def.swing)

  live[m.idx] = m
  hang_markers(m)

  -- THE C++ LOOP TAKES IT, OR THIS ONE DOES -- never both.
  --
  -- dnp.mob_track answers false for a kind whose graph uses something the loop
  -- cannot run, and that is not a failure: it is the Lua path, and the record
  -- above is all it needs. What must not happen is two owners, because both
  -- would write the mob's state and neither would see the other -- so the
  -- think bucket below is built ONLY when C++ declined.
  -- M.use_cpp = false forces every mob onto the Lua path.
  --
  -- FOR THE COMPARISON, and for nothing else. The two runtimes are supposed to
  -- decide the same things, and a claim like that is worth exactly as much as
  -- the ability to run the other side of it -- so the switch exists, defaults
  -- to on, and is what tools/mobdiff drives. It is not an operator setting:
  -- there is one right answer here and it is measured, not configured.
  m.cpp = M.use_cpp ~= false and dnp ~= nil and dnp.mob_track ~= nil and
          dnp.mob_track(m.idx, kind, m.dormant) or false
  M.tracked = (M.tracked or 0) + (m.cpp and 1 or 0)
  M.on_lua = (M.on_lua or 0) + (m.cpp and 0 or 1)

  if not m.cpp then
    -- Round-robin by SPAWN ORDER, so a level's mobs are spread evenly across
    -- the think buckets whatever their entity indices turn out to be.
    local b = spawned % THINK
    spawned = spawned + 1
    local list = buckets[b]
    list[#list + 1] = m
    m.bucket = b
    enter(m, def.initial, dnp.tick())
  end
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
  M.tracked, M.on_lua = 0, 0
  -- And the C++ side, for exactly the same reason: it keys by entity index
  -- too, so a record left behind is one that will be found for somebody else's
  -- entity once the new level starts handing out indices from the bottom.
  if dnp ~= nil and dnp.mobs_reset ~= nil then dnp.mobs_reset() end
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
  -- RESTORING a saved level: the monsters that were alive come back through
  -- the 'mobs' load hook below, where they were, as hurt as they were. Placing
  -- the level's original list on top of them would bring the dead back.
  if dnp ~= nil and dnp.restoring ~= nil and dnp.restoring() then return end
  local scale = list ~= nil and list.scale or nil
  local function coord(v)
    if scale == nil then return v end
    return fx.frac(v, scale)
  end
  local before = (M.tracked or 0) + (M.on_lua or 0)
  for _, it in ipairs(list or {}) do
    M.spawn(it.kind or it.name,
            vec3.new(coord(it.x), coord(it.y), coord(it.z)), it)
  end

  -- THE CENSUS, and it is the number that matters.
  --
  -- "18 kinds compiled" at load says what COULD run on the C++ loop. This says
  -- what does. The two differ whenever a kind compiled but its instances were
  -- refused, which a load-time count cannot see -- and without this line every
  -- end-to-end test passes just as happily with the loop never having taken a
  -- single mob, which is the way a comparison between two runtimes agrees for
  -- the wrong reason.
  --
  -- Here rather than on the first tick because this is when a level's monsters
  -- arrive, and a level with nobody in it is not ticked at all.
  if (M.tracked or 0) + (M.on_lua or 0) > before then
    print(("mobs: placed %d on the C++ loop, %d on the Lua path")
          :format(M.tracked or 0, M.on_lua or 0))
  end
end

-- --------------------------------------------------------------- damage

-- The one place anything loses health, players included.
--
-- The engine has no notion of health: it reports that a swing reached
-- something, that a ray ended on somebody. What that COSTS is here. `by` is
-- the mob that did it, or nil for a player, and it is what makes infighting
-- and pain attribution possible at all.
-- WHAT A MOB LEAVES WHERE IT DIES (mob_vocab.lua drops): each entry's item, or
-- one roll of a weighted choice, from the server's own stream so the outcome is
-- one fact every client then sees. Several items are spread round the body on a
-- golden-angle ring, so a drop of three is three things to walk over rather than
-- one sprite with two more hidden inside it.
local DROP_SPREAD = fx.frac(1, 2)
local GOLDEN_BRAD = 1640531527          -- 2^32 / phi^2: successive items never line up
local drop_failures = {}
local function drop_loot(m)
  local list = m.def.drops
  if list == nil or not m.e:valid() then return end
  local p = m.e:pos()
  local placed = 0
  local function place(item, count)
    for _ = 1, count do
      local at = p
      if placed > 0 then
        local d = dnp.dir((placed * GOLDEN_BRAD) % 4294967296)
        at = vec3.new(p.x + d.x * DROP_SPREAD, p.y + d.y * DROP_SPREAD, p.z)
      end
      -- Protected: an item the catalogue lost since load is a complaint, not a
      -- raise in the middle of a death.
      local ok, made = pcall(dnp.spawn_prop, item, at)
      -- And SAID when it did not appear: a raise, or nil because the prop
      -- table is full -- which is what a region of many levels runs into, and
      -- which looked exactly like the drop table not working.
      if (not ok or made == nil) and not drop_failures[item] then
        drop_failures[item] = true
        print(("mobs: %s could not drop '%s': %s"):format(m.def.name, item,
              ok and "the prop table is full" or tostring(made)))
      end
      placed = placed + 1
    end
  end
  for _, entry in ipairs(list) do
    local n = entry.count or 1
    if entry.item ~= nil then
      place(entry.item, n)
    else
      local total = 0
      for _, c in ipairs(entry.one_of) do total = total + c.weight end
      local r = dnp.random(1, total)
      for _, c in ipairs(entry.one_of) do
        r = r - c.weight
        if r <= 0 then
          if c.item ~= nil then place(c.item, n) end
          break
        end
      end
    end
  end
end
M.drop_loot = drop_loot
-- For a test: the drops checker, which otherwise only M.load reaches (and
-- M.load refuses a second call).
M.check_drops = function(d, where) return check_drops(d, where) end

-- Told to the graph runtime when it is loaded (graphs.lua's when_hurt and
-- when_dies): who, by whom, how much, and what kind of monster.
local function tell_graphs(what, target, by, amount, kind)
  if graphs ~= nil and graphs.notify ~= nil then graphs.notify(what, target, by and by.e or nil, amount, kind) end
end


-- HOW LONG THIS FLINCH LASTS (docs/MOBS.md, `pain_time`). `rolled` is what
-- entering the pain state gave -- its `ticks` plus any `ticks_jitter`, rolled
-- in `enter` so both runtimes draw the same numbers. A base a script set
-- (M.set_pain_ticks) replaces it; the def's `pain_time` adds the hit's share
-- and caps the total. Nil when the pain state has no length to change.
local function pain_ticks_for(m, rolled, amount)
  local ticks = rolled
  if m.pain_base ~= nil then ticks = m.pain_base end
  if ticks == nil or ticks < 0 then return nil end
  local pt = m.def.pain_time
  if pt ~= nil then
    if pt.per_damage ~= nil and amount ~= nil and amount > 0 then
      ticks = ticks + math.max(0, (fx.int(math.floor(amount)) * pt.per_damage).int)
    end
    if pt.max ~= nil and ticks > pt.max then ticks = pt.max end
  end
  return math.max(1, ticks)
end

-- True while a hit handed over from another level is being taken here (see
-- `hurt_player`), so it is never handed on again.
local handed_over = false

function hurt(target, amount, by)
  if target == nil or not target:valid() then return end
  local m = live[target:index()]

  -- ANOTHER LEVEL'S MONSTER. In a region every level's monsters share one
  -- world, so a shot fired here -- by a monster of ours, or by a player whose
  -- weapons this level is firing -- can reach a monster a neighbouring level
  -- keeps. Its health and its pain are that level's, so the hit is handed
  -- over; taking it off here would leave a monster that never dies.
  if m == nil and target.kind ~= nil and target:kind() ~= 0 and dnp.call_owner ~= nil and
     dnp.call_owner(target, "hurt_mob", amount, by and by.e or nil) then
    return
  end

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

    -- ANOTHER LEVEL'S PLAYER. In a region a monster of ours can hit a player
    -- standing next door (a sector this level cannot name: -1). What a hit,
    -- a death and a respawn mean is the level the player is IN -- its hooks,
    -- its graphs' when_hurt / when_dies, its home -- so the hit is handed over;
    -- kept here, a player killed across a seam came back at this level's start
    -- (docs/REGION_GAPS.md §6). Taken here when that level keeps no mobs.lua.
    if not handed_over and target.sector ~= nil and target:sector() == -1 and dnp.call_level ~= nil and
       dnp.call_level(target, "hurt_player", amount, by and by.e or nil) then
      return
    end

    if M.on_player_hurt then
      M.on_player_hurt(target, amount, by)
      return
    end

    -- WHAT A PLAYER SOUNDS LIKE hurt and dying is the module's to say
    -- (M.player_sounds = { pain = name, death = name }); nil is silence.
    local ps = M.player_sounds
    local hp = (target:hp() or 100) - amount
    tell_graphs("hurt", target, by, amount, nil)
    if hp > 0 then
      target:set_hp(hp)
      if ps and ps.pain then dnp.sound(ps.pain, target) end
      return
    end
    -- Heard where they FELL, not where the respawn puts them.
    if ps and ps.death then dnp.sound(ps.death, target, { follow = false }) end

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
    tell_graphs("dies", target, by, amount, nil)
    return
  end
  if m.dead then return end

  m.hp = m.hp - amount
  health_changed(m)
  m.hurt_at = dnp.tick()
  -- A PLAYER is not a mob, and the difference is what infighting means. A
  -- monster shot by another monster turns on it; a monster shot by a player is
  -- being shot by the thing it was already trying to kill, and counting that as
  -- infighting would have Doom's whole bestiary retaliate against each other
  -- because somebody fired a shotgun. `hurt_by` still records WHO, so
  -- `target_attacker` retaliates correctly either way.
  m.hurt_by_mob = by ~= nil and not by.player
  -- WHO, not just whether. `hurt_by_mob` answers the condition; this is what
  -- `target_attacker` needs, and keeping only the boolean is why retaliating
  -- could not be a decision before.
  m.hurt_by = by
  -- MIRRORED, because the C++ loop owns the copy its conditions read. `hp` is
  -- what `hp_below` tests and the pain edge is what `was_hurt` and
  -- `hurt_by_mob` test, so a mob the loop owns would otherwise be hurt here
  -- and healthy there -- and would never flinch.
  if m.cpp then
    dnp.mob_set(m.idx, "hp", m.hp)
    dnp.mob_set(m.idx, "hurt", by ~= nil)
  end
  tell_graphs("hurt", target, by, amount, m.def and m.def.name or m.kind)

  if m.hp <= 0 then
    m.dead = true
    m.target = nil
    drop_markers(m)
    if m.cpp then
      dnp.mob_set(m.idx, "dead", true)
      dnp.mob_set(m.idx, "target", nil)
    end
    if m.e:valid() then m.e:set_solid(false) end
    enter(m, "death", dnp.tick())
    -- THE DEAD DO NOT THINK -- neither runtime gives a dead mob another think
    -- -- so a death state's sequence would never run. What it opens with (its
    -- `at = 0` entries: the scream, a pain elemental's last brood) runs here,
    -- as it dies, and once.
    local dst = m.def.states and m.def.states.death
    for _, a in ipairs((dst and dst.sequence) or {}) do
      if (a.at or 0) == 0 and acts[a.action] then acts[a.action](m, a) end
    end
    -- What it leaves, at the body, before the body may go.
    drop_loot(m)
    if m.def.corpse then
      corpses[#corpses + 1] = m
    elseif m.e:valid() then
      m.e:despawn()
    end
    if M.on_death then M.on_death(m, by) end
    tell_graphs("dies", target, by, amount, m.def and m.def.name or m.kind)
    return
  end

  -- INFIGHTING. Doom's monsters turn on whatever hurt them, and it is most of
  -- what makes a crowded room interesting. `by` is nil for a player, so this
  -- only ever retargets onto another mob.
  -- And not onto something already dead: a corpse cannot have hurt anybody,
  -- but a delayed missile from one can arrive after it died.
  if by ~= nil and m.def.infighting and by.e:valid() and by.idx ~= m.idx and
     not by.dead then
    m.target = by.e
    if m.cpp then dnp.mob_set(m.idx, "target", by.idx) end
  end

  local chance = m.def.pain_chance

  -- WHICH STATE IT IS IN, asked of whichever runtime owns it.
  --
  -- `m.st` is the current state's TABLE and only the Lua path maintains it:
  -- for a tracked mob `enter` delegates and returns, so m.st stays nil, the
  -- `and` chain below short-circuits, and the monster never flinches. It was
  -- exactly that -- silently, because a monster that does not flinch looks
  -- like a monster with a low pain chance -- and it is what the two-runtime
  -- comparison in tools/mobdiff was built to catch.
  local st = m.st
  if m.cpp then
    local name = dnp.mob_state(m.idx)
    st = name ~= nil and m.def.states[name] or nil
  end

  -- PAIN CHANCE, rolled. A monster that ALWAYS flinches can be stun-locked
  -- into never acting; one that never does gives no sign a hit landed.
  --
  -- The roll is the LAST term on purpose: Lua's `and` short-circuits, so a
  -- mob with no pain state or an uninterruptible one must not draw. The two
  -- runtimes share one RNG stream, and a draw one of them makes and the other
  -- does not desynchronises every later one.
  if chance ~= nil and st and st.interruptible ~= false and
     m.def.states.pain ~= nil and dnp.random(0, 255) < chance then
    local rolled = enter(m, "pain", dnp.tick())
    local ticks = pain_ticks_for(m, rolled, amount)
    if ticks ~= nil and ticks ~= rolled then
      m.state_ticks = ticks
      if m.cpp then dnp.mob_set(m.idx, "state_ticks", ticks) end
    end
  end
end
M.hurt = hurt

-- --------------------------------------------------------------- missiles

-- THE VERTICAL SPEED A THROW STARTS WITH, units a second, for a missile with
-- `gravity`. At a target, the one arc that lands on its middle: `speed` is the
-- HORIZONTAL speed, so the flight takes distance / speed seconds whatever the
-- arc, and there is exactly one starting climb that comes down where the target
-- is. A higher lob is a slower missile. With no target, the launch's own slope
-- (a player's pitch) times its speed.
--
-- Solved for the flight as it is actually stepped, not the textbook parabola:
-- each tick the vertical speed drops by g/60 and then the missile rises by
-- speed/60, which lands n ticks later g*n/7200 lower than the continuous arc.
-- Hence the extra g/120 -- without it every throw fell short by g*t/120.
local SIXTY = fx.int(60)
local ONE = fx.int(1)
-- A THROW AT A LAUNCH SPEED (`throw = "low"` / `"high"`): the angle that lands
-- on the target at `speed` along the line of flight, so the horizontal speed is
-- what that angle leaves. Returns the vertical speed and the horizontal one.
--
-- tan = (v^2 -/+ sqrt(v^4 - g (g x^2 + 2 y v^2))) / (g x), written in
-- k = g x / v^2 and m = g y / v^2 so no term is a fourth power of a speed --
-- which in Q32.32 overflows at fifty units a second. Out of reach (nothing
-- under the root) it throws at 45 degrees, its furthest. With no target, along
-- the launch's slope. Then the same g/120 as throw_vz, for the stepped flight.
local function throw_at_speed(def, from, target, slope, at)
  local v, g = def.speed, def.gravity
  local tan = slope or ZERO
  if target ~= nil and target:valid() and v > ZERO then
    local q = at or target:pos()
    local x = dnp.length(vec3.new(q.x - from.x, q.y - from.y, ZERO))
    if x > ZERO then
      local y = q.z + (eye_of(target) - target:pos().z) - from.z
      local k = g * x / (v * v)
      local m = g * y / (v * v)
      local disc = ONE - k * k - m * fx.int(2)
      if disc < ZERO then
        tan = ONE
      else
        local r = fx.sqrt(disc)
        tan = (def.throw == "high" and (ONE + r) or (ONE - r)) / k
      end
    end
  end
  local h = v / fx.sqrt(ONE + tan * tan)
  local vz = h * tan
  if target ~= nil and target:valid() then vz = vz + g / fx.int(120) end
  return vz, h
end

local function throw_vz(def, from, target, slope, at)
  local v = def.speed
  if target ~= nil and target:valid() and v > ZERO then
    -- At where it will be, when the throw leads it.
    local q = at or target:pos()
    local d = dnp.length(vec3.new(q.x - from.x, q.y - from.y, ZERO))
    if d > ZERO then
      local t = d / v
      local eye = q.z + (eye_of(target) - target:pos().z)
      return (eye - from.z) / t + def.gravity * t / fx.int(2)
             + def.gravity / fx.int(120)
    end
  end
  return (slope or ZERO) * v
end

-- A shot's children, all generations together: what `max_children` counts.
local MAX_CHILDREN = 16
-- Launched while update_missiles walks `flying`: queued, and added after the
-- walk. A key added to a table pairs() is walking is undefined in Lua.
local walking, queued = false, {}

-- `slope` is rise per unit of horizontal run, and nil is the flat flight every
-- missile in this file had before there was an auto-aim. See M.autoaim.
function launch(m, kind, ang, offset, slope, origin)
  local def = missiles[kind]
  if def == nil then return end
  -- A whole turn, always. A volley's offsets are added to the aim, and with
  -- the aim near 0 (due east) the left-hand shots came out negative -- which
  -- the bindings take as an unsigned turn, so they all flew at 0: a spread of
  -- five was three lines.
  ang = wrap_brad(ang)
  -- A MISSILE'S CHILD: on the budget of the shot it came from, refused once
  -- that is spent. Each is a prop slot, and a missile that splits into two
  -- that split again fills the table in a second.
  local family = m.parent and m.parent.family or nil
  if family ~= nil then
    if family.left <= 0 then return end
    family.left = family.left - 1
  end
  local from = origin or m.e:pos()
  local d = dnp.dir(ang)
  local off = offset or ZERO
  -- Clear of the shooter's own body, or it is touching its owner on the tick
  -- it appears -- which bursts it on the thing that fired it. A child starts
  -- at its parent's height, which already is where it was flying.
  local p = dnp.spawn_prop(def.prop,
    vec3.new(from.x + d.x * off, from.y + d.y * off,
             from.z + (origin and ZERO or (def.z_offset or ZERO))))
  if p == nil then return end
  -- WHICH WAY IT IS POINTING. A missile is the one prop with a heading worth
  -- replicating, and without this every rocket reported zero -- so an eight-view
  -- sprite with all eight rotations extracted showed rotation 1 from every
  -- angle, which reads as the art missing its rotations.
  p:set_facing(wrap_brad(ang))
  if def.anim_fly then p:set_animation(def.anim_fly) end
  local s = {
    p = p, def = def, dir = d, ang = ang, slope = slope,
    -- What it homes on: a monster's target, or what a player's auto-aim
    -- picked as they fired (M.launch_from), which is not the owner's
    -- `target` -- that stays nil for a player, see synthetic_owner.
    owner = m, target = m.homing_target or m.target, born = dnp.tick(),
    family = family or { left = def.max_children or MAX_CHILDREN },
  }
  -- A FALLING MISSILE carries a vertical speed that gravity changes each tick.
  -- Thrown at its target by a monster; along the launch's slope by a player,
  -- whose own client flies the same arc from the same numbers.
  if def.gravity ~= nil and def.gravity ~= ZERO then
    local aim_at_target = (not m.player and origin == nil) and m.target or nil
    if def.throw ~= nil then
      s.vz, s.speed = throw_at_speed(def, p:pos(), aim_at_target, slope, m.lead_at)
    else
      s.vz = throw_vz(def, p:pos(), aim_at_target, slope, m.lead_at)
    end
  end
  if walking then queued[#queued + 1] = s else flying[p:index()] = s end
end

-- The owner record a non-mob acts through.
--
-- A player has no mob record and must not be given one: `live[]` is what the
-- think loop walks, and a player in it would be thought about as a monster. So
-- this is a record with the fields the action table reads and nothing else --
-- built HERE, in the file that owns the record's shape, rather than in every
-- caller that wants to borrow an action.
--
-- `aim` is passed explicitly because a mob's `aim` is a STEERING field that
-- acts.face and drive maintain, and a player's heading is e:facing().
-- `target` is nil, which is load-bearing: homing is skipped and acts.damage
-- early-returns rather than hurting whoever the last mob was after.
-- `player` is what keeps hurt() from reading a player's shot as infighting.
function M.synthetic_owner(e, ang)
  return {
    e = e, idx = e:index(), aim = ang,
    target = nil, dead = false, vars = {},
    player = true, spread_scale = 1,
  }
end
local synthetic_owner = M.synthetic_owner

-- Launch a missile owned by an ENTITY rather than by a mob record.
--
-- The one thing a player needed and could not have. `launch` is a file-local
-- and every consumer downstream reads an owner RECORD -- m.e, m.idx, m.aim,
-- m.target, m.impact, m.dead -- so a weapon could not call it without building
-- one, and a weapon building a mob record would make the record's shape the
-- weapon's business instead of this file's. See synthetic_owner above.
function M.launch_from(e, kind, ang, offset, slope)
  if e == nil or not e:valid() then return end
  local owner = synthetic_owner(e, ang)
  -- ON THE PLAYER'S CLOCK as well as the world's: which of their inputs this
  -- launch came out of, and when. A rocket's flight is in server ticks, but
  -- its owner's client times the shove of its own blast in its input ticks,
  -- and the two part by a tick whenever the server runs one on a repeated
  -- input during the flight (see damage_area).
  owner.launch_input = e.input_tick ~= nil and e:input_tick() or nil
  owner.launch_tick = dnp.tick()
  -- A HOMING ONE locks onto what the auto-aim would correct this shot
  -- towards, as it is fired: a player has no target of their own.
  local def = missiles[kind]
  if def ~= nil and def.homing then
    owner.homing_target = M.autoaim(e, M.autoaim_range, M.autoaim_h)
  end
  launch(owner, kind, ang, offset, slope)
  owner.homing_target = nil
  return owner
end

-- A PLAYER'S VERTICAL AIM, as a slope (rise per unit of run): from where they
-- look (e:pitch(), a fraction of a turn) when the module's controls are
-- first-person. A pitch is at most 80 degrees, so the run is never zero.
function M.free_aim()
  return dnp.controls ~= nil and dnp.controls() == "fps"
end
function M.pitch_slope(e)
  local p = e.pitch and e:pitch()
  if p == nil or p == ZERO then return nil end
  local brad = math.floor((p * fx.int(65536)).int * 65536) % 4294967296
  local d = dnp.dir(brad)
  return d.y / d.x
end

-- WHAT A PLAYER'S WEAPON DOES TO THE WORLD, in one place.
--
-- The weapon runtime (weapons.lua) decides WHEN -- which button, which phase,
-- how much ammunition -- and this does the shooting: the ray, the missile, the
-- blast, the shove, the warning, all through the actions a monster uses, so a
-- player's shotgun and a monster's are the same arithmetic.
--
-- Called directly when the weapons run in this level's VM, and through
-- dnp.call_level when they are the GAME's (a module's `game` scripts): then the
-- weapons follow the player from level to level, and this is the level they are
-- standing in being asked to fire. Nothing that crosses is a position -- the
-- game sees the world's coordinates and this level its own -- so offsets come
-- in and positions are read here.
function M.weapon_runtime(e, op, a, b, c, d, f)
  if e == nil or not e:valid() then return nil end
  -- First person aims by the view: no target, the facing as it is, and the
  -- slope it looks at.
  if op == "autoaim" then
    if M.free_aim() then return nil, nil, M.pitch_slope(e) end
    return M.autoaim(e)
  end
  if op == "launch" then
    -- kind, the declaration (when the game owns it), heading, offset, slope.
    if missiles[a] == nil and b ~= nil then M.add_missiles({ [a] = b }) end
    if missiles[a] == nil then
      print(("mobs: a weapon fired missile '%s', which nothing declares"):format(tostring(a)))
      return nil
    end
    M.launch_from(e, a, c, d, f)
    return nil
  end
  local m = synthetic_owner(e, e:facing())
  if op == "hitscan" then acts.hitscan(m, a)
  elseif op == "impulse" then acts.impulse(m, a)
  elseif op == "damage_area" then
    -- `offset` puts the centre in front of the body and `height` above it.
    if a.offset and a.offset ~= ZERO then
      local p, dir = e:pos(), dnp.dir(e:facing())
      m.impact = vec3.new(p.x + dir.x * a.offset, p.y + dir.y * a.offset,
                          p.z + (a.height or ZERO))
    end
    acts.damage_area(m, a)
  elseif op == "telegraph" then
    local p = e:pos()
    local origin = p
    if a.offset and a.offset ~= ZERO then
      local dir = dnp.dir(e:facing())
      origin = vec3.new(p.x + dir.x * a.offset, p.y + dir.y * a.offset, p.z)
    end
    M.telegraph(e, a, origin, e:facing())
  else
    print(("mobs: a weapon asked for '%s', which is not something it can do"):format(tostring(op)))
  end
  return nil
end
if dnp ~= nil and dnp.export ~= nil then
  dnp.export("weapon_runtime", function(...) return M.weapon_runtime(...) end)
  -- A hit from a neighbouring level, on one of ours (see hurt). Who dealt it
  -- comes as an entity; it is somebody else's, so it is nobody's record here.
  -- A hit on one of OUR players, from a neighbouring level's monster (see
  -- hurt). Taken as if dealt here.
  dnp.export("hurt_player", function(target, amount, attacker)
    if target == nil or not target:valid() or target.hp == nil then return false end
    local by = nil
    if attacker ~= nil and attacker:valid() then by = synthetic_owner(attacker, attacker:facing()) end
    handed_over = true
    local ok, err = pcall(hurt, target, amount, by)
    handed_over = false
    if not ok then error(err, 0) end
    return true
  end)
  dnp.export("hurt_mob", function(target, amount, attacker)
    if live[target:index()] == nil then return false end
    local by = nil
    if attacker ~= nil and attacker:valid() then by = synthetic_owner(attacker, attacker:facing()) end
    hurt(target, amount, by)
    return true
  end)
end

--- Is this missile name known? For a loader in another file that wants to
--- refuse a typo at LOAD rather than fire nothing at runtime.
function M.has_missile(name) return missiles[name] ~= nil end
--- Is `kind` a monster this level defines? (graphs.lua's `spawn` asks.)
function M.has(kind) return defs[kind] ~= nil end
--- A monster's health, from its record -- the engine's hp slot is a player's.
--- Nil for anything that is not a live monster here.
function M.health(e)
  local m = e ~= nil and e.valid ~= nil and e:valid() and live[e:index()] or nil
  if m == nil or m.dead then return m and 0 or nil end
  return m.hp
end
--- Set a live monster's health (graphs.lua's `heal`), capped at its maximum.
function M.set_health(e, hp)
  local m = e ~= nil and e:valid() and live[e:index()] or nil
  if m == nil or m.dead then return false end
  m.hp = math.max(1, math.min(m.max_hp or hp, math.floor(hp)))
  if m.cpp and dnp.mob_set ~= nil then dnp.mob_set(m.idx, "hp", m.hp) end   -- as hurt mirrors it
  health_changed(m)
  return true
end

--- A live monster's base pain length, in ticks: what a script set, else its
--- `pain` state's `ticks`. Nil for anything that is not a live monster here,
--- or one with no pain state. The hit's share (`pain_time`) comes on top.
function M.pain_ticks(e)
  local m = e ~= nil and e.valid ~= nil and e:valid() and live[e:index()] or nil
  if m == nil or m.dead then return nil end
  if m.pain_base ~= nil then return m.pain_base end
  local st = m.def.states and m.def.states.pain
  return st and st.ticks or nil
end
--- Set a live monster's base pain length (a stun weapon, a frenzy), in ticks,
--- at least 1; nil puts back its own. Takes effect from its next flinch.
function M.set_pain_ticks(e, ticks)
  local m = e ~= nil and e.valid ~= nil and e:valid() and live[e:index()] or nil
  if m == nil or m.dead then return false end
  if ticks == nil then m.pain_base = nil; return true end
  m.pain_base = math.max(1, math.floor(tonumber(ticks) or 1))
  return true
end

-- Register more missile kinds after load.
--
-- mobs.load REFUSES a second call, deliberately -- two tables each declaring
-- monsters is the bug that hook lists exist to prevent. But a weapon file
-- legitimately has projectiles of its own, and typing a player's rockets into
-- the MONSTER table would put them where nothing places them.
--
-- A collision is refused rather than merged silently: two files disagreeing
-- about what "rocket" means is a bug, and the later one winning quietly is how
-- it survives to production.
function M.add_missiles(t)
  local added, clashed = 0, 0
  -- CHECKED AS mobs.load CHECKS ITS OWN: fields, values, and the actions they
  -- run. All of this batch go in first, so one may name another; one with a
  -- problem is then taken out again and said, rather than flying wrong.
  local fresh = {}
  for name, def in pairs(t or {}) do
    if missiles[name] ~= nil then
      clashed = clashed + 1
      print(("mobs: missile '%s' is already declared; the later one is ignored")
            :format(name))
    else
      missiles[name] = def
      fresh[#fresh + 1] = name
    end
  end
  table.sort(fresh)
  for _, name in ipairs(fresh) do
    local def = missiles[name]
    local before = #problems
    if type(def) ~= "table" then
      complain("missile '%s': is not a table", name)
    else
      check_missile_fields(name, def)
      check_action_list(def.on_hit, ("missile '%s' on_hit"):format(name), nil)
      check_action_list(def.on_expire, ("missile '%s' on_expire"):format(name), nil)
      check_action_list(def.on_bounce, ("missile '%s' on_bounce"):format(name), nil)
      check_missile_sequence(def, ("missile '%s'"):format(name))
    end
    if #problems > before then
      for i = before + 1, #problems do print("mobs: " .. problems[i]) end
      for i = #problems, before + 1, -1 do problems[i] = nil end
      missiles[name] = nil
      clashed = clashed + 1
    else
      added = added + 1
    end
  end
  if added > 0 then publish_missiles() end
  return added, clashed
end

-- Missiles in flight.
--
-- Lifted out of the think loop so the empty-level short circuit still runs it:
-- a fireball with nobody left to hit still has to expire. Skipping it leaks a
-- prop slot per shot, which is a slower and much more confusing failure than a
-- dropped frame.
-- A MISSILE'S OWN ACTIONS -- its on_hit, on_expire or sequence -- run by its
-- OWNER, so `melee` and `damage_area` mean the same in a missile as they do in
-- a state and the runtime needs no second copy of either. With `target` the
-- thing it reached (or its own), `impact` where it is, and `parent` this
-- missile: what a `missile` action launches children from.
local function missile_acts(s, list, impact, target)
  local o = s.owner
  local saved_t, saved_p = o and o.target, o and o.parent
  if o then o.target, o.impact, o.parent = target, impact, s end
  for _, a in ipairs(list) do
    local fn = acts[a.action]
    if fn then fn(o, a) end
  end
  if o then o.target, o.impact, o.parent = saved_t, nil, saved_p end
end

local function update_missiles(tick)
  walking = true
  for idx, s in pairs(flying) do
    local def = s.def
    if not s.p:valid() then
      flying[idx] = nil
    elseif s.burst_at ~= nil then
      if tick >= s.burst_at then s.p:despawn(); flying[idx] = nil end
    elseif tick - s.born > (def.life or 300) then
      if def.on_expire then
        missile_acts(s, def.on_expire, s.p:pos(), s.owner and s.owner.target)
      end
      s.p:despawn(); flying[idx] = nil
    else
      -- ITS SEQUENCE, by age: what it does on the way, where it is.
      local seq = def.sequence
      if seq ~= nil then
        local age = tick - s.born
        s.seq_i = s.seq_i or 1
        while s.seq_i <= #seq and (seq[s.seq_i].at or 0) <= age do
          local a = seq[s.seq_i]
          s.seq_i = s.seq_i + 1
          missile_acts(s, { a }, s.p:pos(), s.target)
        end
      end
      local p = s.p:pos()
      -- Its own speed when it has one (a throw at a launch speed, a bounce
      -- that slowed it), the definition's otherwise.
      local step = (s.speed or def.speed) / fx.int(60)

      -- HOMING, before the step: turn towards the target by at most `homing`
      -- degrees, then fly straight for one tick. Turning after moving would
      -- have it arrive at where the target was.
      if def.homing and s.target ~= nil and s.target:valid() and
         tick - s.born >= (def.homing_after or 0) then
        local want = aim_at(p, s.target:pos())
        local d = want - s.ang
        while d > 2147483647 do d = d - 4294967296 end
        while d < -2147483648 do d = d + 4294967296 end
        -- Too far round to chase: let go, and fly straight from here on.
        if def.homing_cone and math.abs(d) > def.homing_cone * 4294967296 / 360 then
          s.target = nil
          d = nil
        end
        local cap = def.homing * 4294967296 / 360
        if d ~= nil then
          if d > cap then d = cap elseif d < -cap then d = -cap end
          s.ang = s.ang + d
          s.dir = dnp.dir(s.ang)
          -- VERTICALLY TOO, for a missile that was launched with a slope. A
          -- revenant's tracer that steers in plan and holds the height it left
          -- with follows you round a corner and then flies over your head.
          -- From the missile's own position, not through slope_to: it is a PROP,
          -- and `p` is the point it is steering from this tick anyway.
          if s.slope ~= nil then
            local q = s.target:pos()
            local run = dnp.length(vec3.new(q.x - p.x, q.y - p.y, ZERO))
            if run > ZERO then s.slope = (eye_of(s.target) - p.z) / run end
          end
          -- And the SPRITE turns with it. A revenant's missile that steers but
          -- keeps its launch heading shows the wrong one of its eight views for
          -- the rest of its flight.
          s.p:set_facing(wrap_brad(s.ang))
        end
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
      -- AT ITS SLOPE, so the floor, a ceiling and a step stop it where its
      -- line meets them: a rocket fired at your feet bursts there, rather than
      -- sinking through the floor until a wall turned up. Gravity is part of
      -- the line for the tick, as the move below adds it.
      local climb = s.slope or ZERO
      -- FALLING: this tick's vertical speed, gravity taken off first, and the
      -- rise it gives. The sweep is along that line, so a lob that drops onto a
      -- floor or a head stops where it meets it.
      local dz = nil
      if s.vz ~= nil then
        s.vz = s.vz - def.gravity / SIXTY
        dz = s.vz / SIXTY
        climb = step > ZERO and dz / step or ZERO
      end
      local what, at, wall_x, wall_y, plane = dnp.sweep_hit(p, s.dir, step,
                                     s.radius or def.radius or du(11),
                                     s.p, s.owner and s.owner.e or nil, climb)
      local hit = (what ~= nil and what ~= false) and what or nil
      local blocked = what ~= nil
      local nx, ny = p.x + s.dir.x * step, p.y + s.dir.y * step

      local bounces = s.bounces
      if bounces == nil then bounces = def.bounces or 0 end
      if hit == nil and blocked and bounces > 0 then
        -- A BOUNCE, off the map only: a body it reaches is what on_hit is for.
        -- A floor or a ceiling turns its climb round; a wall turns its heading
        -- about the wall's normal. Either way it keeps `bounce_keep` of its
        -- speed, and flies on from where it met it next tick.
        s.bounces = bounces - 1
        local keep = def.bounce_keep or fx.frac(6, 10)
        if at ~= nil then s.p:set_pos(at) end
        if plane ~= nil and plane ~= 0 then
          if s.vz ~= nil then s.vz = -s.vz * keep end
          if s.slope ~= nil then s.slope = -s.slope end
        elseif wall_x ~= nil then
          -- The wall's normal, named apart from `nx`/`ny` below (the next
          -- position), which shadowed it the first time and reflected the
          -- missile off a normal eleven units long.
          local dot = s.dir.x * wall_x + s.dir.y * wall_y
          local rx, ry = s.dir.x - wall_x * dot * fx.int(2), s.dir.y - wall_y * dot * fx.int(2)
          s.ang = wrap_brad(dnp.atan2(ry, rx))
          s.dir = dnp.dir(s.ang)
          s.p:set_facing(s.ang)
          if s.vz ~= nil then s.vz = s.vz * keep end
        end
        s.speed = (s.speed or def.speed) * keep
        if def.on_bounce then missile_acts(s, def.on_bounce, at or s.p:pos(), nil) end
      elseif hit ~= nil or blocked then
        if at ~= nil then s.p:set_pos(at) end
        -- ON A WALL TOO, not only on a body.
        --
        -- This used to read `hit ~= nil`, so a missile that stopped against
        -- geometry -- which is most of them -- played its burst animation and
        -- ran none of its on_hit at all: no damage, no blast, no explosion. A
        -- rocket fired at a wall was a puff of sprite and nothing else, and
        -- Doom's whole rocket jump is a rocket fired at the floor.
        --
        -- `target` is then nil, which every action already copes with by
        -- construction: acts.damage early-returns without one, and damage_area
        -- and vfx are about the PLACE -- they read `impact`, which is set just
        -- below whether or not anybody was standing there.
        if def.on_hit then
          -- The missile's OWNER is what runs on_hit, with the thing it reached
          -- as the target -- so `melee` and `damage_area` mean the same in a
          -- missile as they do in a state, and the runtime does not need a
          -- second copy of either.
          -- Where it landed is the impact, for the actions that are about a
          -- PLACE rather than about the mob: damage_area and vfx.
          missile_acts(s, def.on_hit, at or s.p:pos(), hit)
        end
        if def.anim_burst then
          s.p:set_animation(def.anim_burst)
          s.burst_at = tick + (def.burst_ticks or 20)
        else
          s.p:despawn(); flying[idx] = nil
        end
      else
        s.p:set_pos(vec3.new(nx, ny, p.z + (dz or (s.slope ~= nil and s.slope * step or ZERO))))
      end
    end
  end
  walking = false
  -- The children launched on the way, in the order they were: they fly from
  -- the next tick, as a missile a monster fires does.
  for i = 1, #queued do
    local q = queued[i]
    if q.p:valid() then flying[q.p:index()] = q end
    queued[i] = nil
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

  if m.state_ticks ~= nil and tick - m.entered >= m.state_ticks then
    if st.next == PREVIOUS then
      if not resume(m, tick) then enter(m, m.def.initial, tick) end
    else
      enter(m, st.next, tick)
    end
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

  -- WRAPPED, every one of them. A turn is 2^32 and these are plain Lua
  -- numbers, so `aim + 1073741824` runs past the top of the range whenever the
  -- aim is in the last quarter turn -- and dnp.dir casts what it is given to a
  -- u32, where a value outside the range is not a wrap but undefined. It was
  -- never noticed because no monster in maps/mobs/doom_monsters.lua uses
  -- `flee` or `strafe`; the C++ loop does the same addition in u32 arithmetic,
  -- which DOES wrap, and the two runtimes then walked different directions for
  -- a quarter of all aims. Found by tools/mobdiff on a graph written to use
  -- the modes Doom's monsters do not.
  local ang = wrap_brad(walk_aim)
  if mode == "flee" then ang = wrap_brad(aim + 2147483648)
  elseif mode == "strafe" then ang = wrap_brad(aim + 1073741824)
  -- WANDER READS A VARIABLE, and until `set_angle` existed nothing ever wrote
  -- one -- `m.wander` was read here and assigned nowhere in the file, so this
  -- mode walked straight at the aim, identical to `charge`, for as long as it
  -- had existed. The variable is named `wander` by convention: a state that
  -- wants to wander sets it with `set_angle{ var = "wander", towards =
  -- "random" }` and the direction is then the author's, not this runtime's.
  elseif mode == "wander" then ang = wrap_brad(m.vars.wander or m.wander or aim)
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
  -- FIRST, before anything reads or writes input: last tick's presses end here.
  release_presses()
  refresh_players()

  -- Owed rocket shoves whose input has come (see damage_area), before the
  -- step that consumes it -- where the owner's client applies it.
  if #owed_shoves > 0 then
    local keep = {}
    for _, o in ipairs(owed_shoves) do
      local at = o.e:valid() and o.e:input_tick() or nil
      if at == nil then
        -- Gone, or nobody plays it any more: nothing to shove.
      elseif at >= o.due then
        M.apply_hit(o.e, o.origin, o.a, o.share, true)
      else
        keep[#keep + 1] = o
      end
    end
    owed_shoves = keep
  end

  -- NOBODY HERE: stop.
  --
  -- A level keeps ticking after its last player leaves, and every level runs
  -- its own mobs. On a server where clients have walked through several maps,
  -- most of the work each tick would be on behalf of empty rooms. Nothing a
  -- mob does is even defined without a target: it chases one, aims at one,
  -- decides whether it can see one.
  -- THE C++ LOOP'S SHARE, and it runs UNCONDITIONALLY -- above the sleep check
  -- below, not after it.
  --
  -- It owns whichever mobs dnp.mob_track took, and that includes owning their
  -- sleep: it has the same "nobody here, stop" rule and the same one-shot flag
  -- for it. Calling it only when a player is present would mean a tracked mob
  -- never gets the stop input when the last one leaves -- it would keep
  -- walking, on an input set eight ticks before the level emptied, because
  -- set_input is sticky.
  --
  -- Two loops rather than one because they own DISJOINT sets, not because the
  -- work is split: a mob is in exactly one of them, so there is no ordering
  -- question between them.
  if dnp.mobs_tick ~= nil then dnp.mobs_tick() end

  if #players == 0 then
    if not asleep then
      asleep = true
      for _, m in pairs(live) do
        -- Not the tracked ones: their stop came from the call above, through
        -- the loop's own set_move and its own memo of what it last sent.
        -- Writing it again from here would leave that memo believing it had
        -- sent something it had not.
        if not m.cpp and m.e:valid() and not m.dead then set_move(m, ZERO, ZERO, m.aim) end
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
        -- THE ANCHOR, for `stuck`. It moves when the body gets a quarter unit
        -- away from it, and `still_since` is the tick that last happened -- so
        -- "has not got anywhere for N ticks" is a subtraction. Rewriting it
        -- every think instead, which is what the first version did, measures a
        -- window exactly one think long and can never be true.
        if m.anchor == nil then
          m.anchor, m.still_since = m.p, tick
        else
          local dx, dy = m.p.x - m.anchor.x, m.p.y - m.anchor.y
          if dx * dx + dy * dy >= STUCK_EPS2 then m.anchor, m.still_since = m.p, tick end
        end
        if target_gone(m.target) then
          local p = m.p
          local player, d2 = nearest_player(p)
          local sight = m.def.sight
          if player ~= nil and (sight == nil or d2 < sight * sight) then
            m.target = player
          else
            m.target = nil
          end
          m.tp = (not target_gone(m.target)) and m.target:pos() or nil
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
-- WHAT A CONNECTED SWING COSTS.
--
-- The engine decides that a swept collider touched something; what that costs
-- is script, which is the division docs/ENGINE.md:255-258 draws and the reason
-- `attack_damage` is not a replicated parameter -- putting damage in the
-- physics table would hand the client the numbers to compute it with.
--
-- It ASKS THE WEAPON first, and returning nil is a real answer: a weapon with
-- no `swing` does no melee damage at all, however the collider came to be live.
-- That is the second of the two gates that collapse the double-fire, and it is
-- deliberately independent of the first -- `attack_enabled` is replicated state
-- that a script could overwrite, and this is not.
--
-- The lookup is installed by weapons.lua rather than hooked, because hooks are
-- a LIST: an `on_attack` in that file would run ALONGSIDE this one and the
-- double damage would come straight back through the other door.
function on_attack(attacker, target)
  if target == nil or not target:valid() then return end

  -- WHO SWUNG. A monster and a player both reach this hook through the same
  -- swept collider, and what a hit costs comes from whichever declaration the
  -- attacker actually carries -- a mob's `swing` field or the weapon in a
  -- player's hands. Asking the weapon table about a monster would find no
  -- inventory and return nil, which read as monsters doing no melee damage at
  -- all, so the mob is checked FIRST: `live` is the definitive answer to "is
  -- this one of ours".
  local mine = attacker ~= nil and attacker.index ~= nil and live[attacker:index()]
  if mine ~= nil then
    local sw = mine.def and mine.def.swing
    -- No declaration, no damage. The second of two gates, exactly as for a
    -- weapon: `attack_enabled` is replicated state a script could overwrite and
    -- this is not.
    if sw == nil then return end
    hurt(target, sw.damage, mine)
    M.apply_hit(target, attacker:pos(), sw)
    return
  end

  -- The weapons may be the GAME's rather than this level's (a module's `game`
  -- scripts, docs/MODULES.md): then the weapon in the player's hands is in
  -- the game VM, and it is asked there.
  local ask = M.weapon_swing_damage
  if ask == nil and dnp.game_has ~= nil and dnp.game_has("weapon_swing") then
    ask = function(e) return dnp.call_game("weapon_swing", e) end
  end
  if ask ~= nil then
    -- The whole `swing` table, not just its damage: knockback and slow are
    -- properties of the blow and belong with the number it costs.
    local sw = ask(attacker)
    if sw == nil then return end
    if type(sw) == "number" then sw = { damage = sw } end
    hurt(target, sw.damage, nil)
    M.apply_hit(target, attacker:pos(), sw)
    return
  end
  hurt(target, M.player_damage or 25, nil)
end

-- WHAT THE C++ LOOP CALLS when a state's sequence reaches an action it does
-- not run itself.
--
-- It names the mob by entity index, the state by name and the entry by its
-- position in that state's own `sequence` -- which is exactly what the graph
-- carried across, so nothing about what the action DOES ever crossed. The
-- action then runs through the same `acts` table it always did, on the same
-- `m` record, so there is one implementation of `spawn` and it is this one.
--
-- `m.p` is refreshed here because the actions read it and the loop's copy
-- lives on the other side. Cheap: this runs when something happens, not every
-- think, which is the whole reason the split is drawn where it is.
function M.run_action(idx, state_name, i)
  local m = live[idx]
  if m == nil or not m.e:valid() then return end
  local st = m.def.states[state_name]
  if st == nil or st.sequence == nil then return end
  local a = st.sequence[i]
  if a == nil then return end

  -- The fields an action reads that the loop owns. Pulled over rather than
  -- mirrored continuously: four values once per action beats four writes per
  -- think, and an action is rare by construction.
  local _, _, target, aim = dnp.mob_state(idx)
  m.aim = aim
  m.target = target ~= nil and dnp.entity(target) or nil
  m.p = m.e:pos()
  m.tp = (m.target ~= nil and m.target:valid()) and m.target:pos() or nil
  m.st = st

  local fn = acts[a.action]
  if fn == nil then return end
  fn(m, a)

  -- And back, for the one field an action may have moved. `face` in an on_hit
  -- does exactly this, and without it the loop would steer by a stale aim on
  -- the very tick the action was trying to correct.
  if m.aim ~= aim then dnp.mob_set(idx, "aim", m.aim) end
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

-- PERSISTENCE (docs/SAVING.md): the monsters still alive, where they are,
-- how hurt, which state they were in and their vars. The dead do not come back
-- at all.
--
-- THE STATE AND THE VARS TOGETHER, or neither. A boss's "only once" phase
-- change is a hidden var plus the states of the phase it led to; a save that
-- kept the var and dropped the state brought the boss back in phase 1 with the
-- flag that would have moved it on already set -- stuck there for good.
--
-- RESUMED, NOT RE-ENTERED, as `@previous` resumes: entering the state again
-- would run its sequence again, and a `set_cooldown` on the way into a phase
-- would then wipe the cooldown the save had just put back. A monster saved
-- mid-flinch comes back in what the flinch interrupted. The target is not
-- saved; the next think finds one.
--
-- TIMES ARE KEPT AS TIME LEFT or time spent. A cooldown's var holds an
-- absolute tick, and the world a save is loaded into may count from somewhere
-- else.
local function cooldown_vars(def)
  local out = {}
  for _, st in pairs(def.states or {}) do
    for _, a in ipairs(st.sequence or {}) do
      if a.action == "set_cooldown" and a.var then out[a.var] = true end
    end
    for _, tr in ipairs(st.transitions or {}) do
      for _, c in ipairs(tr.when or {}) do
        if c.cond == "cooldown_ready" and c.var then out[c.var] = true end
      end
    end
  end
  return out
end

local function var_of(m, k)
  if m.cpp and dnp.mob_var ~= nil then return dnp.mob_var(m.idx, k) end
  return m.vars[k]
end

if dnp ~= nil and dnp.persist ~= nil then
  dnp.persist("mobs", function()
    local now = dnp.tick()
    local idxs = {}
    for idx, m in pairs(live) do
      if (m.hp or 0) > 0 and m.e:valid() then idxs[#idxs + 1] = idx end
    end
    table.sort(idxs)
    local out = {}
    for _, idx in ipairs(idxs) do
      local m = live[idx]
      -- `done` is how many sequence steps have run, which is the C++ loop's
      -- index and one less than mobs.lua's, so a save reads the same from
      -- either. A length of nil is "no `ticks`", which the C++ loop says as -1.
      local state, elapsed, done, state_ticks
      if m.cpp and dnp.mob_resume_point ~= nil then
        state, elapsed, done, state_ticks = dnp.mob_resume_point(m.idx)
      else
        local p = is_reaction(m.st) and (m.prev or {}) or nil
        if p then
          state, elapsed, done, state_ticks = p.name, p.elapsed, (p.seq_i or 1) - 1, p.state_ticks
        else
          state, elapsed, done, state_ticks = m.state, now - (m.entered or now), (m.seq_i or 1) - 1, m.state_ticks
        end
      end
      if state_ticks ~= nil and state_ticks < 0 then state_ticks = nil end
      local vars, left = nil, nil
      local cool = cooldown_vars(m.def)
      for k in pairs(m.def.vars or {}) do
        local v = var_of(m, k)
        if v ~= nil then
          if cool[k] then
            left = left or {}
            left[k] = math.max(0, v - now)
          else
            vars = vars or {}
            vars[k] = v
          end
        end
      end
      out[#out + 1] = { kind = m.kind, at = m.e:pos(), hp = m.hp,
                        dormant = m.dormant or nil, vars = vars, cooldowns = left,
                        state = state and { name = state, elapsed = elapsed,
                                            done = done, ticks = state_ticks } or nil }
    end
    return out
  end, function(list)
    local back = 0
    local now = dnp.tick()
    for _, s in ipairs(list or {}) do
      local e = M.spawn(s.kind, s.at, { dormant = s.dormant })
      local m = e and live[e:index()]
      if m then
        back = back + 1
        m.hp = s.hp
        health_changed(m)
        if m.cpp and dnp.mob_set ~= nil then dnp.mob_set(m.idx, "hp", s.hp) end
        local function put(k, v)
          if m.def.vars == nil or m.def.vars[k] == nil then return end
          m.vars[k] = v
          if m.cpp and dnp.mob_var ~= nil then dnp.mob_var(m.idx, k, v) end
        end
        for k, v in pairs(s.vars or {}) do put(k, v) end
        for k, v in pairs(s.cooldowns or {}) do put(k, now + v) end
        local at = s.state
        if at ~= nil and m.def.states[at.name] ~= nil then
          enter(m, at.name, now)
          local elapsed, done = at.elapsed or 0, at.done or 0
          if m.cpp then
            dnp.mob_set(m.idx, "resume", elapsed, done, at.ticks or -1)
          else
            m.entered, m.seq_i, m.state_ticks, m.prev = now - elapsed, done + 1, at.ticks, nil
          end
        end
      end
    end
    print(("mobs: restored %d of %d"):format(back, #(list or {})))
  end)
end
