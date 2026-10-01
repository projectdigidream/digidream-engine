--
-- A fixture for server/edge/test/soak.test.js's CONTROL: a deliberate leak.
--
-- Every player who arrives costs a megabyte that is never given back -- the
-- shape of a script keeping per-player state and never forgetting it. The soak
-- test run with this must report growth, or its "memory settles" proves
-- nothing. Unique per arrival, because LuaJIT interns strings and a megabyte of
-- the same text twice is one megabyte.
--
local kept = {}
function on_player_enter(e)
  kept[#kept + 1] = string.rep("x", 1048576) .. tostring(#kept)
end
