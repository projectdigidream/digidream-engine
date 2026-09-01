-- Does the graph BEHAVE?
--
-- tools/mobcheck/check.lua proves a mob table is well formed. That is a
-- different claim from "the state machine runs", and a runtime whose
-- transitions never fire passes the first one perfectly.
--
-- House harness: ok/FAIL lines and a `== N checks, M failures ==` total.
--
-- Every positive claim here has a control that must fail, because the failure
-- mode this file exists to catch is a test that passes for the wrong reason --
-- a mob that reaches `missile` because every state is `missile`, or a shot
-- that lands because the harness fires it.

local W = dofile("tools/mobcheck/world.lua")
dofile("maps/scripts/mob_vocab.lua")
dofile("maps/scripts/mobs.lua")
local mobs = _G.mobs

local checks, fails = 0, 0
local function CHECK(cond, what)
  checks = checks + 1
  if not cond then
    fails = fails + 1
    print("  FAIL " .. what)
  end
end

local ok, problems = true, {}
local real = mobs.load
mobs.load = function(d) ok, problems = real(d); return ok, problems end
dofile("maps/mobs/doom_monsters.lua")
mobs.load = real
if not ok then
  for _, p in ipairs(problems) do print("  " .. p) end
  print("== cannot run: the mob table does not load ==")
  os.exit(1)
end

-- Step the world n ticks, running the runtime's own on_tick each time.
local function step(n)
  for _ = 1, n do
    W.tick_n = W.tick_n + 1
    W.advance()
    on_tick()
  end
end

local function state_of(e) return mobs.live[e:index()].state end
local function rec(e) return mobs.live[e:index()] end

-- ---------------------------------------------------------------------------
print("mobs: a zombieman walks its graph")
do
  W.reset(); mobs.reset()

  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  CHECK(z ~= nil, "the zombieman spawned")
  CHECK(state_of(z) == "look", "it starts in `look`, its declared `initial`")

  -- No player yet: it stays in look however long it waits. A graph whose
  -- first state falls through on its own would pass every later assertion.
  step(40)
  CHECK(state_of(z) == "look", "with nobody in the level it stays in `look`")

  -- A player arrives, in range and visible.
  local p = W.add("player", 5, 0, 0)
  W.see = true
  step(mobs.THINK + 1)
  CHECK(state_of(z) == "chase" or state_of(z) == "missile",
        "it leaves `look` once a player is visible (got " .. state_of(z) .. ")")

  -- It reaches `missile` and fires. The zombieman's attack_range is 2048 Doom
  -- units, so a player 5 units away is well inside it.
  local reached_missile = false
  local start_hp = p.health
  for _ = 1, 120 do
    step(1)
    if state_of(z) == "missile" then reached_missile = true end
  end
  CHECK(reached_missile, "it reaches `missile`")
  CHECK(p.health < start_hp,
        "and the player has been shot (" .. start_hp .. " -> " .. p.health .. ")")

  -- And it comes BACK, rather than staying in the attack. Watching the states
  -- it visits over the run beats sampling the state at the end, which is true
  -- for a mob that is merely between shots and true for one that is stuck.
  local seen = {}
  for _ = 1, 200 do step(1); seen[state_of(z)] = true end
  CHECK(seen["chase"] and seen["missile"],
        "it cycles between `chase` and `missile` rather than sticking in one")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- out of sight, nothing happens")
do
  W.reset(); mobs.reset()

  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 5, 0, 0)
  -- The ONE difference from the test above.
  W.see = false
  local start_hp = p.health
  step(200)
  CHECK(p.health == start_hp,
        "a zombieman that cannot see you never shoots you")
  CHECK(state_of(z) ~= "missile", "and never enters `missile`")
end

-- ---------------------------------------------------------------------------
print("mobs: damage, pain and death")
do
  W.reset(); mobs.reset()

  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 5, 0, 0)
  step(mobs.THINK + 1)

  -- pain_chance 200 of 255. Rolls are ours, so this does not depend on luck.
  W.rolls = { 0 }                       -- always below 200: it always flinches
  mobs.hurt(z, 5, nil)
  CHECK(state_of(z) == "pain", "a hit that does not kill sends it to `pain`")
  CHECK(rec(z).hp == 15, "and takes the damage (20 - 5 = " .. rec(z).hp .. ")")

  -- A zombieman has 20 hp.
  mobs.hurt(z, 100, nil)
  CHECK(state_of(z) == "death", "a fatal hit sends it to `death`")
  CHECK(rec(z).dead, "and it is marked dead")
  CHECK(z.is_solid == false, "and stops being solid, so you can walk over it")

  -- Dead things do not act. This is the assertion that catches a death state
  -- that is only an animation.
  local before = z.anims
  step(120)
  CHECK(state_of(z) == "death", "and stays dead")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- pain_chance 0 means it never flinches")
do
  W.reset(); mobs.reset()
  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 5, 0, 0)
  step(mobs.THINK + 1)

  local saved = mobs.defs.zombieman.pain_chance
  mobs.defs.zombieman.pain_chance = 0
  W.rolls = { 0 }
  mobs.hurt(z, 5, nil)
  CHECK(state_of(z) ~= "pain",
        "with pain_chance 0 a hit does not interrupt it")
  mobs.defs.zombieman.pain_chance = saved
end

-- ---------------------------------------------------------------------------
print("mobs: an imp throws a fireball that hurts what it reaches")
do
  W.reset(); mobs.reset()

  local imp = mobs.spawn("imp", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  -- Far enough that it uses the missile rather than the claw: the imp's melee
  -- range is 64 Doom units, which is 1.33 engine units.
  local p = W.add("player", 12, 0, 0)
  W.see = true
  local start_hp = p.health

  local launched = false
  for _ = 1, 240 do
    step(1)
    for _ in pairs(mobs.flying) do launched = true end
    if p.health < start_hp then break end
  end
  CHECK(launched, "the imp launched a fireball")
  CHECK(p.health < start_hp,
        "and it hurt the player on arrival (" .. p.health .. ")")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- a fireball with no on_hit hurts nobody")
do
  W.reset(); mobs.reset()
  local saved = mobs.missiles.fireball.on_hit
  mobs.missiles.fireball.on_hit = nil

  local imp = mobs.spawn("imp", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 12, 0, 0)
  local start_hp = p.health
  step(240)
  CHECK(p.health == start_hp,
        "the damage comes from the missile's on_hit, not from the runtime")
  mobs.missiles.fireball.on_hit = saved
end

-- ---------------------------------------------------------------------------
print("mobs: infighting -- a mob hurt by a mob turns on it")
do
  W.reset(); mobs.reset()

  local a = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local b = mobs.spawn("imp", { x = W.mkfx(3), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 20, 0, 0)
  step(mobs.THINK + 1)

  W.rolls = { 255 }                     -- above pain_chance: no flinch
  mobs.hurt(a, 1, mobs.live[b:index()])
  CHECK(mobs.live[a:index()].target == b,
        "the zombieman now targets the imp that hurt it")

  -- The control: a PLAYER's hit passes `by = nil`, so it does not retarget
  -- onto a mob that is not there.
  local before = mobs.live[a:index()].target
  mobs.hurt(a, 1, nil)
  CHECK(mobs.live[a:index()].target == before,
        "a player's hit does not retarget it")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- infighting off means no retarget")
do
  W.reset(); mobs.reset()
  local saved = mobs.defs.zombieman.infighting
  mobs.defs.zombieman.infighting = false

  local a = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local b = mobs.spawn("imp", { x = W.mkfx(3), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 20, 0, 0)
  step(mobs.THINK + 1)
  local before = mobs.live[a:index()].target
  W.rolls = { 255 }
  mobs.hurt(a, 1, mobs.live[b:index()])
  CHECK(mobs.live[a:index()].target == before,
        "with infighting false it keeps its target")
  mobs.defs.zombieman.infighting = saved
end

-- ---------------------------------------------------------------------------
print("mobs: a hitscan draws a tracer, and the tracer says where the shot went")
do
  W.reset(); mobs.reset()
  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 6, 0, 0)
  W.see = true
  for _ = 1, 200 do
    W.tick_n = W.tick_n + 1
    W.advance()
    on_tick()
    if #W.vfx > 0 then break end
  end

  CHECK(#W.vfx > 0, "a tracer was emitted")
  if #W.vfx > 0 then
    local t = W.vfx[1]
    -- FOUR arguments, because dnp.vfx takes (kind, position, direction,
    -- param). A tracer with only a position is a line the client cannot draw:
    -- it knows where the shot ended and nothing about where it began.
    --
    -- This is the assertion that was missing. acts.hitscan read the binding's
    -- TWO returns as four, built a malformed vec3, and raised -- which killed
    -- on_tick before the damage, before update_missiles, and took the rest of
    -- the think bucket with it. Twenty-two passing checks did not notice,
    -- because the harness returned four values too.
    CHECK(t.at ~= nil, "the tracer carries a start position")
    CHECK(t.dir ~= nil, "and a direction")
    CHECK(t.param ~= nil and t.param > 0,
          "and a length (" .. tostring(t.param) .. ")")
  end

  -- And the shot still lands, which is what the raise used to prevent.
  CHECK(p.health < 100, "the shot hurt the player (" .. p.health .. ")")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- a hitscan with no `tracer` draws nothing")
do
  W.reset(); mobs.reset()
  local saved = mobs.defs.zombieman.states.missile.sequence[2].tracer
  mobs.defs.zombieman.states.missile.sequence[2].tracer = nil
  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 6, 0, 0)
  W.see = true
  for _ = 1, 200 do W.tick_n = W.tick_n + 1; W.advance(); on_tick() end
  CHECK(#W.vfx == 0, "no tracer without one asked for (" .. #W.vfx .. ")")
  -- But the shot still lands, so the control is about the DRAWING and not
  -- about the monster having stopped shooting.
  CHECK(p.health < 100, "and the shot still lands")
  mobs.defs.zombieman.states.missile.sequence[2].tracer = saved
end

-- ---------------------------------------------------------------------------
print("mobs: a prop is not an entity")
do
  W.reset(); mobs.reset()
  local barrel = dnp.spawn_prop("barrel",
                                { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local mob = mobs.spawn("zombieman",
                         { x = W.mkfx(2), y = W.mkfx(0), z = W.mkfx(0) })

  -- The engine gives props and entities separate userdata types with separate
  -- methods. A prop has a position and no health.
  CHECK(barrel.pos ~= nil, "a prop has a position")
  CHECK(barrel.hp == nil, "a prop has no hp")
  CHECK(barrel.set_input == nil, "a prop takes no input")
  CHECK(mob.hp ~= nil, "an agent does have hp")

  -- Swinging at a barrel must not raise. `hurt` routes anything that is not
  -- one of ours to the player path, which asks for health -- and a barrel has
  -- none, so it took on_attack down with it on a real server.
  local ok = pcall(mobs.hurt, barrel, 10, nil)
  CHECK(ok, "hurting a prop does not raise")

  -- THE CONTROL: asking a prop for its health DOES raise, so the guard above
  -- is doing something. Without this the check passes on a harness where
  -- everything answers everything -- which is exactly the harness this was,
  -- and exactly why the bug reached a server.
  CHECK(not pcall(function() return barrel:hp() end),
        "CONTROL: a prop genuinely has no hp to ask for")
end

-- ---------------------------------------------------------------------------
print("mobs: a monster turns to face you before it fires")
do
  W.reset(); mobs.reset()
  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  -- BEHIND it. Bodies start facing angle 0 -- due east -- so a target due
  -- WEST is the case that matters: the monster has to come most of the way
  -- round before its shot can reach.
  local p = W.add("player", -8, 0, 0)
  W.see = true

  -- It must not have fired yet: it cannot have turned that far in two ticks.
  step(2)
  CHECK(p.health == 100, "it has not fired while still turned away")

  -- Given time to turn, it does fire and it does hit.
  for _ = 1, 400 do
    step(1)
    if p.health < 100 then break end
  end
  CHECK(p.health < 100,
        "once it has come round, the shot lands (" .. p.health .. ")")

  -- And its body really did turn, rather than the shot having been allowed
  -- through regardless.
  local face = z:facing()
  local want = math.floor(math.atan2(0, -8) / (2 * math.pi) * 4294967296)
  local d = face - want
  while d > 2147483647 do d = d - 4294967296 end
  while d < -2147483648 do d = d + 4294967296 end
  CHECK(math.abs(d) < 4294967296 / 8, "it is facing the target when it fires")
end

-- ---------------------------------------------------------------------------
print("mobs: a player who runs out of health respawns")
do
  W.reset(); mobs.reset()
  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 6, 0, 0)
  W.see = true
  -- One tick, so refresh_players records where they started.
  step(1)
  local home = W.fxv(p:pos().x)

  -- Walk them somewhere else, then kill them.
  p.p = { x = W.mkfx(30), y = W.mkfx(30), z = W.mkfx(0) }
  mobs.hurt(p, 999, mobs.live[z:index()])

  -- NOT left at zero. Writing zero and stopping leaves a player alive at no
  -- health forever, taking hits that change nothing -- which is
  -- indistinguishable from damage having stopped working, and is how the gap
  -- was reported.
  CHECK(p.health > 0, "they do not sit at zero (" .. p.health .. ")")
  CHECK(p.health == 100, "they come back at full health")
  CHECK(W.fxv(p:pos().x) == home,
        "and back where they started (" .. W.fxv(p:pos().x) .. ")")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- a survivable hit does not respawn anybody")
do
  W.reset(); mobs.reset()
  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 6, 0, 0)
  step(1)
  p.p = { x = W.mkfx(30), y = W.mkfx(30), z = W.mkfx(0) }
  mobs.hurt(p, 10, mobs.live[z:index()])
  CHECK(p.health == 90, "the health came off (" .. p.health .. ")")
  CHECK(W.fxv(p:pos().x) == 30, "and they were not moved")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- a map that owns death is not overruled")
do
  W.reset(); mobs.reset()
  local called = 0
  mobs.on_player_hurt = function() called = called + 1 end
  local p = W.add("player", 6, 0, 0)
  step(1)
  mobs.hurt(p, 999, nil)
  CHECK(called == 1, "on_player_hurt was consulted")
  -- And the default did NOT also run: a map that installs its own rules gets
  -- them, not them and a respawn as well.
  CHECK(p.health == 100, "the default respawn did not also fire")
  mobs.on_player_hurt = nil
end

-- ---------------------------------------------------------------------------
print("mobs: the shot goes where the target is, not where the body was")
do
  W.reset(); mobs.reset()
  local z = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  -- A bystander a third of a unit to the EAST, which is where the body starts
  -- out pointing -- so anything fired before the monster has turned hits this
  -- rather than the player.
  local bystander = mobs.spawn("imp",
                               { x = W.mkfx(2), y = W.mkfx(0), z = W.mkfx(0) })
  -- The player is NORTH: ninety degrees off the initial facing.
  local p = W.add("player", 0, 8, 0)
  W.see = true

  for _ = 1, 400 do
    step(1)
    if p.health < 100 then break end
  end

  CHECK(p.health < 100, "the player was hit (" .. p.health .. ")")
  CHECK(mobs.live[bystander:index()].hp == mobs.live[bystander:index()].max_hp,
        "and the bystander it was pointing at was not")

  -- HONESTLY: this passes at a 60-degree firing arc too, so it does NOT
  -- measure the arc. turn_speed is 4 turns a second -- 24 degrees a tick --
  -- so a monster corrects sixty degrees in under three ticks and is lined up
  -- long before it fires. What this does test is that the shot follows the
  -- BODY and the body follows the target, which is the pair that was broken
  -- when target_in_front compared against the mob's intent instead.

  -- And the tracer spans the real distance rather than stopping short.
  local v = W.vfx[#W.vfx]
  CHECK(v ~= nil and v.param ~= nil and v.param / 256 > 4,
        "the tracer reaches (" .. tostring(v and v.param and v.param / 256) .. ")")
end

-- ---------------------------------------------------------------------------
print("mobs: nobody keeps shooting a corpse")
do
  W.reset(); mobs.reset()
  local a = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local b = mobs.spawn("imp", { x = W.mkfx(4), y = W.mkfx(0), z = W.mkfx(0) })
  local p = W.add("player", 0, 20, 0)
  W.see = true
  step(mobs.THINK + 2)

  -- Infighting: b hurts a, so a turns on b.
  W.rolls = { 255 }                       -- above pain_chance: no flinch
  mobs.hurt(a, 1, mobs.live[b:index()])
  CHECK(mobs.live[a:index()].target == b, "it turned on the thing that hurt it")

  -- Now b dies. Its ENTITY is still valid -- that is what a corpse is -- and it
  -- stays in `live` so it can be resurrected, so nothing about `valid()`
  -- changes. The target has to be dropped on the grounds of being dead.
  mobs.hurt(b, 99999, nil)
  CHECK(mobs.live[b:index()].dead, "the imp is dead")
  CHECK(b:valid(), "and its corpse is still a valid entity")

  step(mobs.THINK * 3)
  CHECK(mobs.live[a:index()].target ~= b,
        "the zombieman stopped targeting the corpse")
  CHECK(mobs.live[a:index()].target == p,
        "and went back to the player")
end

-- ---------------------------------------------------------------------------
print("mobs: CONTROL -- a LIVE opponent is kept")
do
  W.reset(); mobs.reset()
  local a = mobs.spawn("zombieman", { x = W.mkfx(0), y = W.mkfx(0), z = W.mkfx(0) })
  local b = mobs.spawn("imp", { x = W.mkfx(4), y = W.mkfx(0), z = W.mkfx(0) })
  W.add("player", 0, 20, 0)
  step(mobs.THINK + 2)
  W.rolls = { 255 }
  mobs.hurt(a, 1, mobs.live[b:index()])
  step(mobs.THINK * 3)
  -- The one difference from the test above: b is still alive. Without this,
  -- "dropped the corpse" could equally mean "drops every mob target".
  CHECK(mobs.live[a:index()].target == b, "a living opponent is kept")
end

print(("== %d checks, %d failures =="):format(checks, fails))
os.exit(fails == 0 and 0 or 1)
