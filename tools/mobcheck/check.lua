-- Load a mob table and report every problem, or say it is clean.
--
--   luajit tools/mobcheck/check.lua maps/mobs/doom.lua
--
-- Exit code 1 if anything is wrong, so scripts/test.sh can use it as a gate.
local target = arg[1]
if target == nil then
  io.stderr:write("usage: check.lua <mob-table.lua>\n"); os.exit(2)
end

dofile("tools/mobcheck/stub.lua")
dofile("maps/scripts/mob_vocab.lua")
dofile("maps/scripts/mobs.lua")

-- The table CALLS mobs.load, exactly as it does when the server loads it as one
-- of a map's scripts. Running it any other way here would test a path no map
-- takes.
local ok, problems = true, {}
local chunk, err = loadfile(target)
if chunk == nil then
  io.stderr:write("cannot read " .. target .. ": " .. tostring(err) .. "\n")
  os.exit(2)
end
local real_load = mobs.load
mobs.load = function(data) ok, problems = real_load(data); return ok, problems end
local fine, raised = pcall(chunk)
mobs.load = real_load
if not fine then
  io.stderr:write(target .. ": " .. tostring(raised) .. "\n")
  os.exit(2)
end
for _, p in ipairs(problems) do print("  " .. p) end
local nmobs = 0
for _ in pairs(mobs.defs or {}) do nmobs = nmobs + 1 end
local nmis = 0
for _ in pairs(mobs.missiles or {}) do nmis = nmis + 1 end
print(("%s: %d mobs, %d missiles, %d problems"):format(target, nmobs, nmis, #problems))
os.exit(ok and 0 or 1)
