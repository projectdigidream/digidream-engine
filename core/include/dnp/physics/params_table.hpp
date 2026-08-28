#pragma once
//
// The movement-parameter table, addressed by wire id (docs/protocol.md §8.4).
//
// This lives in core/, not in the server's Lua layer, and the reason is worth
// stating because the plan originally put it in the wrong place: `field_id` is
// a value that travels on the wire, so BOTH sides need the same mapping. The
// server encodes a PARAM_UPDATE by id and the client applies it by id. A table
// that existed only on the server would be half a protocol.
//
// **The id is the index into PARAM_TABLE, and it is a wire contract.**
// Appending a field is safe and backward-compatible. Reordering or removing one
// silently repoints every id after it -- a client would apply `jump_impulse` to
// `friction` and mispredict in a way that looks like a physics bug. Do neither
// without bumping PROTOCOL_VERSION.
//
// Deliberately no reflection and no macro cleverness: C++17 has no reflection,
// and a macro table is harder to read than the thing it generates. What keeps
// it honest instead is the static_assert on sizeof(MoveParams) below.
//
#include <cstddef>

#include "dnp/physics/params.hpp"

namespace dnp::physics {

enum class ParamType : u8 {
    Fx   = 0,
    Bool = 1,
    U8   = 2,
};

struct ParamDesc {
    const char* name;
    ParamType   type;
    u16         offset;
};

#define DNP_PARAM(field, type) \
    ParamDesc{ #field, type, static_cast<u16>(offsetof(MoveParams, field)) }

// Order defines the wire ids. Append only.
inline constexpr ParamDesc PARAM_TABLE[] = {
    DNP_PARAM(radius,               ParamType::Fx),     //  0
    DNP_PARAM(height,               ParamType::Fx),     //  1
    DNP_PARAM(move_speed,           ParamType::Fx),     //  2
    DNP_PARAM(ground_accel,         ParamType::Fx),     //  3
    DNP_PARAM(air_accel,            ParamType::Fx),     //  4
    DNP_PARAM(friction,             ParamType::Fx),     //  5
    DNP_PARAM(gravity_scale,        ParamType::Fx),     //  6
    DNP_PARAM(jump_impulse,         ParamType::Fx),     //  7
    DNP_PARAM(max_jumps,            ParamType::U8),     //  8
    DNP_PARAM(climb_enabled,        ParamType::Bool),   //  9
    DNP_PARAM(climb_speed,          ParamType::Fx),     // 10
    DNP_PARAM(hover_enabled,        ParamType::Bool),   // 11
    DNP_PARAM(hover_fuel_max,       ParamType::Fx),     // 12
    DNP_PARAM(hover_drain,          ParamType::Fx),     // 13
    DNP_PARAM(hover_gravity_scale,  ParamType::Fx),     // 14
    DNP_PARAM(hover_vspeed_clamp,   ParamType::Fx),     // 15
    DNP_PARAM(flight_enabled,       ParamType::Bool),   // 16
    DNP_PARAM(flight_fuel_max,      ParamType::Fx),     // 17
    DNP_PARAM(flight_drain,         ParamType::Fx),     // 18
    DNP_PARAM(flight_speed,         ParamType::Fx),     // 19
    DNP_PARAM(shared_fuel,          ParamType::Bool),   // 20
    DNP_PARAM(dash_speed,           ParamType::Fx),     // 21
    DNP_PARAM(dash_duration,        ParamType::Fx),     // 22
    DNP_PARAM(dash_cooldown,        ParamType::Fx),     // 23
    DNP_PARAM(max_dashes,           ParamType::U8),     // 24
    DNP_PARAM(grapple_enabled,      ParamType::Bool),   // 25
    DNP_PARAM(grapple_range,        ParamType::Fx),     // 26
    DNP_PARAM(swing_damping,        ParamType::Fx),     // 27
    DNP_PARAM(pull_speed,           ParamType::Fx),
    DNP_PARAM(attack_reach,         ParamType::Fx),
    DNP_PARAM(attack_half_arc,      ParamType::Fx),
    DNP_PARAM(attack_sweep_from,    ParamType::Fx),
    DNP_PARAM(attack_sweep_to,      ParamType::Fx),
    DNP_PARAM(attack_ticks,         ParamType::U8),
    // APPENDED, never inserted. The index into this table IS the wire id, so
    // putting a new parameter in the middle silently renumbers every one after
    // it and a client applies the wrong field to the wrong value. That has
    // happened once, with turn_speed.
    DNP_PARAM(attack_min_reach,     ParamType::Fx),
    DNP_PARAM(attack_cooldown,      ParamType::U8),
    // Appended, not inserted beside jump_impulse where the field lives: this
    // table's ORDER is the wire format, and the struct's order is not. Putting
    // it next to its neighbour shifted the id of every parameter after it.
    DNP_PARAM(turn_speed,           ParamType::Fx),     // 28
};

#undef DNP_PARAM

inline constexpr u16 PARAM_COUNT = static_cast<u16>(sizeof(PARAM_TABLE) / sizeof(PARAM_TABLE[0]));

// The drift guard.
//
// A table addressed by index cannot notice that MoveParams grew a field -- it
// just keeps describing the old struct, and the new field is silently
// unscriptable and unreplicated. sizeof() is the one property that changes
// whenever a field is added, removed or reordered, so pinning it turns a silent
// omission into a build failure that says where to go.
//
// If this fires: add the new field to PARAM_TABLE (at the END -- ids are a wire
// contract) and update the number here.
static_assert(sizeof(MoveParams) == 280,
              "MoveParams changed. Append the new field(s) to PARAM_TABLE in this "
              "file -- appending is safe, reordering breaks the wire format -- "
              "then update this size. See docs/protocol.md 8.4.");

// offsetof is only defined for standard-layout types.
static_assert(__is_standard_layout(MoveParams), "MoveParams must stay standard-layout");

// Read a field as its raw integer representation.
inline i64 param_get_raw(const MoveParams& p, u16 id) noexcept {
    if (id >= PARAM_COUNT) return 0;
    const ParamDesc& d = PARAM_TABLE[id];
    const auto* base = reinterpret_cast<const unsigned char*>(&p);
    switch (d.type) {
        case ParamType::Fx:   return reinterpret_cast<const fx*>(base + d.offset)->raw;
        case ParamType::Bool: return *reinterpret_cast<const bool*>(base + d.offset) ? 1 : 0;
        case ParamType::U8:   return *reinterpret_cast<const u8*>(base + d.offset);
    }
    return 0;
}

// Write a field from its raw integer representation. Returns false for an
// unknown id -- which is not an error but a version skew: a newer server may
// name a field this build does not have, and ignoring it is better than
// refusing the whole update.
inline bool param_set_raw(MoveParams& p, u16 id, i64 raw) noexcept {
    if (id >= PARAM_COUNT) return false;
    const ParamDesc& d = PARAM_TABLE[id];
    auto* base = reinterpret_cast<unsigned char*>(&p);
    switch (d.type) {
        case ParamType::Fx:
            *reinterpret_cast<fx*>(base + d.offset) = fx::from_raw(raw);
            return true;
        case ParamType::Bool:
            *reinterpret_cast<bool*>(base + d.offset) = raw != 0;
            return true;
        case ParamType::U8:
            // Clamped rather than truncated: a script asking for 300 jumps gets
            // 255, not 44.
            *reinterpret_cast<u8*>(base + d.offset) =
                static_cast<u8>(raw < 0 ? 0 : (raw > 255 ? 255 : raw));
            return true;
    }
    return false;
}

// Wire id for a name, or -1. Linear scan over 29 entries, called at bind time
// and never in the tick path.
inline i32 param_id_by_name(const char* name) noexcept {
    if (!name) return -1;
    for (u16 i = 0; i < PARAM_COUNT; ++i) {
        const char* a = PARAM_TABLE[i].name;
        const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return static_cast<i32>(i);
    }
    return -1;
}

}  // namespace dnp::physics
