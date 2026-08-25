#pragma once
//
// Wire message definitions. Normative reference: docs/protocol.md.
//
// Everything here is a plain aggregate with fixed-capacity arrays and no heap
// use. Decoders run on data supplied by whoever is on the other end of the
// socket, so an attacker-chosen count must never become an allocation size --
// the caps below are what stops a two-byte field from asking for a gigabyte.
//
#include "dnp/proto/bytes.hpp"

namespace dnp::proto {

using namespace dnp;

inline constexpr u16 PROTOCOL_VERSION = 0;   // 0 == UNSTABLE, see docs/protocol.md
inline constexpr u16 TICK_RATE_HZ     = 60;  // protocol R9: not a server tunable

// Datagram message types (state channel).
inline constexpr u8 MSG_INPUT_BUNDLE = 0x80;
inline constexpr u8 MSG_SNAPSHOT     = 0x81;

// Control-channel message types.
inline constexpr u8 MSG_HELLO   = 0x01;
inline constexpr u8 MSG_WELCOME = 0x02;
inline constexpr u8 MSG_REJECT  = 0x03;
inline constexpr u8 MSG_MAP_DATA = 0x10;

// Decoder-side caps. These bound work and memory on hostile input; they are not
// gameplay limits.
inline constexpr u32 MAX_INPUT_FRAMES   = 16;
inline constexpr u32 MAX_ENTITIES       = 256;
inline constexpr u32 MAX_VFX            = 64;
inline constexpr u32 MAX_VISUAL_PARAMS  = 8;

// Fits inside the measured maxDatagramSize of 1211 bytes (docs/protocol.md 5.4).
inline constexpr size_t MAX_DATAGRAM_BYTES = 1200;

// --------------------------------------------------------------------- handle

// Index in the low 24 bits, generation in the high 8 (docs/protocol.md 9).
// Generation increments on destroy, so a stale handle names an entity that no
// longer exists and every operation on it is a defined no-op.
struct Handle {
    u32 raw = 0;

    static constexpr Handle make(u32 index, u8 generation) noexcept {
        return Handle{(index & 0x00FFFFFFu) | (static_cast<u32>(generation) << 24)};
    }
    static constexpr Handle none() noexcept { return Handle{0}; }

    constexpr u32 index()      const noexcept { return raw & 0x00FFFFFFu; }
    constexpr u8  generation() const noexcept { return static_cast<u8>(raw >> 24); }
    constexpr bool valid()     const noexcept { return raw != 0; }
};

constexpr bool operator==(Handle a, Handle b) noexcept { return a.raw == b.raw; }
constexpr bool operator!=(Handle a, Handle b) noexcept { return a.raw != b.raw; }

// ---------------------------------------------------------------- input frame

namespace button {
inline constexpr u16 JUMP             = 0x0001;
inline constexpr u16 DASH             = 0x0002;
inline constexpr u16 CLIMB            = 0x0004;
inline constexpr u16 HOVER            = 0x0008;
inline constexpr u16 FLIGHT           = 0x0010;
inline constexpr u16 GRAPPLE          = 0x0020;
inline constexpr u16 INTERACT         = 0x0040;
inline constexpr u16 ATTACK_PRIMARY   = 0x0080;
inline constexpr u16 ATTACK_SECONDARY = 0x0100;
inline constexpr u16 USE              = 0x0200;
inline constexpr u16 CROUCH           = 0x0400;
inline constexpr u16 RESERVED_MASK    = 0xF800;   // MUST be sent as zero
}  // namespace button

struct InputFrame {
    u16  buttons = 0;
    i16  move_x  = 0;   // Q1.15, clamped to [-1, 1]
    i16  move_y  = 0;
    brad aim{};
};

constexpr bool operator==(const InputFrame& a, const InputFrame& b) noexcept {
    return a.buttons == b.buttons && a.move_x == b.move_x &&
           a.move_y == b.move_y && a.aim == b.aim;
}

struct InputBundle {
    u32 client_tick        = 0;
    u32 ack_snapshot_tick  = 0;
    u8  frame_count        = 0;
    InputFrame frames[MAX_INPUT_FRAMES]{};   // newest first
};

// Q1.15 <-> fx. Protocol R1: the client MUST feed the QUANTISED value to its own
// prediction, not the raw analogue reading -- otherwise client and server
// simulate different inputs every tick and diverge for reasons that have nothing
// to do with the network.
// Note the SYMMETRIC clamp to +/-32767. Q1.15 can represent -1.0 exactly but
// not +1.0, so the natural range is [-32768, 32767] -- which would make full-left
// very slightly stronger than full-right. That asymmetry is imperceptible per
// tick and completely wrong in aggregate: it breaks the mirror-symmetry property
// the rounding rules exist to preserve (docs/physics-contract.md rule 4), so a
// mirrored level would not play identically to its mirror. Giving up the single
// unreachable value is the cheap half of that trade.
constexpr i16 axis_from_fx(fx v) noexcept {
    const fx c = fx_clamp(v, -fx::one(), fx::one());
    const i64 t = c.raw < 0 ? -((-c.raw) >> 17) : (c.raw >> 17);   // 32 - 15
    if (t >  32767) return  32767;
    if (t < -32767) return -32767;
    return static_cast<i16>(t);
}

constexpr fx axis_to_fx(i16 q) noexcept {
    return fx::from_raw(asl(static_cast<i64>(q), 17));
}

// ------------------------------------------------------------------- snapshot

namespace ent_field {
inline constexpr u32 POS             = 1u << 0;
inline constexpr u32 FACING          = 1u << 1;
inline constexpr u32 ANIMATION_STATE = 1u << 2;
inline constexpr u32 VISUAL_PARAMS   = 1u << 3;
inline constexpr u32 ANCHOR          = 1u << 4;
inline constexpr u32 CONTROLLER_MODE = 1u << 5;
inline constexpr u32 DISPLAY_ID      = 1u << 6;
inline constexpr u32 DESTROYED       = 1u << 7;
}  // namespace ent_field

namespace state_flag {
inline constexpr u16 GROUNDED  = 1u << 0;
inline constexpr u16 CLIMBING  = 1u << 1;
inline constexpr u16 HOVERING  = 1u << 2;
inline constexpr u16 FLYING    = 1u << 3;
inline constexpr u16 GRAPPLING = 1u << 4;
inline constexpr u16 ANCHORED  = 1u << 5;
}  // namespace state_flag

inline constexpr u16 SNAP_RESYNC_REQUIRED = 1u << 0;

struct SnapshotHeader {
    u32 server_tick     = 0;
    u32 baseline_tick   = 0;   // 0 == full snapshot
    u32 last_input_tick = 0;
    i8  input_margin    = 0;
    u16 flags           = 0;
};

// Full precision, no quantisation: this is what reconciliation compares against
// the client's own arithmetic (protocol R2).
struct SelfBlock {
    fx3    pos{};
    fx3    vel{};
    brad   facing{};
    u8     controller_mode = 0;
    u16    state_flags     = 0;
    Handle anchor{};
    u8     jumps_used      = 0;
    u8     dashes_used     = 0;
    i32    sector          = -1;

    // Ability state the controller carries between ticks. Present because
    // reconciliation OVERWRITES local state and replays: anything the
    // controller keeps that is not sent here would survive the overwrite, and
    // the client would then replay from the server's position with its own
    // stale timers. See docs/protocol.md 5.1.
    fx     dash_timer{};
    fx     dash_cooldown{};
    fx     hover_fuel{};
    fx     flight_fuel{};

    // Only encoded when state_flags & GRAPPLING. A swinging player is the
    // exception, and gating it keeps 32 bytes off every other snapshot.
    fx3    grapple_anchor{};
    fx     grapple_dist{};

    u64    state_hash      = 0;
};

struct VisualParam {
    u16 key   = 0;
    i32 value = 0;
};

struct EntityBlock {
    Handle handle{};
    u32    mask = 0;
    fx3    pos{};                 // quantised on the wire
    brad   facing{};
    u16    animation_state = 0;
    u8     visual_count    = 0;
    VisualParam visual[MAX_VISUAL_PARAMS]{};
    Handle anchor{};
    u8     controller_mode = 0;
    u32    display_id      = 0;   // dropped items only; never an item_hash (R7)
};

struct VfxEvent {
    u16  kind = 0;
    fx3  pos{};
    brad dir{};
    u32  param = 0;
};

struct DecodedSnapshot {
    SnapshotHeader header{};
    SelfBlock      self{};
    i32            hp = 0;
    u16            entity_count = 0;
    EntityBlock    entities[MAX_ENTITIES]{};
    u16            vfx_count = 0;
    VfxEvent       vfx[MAX_VFX]{};
};

// ------------------------------------------------------------------ functions

void   encode_input_bundle(ByteWriter& w, const InputBundle& b) noexcept;
bool   decode_input_bundle(ByteReader& r, InputBundle& out) noexcept;

bool   decode_snapshot(ByteReader& r, DecodedSnapshot& out) noexcept;

// Incremental snapshot encoder.
//
// Entities are offered one at a time and each either fits or is rejected and
// rolled back cleanly. That is what makes protocol R10 structural: the encoder
// is handed a budget and physically cannot exceed it, so "log what you dropped"
// becomes the caller's obvious next line rather than an easily-forgotten check.
class SnapshotWriter {
public:
    SnapshotWriter(u8* buf, size_t capacity) noexcept;

    void begin(const SnapshotHeader& h, const SelfBlock& self, i32 hp) noexcept;

    // Returns false if the entity did not fit; the buffer is left exactly as it
    // was, so the caller may keep offering smaller ones or stop.
    bool try_entity(const EntityBlock& e) noexcept;
    bool try_vfx(const VfxEvent& v) noexcept;

    // Patches the two counts and returns the final byte count (0 if the header
    // itself did not fit).
    size_t finish() noexcept;

    u16 entities_written() const noexcept { return entity_count_; }
    u16 vfx_written() const noexcept { return vfx_count_; }

private:
    u8*        buf_;
    ByteWriter w_;
    size_t     entity_count_pos_ = 0;
    size_t     vfx_count_pos_    = 0;
    u16        entity_count_     = 0;
    u16        vfx_count_        = 0;
    bool       began_            = false;
    bool       vfx_started_      = false;
};

}  // namespace dnp::proto
