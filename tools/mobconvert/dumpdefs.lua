-- Dump mobs.defs and mobs.missiles as canonical JSON, for comparing two files.
--
-- THE CONTROL for the conversion in convert.mjs. Load the old file and the new
-- one with the REAL runtime and compare the tables they produce: identical
-- tables mean identical behaviour -- every number, every state, every
-- transition, in order -- which is a much stronger claim than "it loads" or
-- "it looks right", and the only one worth making about a change that rewrites
-- the behaviour of every monster in the game.
--
-- Canonical: keys sorted, arrays kept in order. So a difference in the output
-- is a real difference and not a difference in how Lua felt like iterating.
--
--   L=third_party/luajit/src/luajit
--   $L tools/mobconvert/dumpdefs.lua maps/mobs/doom_monsters.lua        > /tmp/orig.json
--   $L tools/mobconvert/dumpdefs.lua modules/doom2/maps/editor_mobs.lua > /tmp/new.json
--   diff /tmp/orig.json /tmp/new.json && echo IDENTICAL
--
-- Run from the repository root: the dofile paths below are relative, which is
-- how scripts/check_mobs.sh runs the checker too.
local target = arg[1]
dofile("tools/mobcheck/stub.lua")
dofile("maps/scripts/mob_vocab.lua")
dofile("maps/scripts/mobs.lua")
assert(loadfile(target))()

local function enc(v, out)
  local t = type(v)
  if t == "table" then
    local keys = {}
    for k in pairs(v) do keys[#keys+1] = k end
    local allnum = true
    for _, k in ipairs(keys) do if type(k) ~= "number" then allnum = false end end
    if allnum and #keys == #v then
      out[#out+1] = "["
      for i, x in ipairs(v) do if i > 1 then out[#out+1] = "," end enc(x, out) end
      out[#out+1] = "]"
    else
      table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
      out[#out+1] = "{"
      for i, k in ipairs(keys) do
        if i > 1 then out[#out+1] = "," end
        out[#out+1] = string.format("%q", tostring(k)) .. ":"
        enc(v[k], out)
      end
      out[#out+1] = "}"
    end
  elseif t == "string" then out[#out+1] = string.format("%q", v)
  elseif t == "number" then out[#out+1] = tostring(v)
  elseif t == "boolean" then out[#out+1] = tostring(v)
  else out[#out+1] = '"<' .. t .. '>"' end
end

local out = {}
enc({ defs = mobs.defs, missiles = mobs.missiles }, out)
print(table.concat(out))
