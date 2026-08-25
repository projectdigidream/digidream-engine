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

    u8 jumps_used  = 0;
    u8 dashes_used = 0;

    // Edge detection. Held input must not re-trigger, so the previous frame's
    // button state is part of the simulation, not of the input pipeline.
    bool jump_held    = false;
    bool dash_held    = false;
    bool grapple_held = false;

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
inline constexpr u16 PULL    = 1u << 6;   // modifier: reel in instead of swing
}  // namespace act

struct MoveCommand {
    fx2  move{};        // already quantised (protocol R1), each axis in [-1, 1]
    fx   up{};          // vertical axis, used only in flight
    brad aim{};
    u16  buttons = 0;
};

}  // namespace dnp::physics
