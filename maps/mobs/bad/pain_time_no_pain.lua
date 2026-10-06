-- A pain_time on a monster with no `pain` state: nothing for it to lengthen.
-- The same table with the pain state is pain_time_ok's control below.
mobs.load{
  mobs = {
    dummy = {
      hp = 10, initial = "idle",
      pain_time = { per_damage = "0.5", max = 30 },
      states = {
        idle = { anim = 0, move = "stop", face = "hold" },
      },
    },
  },
}
