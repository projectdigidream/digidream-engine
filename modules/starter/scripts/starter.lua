-- The starter module's own script: what a player's swing costs a monster.
--
-- The engine reports that a swing reached something, and mobs.lua's on_attack
-- prices it (docs/INTERACTION.md): a monster's from its `swing`, a player's
-- from the weapon in hand -- and, with no weapons, `mobs.player_damage`. This
-- module has no weapons, so it says that number. It used to add an on_attack
-- of its own, which ran BESIDE mobs.lua's (hooks are a list): every swing that
-- landed cost twice, a monster's bite on the player included.
if mobs ~= nil then mobs.player_damage = 10 end
