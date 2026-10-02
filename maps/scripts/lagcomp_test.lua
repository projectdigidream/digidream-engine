-- LAG COMPENSATION'S FIXTURE (server/edge/test/lagcomp.test.js).
--
-- One weapon that puts one exact ray where the player looks, and one body that
-- walks to and fro across the room in front of them. Every shot is reported on
-- stdout -- HIT when the ray met the walker, MISS when it did not -- so the
-- test can count them without a protocol for it.
--
-- Loaded after mob_vocab.lua, mobs.lua, weapon_vocab.lua and weapons.lua: the
-- shot is the mob runtime's hitscan, as every player's is.

weapons.load {
  weapons = {
    rail = {
      label = "Rail", order = 1, initial = "ready",
      phases = {
        ready = { transitions = { { when = "button_pressed", to = "fire" } } },
        fire = { ticks = 6, next = "ready", sequence = {
          { at = 0, action = "hitscan", damage = 1, range = fx.int(30), spread = 0, tracer = 1 } } },
      },
    },
  },
}
weapons.initial_weapons = { "rail" }

local walker = nil
local dir = 1

mobs.on_shot = function(_, target)
  local hit = target ~= nil and walker ~= nil and target:index() == walker:index()
  print(hit and "[lagcomp] HIT" or "[lagcomp] MISS")
end

function on_tick()
  if walker == nil then
    walker = dnp.spawn_enemy("zombieman", vec3.new(fx.int(4), fx.int(22), fx.int(0)))
    return
  end
  if not walker:valid() then return end
  local p = walker:pos()
  if p.x > fx.int(16) then dir = -1 elseif p.x < fx.int(4) then dir = 1 end
  walker:set_input(fx.int(dir), fx.int(0), dir > 0 and 0 or 2147483648)
end
