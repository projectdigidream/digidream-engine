//
// THIS CLIENT'S OWN SOUNDS, played the moment it predicts its own action.
//
// The server sends every sound, this player's own included -- but that copy
// arrives a round trip after the jump or the shot, which is a delay a player
// hears. So the game tells this client, ahead of time (SOUND_PREDICT,
// docs/protocol.md §5.7), what to play on which of its own actions, and the
// server marks its copy PREDICTED so this client drops it.
//
// The rules here are the SAME rules the server's copy is made by
// (maps/scripts/player_sounds.lua for jumping and landing, the weapon's phases
// for shots), read off the same predicted state: a jump is the jump count going
// up (so a double jump counts), a landing is reaching the ground falling faster
// than the entry's threshold,
// a swing is the attack timer starting. A shot is a press (or, held, a repeat)
// no sooner than the weapon fires again and only while it has shots -- the one
// approximation, because the weapon's phase machine runs on the server alone.
//
// Pure: it takes a state and says what to play. main.js plays it.
//
import { PREDICT } from '../proto.js'
import { BUTTON } from '../proto.js'

export class OwnSounds {
  constructor () {
    this.groups = new Map()     // group hash -> entries
    this.prev = null
    this.lastFire = new Map()   // entry -> tick it last fired
    // What the last step fired, as { group, trigger }: so the view model can
    // kick the gun on the same predicted shot the sound is played for.
    this.fired = []
  }

  /** A group's entries replaced (one SOUND_PREDICT message). */
  set (group, entries) {
    this.groups.set(group >>> 0, entries.map((e) => ({ ...e })))
  }

  clear () { this.groups.clear(); this.prev = null; this.lastFire.clear() }

  /**
   * One step of this client's predicted state. Returns the sound hashes to
   * play now.
   *
   * @param s  { grounded, velZ (world units a second), jumps, attackLeft, buttons, tick }
   */
  step (s) {
    const out = []
    this.fired = []
    const p = this.prev
    this.prev = { ...s }
    if (!p) return out
    for (const [group, entries] of this.groups) {
      for (const e of entries) {
        if (this.#fires(e, p, s)) {
          out.push(e.sound >>> 0)
          this.fired.push({ group, trigger: e.trigger })
          this.lastFire.set(e, s.tick)
          if (e.shots > 0) e.shots--
        }
      }
    }
    return out
  }

  #fires (e, p, s) {
    switch (e.trigger) {
      case PREDICT.JUMP:
        return (s.jumps | 0) > (p.jumps | 0)
      case PREDICT.LAND:
        return !p.grounded && s.grounded && p.velZ <= -(e.param / 16)
      case PREDICT.SWING:
        return p.attackLeft === 0 && s.attackLeft > 0
      case PREDICT.PRIMARY: case PREDICT.PRIMARY_HELD:
      case PREDICT.SECONDARY: case PREDICT.SECONDARY_HELD: {
        if (e.shots === 0) return false
        const primary = e.trigger === PREDICT.PRIMARY || e.trigger === PREDICT.PRIMARY_HELD
        const held = e.trigger === PREDICT.PRIMARY_HELD || e.trigger === PREDICT.SECONDARY_HELD
        const bit = primary ? BUTTON.ATTACK_PRIMARY : BUTTON.ATTACK_SECONDARY
        const down = (s.buttons & bit) !== 0
        const was = (p.buttons & bit) !== 0
        if (!down || (!held && was)) return false
        const last = this.lastFire.get(e)
        return last === undefined || s.tick - last >= e.param
      }
    }
    return false
  }
}
