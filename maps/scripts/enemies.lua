--
-- Doom's monsters, as script.
--
-- The engine knows nothing about any of this. It moves agents through the
-- controller a player uses, answers "what is along this ray", and draws what
-- assets/props.txt describes; whether a thing chases you, when it shoots, and
-- what it looks like while doing so is all here. That division is the same one
-- doors and switches follow, and for the same reason: a client predicts the
-- engine and never runs a line of this file.
--
-- NO DAMAGE YET, deliberately. They shoot and they bite and nothing is hurt --
-- health, armour and dying are a slice of their own, and building them into the
-- first version of the AI would mean unpicking them from it later.
--
-- Two kinds of attack, because they need different machinery:
--
--   HITSCAN (zombieman)  the shot arrives instantly. dnp.hitscan answers what
--                        it hit and where it stopped, and a tracer is drawn
--                        from muzzle to impact -- which is why the impact point
--                        matters and not just the target.
--
--   PROJECTILE (imp)     a fireball is a PROP that travels. It needs a position
--                        every tick and something to stop it, so it is moved
--                        here and asked about the world each tick.
--

enemies = enemies or {}
local M = enemies

local SCALE = 48
local function du(n) return fx.frac(n, SCALE) end

local ZERO = fx.int(0)

-- Animation states, matching assets/props.txt. Named so a change to the
-- catalogue's numbering is one edit here rather than a hunt for magic numbers.
local IDLE, WALK, ATTACK, PAIN, DIE = 0, 1, 2, 3, 4

-- What one player swing takes off, and what a monster's attacks take off.
--
-- Numbers here rather than parameters on the attacker, because the engine's
-- attack carries no damage: MoveParams shapes the COLLIDER -- its reach, its
-- arc, how long it sweeps -- and what a hit MEANS is never the engine's opinion.
--
-- On M rather than as locals, so another script can change them. The debug
-- panel does exactly that, and when these were locals it set a field nothing
-- ever read: the panel moved a slider and the number it was pointing at was not
-- the number being used.
M.melee_damage = M.melee_damage or 20   -- a player swing
M.shot_damage  = M.shot_damage  or 9    -- a zombieman's bullet, Doom's average
M.claw_damage  = M.claw_damage  or 13   -- an imp's claw, and its fireball

-- How many corpses to leave lying about.
--
-- They STAY -- a level you have fought through should look like it -- but not
-- without a bound: a corpse is still an agent, agent slots are finite, and a
-- long session on a busy map would otherwise fill them and quietly stop new
-- monsters spawning. The oldest goes when the cap is reached, which is the
-- least surprising one to lose.
--
-- The death animation holds its last frame (`once` in the catalogue) rather
-- than looping, so what is left on the floor is a body and not a monster dying
-- over and over.
local MAX_CORPSES = 32

-- How long the flinch lasts. Doom's monsters interrupt what they were doing
-- when hit, which is most of what makes a fight readable -- without it, a
-- monster shrugs off a hit mid-stride and you cannot tell whether you connected.
local PAIN_TICKS = 8

-- What each kind does. Everything about a monster's behaviour is one entry.
local KINDS = {
  zombieman = {
    -- Doom's own. The engine has no idea what health is: it reports that a
    -- swing reached something and stops there, exactly as it does for crushing
    -- and for interaction. What that costs is this file's business.
    hp = 20,
    -- How FAST it walks is not here: it is `speed` on the catalogue's prop
    -- line, applied as the entity's move_speed at spawn. That keeps one
    -- question in one file -- how big a thing is and how fast it moves are both
    -- facts about the thing -- and it makes the speed a replicated parameter
    -- rather than something this script multiplies an input by.
    sight = du(1024),
    attack = "hitscan",
    range = du(2048),
    cooldown = 90,              -- ticks between shots
    windup = 8,                 -- ticks of attack animation before it fires
    tracer = 1,
  },
  imp = {
    hp = 60,
    sight = du(1024),
    attack = "projectile",
    melee = du(64),
    range = du(2048),
    cooldown = 100,
    windup = 10,
    projectile = "fireball",
    speed_shot = du(10 * 35),   -- Doom's fireball: 10 map units per tic
  },
}

-- Live monsters: entity index -> record.
--
-- `live` is for identity -- "is this entity one of mine" -- and is never walked
-- per tick. Iteration goes through `buckets` instead.
local live = {}

-- How often a monster reconsiders, in ticks.
--
-- Doom's own monsters think far more slowly than its frame rate, and so should
-- these: an input written with set_input PERSISTS until it is changed, so a
-- monster keeps walking between decisions without anything being called for it.
-- Staggered by index so twenty-nine of them do not all think on the same tick
-- and turn a smooth frame into a spike every fourth one.
local THINK = 4

-- The same records, split THINK ways by spawn order, so each tick walks only
-- the share of them that is actually going to decide anything. A monster's
-- input persists between decisions, so the ones not in this tick's bucket carry
-- on doing what they were last told at no cost whatsoever.
-- Pre-created, all THINK of them, and never created lazily.
--
-- They were made on demand in spawn(), which leaves the buckets no monster
-- landed in as nil -- and on a map with fewer monsters than THINK that is most
-- of them, so on_tick raised "attempt to get length of local 'bucket'" on two
-- ticks in four. It had been doing so for a while: the tests that were watching
-- only needed SOME monster to act, and half the ticks working looks a great
-- deal like all of them.
local buckets = {}

-- Monsters mid-windup. Short, usually empty, and separate because finding them
-- by scanning every monster is the thing this file is trying to stop doing.
local firing = {}
-- Fireballs in flight: prop index -> { p, dir, speed, born }
local shots = {}

-- The players, rebuilt ONCE per tick and shared by every monster.
--
-- This used to be a dnp.entities() call inside each monster's own update, which
-- allocated a table of handles per monster per tick: twenty-nine monsters on
-- MAP01, thirty entities each, sixty times a second, is fifty thousand userdata
-- allocations a second. It cost sixteen milliseconds of a sixteen-millisecond
-- frame with nobody even connected, and the collector chasing it is what took
-- the server down.
--
-- The list is small and the monsters all want the same one. Building it once is
-- not an optimisation, it is the difference between a level that runs and a
-- level that does not.
local players = {}
local players_tick = -1
-- Where each player first appeared, which is where they go back to on death.
-- Declared here rather than beside hurt_player because refresh_players fills
-- it, and a local used above its declaration is a global -- which in this
-- sandbox is nil, and indexing nil is an error on the first shot fired.
local spawn_of = {}
-- Whether the monsters have already been put down for the night. A flag rather
-- than doing it every tick: the point of the check is to stop doing work.
local asleep = false
-- How many have been placed, for round-robin bucketing.
local spawned = 0
for b = 0, THINK - 1 do buckets[b] = {} end

local function refresh_players(tick)
  if players_tick == tick then return end
  players_tick = tick
  for i = #players, 1, -1 do players[i] = nil end

  -- The cheap question first. dnp.player_count() allocates nothing, and the
  -- answer on a server hosting several maps is almost always zero: a level
  -- keeps ticking after the last player leaves it, and there is no reason for
  -- an empty room to be building a table of handles sixty times a second. That
  -- is not a micro-optimisation -- it was 39 ms in a single tick, on levels
  -- nobody was in.
  if dnp.player_count() == 0 then return end

  -- And when somebody IS here, ask for the players rather than for everything.
  -- dnp.entities() built a handle for all thirty-eight monsters on MAP02 to
  -- find the one person among them.
  for _, e in ipairs(dnp.players()) do
    local p = e:pos()
    players[#players + 1] = { e = e, pos = p }
    -- Where this player first appeared, which is where they go back to. Taken
    -- from the first sighting rather than from a spawn tag, because a script
    -- that hardcoded a tag would be wrong on every map that named it something
    -- else.
    if spawn_of[e:index()] == nil then spawn_of[e:index()] = p end
  end
end

-- Returns the entity, the squared distance, AND the position already read for
-- it this tick. The caller wanted all three and used to ask the engine for the
-- third again -- player:pos() allocates a vec3 userdata, and it was doing so
-- once per monster per decision for a value refresh_players had just read.
-- Set the animation only if it is CHANGING.
--
-- Every one of these is a call into the engine and a replicated field. A
-- monster standing in a corridor nobody has walked down was being told it was
-- idle, and pointed where it was already pointing, four times a second forever.
local function set_state(m, st)
  if m.state ~= st then
    m.state = st
    m.e:set_animation(st)
  end
end

-- And the same for input. `aim` may be nil for a monster that has never seen
-- anybody, which means "keep facing wherever you are facing" -- previously
-- expressed as a facing() call into the engine to read a value only to hand it
-- straight back.
local function set_move(m, mx, my, aim)
  if m.mx == mx and m.my == my and m.aim_sent == aim then return end
  m.mx, m.my, m.aim_sent = mx, my, aim
  m.e:set_input(mx, my, aim or m.e:facing() or 0)
end

local function nearest_player(from)
  local best, bestd, bestp = nil, nil, nil
  for i = 1, #players do
    local p = players[i].pos
    if p then
      local dx, dy = p.x - from.x, p.y - from.y
      local d = dx * dx + dy * dy
      if bestd == nil or d < bestd then best, bestd, bestp = players[i].e, d, p end
    end
  end
  return best, bestd, bestp
end


-- Aim, as the engine's own angle unit. A turn is 2^32 and the engine turns at a
-- rate, so writing the aim is asking to face something rather than facing it.
local function aim_at(from, to)
  return dnp.atan2(to.y - from.y, to.x - from.x)
end

-- Place a list of monsters, as the converter emits them.
--
-- Capped at what the world has room for. MAX_AGENTS is 64 and includes players,
-- so a Doom level with ninety monsters in it does not fit -- and dropping the
-- overflow silently would make a level quietly easier than the one it was
-- converted from.
function M.place(list)
  local placed, dropped = 0, 0
  for _, e in ipairs(list) do
    if M.spawn(e.name, e.x, e.y, e.z) then placed = placed + 1 else dropped = dropped + 1 end
  end
  print(("[enemies] %d placed%s"):format(
    placed, dropped > 0 and (", " .. dropped .. " did not fit (MAX_AGENTS)") or ""))
end

-- `opts.dormant` places a monster that never chases and never fires.
--
-- Not a gameplay feature -- a TEST one. A map that wants a monster present as a
-- replicated entity, without it also being a moving obstacle, has no other way
-- to say so: monsters chase, so "put it somewhere the test does not walk" stops
-- being true the moment the test starts walking anywhere. doors.map wanted
-- exactly that and its comment claiming the monsters were out of the way was
-- quietly false.
function M.spawn(name, x, y, z, opts)
  local def = KINDS[name]
  if def == nil then
    print("[enemies] no behaviour for '" .. tostring(name) .. "'")
    return nil
  end
  local e = dnp.spawn_enemy(name, vec3.new(du(x), du(y), du(z)))
  if e == nil then return nil end
  -- `ready_at` and `fire_at` are absolute ticks rather than counters, so a
  -- monster nobody is near costs nothing between its own decisions.
  local m = {
    e = e, def = def, idx = e:index(),
    ready_at = 0, fire_at = nil, aim = nil, state = IDLE,
    hp = def.hp or 20, dead_at = nil, pain_until = 0,
    dormant = opts ~= nil and opts.dormant or false
  }
  live[m.idx] = m
  -- Round-robin across the think buckets by spawn order, so a level's monsters
  -- are spread evenly whatever their entity indices turn out to be.
  local b = spawned % THINK
  spawned = spawned + 1
  local list = buckets[b]
  list[#list + 1] = m
  e:set_animation(IDLE)
  return e
end

-- Hurt a player, and put them back on their feet if that finished them.
--
-- The engine has no notion of health, so nothing here is engine behaviour: a
-- monster's shot is a raycast the engine answered, and what it COSTS is this
-- file's decision, exactly as a player's swing killing a monster is.
--
-- Death is a respawn rather than anything more elaborate. A player left at zero
-- with nothing happening is worse than either a death screen or a respawn, and
-- of the three this is the one that does not need a whole feature behind it.
local function hurt_player(e, amount)
  if e == nil or not e:valid() then return end
  local hp = (e:hp() or 100) - amount
  if hp > 0 then
    e:set_hp(hp)
    return
  end
  e:set_hp(100)
  local home = spawn_of[e:index()]
  if home then e:teleport(home) end
end

local function fire_hitscan(m, from, aim)
  local target, at = dnp.hitscan(m.e, m.def.range)
  if at == nil then return nil end
  -- The tracer carries its START, its direction and its LENGTH -- not just the
  -- impact. A client given only the far end has no idea where the shot came
  -- from, and a client given only a direction draws a line to infinity through
  -- whatever it hit.
  --
  -- Length in 1/256 units, which is a hundredth of a Doom unit at this scale
  -- and comfortably inside the u32 the event carries.
  --
  -- The muzzle height is `at.z` -- the height the ENGINE cast the ray at --
  -- rather than a guess like "shoulders, about 32 units up". They were two
  -- different numbers, so the line on screen was near the shot without being
  -- the shot, and a tracer that is nearly right is worse than none: it is the
  -- thing you check the hit against when the hit looks wrong.
  --
  -- And the DIRECTION is the body's facing, not `aim`. dnp.hitscan casts along
  -- facing, and since turning became a rate rather than a snap those are two
  -- different angles for as long as the monster is still coming round. Drawing
  -- the intent instead of the shot puts the line beside the thing it hit.
  local muzzle = vec3.new(from.x, from.y, at.z)
  local dx, dy = at.x - muzzle.x, at.y - muzzle.y
  local len = dnp.length(vec3.new(dx, dy, ZERO))
  dnp.vfx(m.def.tracer, muzzle, m.e:facing(), (len * fx.int(256)).int)

  -- The shot ARRIVES. dnp.hitscan already answered what was in the way, walls
  -- included, so a target here is one the bullet actually reached.
  if target ~= nil and target:valid() and target:kind() == 0 then
    hurt_player(target, M.shot_damage)
  end
  return target
end

local function fire_projectile(m, from, aim)
  local dir = dnp.dir(aim)
  -- Launched clear of the shooter's own body, or it would be touching its owner
  -- on the tick it appears.
  local off = du(40)
  local p = dnp.spawn_prop(m.def.projectile,
    vec3.new(from.x + dir.x * off, from.y + dir.y * off, from.z + du(32)))
  if p == nil then return end
  shots[p:index()] = { p = p, dir = dir, speed = m.def.speed_shot, born = dnp.tick() }
end

-- Projectiles in flight, and the bursts they leave behind.
--
-- Lifted out of on_tick so that the empty-level short circuit above can
-- still run it. A fireball with nobody left to hit still has to expire --
-- skipping it would leak a prop slot per shot, which is a slower and much
-- more confusing failure than a stall.
-- Corpses whose death animation has finished.
--
-- Its own short list, like the mid-windup monsters: the alternative is a
-- deadline on every monster checked every tick, which is the shape this file
-- spent a commit getting rid of.
local corpses = {}

-- Corpses are not on a timer any more, so this only enforces the cap and
-- forgets ones that have gone for other reasons -- a map change, a despawn from
-- somewhere else.
local function update_corpses(_tick)
  for i = #corpses, 1, -1 do
    if not corpses[i].e:valid() then table.remove(corpses, i) end
  end
  while #corpses > MAX_CORPSES do
    local oldest = table.remove(corpses, 1)
    live[oldest.idx] = nil
    if oldest.e:valid() then oldest.e:despawn() end
  end
end

local function update_shots(tick)
  -- Fireballs whose burst has finished playing.
  if M.expire then
    for idx, x in pairs(M.expire) do
      if tick >= x.at then
        if x.p:valid() then x.p:despawn() end
        M.expire[idx] = nil
      end
    end
  end

  -- Fireballs. Moved here rather than by the engine because a projectile is a
  -- gameplay object: what it does when it arrives is a decision, and the engine
  -- has no opinion about any of it.
  for idx, s in pairs(shots) do
    if not s.p:valid() then
      shots[idx] = nil
    elseif tick - s.born > 300 then
      s.p:despawn()
      shots[idx] = nil
    else
      local p = s.p:pos()
      local step = s.speed / fx.int(60)
      -- Does it reach? A projectile that ignored geometry sailed through every
      -- wall in the map, which is not a fireball, it is a rumour.
      local blocked, at = dnp.sweep(p, s.dir, step)
      if blocked then
        s.p:set_pos(at)
        s.p:set_animation(1)
        shots[idx] = nil
        M.expire = M.expire or {}
        M.expire[idx] = { p = s.p, at = tick + 20 }
      else
        s.p:set_pos(vec3.new(p.x + s.dir.x * step, p.y + s.dir.y * step, p.z))
      end
    end
  end
end

-- No chaining needed, and none possible.
--
-- The engine collects each script's hooks after it loads and calls every one of
-- them in load order, so two scripts defining on_tick both run. This used to be
-- a plain global, where the second definition silently replaced the first --
-- and a map loading both this and doom.lua lost every door the moment it gained
-- a monster.
function on_tick()
  local tick = dnp.tick()


  refresh_players(tick)

  -- NOBODY HERE: stop.
  --
  -- A level stays loaded and keeps ticking after the last player leaves it, and
  -- every level runs its own copy of this script over its own monsters. So on a
  -- server where clients have walked through several maps, most of the monster
  -- work each tick is being done on behalf of empty rooms -- and the stall
  -- warnings arrive four at a time for one tick, once per level, which is
  -- exactly what that looks like from the outside.
  --
  -- Nothing a monster does is defined without a player: it chases one, it aims
  -- at one, it decides whether it can see one. The only state that has to be
  -- right when somebody arrives is the animation, and that is set once on the
  -- transition rather than every tick.
  if #players == 0 then
    if not asleep then
      asleep = true
      for _, m in pairs(live) do
        if m.e:valid() then
          set_state(m, IDLE)
          set_move(m, ZERO, ZERO, m.aim)
        end
      end
    end
    -- Fireballs in flight and corpses still have to finish: a projectile with
    -- nobody to hit, and a body nobody is watching, both still have to stop
    -- existing. Leaking either one leaks a slot per shot and per kill.
    update_shots(tick)
    update_corpses(tick)
    return
  end
  asleep = false

  -- MID-ATTACK monsters, which is normally none of them.
  --
  -- Their own short list rather than found by walking every monster: a windup
  -- lasts eight ticks and at most a couple are ever in one, so scanning
  -- twenty-nine records to find them is the wrong shape.
  for i = #firing, 1, -1 do
    local m = firing[i]
    if not m.e:valid() or m.fire_at == nil then
      table.remove(firing, i)
    elseif tick >= m.fire_at then
      m.fire_at = nil
      table.remove(firing, i)
      local from = m.e:pos()
      -- CLOSE ENOUGH TO CLAW. Doom's imp throws a fireball at range and swipes
      -- when you are next to it, and the difference matters more than it looks:
      -- a fireball spawned inside somebody explodes on its owner or sails past,
      -- so a monster with only a ranged attack is harmless the moment you are
      -- standing in it. `melee` was in the catalogue from the start and nothing
      -- read it.
      local melee = m.def.melee
      local target, d2 = nearest_player(from)
      if melee ~= nil and target ~= nil and d2 ~= nil and d2 < melee * melee then
        hurt_player(target, M.claw_damage)
      elseif m.def.attack == "hitscan" then
        fire_hitscan(m, from, m.aim or 0)
      else
        fire_projectile(m, from, m.aim or 0)
      end
      m.ready_at = tick + m.def.cooldown
      set_state(m, WALK)
    end
  end

  -- And the monsters whose turn it is to think.
  --
  -- One bucket of the THINK-way split, not the whole population. The old loop
  -- walked every monster every tick to ask `(tick + idx) % THINK == 0` and threw
  -- away three quarters of the answers -- twenty-nine hash-table steps and
  -- twenty-nine valid() calls into the engine, sixty times a second, to do
  -- seven monsters' worth of work.
  local bucket = buckets[tick % THINK]
  for i = #bucket, 1, -1 do
    local m = bucket[i]
    if not m.e:valid() then
      live[m.idx] = nil
      table.remove(bucket, i)
    elseif m.dead_at ~= nil or m.dormant or m.fire_at ~= nil then
      -- Dead, or a fixture, or already mid-attack and handled above.
    elseif tick < m.pain_until then
      -- Flinching. It stops where it is, which is what makes a hit readable.
      set_move(m, ZERO, ZERO, m.aim)
    else
      local from = m.e:pos()
      local player, d2, ppos = nearest_player(from)
      local sight = m.def.sight

      if player == nil or d2 >= sight * sight then
        -- Nothing in range. set_state and set_move do nothing when nothing
        -- would change, which for a monster nobody has walked near yet is
        -- every decision it will ever make.
        set_state(m, IDLE)
        set_move(m, ZERO, ZERO, m.aim)
      else
        local aim = aim_at(from, ppos)
        m.aim = aim
        -- CAN IT SEE YOU. Distance alone put monsters through walls, shooting
        -- at people two rooms away -- "is it close" is not "is it visible", and
        -- the engine already knows the difference because the mover does.
        if not dnp.can_see(m.e, player) then
          set_state(m, IDLE)
          set_move(m, ZERO, ZERO, aim)
        elseif tick >= m.ready_at then
          -- An absolute tick, not a counter ticked down. A countdown has to be
          -- visited every tick by every monster whether or not anything about
          -- it is due; a deadline is read only when the monster is already
          -- being looked at.
          m.fire_at = tick + m.def.windup
          firing[#firing + 1] = m
          set_state(m, ATTACK)
          set_move(m, ZERO, ZERO, aim)
        else
          -- A WORLD direction, not "forward".
          --
          -- move_x/move_y are world axes: the client computes a screen-relative
          -- direction and sends the world vector, so the engine never has to
          -- know where a camera is. Sending (0, 1) meant "walk north" -- which
          -- every monster did, in unison, regardless of where the player was.
          local d = dnp.dir(aim)
          set_state(m, WALK)
          set_move(m, d.x, d.y, aim)
        end
      end
    end
  end

  update_shots(tick)
  update_corpses(tick)

end

-- A swing reached something.
--
-- The engine reports the contact and stops: it says which entity swung and what
-- the moving collider found, and has no opinion about health, damage or death.
-- The same division it draws for crushing and for interaction.
--
-- `target` may be a prop or an agent -- the engine pushes each as the handle
-- type it is -- so this asks whether it is one of ours rather than comparing
-- an index against a limit it would then have to know.
function on_attack(attacker, target)
  print('[dbg] on_attack idx=' .. tostring(target and target:index()) .. ' known=' .. tostring(target and live[target:index()] ~= nil))
  if target == nil or not target:valid() then return end
  local m = live[target:index()]
  if m == nil or m.dead_at ~= nil then return end

  m.hp = m.hp - M.melee_damage
  if m.hp > 0 then
    -- Flinch. It interrupts whatever the monster was doing, which is most of
    -- what makes a fight readable: without it a hit looks identical to a miss
    -- until the thing finally falls over.
    local tick = dnp.tick()
    m.pain_until = tick + PAIN_TICKS
    m.fire_at = nil
    set_state(m, PAIN)
    set_move(m, ZERO, ZERO, m.aim)
    return
  end

  -- Dead. It stops deciding anything and plays its death animation, which holds
  -- on its last frame -- so what is left is a body on the floor. It is removed
  -- only when the corpse cap pushes it out.
  m.dead_at = dnp.tick()
  m.fire_at = nil
  corpses[#corpses + 1] = m
  set_state(m, DIE)
  set_move(m, ZERO, ZERO, m.aim)
  -- Nothing walks into a corpse. Doom's do not block either, and a body left
  -- solid in a doorway for a second and a half is a wall that appeared where
  -- you were about to run.
  target:set_solid(false)
end

-- A fireball that touches something goes off. Reported by the engine's overlap
-- test, which is the same one a key uses -- a projectile is a pickup that hurts.
function on_touch(entity, prop)
  local s = shots[prop:index()]
  if s == nil then return end
  shots[prop:index()] = nil
  -- What it hit. A fireball touches whatever gets in its way, which includes
  -- the monster that fired it -- so only a player is hurt by one, and the
  -- alternative would be imps killing each other in corridors.
  if entity ~= nil and entity:valid() and entity:kind() == 0 then
    hurt_player(entity, M.claw_damage)
  end
  prop:set_animation(1)
  -- Left visible long enough for the burst to play, then removed. The client
  -- decides how fast it plays; this only says when it stops existing.
  M.expire = M.expire or {}
  M.expire[prop:index()] = { p = prop, at = dnp.tick() + 20 }
end
