--
-- Weapons: a phase machine per player, driven by the weapon vocabulary.
--
-- WHAT THIS REPLACED, AND WHY. Six flat records -- damage, range, count,
-- spread, cooldown, ammo -- with the firing rule written into an `if` at the
-- bottom of on_tick. That expressed Doom exactly and nothing else: no shot that
-- charges while the button is held, no second press that continues a combo, no
-- weapon that shoves you backwards, no alternate fire. Every one of those is
-- ordinary in a platformer, and none of them is a small addition to a flat
-- record.
--
-- It was also the mistake docs/MOBS.md:70-73 records about the framework that
-- preceded the mob runtime -- `cooldown` as one number is a hardcoding of "wind
-- up, fire, recover" for the two weapons that happened to exist -- so the shape
-- here is the mob runtime's, for the reason that argument already won:
-- maps/scripts/weapon_vocab.lua declares the phases, conditions and actions,
-- and this file is checked against it at load in BOTH directions.
--
-- WHAT IS SHARED WITH THE MOB RUNTIME, AND WHAT IS NOT. `hitscan`, `impulse`
-- and `launch` are implemented once, in mobs.lua, and called from here through
-- `mobs.acts` and a synthetic owner record that mobs.lua builds. So a monster's
-- shotgun and a player's are the same arithmetic rather than two that agree
-- today. What is NOT shared is the machine: a mob's graph is driven by what it
-- can see, a weapon's by what button is down, and the conditions have almost
-- nothing in common.
--
-- PER-PLAYER STATE, keyed by AGENT INDEX and forgotten when that index is
-- reused. A slot is handed to whoever arrives next, so an inventory that
-- outlived its owner would arm a new player with the last one's shotgun.
--
-- WHY THIS IS A SEPARATE SCRIPT. The VM harvests hooks as a LIST of handlers,
-- one per script, so this file's on_tick and on_touch run alongside doom.lua's
-- rather than replacing them. That is what lets weapons be added to the DOOM II
-- maps without editing the file that knows about doors.
--
weapons = weapons or {}
local W = weapons

local ZERO = fx.int(0)
local ONE  = fx.int(1)

-- Degrees to brad. A turn is 2^32, which is why this is a multiplication and
-- not a call to anything trigonometric.
local function deg(n) return math.floor(n * 4294967296 / 360) end

-- ---------------------------------------------------------------- complaining
--
-- Loudly, with the file and the name, and then carry on. A weapon table that
-- half loads is better than a server that will not start, and a message naming
-- the weapon is what makes a typo a five-second fix instead of a bisect.
local load_errors = 0
local function complain(fmt, ...)
  load_errors = load_errors + 1
  print("[weapons] " .. string.format(fmt, ...))
end
function W.load_errors() return load_errors end

-- ---------------------------------------------------------------- the vocabulary
--
-- Loaded as a GLOBAL by maps/scripts/weapon_vocab.lua, which every map that
-- uses weapons lists before this file -- exactly as mob_vocab.lua precedes
-- mobs.lua. A missing vocabulary is fatal rather than a shrug: without it
-- nothing below can be validated and every typo becomes silent.
local vocab = weapon_vocab
if vocab == nil then
  error("[weapons] maps/scripts/weapon_vocab.lua must be loaded before this " ..
        "file -- it is the table every definition here is checked against")
end

local function index_by_name(list)
  local out = {}
  for _, entry in ipairs(list or {}) do out[entry.name] = entry end
  return out
end

local COND         = index_by_name(vocab.conditions)
local ACT          = index_by_name(vocab.actions)
local WEAPON_FIELD = index_by_name(vocab.weapon_fields)
local SWING_FIELD  = index_by_name(vocab.swing_fields)
local PHASE_FIELD  = index_by_name(vocab.phase_fields)
local AMMO_FIELD   = index_by_name(vocab.ammo_fields)
local PICKUP_FIELD = index_by_name(vocab.pickup_fields)
local HEALTH_FIELD = index_by_name(vocab.health_fields)

-- ------------------------------------------------------------------ argument checks
--
-- Every argument is checked at LOAD, against the vocabulary, and an unknown one
-- is an error rather than a shrug. A silently ignored `dammage = 20` is a
-- weapon that looks configured and does nothing, which is the failure this
-- whole file is arranged to avoid.

local function is_int(v) return type(v) == "number" and v == math.floor(v) end

-- fx arrives as a string ("0.25"), an integer, or an fx already. A non-integral
-- Lua NUMBER is refused by name: 0.1 is not 0.1 in a double, and quantising it
-- silently is how a weapon ends up a hair shorter on one build than another.
local function as_fx(v, where, what)
  if type(v) == "string" then return fx.parse(v) end
  if is_int(v) then return fx.int(v) end
  if type(v) == "number" then
    complain("%s: `%s` is %s, which is not exact. Write it as a string " ..
             "('%s') or a fraction, so it means the same on every build.",
             where, what, tostring(v), tostring(v))
    return ZERO
  end
  return v
end

-- One value against one spec entry. Used for action ARGUMENTS and for
-- weapon/phase/ammo/pickup FIELDS, because they are the same question -- and
-- having asked it two ways is exactly how mobs.lua once accepted
-- `move = "charge!"`.
local function check_value(spec, v, where, ctx)
  local t = spec.type
  if t == "int" and not is_int(v) then
    complain("%s: `%s` must be a whole number", where, spec.name)
  elseif t == "bool" and type(v) ~= "boolean" then
    complain("%s: `%s` must be true or false", where, spec.name)
  elseif t == "string" and type(v) ~= "string" then
    complain("%s: `%s` must be a string", where, spec.name)
  elseif t == "table" and type(v) ~= "table" then
    complain("%s: `%s` must be a table", where, spec.name)
  elseif t == "enum" then
    local ok = false
    for _, allowed in ipairs(spec.values or {}) do if v == allowed then ok = true end end
    if not ok then
      complain("%s: `%s` is '%s', which is not one of %s",
               where, spec.name, tostring(v), table.concat(spec.values or {}, ", "))
    end
  elseif t == "phase" then
    if ctx.phases == nil or ctx.phases[v] == nil then
      complain("%s: `%s` names phase '%s', which this weapon does not have",
               where, spec.name, tostring(v))
    end
  elseif t == "weapon" then
    ctx.deferred[#ctx.deferred + 1] = { kind = "weapon", name = v, where = where,
                                        field = spec.name }
  elseif t == "ammo" then
    ctx.deferred[#ctx.deferred + 1] = { kind = "ammo", name = v, where = where,
                                        field = spec.name }
  elseif t == "missile" then
    -- Deferred like the rest: the mob runtime's table is filled by
    -- mobs.add_missiles, which W.load calls BEFORE the weapons -- but a module
    -- may also declare a missile in a later call, so the answer is only final
    -- once every load has run. A typo here otherwise fires nothing at all and
    -- reads as a weapon that does not work.
    ctx.deferred[#ctx.deferred + 1] = { kind = "missile", name = v, where = where,
                                        field = spec.name }
  elseif t == "var" then
    if ctx.vars == nil or ctx.vars[v] == nil then
      complain("%s: `%s` names variable '%s', which this weapon does not " ..
               "declare in `vars`", where, spec.name, tostring(v))
    end
  end
end

-- A whole table against a whole field list. `where` is what a complaint says.
local function check_fields(t, spec_by_name, where, ctx, spec_list)
  for k, v in pairs(t) do
    -- `at`, `action`, `when` and `to` are the STRUCTURE of a sequence entry or
    -- a transition rather than arguments of one, so they are skipped by the
    -- callers that pass them; anything else unknown is a typo.
    local spec = spec_by_name[k]
    if spec == nil then
      local known = {}
      for _, s in ipairs(spec_list or {}) do known[#known + 1] = s.name end
      table.sort(known)
      complain("%s: unknown field `%s`. Known: %s", where, tostring(k),
               table.concat(known, ", "))
    else
      if spec.type == "fx" then t[k] = as_fx(v, where, k)
      else check_value(spec, v, where, ctx) end
    end
  end
  for _, spec in ipairs(spec_list or {}) do
    if not spec.optional and t[spec.name] == nil then
      complain("%s: `%s` is required", where, spec.name)
    end
  end
end

-- ------------------------------------------------------------------ the tables
--
-- Everything below is AUTHORED. The four ammunition kinds this engine shipped
-- with were named independently in three places -- the cap table, the UI widget
-- ids and the starting inventory -- so a fifth meant editing code in three, and
-- forgetting one of them was a pool a player could pick up and never see.
W.ammo    = {}      -- name -> { label, max, initial, order }
W.defs    = {}      -- name -> weapon
W.pickups = {}      -- catalogue name -> what walking over it does
W.health  = { max = 100, bonus_max = 200 }

-- Declaration order, which several things need and `pairs` does not give: the
-- panel's read-out list, and the "first phase declared" default.
local ammo_order = {}

local function sorted_ammo()
  local out = {}
  for name, a in pairs(W.ammo) do out[#out + 1] = { name = name, def = a } end
  table.sort(out, function(x, y)
    local ox, oy = x.def.order or 0, y.def.order or 0
    if ox ~= oy then return ox < oy end
    return x.name < y.name             -- a total order, so the panel is stable
  end)
  return out
end

--- Load a weapon set. Called once per module, and additive across modules.
--
-- ADDITIVE rather than exclusive, unlike mobs.load: a module legitimately adds
-- weapons to a game that already has some, and the hook list means two files
-- can both call this. A COLLISION is refused rather than merged silently --
-- two files disagreeing about what "shotgun" means is a bug, and the later one
-- winning quietly is how it survives to production.
function W.load(t)
  t = t or {}

  -- Ammunition first: weapons name pools, so the pools have to exist to be
  -- named. The deferred list below is what lets a weapon name one declared in
  -- the same call rather than only in an earlier one.
  for name, a in pairs(t.ammo or {}) do
    if W.ammo[name] ~= nil then
      complain("ammunition '%s' is already declared; the later one is ignored", name)
    else
      check_fields(a, AMMO_FIELD, "ammo '" .. name .. "'",
                   { deferred = {} }, vocab.ammo_fields)
      W.ammo[name] = a
      ammo_order[#ammo_order + 1] = name
    end
  end

  if t.health then
    check_fields(t.health, HEALTH_FIELD, "health", { deferred = {} },
                 vocab.health_fields)
    W.health = t.health
  end

  -- Missiles, through the mob runtime, because that is what flies them: the
  -- update loop, the impact test and the explosion all live there and a second
  -- copy would be a second set of bugs.
  if t.missiles then
    if mobs == nil or mobs.add_missiles == nil then
      complain("missiles were declared but maps/scripts/mobs.lua is not loaded; " ..
               "it owns the launcher and the flight loop")
    else
      mobs.add_missiles(t.missiles)
    end
  end

  local deferred = {}

  for name, def in pairs(t.weapons or {}) do
    if W.defs[name] ~= nil then
      complain("weapon '%s' is already declared; the later one is ignored", name)
    else
      W.defs[name] = def
      def.name = name
      W.check_weapon(name, def, deferred)
    end
  end

  for name, p in pairs(t.pickups or {}) do
    if W.pickups[name] ~= nil then
      complain("pickup '%s' is already declared; the later one is ignored", name)
    else
      check_fields(p, PICKUP_FIELD, "pickup '" .. name .. "'",
                   { deferred = deferred }, vocab.pickup_fields)
      W.pickups[name] = p
    end
  end

  -- CROSS-FILE NAMES, resolved last. A weapon may name ammunition declared in
  -- this call or a previous one, and a pickup may name a weapon either way, so
  -- these cannot be checked where they are read.
  for _, d in ipairs(deferred) do
    local known
    if d.kind == "weapon" then known = W.defs[d.name] ~= nil
    elseif d.kind == "ammo" then known = W.ammo[d.name] ~= nil
    else
      -- A missile lives in the mob runtime's table, not in one of ours.
      known = mobs ~= nil and mobs.has_missile ~= nil and mobs.has_missile(d.name)
    end
    if type(d.name) ~= "string" or not known then
      complain("%s: `%s` names %s '%s', which nothing declares",
               d.where, d.field, d.kind, tostring(d.name))
    end
  end

  return load_errors
end

-- Check one weapon, its swing, its phases, their sequences and transitions.
--
-- Separate and public so tools/weaponcheck can feed it a table without standing
-- up a server, which is the whole reason the `bad/` directory of refusals can
-- exist as a test.
function W.check_weapon(name, def, deferred)
  deferred = deferred or {}
  local where = "weapon '" .. name .. "'"

  if type(def.phases) ~= "table" then
    complain("%s: `phases` must be a table of named phases", where)
    return
  end

  local ctx = { phases = def.phases, vars = def.vars or {}, deferred = deferred }

  -- `name` is written in by the loader and is not an authored field, so it is
  -- taken out before the unknown-field sweep and put back after. Leaving it in
  -- would mean either a vocabulary entry for something nobody writes, or a
  -- special case in the sweep -- and a special case in the sweep is how a real
  -- typo gets excused later.
  local written = def.name
  def.name = nil
  check_fields(def, WEAPON_FIELD, where, ctx, vocab.weapon_fields)
  def.name = written

  if def.swing then
    check_fields(def.swing, SWING_FIELD, where .. " swing", ctx, vocab.swing_fields)
  end

  -- The resting phase. Declaration order is not a thing `pairs` gives, so an
  -- absent `initial` takes whichever phase `pairs` reaches first -- which is
  -- stable within a run and not across builds. Refused rather than guessed:
  -- a weapon whose resting phase changes between two servers is a divergence
  -- nobody would think to look for.
  if def.initial == nil then
    local n = 0
    for _ in pairs(def.phases) do n = n + 1 end
    if n == 1 then for k in pairs(def.phases) do def.initial = k end
    else
      complain("%s: `initial` is required when there is more than one phase -- " ..
               "declaration order is not something a Lua table has, so the " ..
               "resting phase cannot be guessed", where)
      for k in pairs(def.phases) do def.initial = k break end
    end
  elseif def.phases[def.initial] == nil then
    complain("%s: `initial` names phase '%s', which this weapon does not have",
             where, tostring(def.initial))
  end

  for pname, ph in pairs(def.phases) do
    local pwhere = where .. " phase '" .. pname .. "'"
    if type(ph) ~= "table" then
      complain("%s: must be a table", pwhere)
    else
      local seq, trans, psw = ph.sequence, ph.transitions, ph.swing
      ph.sequence, ph.transitions, ph.swing = nil, nil, nil
      check_fields(ph, PHASE_FIELD, pwhere, ctx, vocab.phase_fields)
      ph.sequence, ph.transitions, ph.swing = seq, trans, psw

      -- A phase's own swing, field by field, exactly as the weapon's. Skipping
      -- it would let `raech` through on a combo finisher and nowhere else,
      -- which is the worst kind of half-checked.
      if psw ~= nil then
        check_fields(psw, SWING_FIELD, pwhere .. " swing", ctx, vocab.swing_fields)
      end

      for i, entry in ipairs(seq or {}) do
        local ewhere = pwhere .. " step " .. i
        if type(entry) ~= "table" or type(entry.action) ~= "string" then
          complain("%s: every step is {at = <ticks>, action = '<name>', ...}", ewhere)
        else
          local spec = ACT[entry.action]
          if spec == nil then
            complain("%s: unknown action '%s'", ewhere, entry.action)
          else
            if entry.at ~= nil and not is_int(entry.at) then
              complain("%s: `at` must be a whole number of ticks", ewhere)
            end
            local args = {}
            for k, v in pairs(entry) do
              if k ~= "at" and k ~= "action" then args[k] = v end
            end
            check_fields(args, index_by_name(spec.args), ewhere, ctx, spec.args)
            -- fx conversion happened inside check_fields, on the COPY, so it is
            -- written back. Missing this made every `range` a string at
            -- runtime, which dnp.hitscan accepted and read as zero.
            for k, v in pairs(args) do entry[k] = v end
          end
        end
      end

      for i, tr in ipairs(trans or {}) do
        local twhere = pwhere .. " transition " .. i
        if type(tr) ~= "table" or (type(tr.when) ~= "string" and
                                   type(tr.when) ~= "table") then
          complain("%s: every transition is {when = '<condition>', to = " ..
                   "'<phase>', ...} or {when = {{cond = '<condition>', ...}, " ..
                   "...}, to = '<phase>'}", twhere)
        else
          if def.phases[tr.to] == nil then
            complain("%s: `to` names phase '%s', which this weapon does not have",
                     twhere, tostring(tr.to))
          end
          -- ONE CONDITION OR SEVERAL, and the several are ANDed.
          --
          -- The single-string form was the whole vocabulary at first, and it
          -- could not say "the button went down AND there is no ammunition" --
          -- so `no_ammo` alone routed an empty weapon to its dry click while it
          -- was merely being HELD, and the auto-switch took it out of the
          -- player's hands the instant they selected it from the panel. A
          -- weapon you cannot choose to carry is not a weapon.
          --
          -- The list form is the mob vocabulary's shape, which has always had
          -- it: `when = { { cond = "...", ... }, ... }`.
          if type(tr.when) == "string" then
            local args = {}
            for k, v in pairs(tr) do
              if k ~= "when" and k ~= "to" then args[k] = v end
            end
            local spec = COND[tr.when]
            if spec == nil then
              complain("%s: unknown condition '%s'", twhere, tr.when)
            else
              check_fields(args, index_by_name(spec.args), twhere, ctx, spec.args)
              for k, v in pairs(args) do tr[k] = v end
            end
          elseif #tr.when == 0 then
            -- Vacuously true, which is `always` written the long way round and
            -- almost certainly a truncated edit.
            complain("%s: `when` is an empty list, which would fire every " ..
                     "tick. Write `when = 'always'` if that is the intent.",
                     twhere)
          else
            for j, one in ipairs(tr.when) do
              local owhere = ("%s condition %d"):format(twhere, j)
              if type(one) ~= "table" or type(one.cond) ~= "string" then
                complain("%s: every entry is {cond = '<condition>', ...}", owhere)
              else
                local spec = COND[one.cond]
                if spec == nil then
                  complain("%s: unknown condition '%s'", owhere, one.cond)
                else
                  local args = {}
                  for k, v in pairs(one) do if k ~= "cond" then args[k] = v end end
                  check_fields(args, index_by_name(spec.args), owhere, ctx, spec.args)
                  for k, v in pairs(args) do one[k] = v end
                end
              end
            end
          end
        end
      end
    end
  end
end

-- ------------------------------------------------------------------ inventory

W.of = {}
local run  = {}         -- agent index -> the phase machine's state
local btn  = {}         -- agent index -> { held, before, down_since = {} }

local function starting_ammo()
  local out = {}
  for name, a in pairs(W.ammo) do out[name] = a.initial or 0 end
  return out
end

-- Everyone starts with whatever is marked `order = 1` and anything the module
-- put in `W.initial_weapons`. Held as a function rather than a constant so a
-- module loaded after this file still affects it.
W.initial_weapons = nil

local function fresh()
  local have = {}
  if W.initial_weapons then
    for _, n in ipairs(W.initial_weapons) do have[n] = true end
  else
    -- The lowest-order weapon, which is the one that always works. Without a
    -- module saying otherwise this is the sensible default and it cannot be
    -- empty: a player holding nothing has no `current` and every branch below
    -- would have to cope with that.
    local best
    for n, d in pairs(W.defs) do
      if best == nil or (d.order or 0) < (W.defs[best].order or 0) then best = n end
    end
    if best then have[best] = true end
  end
  local current
  for n in pairs(have) do
    if current == nil or (W.defs[n].order or 0) > (W.defs[current].order or 0) then
      current = n
    end
  end
  return { have = have, ammo = starting_ammo(), armour = 0, current = current }
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
  run[idx] = nil
  btn[idx] = nil
end

--- Every weapon a player holds, in display order.
function W.list(idx)
  local inv = W.get(idx)
  if inv == nil then return {} end
  local out = {}
  for name, def in pairs(W.defs) do
    if inv.have[name] then out[#out + 1] = { name = name, def = def } end
  end
  table.sort(out, function(a, b)
    local oa, ob = a.def.order or 0, b.def.order or 0
    if oa ~= ob then return oa < ob end
    return a.name < b.name
  end)
  return out
end

--- Switch to a weapon, if it is held. True when the selection changed.
function W.select(idx, name)
  local inv = W.get(idx)
  if inv == nil or not inv.have[name] or inv.current == name then return false end
  inv.current = name
  return true
end

local function has_ammo(inv, def, n)
  if def == nil or def.ammo == nil then return true end
  return (inv.ammo[def.ammo] or 0) >= (n or def.per or 1)
end

-- Fall back to the best weapon that still has ammunition.
--
-- BEST, not first: highest order wins, so running a shotgun dry drops to the
-- pistol rather than to the fists. Doom does this and it is not a nicety -- a
-- player who empties a shotgun while something is chasing them is otherwise
-- holding an inert object, pressing fire and getting nothing, with fifty
-- bullets in their pocket.
local function switch_to_usable(inv)
  local best = nil
  for name, def in pairs(W.defs) do
    if inv.have[name] and has_ammo(inv, def) then
      if best == nil or (def.order or 0) > (W.defs[best].order or 0) then best = name end
    end
  end
  if best then inv.current = best end
  return best
end
W.switch_to_usable = switch_to_usable

-- ------------------------------------------------------------ the swing, replicated
--
-- Selecting a weapon writes its swing SHAPE into the replicated parameter
-- table, so the collider a player's melee sweeps is predicted by the client
-- with no new machinery and no new wire field: those parameters already
-- replicate with an epoch and the client already runs the same controller.
--
-- Through mobs.tagged_effect, under one tag, because add_effect STACKS:
-- switching weapons ten times without a tag leaves ten effects per parameter,
-- each recomputed every tick, and the tenth reach is the product of ten.
local SWING_PARAM = {
  reach = "attack_reach", half_arc = "attack_half_arc",
  sweep_from = "attack_sweep_from", sweep_to = "attack_sweep_to",
  min_reach = "attack_min_reach",
  -- The vertical half of the arc, which makes an overhead chop a different
  -- volume from a horizontal slash rather than the same one drawn differently.
  pitch_from = "attack_pitch_from", pitch_to = "attack_pitch_to",
  half_pitch = "attack_half_pitch",
}
local SWING_U8 = { ticks = "attack_ticks", cooldown = "attack_cooldown" }

local function apply_weapon_params(e, def)
  if mobs == nil or mobs.tagged_effect == nil then return end
  mobs.clear_tagged(e, "weapon")
  local sw = def and def.swing

  -- WHETHER THIS WEAPON SWINGS AT ALL, and it is the line that collapses the
  -- double-fire. Before `attack_enabled` existed, pressing fire with a shotgun
  -- ALSO started the engine's swept collider -- attack_reach of 0 still
  -- connects at the target's own radius and attack_ticks of 0 is re-read as 1
  -- -- so one press ran two independent damage paths.
  mobs.tagged_effect(e, "weapon/enabled", "attack_enabled", "set",
                     sw and ONE or ZERO)
  mobs.tagged_effect(e, "weapon/aim_snap", "aim_snap", "set",
                     fx.int(def and def.aim_snap or 0))

  if sw == nil then return end
  for field, param in pairs(SWING_PARAM) do
    if sw[field] ~= nil then
      mobs.tagged_effect(e, "weapon/" .. field, param, "set", sw[field])
    end
  end
  for field, param in pairs(SWING_U8) do
    if sw[field] ~= nil then
      mobs.tagged_effect(e, "weapon/" .. field, param, "set", fx.int(sw[field]))
    end
  end
end

--- What a connected swing costs, for the mob runtime's on_attack hook.
--
-- Returns NIL when the held weapon has no swing, which is the second half of
-- the double-fire fix: even if `attack_enabled` were somehow 1, no damage
-- lands. Two independent gates, because the parameter is replicated state that
-- a script could overwrite and the damage is not.
function W.swing_damage(attacker)
  if attacker == nil or attacker.index == nil then return nil end
  local inv = W.of[attacker:index()]
  if inv == nil then return nil end
  local def = W.defs[inv.current]
  if def == nil then return nil end
  -- THE PHASE'S OWN SWING FIRST, then the weapon's.
  --
  -- The parameters the collider ran with came from the phase (see phase_swing),
  -- so the damage has to as well -- otherwise a combo escalates in reach and
  -- arc and every hit costs the weapon's one number, which is the bug this
  -- whole feature exists to remove, surviving in the half nobody replicates.
  local st = run[attacker:index()]
  local ph = st and def.phases and def.phases[st.phase]
  local sw = (ph and ph.swing) or def.swing
  if sw == nil then return nil end
  -- THE WHOLE TABLE, not just the number. Knockback and slow are properties of
  -- the blow and belong with what it costs; returning only `damage` meant a
  -- melee weapon could not shove, which is most of what melee is for.
  return sw
end

-- ------------------------------------------------------------------ conditions
--
-- Every one is answerable from what the server already knows about this player
-- this tick. None casts a ray or walks the level, because this runs for every
-- player every tick.

local PRIMARY   = nil          -- resolved on first use: dnp.button is a binding
local SECONDARY = nil
local function button_mask(which)
  if PRIMARY == nil then
    PRIMARY, SECONDARY = dnp.button.ATTACK_PRIMARY, dnp.button.ATTACK_SECONDARY
  end
  return (which == "secondary") and SECONDARY or PRIMARY
end

local conds = {}

function conds.always() return true end

function conds.button_pressed(c, a)
  local m = button_mask(a.button)
  return bit.band(c.held, m) ~= 0 and bit.band(c.before, m) == 0
end

function conds.button_released(c, a)
  local m = button_mask(a.button)
  return bit.band(c.held, m) == 0 and bit.band(c.before, m) ~= 0
end

function conds.button_held(c, a)
  return bit.band(c.held, button_mask(a.button)) ~= 0
end

function conds.held_for(c, a)
  local m = button_mask(a.button)
  if bit.band(c.held, m) == 0 then return false end
  local since = c.state.down_since[m]
  return since ~= nil and (c.now - since) >= a.ticks
end

function conds.has_ammo(c, a) return has_ammo(c.inv, c.def, a.n) end
function conds.no_ammo(c)     return not has_ammo(c.inv, c.def) end

function conds.phase_elapsed(c, a) return c.elapsed >= a.ticks end

function conds.chance(c, a) return dnp.random(0, 99) < a.percent end

function conds.var_at_least(c, a) return (c.state.vars[a.var] or 0) >= a.value end
function conds.var_is(c, a)       return (c.state.vars[a.var] or 0) == a.value end

-- WHAT THE BODY IS DOING, so an attack can differ in the air.
--
-- A down-slash while falling and a stomp on landing are ordinary platformer
-- attacks, and they are transitions rather than a feature: put the airborne
-- branch above the grounded one in `ready` and the same button does two
-- different things. The engine already answers both questions.
function conds.grounded(c, a)
  local g = c.e:grounded() and true or false
  if a.value == nil then return g end
  return g == a.value
end

function conds.moving(c, a)
  local v = c.e:vel()
  local sq = v.x * v.x + v.y * v.y
  local lim = a.speed or fx.frac(1, 2)
  return sq > lim * lim
end

function conds.falling(c, a)
  return c.e:vel().z < (a.speed and -a.speed or fx.zero())
end

function conds.lua(c, a)
  local fn = _G[a.fn]
  if type(fn) ~= "function" then return false end
  return fn(c.e, c.def.name, c.state.phase) and true or false
end

-- ------------------------------------------------------------------- actions

local acts = {}

-- The synthetic owner record the shared mob actions read. Built by mobs.lua,
-- which owns the record's shape, and rebuilt per call because `aim` is the
-- player's facing THIS tick.
local function owner_of(c)
  return mobs.synthetic_owner(c.e, c.e:facing())
end

function acts.hitscan(c, a)
  -- Straight into the mob runtime's own implementation. One arithmetic for a
  -- monster's shotgun and a player's, rather than two that agree today: the
  -- spread roll, the tracer geometry and the "a shot that reached nothing is
  -- still a shot" report are all decisions this file should not be making
  -- again differently.
  mobs.acts.hitscan(owner_of(c), a)
end

function acts.launch(c, a)
  local base = c.e:facing()
  local n = a.count or 1
  for i = 1, n do
    local ang = base
    if a.angle then ang = ang + deg(a.angle) end
    if n > 1 and a.arc then
      ang = ang + deg((i - 1 - (n - 1) / 2) * a.arc)
    end
    mobs.launch_from(c.e, a.missile, ang % 4294967296, a.offset)
  end
end

function acts.consume(c, a)
  local kind = a.ammo or (c.def and c.def.ammo)
  if kind == nil then return end
  local n = a.n or (c.def and c.def.per) or 1
  c.inv.ammo[kind] = math.max(0, (c.inv.ammo[kind] or 0) - n)
end

function acts.impulse(c, a)
  mobs.acts.impulse(owner_of(c), a)
end

-- A CIRCLE OF DAMAGE, centred where the weapon says rather than on the player.
--
-- The mob runtime's own blast, which rockets have used since the cyberdemon:
-- the falloff, the "does not hurt its owner" rule and the knockback all live
-- there. `offset` puts the centre in front of the body -- a ground slam at your
-- feet is 0, a shockwave thrown forward is a few units -- and that is the
-- "chosen point" without a second way of naming one.
function acts.damage_area(c, a)
  local m = owner_of(c)
  if a.offset and a.offset ~= ZERO then
    local p, d = c.e:pos(), dnp.dir(c.e:facing())
    m.impact = vec3.new(p.x + d.x * a.offset, p.y + d.y * a.offset,
                        p.z + (a.height or ZERO))
  end
  mobs.acts.damage_area(m, a)
end

-- Announce what is about to happen, so a client can draw a warning. Rides the
-- existing vfx event; see mobs.telegraph for what `param` carries.
function acts.telegraph(c, a)
  local p, origin = c.e:pos(), nil
  if a.offset and a.offset ~= ZERO then
    local d = dnp.dir(c.e:facing())
    origin = vec3.new(p.x + d.x * a.offset, p.y + d.y * a.offset, p.z)
  else
    origin = p
  end
  mobs.telegraph(c.e, a, origin, c.e:facing())
end

function acts.set_var(c, a) c.state.vars[a.var] = a.value end
function acts.add_var(c, a) c.state.vars[a.var] = (c.state.vars[a.var] or 0) + a.value end

function acts.goto(c, a) c.jump = a.phase end

function acts.set_anim(c, a) c.e:set_animation(a.state) end

function acts.switch_if_empty(c)
  if not has_ammo(c.inv, c.def) then switch_to_usable(c.inv) end
end

function acts.vfx(c, a)
  local p = c.e:pos()
  dnp.vfx(a.effect, p, c.e:facing(), a.length or 0)
end

function acts.lua(c, a)
  local fn = _G[a.fn]
  if type(fn) == "function" then fn(c.e, c.def.name, c.state.phase) end
end

-- ------------------------------------------- THE CONTROL FOR THE VOCABULARY
--
-- A spec that lists an action the runtime does not implement is worse than no
-- spec: an editor offers it, an author uses it, and it silently does nothing.
-- So the runtime asserts at load that the two agree in BOTH directions, which
-- is what mobs.lua and triggers.lua do and what found two mismatches the first
-- time each of them ran.
function W.check_vocabulary()
  -- RE-INDEXED on every call rather than using the tables captured at load, so
  -- a caller that changed the vocabulary is actually compared against the
  -- changed one. Captured tables would make the control below pass vacuously.
  local COND = index_by_name(vocab.conditions)
  local ACT  = index_by_name(vocab.actions)
  local before = load_errors
  for name in pairs(COND) do
    if conds[name] == nil then
      complain("vocabulary lists condition '%s' and the runtime does not " ..
               "implement it", name)
    end
  end
  for name in pairs(conds) do
    if COND[name] == nil then
      complain("runtime implements condition '%s' and the vocabulary does not " ..
               "list it, so no editor will ever offer it", name)
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
  return load_errors - before
end
-- Run at LOAD, and public so a test can run it again after mutating either
-- side. A check that can only ever pass cannot be shown to work: the control
-- for this one removes a vocabulary entry and requires a complaint, which is
-- the only way to know the comparison is looking at anything.
W.check_vocabulary()

W.conds, W.acts = conds, acts

-- ------------------------------------------------------------- the phase machine

-- How many phase changes one tick may make. A weapon whose `ready` transitions
-- to itself on `always` would otherwise spin forever inside one tick and take
-- the whole server with it -- and that is an easy table to write by accident.
-- Four is enough for ready -> fire -> recover -> ready, which is the longest
-- legitimate chain, plus one.
local MAX_HOPS = 5

local function enter(state, phase, now)
  state.phase   = phase
  state.entered = now
  state.done    = {}
end

-- A PHASE MAY CARRY ITS OWN SWING, which is what makes a combo a combo.
--
-- Three presses reaching `hit1`, `hit2`, `hit3` were already the shape the
-- phase graph gave -- and all three swung the identical volume for the
-- identical damage, because `swing` was a property of the WEAPON and there was
-- one of it. A finisher that hits harder and sweeps wider is the whole reason
-- anyone writes a combo.
--
-- Written on ENTRY, through the same tagged effects the weapon's own swing
-- uses, so the last phase to speak wins and putting the weapon away still
-- clears everything. A phase with no `swing` leaves the weapon's in place,
-- which is what every phase did before this line existed.
local function phase_swing(e, def, ph)
  if ph == nil or ph.swing == nil then return end
  apply_weapon_params(e, { swing = ph.swing, aim_snap = def.aim_snap })
end

--- Reset a player's weapon machine. For a weapon change, and for a fresh slot.
local function rearm(e, idx, inv, now)
  local st = run[idx]
  local def = W.defs[inv.current]
  if st == nil then
    st = { vars = {}, down_since = {} }
    run[idx] = st
  end
  st.weapon = inv.current
  st.vars = {}
  for k, v in pairs((def and def.vars) or {}) do st.vars[k] = v end
  enter(st, def and def.initial, now)
  apply_weapon_params(e, def)
end

local function run_sequence(c, ph)
  for i, entry in ipairs(ph.sequence or {}) do
    if not c.state.done[i] and c.elapsed >= (entry.at or 0) then
      c.state.done[i] = true
      local fn = acts[entry.action]
      if fn then fn(c, entry) end
      -- `goto` abandons the rest of the sequence, which is the point: it is how
      -- a shot that finds no ammunition stops before it draws a tracer.
      if c.jump then return end
    end
  end
end

-- Does one transition fire? `when` is a condition NAME, whose arguments live on
-- the transition itself, or a LIST of {cond = name, ...args} ALL of which must
-- hold. The list is what lets a transition say "the button went down and there
-- is no ammunition", which one condition cannot.
local function transition_fires(c, tr)
  if type(tr.when) == "string" then
    local fn = conds[tr.when]
    return fn ~= nil and fn(c, tr) and true or false
  end
  -- An empty list would be vacuously true, which is `always` written the long
  -- way round and almost certainly a mistake; it is refused at load.
  for _, one in ipairs(tr.when or {}) do
    local fn = conds[one.cond]
    if fn == nil or not fn(c, one) then return false end
  end
  return true
end

local function first_transition(c, ph)
  for _, tr in ipairs(ph.transitions or {}) do
    if transition_fires(c, tr) then return tr.to end
  end
  return nil
end

local function tick_player(e, idx, now)
  local inv = W.get(idx)
  if inv == nil or inv.current == nil then return end

  local state = run[idx]
  if state == nil or state.weapon ~= inv.current then
    rearm(e, idx, inv, now)
    state = run[idx]
  end

  local def = W.defs[inv.current]
  if def == nil then return end

  -- Button edges, and how long each has been down. Kept here rather than read
  -- from the engine because the ENGINE has no notion of how long a button has
  -- been held -- it carries this tick's mask and nothing else -- and a charge
  -- is exactly that number.
  local b = btn[idx]
  if b == nil then b = { held = 0, before = 0, down_since = {} }; btn[idx] = b end
  b.before = b.held
  b.held = e:buttons() or 0
  state.down_since = state.down_since or {}
  for _, which in ipairs({ "primary", "secondary" }) do
    local m = button_mask(which)
    if bit.band(b.held, m) ~= 0 then
      if state.down_since[m] == nil then state.down_since[m] = now end
    else
      state.down_since[m] = nil
    end
  end

  local c = {
    e = e, idx = idx, inv = inv, def = def, state = state, now = now,
    held = b.held, before = b.before, elapsed = 0, jump = nil,
  }

  local hops = 0
  while true do
    local ph = def.phases[state.phase]
    if ph == nil then enter(state, def.initial, now); ph = def.phases[state.phase] end
    if ph == nil then return end

    c.elapsed = now - state.entered
    c.jump = nil
    run_sequence(c, ph)

    local to = c.jump or first_transition(c, ph)
    if to == nil and ph.ticks and c.elapsed >= ph.ticks then
      to = ph.next or def.initial
    end
    if to == nil then break end

    hops = hops + 1
    if hops > MAX_HOPS then
      complain("weapon '%s' changed phase %d times in one tick around '%s' -- " ..
               "a transition loop. Stopping so the tick can finish.",
               def.name, hops, tostring(state.phase))
      break
    end
    enter(state, to, now)
    phase_swing(e, def, def.phases[to])
  end
end

--- Drive one player's weapon for one tick. Public so a test can step it.
function W.tick_player(e, idx, now) return tick_player(e, idx, now) end

--- Drop the phase machine, keeping the inventory.
--
-- The next tick rearms from scratch: the resting phase, fresh `vars`, and the
-- swing parameters written again. For a RESPAWN -- a player who died halfway
-- through a charge should not come back still holding it -- and for a test that
-- wants two runs of the same weapon to start from the same place.
--
-- Distinct from W.forget, which drops the inventory too because the agent slot
-- is being handed to somebody else.
function W.rearm_player(idx)
  run[idx] = nil
  btn[idx] = nil
end

--- Which phase a player's weapon is in. For tests and for a debug panel.
function W.phase_of(idx)
  local st = run[idx]
  return st and st.phase or nil
end

--- A player's per-weapon variables. For tests.
function W.vars_of(idx)
  local st = run[idx]
  return st and st.vars or nil
end

-- ------------------------------------------------------------- picking things up

local function give_ammo(inv, what)
  local took = false
  for kind, n in pairs(what or {}) do
    local pool = W.ammo[kind]
    local max = pool and pool.max or 0
    local before = inv.ammo[kind] or 0
    if before < max then
      inv.ammo[kind] = math.min(max, before + n)
      took = true
    end
  end
  return took
end

-- Public, and the global hook below is a one-line delegate to it.
--
-- The VM HARVESTS hooks: `function on_touch` is collected into a per-hook list
-- and then cleared from _G, so after loading there is no global to call. That
-- is right for the server and wrong for anything else that legitimately wants
-- to run a pickup -- a test, a trigger that hands a player an item, the editor's
-- preview -- so the implementation has a name of its own.
function W.pickup(entity, prop)
  -- PLAYERS ONLY. Every other agent carries the catalogue id of what it was
  -- spawned as, and a level of monsters walking over the medikits would clear a
  -- Doom map of pickups in seconds -- which is exactly what happened to the
  -- keys before doom.lua grew the same guard.
  if entity:kind() ~= 0 then return end
  local idx = entity:index()
  if idx == nil then return end

  local name = prop:name()
  if name == nil then return end
  local p = W.pickups[name]
  if p == nil then return end

  local inv = W.get(idx)
  local took = false

  if p.not_yet then
    print(("[weapons] the %s is not implemented yet; leaving it on the floor")
          :format(p.not_yet))
  end

  if p.heal then
    local hp = entity:hp() or 0
    -- Which ceiling is NAMED by the pickup. It used to be a comparison against
    -- the pickup's own catalogue name buried in this function, which meant the
    -- fact that a health bonus trickles past 100 lived in the runtime instead
    -- of in the data that every other property of the pickup lives in.
    local ceiling = (p.ceiling == "bonus_max")
        and (W.health.bonus_max or W.health.max) or W.health.max
    if hp < ceiling then
      entity:set_hp(math.min(ceiling, hp + p.heal))
      took = true
    end
  end

  if p.armour and inv.armour < p.armour then
    inv.armour = p.armour
    took = true
  end

  if p.weapon then
    -- The weapon is taken once; its ammunition is taken every time -- which is
    -- why a second shotgun on a map is still worth walking over, and why `took`
    -- is an OR of both.
    if not inv.have[p.weapon] then
      inv.have[p.weapon] = true
      inv.current = p.weapon
      took = true
    end
    local def = W.defs[p.weapon]
    if def and give_ammo(inv, def.gives) then took = true end
  end

  if give_ammo(inv, p.ammo) then took = true end

  -- REFUSED PICKUPS STAY ON THE FLOOR. Walking over a medikit at full health
  -- must not consume it, or a player who is topped up destroys the health they
  -- will need in a minute. Doom does the same.
  if took then prop:despawn() end
end

function on_touch(entity, prop) return W.pickup(entity, prop) end

-- ------------------------------------------------------------- the weapon panel
--
-- A `select` listing what you are carrying and one read-out per ammunition
-- pool. The declarative UI protocol (docs/protocol.md §11) is the only way a
-- script can put anything on screen, and it is enough for this.
--
-- THE READ-OUT LIST IS GENERATED from the ammunition table's `order`, which is
-- the thing that stops a newly authored pool being invisible: it used to be a
-- literal list of four names in this file, so a fifth kind could be picked up,
-- counted and capped, and never appear.
--
-- REBUILT ONLY WHEN THE SET CHANGES. A panel is built from state at the moment
-- it opens and nothing pushes changes into one, so a weapon picked up has to
-- reopen it -- but ammunition changes several times a second and reopening for
-- that would flicker the window on every shot. dnp.ui_set changes one widget in
-- place, which is what the read-outs use.
--
-- WHY SLIDERS FOR AMMUNITION. A label's text is fixed when the panel opens, so
-- it cannot count down; a slider carries a value ui_set can change. Its events
-- are ignored below -- dragging one is a request to have more bullets, and the
-- server is not obliged to grant it.
local W_SELECT = 1
local AMMO_WIDGET_BASE = 10

local panel_of = {}
local shown = {}

local function ammo_widget_id(i) return AMMO_WIDGET_BASE + i end

-- A SELECT'S VALUE IS A ZERO-BASED INDEX INTO ITS OPTIONS.
--
-- The protocol says so (docs/protocol.md 11), the client numbers its `<option>`
-- elements from 0 (client/src/render/ui.js), and the server DROPS an index
-- outside [0, count-1] rather than clamping it (main.cpp:912). Lua counts from
-- one, and this panel sent Lua's number: so choosing the first weapon in the
-- list did nothing at all, choosing the second gave you the first, and the
-- highlight on a freshly opened panel was off by one or dropped outright.
--
-- It reads like "no weapon" occupying a slot in the list, which is exactly what
-- it is -- index 0, which nothing here ever named. maps/scripts/debug_panel.lua
-- had it right all along.
local function to_option(i) return i - 1 end          -- Lua index -> wire index
local function from_option(v) return v + 1 end        -- wire index -> Lua index

local function build_panel(idx)
  local inv = W.get(idx)
  local list = W.list(idx)
  local options, value = {}, 0
  for i, w in ipairs(list) do
    options[i] = w.def.label
    if w.name == inv.current then value = to_option(i) end
  end
  if #options == 0 then options[1] = "(none)" end

  local widgets = {
    { kind = "label",  id = 0, label = "Weapon" },
    { kind = "select", id = W_SELECT, label = "Held", value = value,
      lo = 0, hi = #options - 1, options = options },
    { kind = "label",  id = 0, label = "Ammunition" }
  }
  for i, a in ipairs(sorted_ammo()) do
    widgets[#widgets + 1] = {
      kind = "slider", id = ammo_widget_id(i), label = a.def.label or a.name,
      value = inv.ammo[a.name] or 0, lo = 0, hi = a.def.max or 0
    }
  end
  return { title = "Weapons", widgets = widgets }
end

--- The panel a player would be shown, without opening one.
---
--- For a test, and for a debug view. The read-out list is GENERATED from the
--- ammunition table's `order`, and that is the thing worth being able to check:
--- it used to be a literal list of four names in this file, so a fifth pool
--- could be declared, picked up, counted and capped, and never appear on screen.
function W.panel_for(idx) return build_panel(idx) end

local function signature(idx)
  local names = {}
  for _, w in ipairs(W.list(idx)) do names[#names + 1] = w.name end
  return table.concat(names, ",")
end

local function open_panel(e, idx)
  -- CLOSE THE OLD ONE FIRST. Every open allocates a fresh ui_id, so reopening
  -- does not replace: the client keys panels by id, sees a new one, and draws
  -- it beside the old.
  if panel_of[idx] then dnp.ui_close(e, panel_of[idx]) end
  panel_of[idx] = nil
  local id = dnp.ui_open(e, build_panel(idx))
  if id ~= 0 then
    panel_of[idx] = id
    shown[idx] = { list = signature(idx), ammo = {} }
  end
end

local function refresh_panel(e, idx)
  local inv = W.get(idx)
  if panel_of[idx] == nil or shown[idx] == nil then open_panel(e, idx); return end
  if signature(idx) ~= shown[idx].list then open_panel(e, idx); return end

  local list = W.list(idx)
  for i, w in ipairs(list) do
    if w.name == inv.current and shown[idx].sel ~= to_option(i) then
      shown[idx].sel = to_option(i)
      dnp.ui_set(e, panel_of[idx], W_SELECT, to_option(i))
    end
  end

  -- Only where it moved. This runs sixty times a second and a reliable control
  -- message per counter per tick is a waste of the channel that carries map
  -- data.
  for i, a in ipairs(sorted_ammo()) do
    local n = inv.ammo[a.name] or 0
    if shown[idx].ammo[a.name] ~= n then
      shown[idx].ammo[a.name] = n
      dnp.ui_set(e, panel_of[idx], ammo_widget_id(i), n)
    end
  end
end

--- What the player did with the panel.
---
--- Public, with the global hook below a one-line delegate, for the same reason
--- W.pickup is: the VM HARVESTS hooks out of _G after loading, so there is no
--- global left for a test -- or a trigger, or the editor's preview -- to call.
function W.ui_event(e, ui_id, widget, value)
  local idx = e:index()
  if idx == nil or panel_of[idx] ~= ui_id then return end
  if widget ~= W_SELECT then
    -- An ammunition read-out. Dragging one is a request for more bullets and
    -- the answer is no -- but the widget has already moved on the client, so it
    -- is put back rather than left showing a number nobody has.
    local inv = W.get(idx)
    for i, a in ipairs(sorted_ammo()) do
      if ammo_widget_id(i) == widget then
        dnp.ui_set(e, ui_id, widget, inv.ammo[a.name] or 0)
      end
    end
    return
  end

  local list = W.list(idx)
  local chosen = list[from_option(value)]
  if chosen then
    W.select(idx, chosen.name)
    if shown[idx] then shown[idx].sel = value end
  end
end

function on_ui_event(e, ui_id, widget, value)
  return W.ui_event(e, ui_id, widget, value)
end

--- Choose a weapon by its WIRE index, as the client's message does.
---
--- For a test. The index convention is the thing worth checking and it only
--- shows up when an event comes back in, so this routes through the same code
--- a real message does rather than reaching for W.select.
function W.choose(e, option)
  return W.ui_event(e, panel_of[e:index()], W_SELECT, option)
end

function on_tick()
  -- NO ARGUMENT. Vm::call_hook takes a tick but uses it only for error
  -- accounting -- it pushes the handler with zero arguments -- so a hook
  -- declared as on_tick(tick) gets nil and every comparison against it raises.
  local now = dnp.tick()

  local here = {}
  for _, e in ipairs(dnp.players()) do here[e:index()] = true end
  for idx in pairs(W.of) do
    if not here[idx] then W.forget(idx); panel_of[idx] = nil; shown[idx] = nil end
  end

  for _, e in ipairs(dnp.players()) do
    local idx = e:index()
    if idx ~= nil then
      refresh_panel(e, idx)
      tick_player(e, idx, now)
    end
  end
end

-- The mob runtime asks what a connected swing costs. Installed here rather than
-- as an `on_attack` hook of our own, because hooks are a LIST: both handlers
-- would run and the double damage this phase exists to collapse would return
-- through the other door.
if mobs ~= nil then mobs.weapon_swing_damage = W.swing_damage end
