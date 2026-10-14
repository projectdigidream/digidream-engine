-- The starter module's own script: what a player's swing costs a monster.
--
-- The engine reports that a swing reached something; what that means is
-- script (docs/INTERACTION.md). modules/doom2 gets this from weapons.lua;
-- this module has no weapons and needs one line of it.
function on_attack(attacker, target)
  if mobs ~= nil and mobs.hurt ~= nil and target ~= nil then mobs.hurt(target, 10, attacker) end
end
