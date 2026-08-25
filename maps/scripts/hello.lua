-- Smoke script for the Lua runtime.
--
-- Nothing is bound to the world yet: these slices are the VM, its budgets, its
-- trust profile and the fx type. What this proves is that a script loads, runs,
-- reaches the machine under the default trust model, and can name a number the
-- simulation will accept.
print("hello from " .. _VERSION .. ", jit=" .. tostring(jit and jit.status()))

local f = io.open("/dev/null", "r")
print("filesystem reachable: " .. tostring(f ~= nil))
if f then f:close() end
print("ffi reachable: " .. tostring((pcall(require, "ffi"))))

-- The fx type. Exact constructors only; a bare 8.3 would be refused at the
-- call site, which is the point.
local speed = fx.parse("8.25")
local half  = fx.frac(1, 2)
print("speed=" .. tostring(speed) .. " half=" .. tostring(half) ..
      " product=" .. tostring(speed * half))
print("a position: " .. tostring(vec3.new(10, 20, fx.parse("2.5"))))
