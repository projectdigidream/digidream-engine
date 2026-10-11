-- A missile with a `damage` field: never read -- its damage is what it does on
-- arrival, an `on_hit` action -- so the loader refuses it, saying where it goes.
mobs.load{
  missiles = {
    dart = { prop = "fireball", speed = "10", life = 100, damage = 12 },
  },
  mobs = {
    dummy = {
      hp = 10, initial = "idle",
      states = { idle = { anim = 0, move = "stop", face = "hold" } },
    },
  },
}
