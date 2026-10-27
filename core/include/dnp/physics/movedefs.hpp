#pragma once
//
// MOVEMENT STATES, DEFINED BY DATA (docs/MOVEMENT.md).
//
// A body is always in one movement state. State 0 is `default`: the controller
// as it always was -- walking, jumping, climbing, ledges, flight, hover. States
// 1..8 are the module's, written as `movestate` lines in its module.txt: a
// group, which says what the stick moves you through, and the numbers that
// differ from the player's own. Sliding is walking that will not stop; wading
// is slow walking; a stun is no control at all.
//
// Ways from one state to another are `transition` lines: from a state (or any)
// to a state, when every condition holds, doing at most one thing as it happens.
// They are taken at one point in the tick, before anything moves, and a state's
// own come before `any`'s, in file order; the first that holds wins.
//
// In the CORE, and parsed by the core, because both sides run it: the server
// sends the module's text at join (MOVE_DEFS) and the client's core reads it
// with this same parser, so the state a client predicts is the state the
// server will say. Fixed arrays, no allocation, no floats -- as the rest of it.
//
#include "dnp/math/fixed.hpp"
#include "dnp/physics/params.hpp"

namespace dnp::physics {

inline constexpr u8 MAX_MOVE_STATES = 9;          // `default` and eight of the module's
inline constexpr u8 MAX_MOVE_TRANSITIONS = 32;
inline constexpr u8 MAX_MOVE_CONDITIONS = 4;
inline constexpr u8 MOVE_NAME_LEN = 16;           // with its terminating NUL
inline constexpr u8 MOVE_ANY = 0xFF;              // a transition from any state
inline constexpr u8 MAX_METER_RULES = 16;
inline constexpr u8 MAX_LAYERED_MOVES = 8;
inline constexpr u8 NO_METER = 0xFF;

// What the stick moves you through.
enum class MoveGroup : u8 {
    Default = 0,   // state 0 only: the controller as it always was
    Floor,         // along the ground, as walking does; in the air as a fall does
    Air,           // as in the air, all the time: a glide, a slow fall
    Liquid,        // in 3D: the stick on the level, JUMP up and CROUCH down
    None,          // not at all: stunned, frozen, a scene
};

// Flags a state carries, read by the rest of the engine.
namespace move_flag {
inline constexpr u16 NO_JUMP          = 1u << 0;
inline constexpr u16 NO_DASH          = 1u << 1;
inline constexpr u16 NO_ATTACK        = 1u << 2;
inline constexpr u16 NO_USE           = 1u << 3;
inline constexpr u16 NO_CROUCH        = 1u << 4;
inline constexpr u16 WEAPON_HIDDEN    = 1u << 5;
inline constexpr u16 COUNTS_AS_GROUND = 1u << 6;   // jumps and coyote time come back, as on the ground
inline constexpr u16 INVULNERABLE     = 1u << 7;
inline constexpr u16 OFF              = 1u << 8;   // starts switched off: a script turns it on
}  // namespace move_flag

// Which of a state's numbers it sets; the rest are the player's own.
namespace move_has {
inline constexpr u8 SPEED    = 1u << 0;   // the player's move_speed
inline constexpr u8 ACCEL    = 1u << 1;   // ground_accel (air_accel in the air)
inline constexpr u8 FRICTION = 1u << 2;   // friction
inline constexpr u8 GRAVITY  = 1u << 3;   // gravity_scale
inline constexpr u8 BUOYANCY = 1u << 4;   // its own: upward, units/s^2 (none)
inline constexpr u8 JUMP     = 1u << 5;   // jump_impulse
}  // namespace move_has

struct MoveStateDef {
    char name[MOVE_NAME_LEN]{};
    char anim[MOVE_NAME_LEN]{};   // its animation condition; the renderer's
    MoveGroup group = MoveGroup::Default;
    u16 flags = 0;
    u8  has = 0;
    fx  speed{}, accel{}, friction{}, gravity{}, buoyancy{}, jump{};
};

// The buttons a condition can name: the controller's (act::), and the vertical
// axis read as two more -- CROUCH when it is down, UP when it is up -- because
// crouching is that axis, not a button.
namespace move_button {
inline constexpr u16 CROUCH = 1u << 14;
inline constexpr u16 UP     = 1u << 15;
}  // namespace move_button

// What a tick's buttons are to the movement states: the controller's own, and
// the vertical axis as CROUCH or UP. One expression, for the controller and for
// the client deriving last tick's (sim::move_buttons_of_input).
inline u16 move_buttons(const MoveCommand& cmd) noexcept {
    return static_cast<u16>(cmd.buttons | (cmd.up < fx::zero() ? move_button::CROUCH : 0) |
                            (cmd.up > fx::zero() ? move_button::UP : 0));
}

enum class MoveCond : u8 {
    None = 0,
    Grounded, Airborne, Rising, Falling,
    Pressed, Held, Released,        // arg: the button's bit
    SpeedAbove, SpeedBelow,         // value: units/s, horizontally
    After,                          // arg: ticks in the state
    FloorTag,                       // arg: the tag of the sector stood on
    DepthAtLeast,                   // arg: 1 feet, 2 waist, 3 eyes (geom::liquid_depth)
    DepthNone,                      // out of every liquid
    DepthBelow,                     // arg: less deep than this
    MeterEmpty, MeterFull,          // arg: the meter (value: its max, for full)
    MeterBelow, MeterAbove,         // arg: the meter; value: the level
    Asked,                          // a script asked for the state this goes to
    TouchingWall,                   // last tick's move met a wall
};

struct MoveCondition {
    MoveCond kind = MoveCond::None;
    u32 arg = 0;
    fx  value{};
};

enum class MoveAction : u8 {
    None = 0,
    Jump,                     // the new state's jump, straight up
    Stop,                     // all velocity gone
    ImpulseUp,                // value: units/s added up
    ImpulseFacing,            // value: units/s added the way the body aims
    ImpulseBack,              // value: units/s added the other way
    MeterSet,                 // meter: which; value: to what
    MeterAdd,                 // meter: which; value: how much (less than 0 takes)
    StartMove,                // meter: the layered move's index
};

struct MoveTransition {
    u8 from = 0;              // a state, or MOVE_ANY
    u8 to = 0;
    u8 cond_count = 0;
    MoveCondition cond[MAX_MOVE_CONDITIONS]{};
    MoveAction action = MoveAction::None;
    u8 meter = 0;             // for MeterSet / MeterAdd; the move for StartMove
    fx value{};
};

// A METER: a number per body, 0..max, that fills or drains by itself at
// `rate` a second (+ fills, - drains) -- breath, stamina. Its rate can depend
// on the state and on conditions (MeterRule); transitions read it and change it.
struct MeterDef {
    char name[MOVE_NAME_LEN]{};
    fx max{}, start{}, rate{};
};

// `meter <name> in <state|any> rate <v> [when <conditions>]`: while it holds,
// the meter's rate is this one. A state's own rules first, then `any`'s, in
// file order; the first that holds wins, and with none the meter's own rate.
struct MeterRule {
    u8 meter = 0;
    u8 in = 0;                // a state, or MOVE_ANY
    u8 cond_count = 0;
    MoveCondition cond[MAX_MOVE_CONDITIONS]{};
    fx rate{};
};

// A LAYERED MOVE: for a time, on top of whatever state runs and surviving a
// change of state, it holds one axis -- the level (`facing`, `back`) or the
// vertical (`up`, `down`). `set` holds the velocity there at `speed`; `add`
// adds `speed` once as it starts and keeps the state's steering off that axis
// for its time. One per axis: a new one replaces the running one when its
// priority is no lower, and otherwise does not start. It starts by itself when
// its conditions hold (`when`), from a transition's `do move`, or a script.
enum class MoveMix : u8 { Add, Set };
enum class MoveDir : u8 { Facing, Back, Up, Down };

struct LayeredMoveDef {
    char name[MOVE_NAME_LEN]{};
    MoveMix mix = MoveMix::Add;
    MoveDir dir = MoveDir::Facing;
    fx  speed{};
    u16 ticks = 0;            // 0: an `add` that is only its impulse
    u8  priority = 0;
    u8  cost_meter = NO_METER;
    fx  cost{};
    u8  cond_count = 0;       // 0: it starts only when something starts it
    MoveCondition cond[MAX_MOVE_CONDITIONS]{};
    bool off = false;         // starts switched off: a script turns it on
};

struct MoveDefs {
    u8 state_count = 1;       // `default` is always state 0
    MoveStateDef states[MAX_MOVE_STATES]{};
    u8 transition_count = 0;
    MoveTransition transitions[MAX_MOVE_TRANSITIONS]{};
    u8 meter_count = 0;
    MeterDef meters[MAX_MOVE_METERS]{};
    u8 meter_rule_count = 0;
    MeterRule meter_rules[MAX_METER_RULES]{};
    u8 move_count = 0;
    LayeredMoveDef moves[MAX_LAYERED_MOVES]{};
    // Whether anything asks how deep a body stands -- a `depth` condition, or a
    // liquid state -- so the controller measures it only then.
    bool uses_depth = false;
};

// Read a module's movement text: every `movestate` and `meter` declaration,
// then every `move`, then every `transition` line and meter rule (so a line
// may name a state, a meter or a move declared after it).
// Other lines are not ours and are skipped. On the first refusal, false, with
// the line number and the reason in `err`, and `out` reset to `default` alone:
// a half-read set would move one client differently from the server.
bool parse_move_defs(MoveDefs& out, const char* text, u32 len, char* err, u32 err_cap) noexcept;

// A state by name, or -1.
i32 move_state_index(const MoveDefs& defs, const char* name) noexcept;

// A meter by name, or -1.
i32 meter_index(const MoveDefs& defs, const char* name) noexcept;

// A layered move by name, or -1.
i32 layered_move_index(const MoveDefs& defs, const char* name) noexcept;

}  // namespace dnp::physics
