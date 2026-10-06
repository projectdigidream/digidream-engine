-- A pain_time with a misspelt field (`per_dmg`): the loader must refuse it,
-- or a flinch that was meant to grow with the hit silently never does.
mobs.load{
  mobs = {
    dummy = {
      hp = 10, initial = "idle",
      pain_time = { per_dmg = "0.5", max = 30 },
      states = {
        idle = { anim = 0, move = "stop", face = "hold" },
        pain = { anim = 3, move = "stop", face = "hold", ticks = 10, next = "idle" },
      },
    },
  },
}
