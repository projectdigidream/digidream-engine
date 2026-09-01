-- The harness must not offer what the engine does not.
--
-- This exists because it kept happening. tools/mobcheck/world.lua stood in for
-- the engine's Lua surface, and every place it was MORE generous than the real
-- thing, maps/scripts/mobs.lua quietly grew a dependency on the difference and
-- the tests stayed green:
--
--   set_input took a table          the binding takes positional arguments
--   fx and vec3 lived under `dnp`   they are globals
--   fx was callable                 it is a plain table of constructors
--   dnp.facing existed              `facing` is an entity method; dnp has atan2
--
-- Four bugs, none caught by 55 passing checks, all four found by starting a
-- server. A fake that is more permissive than the thing it stands in for tests
-- nothing, so this compares the two lists mechanically.
--
-- THE ENGINE'S LIST IS READ OUT OF THE BINDING, not copied. server/src/lua/
-- world_binding.cpp registers dnp.* in one table literal, and this greps it --
-- so adding a binding there is enough, and a name that exists only here is an
-- error.

local W = dofile("tools/mobcheck/world.lua")

local checks, fails = 0, 0
local function CHECK(cond, what)
  checks = checks + 1
  if not cond then fails = fails + 1; print("  FAIL " .. what) end
end

print("mobs: the harness offers only what the engine offers")

-- Names the engine registers on `dnp`, from the binding's own registration
-- table. A crude parse of one specific literal, which is why it asserts it
-- found a plausible number of them rather than trusting a possibly-empty grep.
local engine = {}
local f = assert(io.open("server/src/lua/world_binding.cpp", "r"))
local src = f:read("*a")
f:close()
for name in src:gmatch('{"([a-z_0-9]+)",%s*l_dnp_') do engine[name] = true end
-- dnp.button is a table, not a function, so it is registered differently.
if src:find('lua_setfield%(L, %-2, "button"%)') then engine.button = true end

local n = 0
for _ in pairs(engine) do n = n + 1 end
CHECK(n > 20, "the binding list parsed (" .. n .. " names)")

-- Every name the fake offers must exist in the engine.
local extra = {}
for name in pairs(dnp) do
  if not engine[name] then extra[#extra + 1] = name end
end
table.sort(extra)
CHECK(#extra == 0,
      "the harness invents nothing: " .. (#extra > 0 and table.concat(extra, ", ") or "-"))

-- And fx/vec3 are GLOBALS, not fields of dnp -- which is where they were.
CHECK(_G.fx ~= nil, "fx is a global")
CHECK(_G.vec3 ~= nil, "vec3 is a global")
CHECK(dnp.fx == nil, "fx is NOT a field of dnp")
CHECK(dnp.vec3 == nil, "vec3 is NOT a field of dnp")

-- fx is a plain table of constructors with no __call.
CHECK(type(_G.fx) == "table", "fx is a table")
CHECK(getmetatable(_G.fx) == nil or getmetatable(_G.fx).__call == nil,
      "fx is not callable; use fx.parse")
for _, k in ipairs({ "int", "frac", "raw", "parse" }) do
  CHECK(type(_G.fx[k]) == "function", "fx." .. k .. " exists")
end

-- THE CONTROL. Add a name the engine does not have and the check above must
-- fail -- otherwise it is comparing two empty lists and reporting success.
dnp.__not_a_real_binding = function() end
local caught = false
for name in pairs(dnp) do if not engine[name] then caught = true end end
dnp.__not_a_real_binding = nil
CHECK(caught, "CONTROL: an invented name IS detected")

print(("== %d checks, %d failures =="):format(checks, fails))
os.exit(fails == 0 and 0 or 1)
