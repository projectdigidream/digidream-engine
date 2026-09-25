#pragma once
//
// The per-entity parameter table (design §7.2).
//
// This is the ONLY scriptable surface of the physics core. Effects, items and
// gameplay Lua tune movement by editing these values and by invoking engine
// triggers; they never reimplement the integrator, because the client has to be
// able to run the same maths without ever seeing server Lua.
//
// A fixed-schema struct, deliberately not a key/value map. It is hashed every
// tick for the determinism gate, replicated, and bound to Lua, and a map's
// iteration order is not something the simulation may depend on.
//
#include "dnp/math/fixed.hpp"
#include "dnp/math/trig.hpp"
#include "dnp/math/vec.hpp"
#include "dnp/sim/constants.hpp"

namespace dnp::physics {

using namespace dnp;

// design §7.4. Standard physics and every ability in §7.1.1 live inside the
// `Standard` branch, so `controller_mode != Standard` gates all of them at once
// and no ability needs its own exclusion logic.
enum class ControllerMode : u8 {
    Standard = 0,
    Rail = 1,
    Wire = 2,
    VehicleDriver = 3,
    VehiclePassenger = 4
};

struct MoveParams {
    // --- body ---
    fx radius = fx::from_frac(1, 2);
    fx height = fx::from_int(2);

    // --- locomotion ---
    fx move_speed    = fx::from_int(8);
    fx ground_accel  = fx::from_int(80);
    fx air_accel     = fx::from_int(24);
    fx friction      = fx::from_int(60);
    fx gravity_scale = fx::one();
    fx jump_impulse  = fx::from_int(9);

    // How fast a body can turn, in TURNS PER SECOND.
    //
    // A parameter rather than a constant, and a rate rather than a snap: an
    // entity that faces a new direction the instant its input changes has no
    // weight, and cannot be outmanoeuvred. 4 turns per second is a quarter turn
    // in about 60 ms -- responsive, and slow enough that a swing aimed while
    // spinning lands where the body was actually pointing.
    //
    // Turns because that is the engine's own unit: brad is a turn in 2^32, so
    // a turn fraction and a brad are the same number and the conversion cannot
    // pick up a factor of pi.
    fx turn_speed    = fx::from_int(4);

    // What the movement stick is multiplied by before anything reads it.
    //
    // For CINEMATICS. A script cannot drive a player -- set_input refuses one,
    // deliberately, because a script that could write a player's input could
    // walk them into a pit and no amount of prediction would survive it -- so
    // taking the controls away has to be a RULE rather than an instruction, and
    // rules are what this table is.
    //
    // One knob rather than zeroing move_speed, jump_impulse, turn_speed and
    // max_jumps in turn, and a parameter rather than a client-side lock,
    // because parameters replicate with an epoch: the client predicts a frozen
    // player exactly, for free, and a stale set is already classified as
    // params-stale rather than as a misprediction (docs/protocol.md 8.4).
    //
    // It gates MOVEMENT, not buttons. A frozen player can still attack and use;
    // a cutscene that must suppress those zeroes attack_reach alongside.
    // Stopping the button bits properly would mean the server dropping them and
    // the client predicting the same, which is a larger change than a camera
    // needs.
    fx input_scale   = fx::one();

    u8 max_jumps     = 2;      // generalises to any count; "double jump" is a value

    // --- climbing (§7.1.1) ---
    bool climb_enabled = true;
    fx   climb_speed   = fx::from_int(4);

    // --- hover: holds altitude, keeps lateral control ---
    bool hover_enabled       = false;
    fx   hover_fuel_max      = fx::from_int(2);
    fx   hover_drain         = fx::one();            // per second
    fx   hover_gravity_scale = fx::from_frac(4, 100);
    fx   hover_vspeed_clamp  = fx::from_frac(1, 2);

    // --- flight: gravity off, input maps to full 3D velocity ---
    bool flight_enabled = false;
    fx   flight_fuel_max = fx::from_int(3);
    fx   flight_drain    = fx::one();
    fx   flight_speed    = fx::from_int(12);

    // Whether hover and flight draw from one pool is a per-server choice
    // (design §7.1.1), not an engine rule.
    bool shared_fuel = false;

    // --- dash ---
    fx dash_speed    = fx::from_int(30);
    fx dash_duration = fx::from_frac(12, 100);
    fx dash_cooldown = fx::from_frac(45, 100);
    u8 max_dashes    = 1;

    // --- grapple / whip-grip ---
    bool grapple_enabled = false;
    fx   grapple_range   = fx::from_int(14);
    fx   swing_damping   = fx::from_frac(3, 1000);   // energy lost per tick
    fx   pull_speed      = fx::from_int(18);

    // --- the swing (§7.1.1's attacks) ---
    //
    // A swing is a VOLUME MOVING THROUGH TIME, not an instant. These are what
    // shape it, and they live in the parameter table rather than in a constant
    // or a catalogue for a specific reason: the table is already replicated,
    // already predicted, and already settable from Lua. So one entity can carry
    // a spear and another a hammer, a script can lengthen a swing mid-match,
    // and the client predicts both without the engine learning what a weapon
    // is.
    //
    // Angles are TURN FRACTIONS, not radians and not degrees: a turn is the
    // engine's own unit (brad is a turn in 2^32), it wraps exactly, and 1/8 of
    // a turn reads as an eighth of the way round rather than as 0.7853981634.
    fx attack_reach     = fx::from_int(2);            // to the target's surface
    fx attack_half_arc  = fx::from_frac(1, 8);        // 45 degrees either side
    // Where the collider starts and ends, relative to the aim. The default
    // sweeps right to left through the front, which is what a swing looks like;
    // equal values make a thrust that does not rotate at all.
    fx attack_sweep_from = fx::from_frac(-1, 8);
    fx attack_sweep_to   = fx::from_frac(1, 8);
    // How long the collider is live. A swing that lands on the tick it is
    // pressed is the thing this replaces: with a duration, a target that steps
    // out of the way mid-swing is missed, which is the entire point of a
    // collider that moves.
    // 16 ticks is about 270 ms, which is a swing you can watch land. Ten was
    // technically a sweep and read as an instant -- the collider was on screen
    // for a sixth of a second, which is not long enough to see anything travel.
    u8 attack_ticks      = 16;

    // The INNER edge of the collider, as a distance from the body's surface.
    //
    // Zero is a wedge starting at the body -- a fist or a sword. Non-zero makes
    // it a ring segment, which misses anything pressed right up against you: a
    // spear or a halberd, where getting inside the weapon's reach is how you
    // beat it. This is the parameter that changes the SHAPE rather than the
    // size, and it is the one a swing had no way to express.
    fx attack_min_reach  = fx::zero();

    // Ticks before another swing may start. Zero is the old behaviour: bounded
    // only by the button being released, since the attack is edge-triggered.
    //
    // A u8 caps a cooldown at a little over four seconds, which is long for a
    // melee weapon and short for a siege engine; the day something wants the
    // latter it should be a u16 rather than a second parameter.
    u8 attack_cooldown   = 0;
};

// Runtime state the abilities carry between ticks.
//
// All of it is simulation state and all of it is hashed: a reconciliation
// replay that started from the wrong dash timer or jump-edge flag would diverge
// from the server for reasons no amount of position correction repairs.
struct MoveState {
    ControllerMode mode = ControllerMode::Standard;

    bool grounded = false;
    i32  sector   = -1;

    // The sector this entity rides (design §3.2.5). By default the last one it
    // stood on -- footprint contact, not proximity -- and it stays until the
    // entity stands on a different one. While anchored the entity moves rigidly
    // with the sector; on detach it inherits the sector's velocity once.
    i32  anchor   = -1;
    bool anchored = false;

    u8 jumps_used  = 0;
    u8 dashes_used = 0;

    // Edge detection. Held input must not re-trigger, so the previous frame's
    // button state is part of the simulation, not of the input pipeline.
    bool jump_held     = false;
    bool dash_held     = false;
    bool grapple_held  = false;
    // Interaction is edge-triggered like the rest: held INTERACT must not fire
    // a door every tick. The flag is simulation state and is hashed, so a
    // reconciliation replay resumes from the right edge rather than re-pressing.
    bool interact_held = false;
    // Edge-triggered like the rest. A held button is one swing, not sixty a
    // second -- and the rate a weapon can actually swing at is a parameter a
    // script sets, not something the engine should have an opinion about.
    bool attack_held   = false;

    // Ticks of swing left, counting down. Non-zero means the collider is live
    // this tick and sweeping; the client reads it to draw the thing.
    u8   attack_left   = 0;
    // Ticks until the next swing is allowed. Simulation state and hashed: a
    // client that thought its weapon was ready when the server did not would
    // predict a swing that never happened.
    u8   attack_cooldown_left = 0;
    // Targets already hit by the swing in progress, as a bitmask over entity
    // slots. A swing lasting ten ticks must hit a barrel once, not ten times,
    // and "once per swing" is a property of the swing rather than of the
    // script -- a script deduplicating it would have to know when a swing
    // started, which is exactly what this is.
    // One bit per agent slot, so it is as wide as the agent space. It was a
    // single u64, which was exactly MAX_AGENTS bits by coincidence rather than
    // by construction -- raising the agent count would have silently let a
    // swing hit everything above slot 63 once per tick instead of once per
    // swing.
    u64  attack_hit_agents[sim::MAX_AGENTS / 64]{};
    u64  attack_hit_props[3]{};

    fx  dash_timer{};
    fx  dash_cooldown_timer{};
    fx2 dash_dir{};

    bool climbing = false;
    fx2  climb_normal{};

    bool grappling = false;
    bool grapple_pulling = false;
    fx3  grapple_anchor{};
    fx   grapple_dist{};

    fx hover_fuel{};
    fx flight_fuel{};
    bool hovering = false;
    bool flying = false;
};

// Button bits the controller reads. These mirror proto::button, but the physics
// core does not depend on the wire format -- the caller translates.
namespace act {
inline constexpr u16 JUMP    = 1u << 0;
inline constexpr u16 DASH    = 1u << 1;
inline constexpr u16 CLIMB   = 1u << 2;
inline constexpr u16 HOVER   = 1u << 3;
inline constexpr u16 FLIGHT  = 1u << 4;
inline constexpr u16 GRAPPLE = 1u << 5;
inline constexpr u16 PULL     = 1u << 6;   // modifier: reel in instead of swing
inline constexpr u16 INTERACT = 1u << 7;   // work a switch, door or lift
inline constexpr u16 ATTACK   = 1u << 8;   // swing at whatever is in front
}  // namespace act

struct MoveCommand {
    fx2  move{};        // already quantised (protocol R1), each axis in [-1, 1]
    fx   up{};          // vertical axis, used only in flight
    brad aim{};
    u16  buttons = 0;
};

}  // namespace dnp::physics
