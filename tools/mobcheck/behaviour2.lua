-- The corners maps/mobs/doom.lua does not reach.
--
-- doom.lua exercises look/chase/attack/pain/death and one straight missile.
-- These are the paths nothing had run: a spawn budget, a resurrection, a
-- homing missile, a mid-sequence `goto`, a blast radius, and both halves of
-- the `lua` escape hatch. Each with a control.

local W = dofile("tools/mobcheck/world.lua")
dofile("maps/scripts/mob_vocab.lua")
dofile("maps/scripts/mobs.lua")
local mobs = _G.mobs

local checks, fails = 0, 0
local function CHECK(cond, what)
  checks = checks + 1
  if not cond then fails = fails + 1; print("  FAIL " .. what) end
end

-- The table CALLS mobs.load, exactly as it does when a map declares it as one
-- of its scripts, so this wraps the call rather than invoking it a second way.
local ok, problems = true, {}
local real = mobs.load
mobs.load = function(d) ok, problems = real(d); return ok, problems end
dofile("maps/mobs/exercise.lua")
mobs.load = real
if not ok then
  for _, p in ipairs(problems) do print("  " .. p) end
  print("== cannot run: the table does not load ==")
  os.exit(1)
end

local function step(n)
  for _ = 1, n do W.tick_n = W.tick_n + 1; W.advance(); on_tick() end
end
local function rec(e) return mobs.live[e:index()] end
local function count(kind)
  local n = 0
  for _, m in pairs(mobs.live) do
    if m.def.name == kind and m.e:valid() then n = n + 1 end
  end
  return n
end

-- ---------------------------------------------------------------------------
print("mobs: a spawn budget is a ceiling, not a rate")
do
  W.reset(); mobs.reset()
  local b = mobs.spawn("brooder", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 3, 0, 0)
  step(400)
  CHECK(count("spawnling") == 3,
        "it stopped at its limit of 3 (made " .. count("spawnling") .. ")")
  -- It kept TRYING, which is what makes the assertion above about the limit
  -- rather than about the brooder having run out of turns.
  CHECK(rec(b).vars.made > 5,
        "and it went on trying (" .. rec(b).vars.made .. " attempts)")

  -- CONTROL: a dead child frees a slot, so the budget counts what is ALIVE
  -- rather than what was ever made.
  for _, m in pairs(mobs.live) do
    if m.def.name == "spawnling" then m.e:despawn(); break end
  end
  step(40)
  CHECK(count("spawnling") == 3, "a dead child frees its place in the budget")
end

-- ---------------------------------------------------------------------------
print("mobs: revive puts a corpse back on its feet")
do
  W.reset(); mobs.reset()
  local h = mobs.spawn("healer", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local victim = mobs.spawn("spawnling", { x = W.mkfx(2), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 30, 0, 0)

  mobs.hurt(victim, 999, nil)
  CHECK(rec(victim).dead, "the victim is dead to begin with")

  step(20)
  CHECK(not rec(victim).dead, "and the healer brought it back")
  CHECK(rec(victim).hp == rec(victim).max_hp, "at full health")
  CHECK(victim.is_solid == true, "and solid again")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- revive does not reach across the map")
do
  W.reset(); mobs.reset()
  local h = mobs.spawn("healer", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  -- The healer's radius is 10. This is 50 away.
  local victim = mobs.spawn("spawnling", { x = W.mkfx(50), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 30, 0, 0)
  mobs.hurt(victim, 999, nil)
  step(40)
  CHECK(rec(victim).dead, "a corpse outside the radius stays down")
end

-- ---------------------------------------------------------------------------
print("mobs: `goto` abandons the rest of the sequence")
do
  W.reset(); mobs.reset()
  local j = mobs.spawn("jumper", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 3, 0, 0)
  step(30)
  CHECK(rec(j).state == "landed", "it went where the goto said")
  CHECK(rec(j).vars.reached == 1, "the entry before the goto ran")
  -- THE POINT. An implementation that ran the whole sequence and then jumped
  -- would satisfy every other assertion here.
  CHECK(rec(j).vars.skipped == 0,
        "and the entry AFTER it never ran (" .. rec(j).vars.skipped .. ")")
end

-- ---------------------------------------------------------------------------
print("mobs: the lua escape hatch, both halves")
do
  W.reset(); mobs.reset()
  local s = mobs.spawn("scripted", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 3, 0, 0)
  step(60)
  CHECK(rec(s).vars.n >= 3, "the lua ACTION ran and counted (" .. rec(s).vars.n .. ")")
  CHECK(rec(s).state == "done", "and the lua CONDITION fired the transition")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- a lua condition that is false moves nothing")
do
  W.reset(); mobs.reset()
  local saved = mobs.defs.scripted.states.count.transitions
  mobs.defs.scripted.states.count.transitions = {
    { to = "done", when = { { cond = "lua", fn = function() return false end } } },
  }
  local s = mobs.spawn("scripted", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 3, 0, 0)
  step(60)
  CHECK(rec(s).state == "count", "it stayed put")
  CHECK(rec(s).vars.n >= 3, "though the action still ran, so it was ticking")
  mobs.defs.scripted.states.count.transitions = saved
end

-- ---------------------------------------------------------------------------
print("mobs: a rocket's blast reaches past what it hit")
do
  W.reset(); mobs.reset()
  local g = mobs.spawn("gunner", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local near = W.add("player", 10, 0, 0)
  -- Two units past the one it will hit, and well inside the blast radius of 3.
  local bystander = W.add("player", 11, 0, 0)
  step(300)
  CHECK(near.health < 100, "the player it hit was hurt (" .. near.health .. ")")
  CHECK(bystander.health < 100,
        "and so was the one standing next to them (" .. bystander.health .. ")")
  -- Falloff: the further one takes less. This is what `falloff = true` means,
  -- and without the assertion it could be ignored entirely.
  CHECK(bystander.health > near.health,
        "and the further one took less")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- the gunner does not hurt itself")
do
  W.reset(); mobs.reset()
  local g = mobs.spawn("gunner", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 2, 0, 0)          -- close enough that the blast covers 0,0
  local hp0 = rec(g).hp
  step(300)
  CHECK(rec(g).hp == hp0,
        "hurts_owner = false means its own rocket does not kill it")
end

-- ---------------------------------------------------------------------------
print("mobs: a homing missile turns towards a moving target")
do
  W.reset(); mobs.reset()
  local g = mobs.spawn("seeker_gun", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 20, 0, 0)
  -- Let it fire while the player is due east. Enough ticks to cover a think
  -- bucket (8) plus the sequence's own offset, rather than a guess.
  step(20)
  local fired = false
  for _ in pairs(mobs.flying) do fired = true end
  CHECK(fired, "the seeker was launched")

  -- Now move the player far off the missile's original line. A straight
  -- missile cannot reach them; a homing one can.
  -- 300 ticks, not 400. The gun's cooldown is 400, so a longer window lets it
  -- fire a SECOND missile -- which, aimed after the player moved, hits whether
  -- or not the first one homed. The control below failed for exactly that
  -- reason and was measuring re-aiming, not homing.
  p.p = { x = W.mkfx(20), y = W.mkfx(14), z = W.mkfx(0) }
  step(300)
  CHECK(p.health < 100,
        "the homing missile found the moved target (" .. p.health .. ")")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- without `homing` the same shot misses")
do
  W.reset(); mobs.reset()
  local saved = mobs.missiles.seeker.homing
  mobs.missiles.seeker.homing = nil

  local g = mobs.spawn("seeker_gun", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 20, 0, 0)
  -- 20, the SAME as the test above, so the missile is already airborne when
  -- the player moves. With 10 the player moved first and the gun simply aimed
  -- at the new position -- the control then passed or failed on the aim and
  -- said nothing at all about homing.
  step(20)
  p.p = { x = W.mkfx(20), y = W.mkfx(14), z = W.mkfx(0) }
  step(300)                              -- the same window as the test above
  CHECK(p.health == 100,
        "a straight missile flies past (" .. p.health .. ")")
  mobs.missiles.seeker.homing = saved
end

-- ---------------------------------------------------------------------------
print("mobs: reset forgets the buckets, not just the table")
do
  W.reset(); mobs.reset()
  mobs.spawn("spawnling", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  mobs.spawn("spawnling", { x = W.mkfx(1), y = W.mkfx(0), z = W.mkfx(0) })
  mobs.reset()

  local n = 0
  for _ in pairs(mobs.live) do n = n + 1 end
  CHECK(n == 0, "live is empty")
  -- THE POINT: clearing `live` alone left the buckets holding the mobs, and
  -- since `live` is keyed by entity index and a rebuilt level starts its
  -- indices again from the bottom, a stale record is found for somebody
  -- ELSE'S entity. Stepping after a reset must touch nothing.
  W.reset()
  local fresh = W.add("player", 0, 0, 0)
  step(20)
  CHECK(fresh.health == 100, "and a new level's entities are left alone")
end

-- ---------------------------------------------------------------------------
print("mobs: chase follows a route, not the straight line")
do
  W.reset(); mobs.reset()
  local z = mobs.spawn("gunner", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  z.sec = 0
  local p = W.add("player", 0, 0, 0)
  p.p = { x = W.mkfx(20), y = W.mkfx(0), z = W.mkfx(0) }
  p.sec = 2

  -- The route goes NORTH first: sector 0, then a waypoint at (0, 20), then
  -- the target's sector. A mob that walked at the target would go due east.
  W.routes = { [0] = { [2] = {
    { sector = 0, x = W.mkfx(0),  y = W.mkfx(0)  },
    { sector = 1, x = W.mkfx(0),  y = W.mkfx(20) },
    { sector = 2, x = W.mkfx(20), y = W.mkfx(20) },
  } } }

  -- `gunner` stops to fire, so drive it through `chase` directly.
  mobs.live[z:index()].st = { anim = 1, move = "chase", face = "target" }
  mobs.live[z:index()].state = "chase"
  mobs.live[z:index()].target = p
  -- Enough to reach this mob's THINK bucket. Steering happens on the think
  -- tick -- set_input is sticky, so rewriting it every tick buys nothing --
  -- and four ticks is not guaranteed to include the mob's own turn.
  step(mobs.THINK + 2)

  local inp = z.input
  CHECK(inp ~= nil, "it was given an input")
  -- Walking NORTH: +y, and not east.
  CHECK(W.fxv(inp.move_y) > 0.5,
        "it walks towards the waypoint, north (" .. tostring(inp.move_y) .. ")")
  CHECK(math.abs(W.fxv(inp.move_x)) < 0.5,
        "and not straight at the target, east (" .. tostring(inp.move_x) .. ")")

  -- AND IT STILL FACES THE TARGET. Where it walks and where it looks are
  -- different questions: a monster rounding a corner is still looking at you,
  -- and its shot goes at you rather than along its own route.
  local want_face = math.floor(math.atan2(0, 20) / (2 * math.pi) * 4294967296)
  CHECK(inp.facing == want_face,
        "and still faces the target, not the waypoint")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- with no route it walks straight at the target")
do
  W.reset(); mobs.reset()
  local z = mobs.spawn("gunner", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  z.sec = 0
  local p = W.add("player", 0, 0, 0)
  p.p = { x = W.mkfx(20), y = W.mkfx(0), z = W.mkfx(0) }
  p.sec = 2
  W.routes = nil                      -- the ONE difference

  mobs.live[z:index()].st = { anim = 1, move = "chase", face = "target" }
  mobs.live[z:index()].state = "chase"
  mobs.live[z:index()].target = p
  -- Enough to reach this mob's THINK bucket. Steering happens on the think
  -- tick -- set_input is sticky, so rewriting it every tick buys nothing --
  -- and four ticks is not guaranteed to include the mob's own turn.
  step(mobs.THINK + 2)

  local inp = z.input
  CHECK(W.fxv(inp.move_x) > 0.5,
        "it falls back to the straight line, east (" .. tostring(inp.move_x) .. ")")
  CHECK(math.abs(W.fxv(inp.move_y)) < 0.5, "and not north")
end

-- ---------------------------------------------------------------------------
print("mobs: a mob in the target's own sector does not ask for a route")
do
  W.reset(); mobs.reset()
  W.path_calls = 0
  W.routes = { [0] = { [0] = { { sector = 0, x = W.mkfx(0), y = W.mkfx(0) } } } }
  local z = mobs.spawn("gunner", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  z.sec = 0
  local p = W.add("player", 5, 0, 0)
  p.sec = 0                            -- the SAME sector

  mobs.live[z:index()].st = { anim = 1, move = "chase", face = "target" }
  mobs.live[z:index()].state = "chase"
  mobs.live[z:index()].target = p
  step(60)
  -- A* over a level for a monster standing next to you is the storm the
  -- binding's per-tick budget exists to stop, and the cheapest place to not
  -- have it is here.
  CHECK(W.path_calls == 0,
        "no A* was run (" .. tostring(W.path_calls) .. " calls)")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- a different sector DOES ask, and not every tick")
do
  W.reset(); mobs.reset()
  W.path_calls = 0
  W.routes = { [0] = { [2] = {
    { sector = 0, x = W.mkfx(0), y = W.mkfx(0) },
    { sector = 2, x = W.mkfx(20), y = W.mkfx(0) },
  } } }
  local z = mobs.spawn("gunner", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  z.sec = 0
  local p = W.add("player", 20, 0, 0)
  p.sec = 2

  mobs.live[z:index()].st = { anim = 1, move = "chase", face = "target" }
  mobs.live[z:index()].state = "chase"
  mobs.live[z:index()].target = p
  step(60)
  CHECK(W.path_calls > 0, "it asked for a route")
  -- Rate-limited: 60 ticks must not be 60 searches. Without the deadline this
  -- is one A* per mob per tick, which on a Doom level is the exact failure the
  -- binding's budget refuses -- and a refused path is a monster that stops.
  CHECK(W.path_calls <= 3,
        "and not once a tick (" .. tostring(W.path_calls) .. " in 60)")
end

-- ---------------------------------------------------------------------------
print("mobs: a route through a jump is a promise the runtime keeps")
do
  W.reset(); mobs.reset()
  -- The route's second waypoint needs a jump.
  W.routes = { [0] = { [2] = {
    { sector = 0, x = W.mkfx(0),  y = W.mkfx(0), jump = false },
    { sector = 1, x = W.mkfx(10), y = W.mkfx(0), jump = true  },
    { sector = 2, x = W.mkfx(20), y = W.mkfx(0), jump = false },
  } } }

  local h = mobs.spawn("hopper", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  h.sec = 0
  local p = W.add("player", 20, 0, 0)
  p.sec = 2
  mobs.live[h:index()].target = p
  step(mobs.THINK + 2)

  CHECK(h.input ~= nil, "it was driven")
  -- The engine's jump is edge-triggered off the button and owns max_jumps and
  -- the charge rules, so the runtime HOLDS it rather than guessing the tick.
  CHECK(h.input.buttons == 0x0001,
        "it holds JUMP for the step the route flagged (" ..
        tostring(h.input.buttons) .. ")")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- an unflagged step is walked, not jumped")
do
  W.reset(); mobs.reset()
  -- The SAME route with the flag cleared. One field different.
  W.routes = { [0] = { [2] = {
    { sector = 0, x = W.mkfx(0),  y = W.mkfx(0), jump = false },
    { sector = 1, x = W.mkfx(10), y = W.mkfx(0), jump = false },
    { sector = 2, x = W.mkfx(20), y = W.mkfx(0), jump = false },
  } } }

  local h = mobs.spawn("hopper", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  h.sec = 0
  local p = W.add("player", 20, 0, 0)
  p.sec = 2
  mobs.live[h:index()].target = p
  step(mobs.THINK + 2)
  CHECK(h.input.buttons == 0,
        "no jump where the route did not ask for one (" ..
        tostring(h.input.buttons) .. ")")
end

-- ---------------------------------------------------------------------------
print("mobs: a mob with no jump_height is routed as a walker")
do
  W.reset(); mobs.reset()
  W.last_jump_arg = nil
  W.routes = { [0] = { [2] = {
    { sector = 0, x = W.mkfx(0),  y = W.mkfx(0) },
    { sector = 2, x = W.mkfx(20), y = W.mkfx(0) },
  } } }

  local w = mobs.spawn("walker", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  w.sec = 0
  local p = W.add("player", 20, 0, 0)
  p.sec = 2
  mobs.live[w:index()].target = p
  step(mobs.THINK + 2)
  -- Design §3.2.2 is the DEFAULT: absent means nil is passed, and the graph
  -- plans it as something that does not jump.
  CHECK(W.last_jump_arg == nil,
        "no jump height reaches the search (" .. tostring(W.last_jump_arg) .. ")")

  -- CONTROL: the hopper's does.
  W.reset(); mobs.reset()
  W.last_jump_arg = nil
  W.routes = { [0] = { [2] = {
    { sector = 0, x = W.mkfx(0),  y = W.mkfx(0) },
    { sector = 2, x = W.mkfx(20), y = W.mkfx(0) },
  } } }
  local h = mobs.spawn("hopper", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  h.sec = 0
  local p2 = W.add("player", 20, 0, 0)
  p2.sec = 2
  mobs.live[h:index()].target = p2
  step(mobs.THINK + 2)
  CHECK(W.last_jump_arg ~= nil, "the hopper's does")
end

print(("== %d checks, %d failures =="):format(checks, fails))
os.exit(fails == 0 and 0 or 1)
