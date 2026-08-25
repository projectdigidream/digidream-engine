-- Smoke script for the Lua runtime.
--
-- Nothing is bound to the world yet: this slice is the VM, its budgets and its
-- trust profile. What it proves is that a script loads, runs, and can reach the
-- machine under the default profile -- which is the decided trust model.
print("[lua] hello from " .. _VERSION .. ", jit=" .. tostring(jit and jit.status()))

local f = io.open("/dev/null", "r")
print("[lua] filesystem reachable: " .. tostring(f ~= nil))
if f then f:close() end

local ok, ffi = pcall(require, "ffi")
print("[lua] ffi reachable: " .. tostring(ok))
