--
-- What a player's body sounds like: jumping, and landing hard.
--
-- A GAME script (docs/MODULES.md, "Game scripts"): what a player sounds like is
-- the player's, not one level's. Two halves, kept in one file because they
-- must agree:
--
--   1. each player's OWN client is told what to play when it predicts its own
--      jump or landing (dnp.predict_sounds), so they hear it at once;
--   2. everybody else hears the server's copy, made here by the SAME rule and
--      marked `predicted`, so the player's own client drops it.
--
-- The rule (client/src/audio/ownsounds.js reads it off the same state): a jump
-- is the body's jump count going up -- off the ground or in the air, so a
-- double jump is heard as well; a landing is reaching the ground falling faster
-- than `fall` -- harder than coming down from an ordinary jump, which leaves at
-- 9 units a second.
--
player_sounds = player_sounds or {
  jump = "player.jump",
  land = "player.oof",
  fall = fx.int(12),      -- world units a second
}
local S = player_sounds

local was = {}   -- player -> { grounded, vz, jumps }

local function key_of(e) return e:player() or e:index() end

function on_player_enter(e)
  was[key_of(e)] = nil
  local list = {}
  if S.jump then list[#list + 1] = { on = "jump", sound = S.jump } end
  if S.land then list[#list + 1] = { on = "land", sound = S.land, fall = S.fall } end
  if dnp.predict_sounds then dnp.predict_sounds(e, "move", list) end
end

function on_player_leave(player) was[player] = nil end

function on_tick()
  for _, e in ipairs(dnp.players()) do
    local k = key_of(e)
    local g = e:grounded() and true or false
    local vz = e:vel().z
    local j = e:jumps() or 0
    local p = was[k]
    if p ~= nil then
      if S.jump and j > p.jumps then
        dnp.sound(S.jump, e, { predicted = true })
      elseif S.land and not p.grounded and g and p.vz <= -S.fall then
        dnp.sound(S.land, e, { predicted = true })
      end
    end
    was[k] = { grounded = g, vz = vz, jumps = j }
  end
end
