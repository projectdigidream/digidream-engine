-- The control for maps/mobs/bad/pain_time_*.lua: the same monster, written
-- right, loads. Without it the two refusals could be refusing anything.
mobs.load{
  mobs = {
    dummy = {
      hp = 10, initial = "idle", pain_chance = 128,
      pain_time = { per_damage = "0.5", max = 30 },
      states = {
        idle = { anim = 0, move = "stop", face = "hold" },
        pain = { anim = 3, move = "stop", face = "hold", ticks = 10, next = "idle" },
      },
    },
  },
}
