-- What lives in maps/monsters.map.
--
-- Doom units, like every other map script here: 48 to an engine unit. The
-- player spawns at engine (5, 10) -- Doom (240, 480) -- and these two stand in
-- the far room, past the opening in the shared wall at x = 20.
--
-- AWAKE, unlike doors.map's pair, because chasing and shooting are what this
-- fixture exists to exercise. Placed apart from each other rather than on one
-- spot, so "they moved" can be told from "they were separated on the first
-- tick".
--
-- Run at load time, top level, after scripts/enemies.lua has published itself
-- as a global: the map declares the two files in that order and the server
-- loads them in the order declared.

-- The hitscan one, with a clear line to the player through the opening.
--
-- Doom (1104, 480) is engine (23, 10) and the player spawns at (5, 10): 18
-- units apart, inside the zombieman's 1024-unit sight (21.3 engine units) with
-- enough margin that the test is not measuring the edge of it. The first
-- version of this file put it at 30 units and the monsters correctly did
-- nothing at all, which read as broken AI and was a badly placed fixture.
enemies.spawn("zombieman", 1104, 480, 48)

-- And the projectile one, off that line until it moves -- so a fireball that
-- reaches the player got there by the imp walking into the doorway, which is
-- what "the projectile respects walls" is supposed to look like.
-- Engine (24, 7). The line from the player to here crosses the wall at x = 20
-- at y = 7.6, below the 8..12 opening -- so the wall is between them until the
-- imp moves into the doorway.
enemies.spawn("imp", 1152, 336, 48)

-- A THIRD monster, dormant, right next to the spawn.
--
-- The damage test needs a target it can reach in the first second: the session
-- is young then, and a test that first watches the level for seven seconds and
-- only then starts swinging has been disconnected before it begins. Dormant so
-- it stays where it is put -- what is under test is that a swing kills it, not
-- that it can be chased down.
--
-- Doom (336, 480) is engine (7, 10): two units east of the player's spawn, well
-- inside the swing's reach.
enemies.spawn("zombieman", 336, 480, 48, { dormant = true })
