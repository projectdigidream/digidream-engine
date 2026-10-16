#pragma once
//
// THE WEAPON MACHINE, in the core (docs/WEAPON_PREDICTION.md).
//
// A player's weapon -- its phases, their sequences and transitions, its ammo and
// what it is holding -- run here, deterministically, so the server and the
// client's wasm step the SAME machine from the same input and the client can
// predict its own shots exactly. It is the rules of engine/maps/scripts/
// weapons.lua's tick_player, one for one; that file stays the authority on the
// LANGUAGE (it validates a weapon table against weapon_vocab.lua and compiles
// it into the WeaponTable below), and its own interpreter is kept, behind
// DNP_WEAPONS_LUA, as the other half of the parity test.
//
// WHAT THIS DOES NOT DO is touch the world. A hitscan, a launch, a blast, a
// sound, a visual effect: each becomes a WeaponEvent, with everything the core
// decided about it (the per-pellet directions, the auto-aim), and the server's
// Lua carries it out with today's implementations. The client turns its OWN
// events into a tracer, a flash and a sound the moment it predicts them.
//
// Fixed sizes throughout: no allocation in the tick path, and a table that is
// the same bytes on every build so it can be hashed and sent.
//
#include "dnp/math/fixed.hpp"
#include "dnp/proto/bytes.hpp"
#include "dnp/sim/state_hash.hpp"

namespace dnp::sim {

inline constexpr u32 MAX_WEAPONS          = 24;
inline constexpr u32 MAX_WEAPON_PHASES    = 16;
inline constexpr u32 MAX_PHASE_STEPS      = 16;   // sequence entries
inline constexpr u32 MAX_PHASE_TRANSITIONS = 8;
inline constexpr u32 MAX_TRANSITION_CONDS = 4;    // an AND-list
inline constexpr u32 MAX_WEAPON_VARS      = 4;
inline constexpr u32 MAX_WEAPON_COOLDOWNS = 8;    // the whole table's, see WeaponDef::cool_slot
inline constexpr u32 MAX_AMMO_POOLS       = 8;
inline constexpr u32 MAX_WEAPON_EVENTS    = 96;   // per tick, the whole world
inline constexpr u32 MAX_PELLETS          = 32;
inline constexpr u32 WEAPON_NAME_LEN      = 23;
inline constexpr u32 WEAPON_HOPS          = 5;    // weapons.lua's MAX_HOPS

inline constexpr u8  NO_WEAPON = 0xFF;
inline constexpr u8  NO_PHASE  = 0xFF;
inline constexpr u8  NO_COOL   = 0xFF;
inline constexpr i8  NO_POOL   = -1;
inline constexpr u16 PHASE_WAITS = 0xFFFF;        // a phase with no `ticks`

// The vocabulary's conditions (weapon_vocab.lua), in its order. `Lua` cannot
// run here: a weapon that uses it is not compiled, and stays on the Lua path.
enum class WCond : u8 {
    Always = 0, Pressed, Released, Held, HeldFor, HasAmmo, NoAmmo,
    PhaseElapsed, Chance, VarAtLeast, VarIs, Grounded, Moving, Falling,
    CooldownReady,
};

// And its actions. The first group the core performs itself; the second it
// REPORTS, as a WeaponEvent, for the world to carry out.
enum class WAct : u8 {
    Consume = 0, SetVar, AddVar, Goto, SetAnim, SwitchIfEmpty, Impulse, SetCooldown,
    // reported:
    Hitscan = 16, Launch, DamageArea, Telegraph, Vfx, Sound,
};
inline constexpr bool reported(WAct a) noexcept { return static_cast<u8>(a) >= 16; }

enum class WTowards : u8 { Facing = 0, Away = 1, Random = 2 };

inline constexpr u8 TRIGGER_PRIMARY = 0;
inline constexpr u8 TRIGGER_SECONDARY = 1;

struct WCondDef {
    WCond kind = WCond::Always;
    u8    trigger = TRIGGER_PRIMARY;   // which button, for the button conditions
    u8    var = 0;                     // a var's slot
    bool  flag = false;                // grounded's `value`; `has` = a value given
    bool  has = false;
    bool  negate = false;              // holds when the condition does not
    i32   n = 0;                       // ticks, n, percent, value
    fx    speed{};                     // moving / falling; `has` says given
};

struct WTransition {
    u8       count = 0;
    WCondDef conds[MAX_TRANSITION_CONDS]{};
    u8       to = NO_PHASE;
};

// What a `launch` fires, as much as a predicting client needs to fly it itself
// (docs/WEAPON_PREDICTION.md): its body, how it moves, and what its impact
// draws. The server's own flight is mobs.lua's, from the same numbers. A kind of
// 0 is a missile nobody described, which a client does not predict.
inline constexpr u32 MAX_MISSILE_VFX = 2;
inline constexpr u16 NO_ANIM = 0xFFFF;
struct WMissile {
    u16 kind = 0;                 // the catalogue prop it is drawn as
    fx  speed{};                  // units a second
    fx  radius{};                 // what it sweeps
    fx  z_offset{};               // above the shooter's feet
    fx  offset{};                 // ahead of the shooter (the launch's `offset`)
    fx  gravity{};                // units a second squared it falls by; 0 flies its slope
    // It HOMES (a `homing` turn rate). Its path depends on where the target
    // goes, which only the server sees, so a client does not fly a copy of it:
    // a ghost flying straight would be wrong for the whole flight.
    bool homes = false;
    // It BOUNCES off the map: the same reason, the server alone flies it.
    bool bounces = false;
    // `speed` is its LAUNCH speed (`throw`), so the horizontal is what the
    // aim's slope leaves of it: speed / sqrt(1 + slope^2), as mobs.lua's
    // throw_at_speed. A client flies that too.
    bool throws = false;
    u16 life = 0;                 // ticks before it expires
    u16 burst_anim = NO_ANIM;     // the state it bursts in
    u16 burst_ticks = 0;
    u8  vfx_count = 0;            // on_hit's impact effects
    u16 vfx_kind[MAX_MISSILE_VFX]{};
    fx  vfx_radius[MAX_MISSILE_VFX]{};
    // Its blast, when it shoves (a damage_area with `knockback`): what the
    // shooter's client predicts it does to the bodies around it, and to the
    // shooter too when it `shoves_owner` without hurting (sim/blast.hpp).
    bool has_blast = false;
    bool self_shove = false;
    bool blast_falloff = false;
    fx   blast_radius{};
    fx   blast_knockback{};
    fx   blast_lift{};
};

// One sequence entry. Only what the CORE reads is resolved here; a reported
// action's other arguments stay in the Lua table, which the server reads by the
// (weapon, phase, step) the event names.
struct WStep {
    u16  at = 0;
    WAct action = WAct::Consume;
    u8   var = 0;          // set_var / add_var / set_cooldown
    u8   phase = NO_PHASE; // goto
    i8   pool = NO_POOL;   // consume's `ammo`, NO_POOL = the weapon's own
    i32  value = 0;        // set_var / add_var value; consume n (0 = the weapon's per);
                           // set_cooldown's ticks
    u16  state = 0;        // set_anim
    // impulse
    fx   speed{};
    fx   vertical{};
    WTowards towards = WTowards::Facing;
    i32  jitter = 0;       // degrees; set_cooldown: up to this many extra ticks
    // hitscan: what the core needs to aim it, and what a predicting client
    // needs to draw it
    u8   count = 1;        // pellets / missiles
    i32  spread = 0;       // degrees, hitscan
    i32  arc = 0;          // degrees, launch
    i32  angle = 0;        // degrees, launch
    fx   range{};          // hitscan
    u16  effect = 0;       // hitscan's tracer, vfx's effect, telegraph's effect
    u32  sound = 0;        // sound: the name's hash, as the wire carries it
    bool predicted = false;
    bool attach = false;   // vfx
    WMissile missile{};    // launch
};

// What a swing replaces in the agent's attack parameters, field by field.
struct WSwing {
    bool present = false;
    u16  set = 0;          // a bit per field below that the table gave
    fx   reach{}, half_arc{}, sweep_from{}, sweep_to{}, min_reach{};
    fx   pitch_from{}, pitch_to{}, half_pitch{};
    u8   ticks = 0, cooldown = 0;
};
namespace swing_field {
inline constexpr u16 REACH = 1u << 0, HALF_ARC = 1u << 1, SWEEP_FROM = 1u << 2,
    SWEEP_TO = 1u << 3, MIN_REACH = 1u << 4, PITCH_FROM = 1u << 5, PITCH_TO = 1u << 6,
    HALF_PITCH = 1u << 7, TICKS = 1u << 8, COOLDOWN = 1u << 9;
}

struct WPhase {
    char name[WEAPON_NAME_LEN + 1]{};
    // The animation state the body takes on ENTERING this phase (its `anim`),
    // or NO_ANIM. Applied before the phase's steps, so a `set_anim` at 0 wins.
    u16  anim = NO_ANIM;
    u16  ticks = PHASE_WAITS;
    u8   next = NO_PHASE;            // NO_PHASE: the weapon's initial
    WSwing swing{};
    u8   step_count = 0;
    WStep steps[MAX_PHASE_STEPS]{};
    u8   transition_count = 0;
    WTransition transitions[MAX_PHASE_TRANSITIONS]{};
};

struct WeaponDef {
    char name[WEAPON_NAME_LEN + 1]{};
    i16  order = 0;
    i8   pool = NO_POOL;             // the ammunition it draws, NO_POOL = none
    u16  per = 1;
    u8   initial = 0;
    u8   aim_snap = 0;
    WSwing swing{};
    u8   var_count = 0;
    i32  vars[MAX_WEAPON_VARS]{};    // starting values
    // COOLDOWNS OUTLIVE A SWITCH. A var that `set_cooldown` or `cooldown_ready`
    // names holds a deadline (an absolute tick, as a monster's does), and a
    // weapon's vars start again whenever it is re-armed -- so switching away
    // and back would clear the cooldown. Each such var therefore has a slot in
    // the player's WeaponState::cool, which a re-arm restores it from. Derived
    // from the steps and conditions (weapon_assign_cooldowns), never sent.
    u8   cool_slot[MAX_WEAPON_VARS]{NO_COOL, NO_COOL, NO_COOL, NO_COOL};
    static_assert(MAX_WEAPON_VARS == 4, "cool_slot's initializer names every var");
    u8   phase_count = 0;
    WPhase phases[MAX_WEAPON_PHASES]{};
};

struct AmmoPool {
    char name[WEAPON_NAME_LEN + 1]{};
    u16  max = 0;
    u16  initial = 0;
};

inline constexpr u32 MAX_MASSES = 64;

struct WeaponTable {
    u8        weapon_count = 0;
    WeaponDef weapons[MAX_WEAPONS]{};
    u8        pool_count = 0;
    AmmoPool  pools[MAX_AMMO_POOLS]{};
    // What a fresh player holds: the module's W.initial_weapons, or (0) the
    // lowest-order weapon, which is weapons.lua's rule.
    u32       initial_have = 0;
    // AUTO-AIM for a player's shot (mobs.lua's M.autoaim_range and
    // M.autoaim_h): how far it looks when the shot names no range of its own,
    // and how far off the nose it corrects, in brad either way. 0 is no
    // auto-aim. A first-person body aims with its view instead.
    fx        autoaim_range{};
    u32       autoaim_cone = 0;
    // HOW HEAVY EACH MONSTER IS, by catalogue kind (mob_vocab.lua `mass`):
    // what a predicting client needs to show its own blast shoving them by
    // the server's rule (sim/blast.hpp). Not read by the machine.
    u8        mass_count = 0;
    u16       mass_kind[MAX_MASSES]{};
    u16       mass_value[MAX_MASSES]{};
    // The mass of a catalogue kind, or 0 for one nobody gave a mass.
    u16 mass_of(u16 kind) const noexcept {
        for (u32 i = 0; i < mass_count; ++i) if (mass_kind[i] == kind) return mass_value[i];
        return 0;
    }

    void clear() noexcept;
    // Content hash: what WELCOME can carry so a client running other weapons
    // is a detected condition rather than a stream of mispredictions.
    u64 hash() const noexcept;
    // The index of a weapon or pool by name, or NO_WEAPON / NO_POOL.
    u8 find(const char* name) const noexcept;
    i8 find_pool(const char* name) const noexcept;
};

// One player's weapon, as the machine sees it. Lives beside the AgentState and
// is predicted and hashed with it.
struct WeaponState {
    u32 have = 0;                    // a bit per weapon index
    u8  current = NO_WEAPON;
    u8  phase = NO_PHASE;
    u8  armed = NO_WEAPON;           // which weapon the phase machine is set up for
    // Whose swing is in force: a phase that declares one keeps it after it is
    // left, until another phase declares its own or the weapon is re-armed --
    // weapons.lua writes it on ENTRY and nothing clears it. NO_PHASE: the
    // weapon's own.
    u8  swing_phase = NO_PHASE;
    u8  prev_buttons = 0;            // last tick's triggers: bit 0 primary, 1 secondary
    u32 entered = 0;                 // the tick the phase was entered
    u32 done = 0;                    // a bit per sequence step already run
    u32 held_since[2]{};             // tick + 1 the trigger went down; 0 = up
    i32 vars[MAX_WEAPON_VARS]{};
    u16 ammo[MAX_AMMO_POOLS]{};
    // Every weapon's cooldown deadlines, by WeaponDef::cool_slot: what a
    // re-arm puts back in its vars.
    i32 cool[MAX_WEAPON_COOLDOWNS]{};
};

// Something the world must do because a weapon said so.
struct WeaponEvent {
    u32  agent = 0;
    u32  tick = 0;
    u8   weapon = 0;
    u8   phase = 0;
    u8   step = 0;
    WAct action = WAct::Hitscan;
    // THE AIM, decided here so both sides agree: the body's heading (after
    // auto-aim), the vertical slope (fx), and per pellet / missile the offset
    // from that heading, in brad.
    brad aim{};
    fx   slope{};
    bool slope_set = false;
    u8   count = 0;
    i32  offsets[MAX_PELLETS]{};
};

class WeaponEventLog {
public:
    void clear() noexcept { count_ = 0; dropped_ = 0; }
    bool push(const WeaponEvent& e) noexcept {
        if (count_ >= MAX_WEAPON_EVENTS) { ++dropped_; return false; }
        events_[count_++] = e;
        return true;
    }
    u32 count() const noexcept { return count_; }
    const WeaponEvent& at(u32 i) const noexcept { return events_[i]; }
    WeaponEvent& at_mut(u32 i) noexcept { return events_[i]; }
    u32 dropped() const noexcept { return dropped_; }
private:
    WeaponEvent events_[MAX_WEAPON_EVENTS]{};
    u32 count_ = 0;
    u32 dropped_ = 0;
};

// What a step reads about the body, and writes back.
struct WeaponBody {
    bool grounded = false;
    fx3  vel{};
    brad facing{};
    i16  pitch = 0;
    // Written by the machine: a velocity to add (impulse), an animation state
    // to set (set_anim; `anim_set` says one was), and the swing to apply.
    fx3  impulse{};
    bool anim_set = false;
    u16  anim = 0;
};

// The per-tick, per-player random stream (docs/WEAPON_PREDICTION.md 2): a
// function of the seed, the agent, the tick and the draw's own index, so the
// server and a predicting client draw the same numbers without sharing a
// stream with anything else. `lo..hi` inclusive.
i32 weapon_random(u64 seed, u32 agent, u32 tick, u32 salt, i32 lo, i32 hi) noexcept;

// THE MACHINE: one tick of one player's weapon. `buttons` is this tick's
// trigger mask (bit 0 primary, bit 1 secondary); `want` a weapon index to
// switch to, NO_WEAPON for none. Events are pushed to `log`. Returns the number
// of phase changes it made (for a test).
u32 step_weapon(const WeaponTable& t, WeaponState& s, WeaponBody& body,
                u8 buttons, u8 want, u32 tick, u32 agent, u64 seed,
                WeaponEventLog& log) noexcept;

// A player's state when first armed: weapons.lua's fresh(). The initial
// weapons held (or the lowest-order one), the highest-order of them current,
// every pool at its `initial`.
void weapon_fresh(const WeaponTable& t, WeaponState& s) noexcept;
// Every cooldown back to its weapon's declared start: part of weapon_fresh,
// and of a reset that keeps the inventory (a respawn, a new world).
void weapon_fresh_cooldowns(const WeaponTable& t, WeaponState& s) noexcept;

// Whether the current weapon can fire `n` (0: its own `per`).
bool weapon_has_ammo(const WeaponTable& t, const WeaponState& s, u8 weapon, u16 n = 0) noexcept;

// The swing in force: the phase's if it has one, else the weapon's; null if
// the weapon does not swing.
const WSwing* weapon_swing(const WeaponTable& t, const WeaponState& s) noexcept;

void hash_weapon_state(StateHasher& h, const WeaponState& s) noexcept;

// Gives each var a cooldown names its WeaponDef::cool_slot, in weapon then var
// order. False when the table names more than MAX_WEAPON_COOLDOWNS, which the
// compiler refuses (the weapons then run in Lua) and the decoder rejects.
// Both call it, so the two sides derive the same slots from the same table.
bool weapon_assign_cooldowns(WeaponTable& t) noexcept;

// The table on the wire (WEAPON_DEFS). One encoder, one decoder; the decoder
// refuses anything out of range, as the map loader does.
bool encode_weapon_table(ByteWriter& w, const WeaponTable& t) noexcept;
bool decode_weapon_table(ByteReader& r, WeaponTable& t) noexcept;

}  // namespace dnp::sim
