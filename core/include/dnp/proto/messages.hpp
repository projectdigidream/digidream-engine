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
inline constexpr u8 MSG_SECTOR_MOTION_SET = 0x12;
inline constexpr u8 MSG_PARAM_UPDATE = 0x14;
// UI (docs/protocol.md §11). Reliable, never datagrams: a panel that arrived
// late is still a panel; one that arrived never is a player staring at a shop
// that does not open.
inline constexpr u8 MSG_UI_OPEN  = 0x20;
inline constexpr u8 MSG_UI_EVENT = 0x21;
inline constexpr u8 MSG_UI_CLOSE = 0x22;
// One widget's value, in a panel that is already open. Without it a panel that
// merely SHOWS something changing has to be closed and reopened to refresh --
// which allocates a new id and, from the player's side, is a window that
// flickers every time their health does.
inline constexpr u8 MSG_UI_SET   = 0x23;
// Camera settings for one client (docs/protocol.md §11.1, design §4.1). The
// camera lives on the client and the server never says where it IS -- only how
// it should behave.
inline constexpr u8 MSG_CAMERA_SET = 0x24;
// One field of one side changes its picture (docs/protocol.md §11.1). A switch
// that has been thrown. Not simulation state -- never hashed, never predicted.
inline constexpr u8 MSG_SIDE_TEXTURE = 0x25;
// The map's music changes cue (docs/protocol.md §11.1). A name, because the
// client owns the manifest that maps a cue to a track; the server never sees a
// track. Not simulation state -- never hashed, never predicted, not replayed
// to a client joining later.
inline constexpr u8 MSG_MUSIC = 0x26;
// The screen itself: a black curtain and letterbox bars (docs/protocol.md
// §11.1). For cinematics. Not simulation state -- never hashed, never
// predicted, and client-local in exactly the sense the camera is.
inline constexpr u8 MSG_SCREEN_SET = 0x27;

// Decoder-side caps. These bound work and memory on hostile input; they are not
// gameplay limits.
inline constexpr u32 MAX_INPUT_FRAMES   = 16;
// Must be at least sim::MAX_AGENTS + sim::MAX_PROPS, since every one of those
// can appear in a snapshot. Kept as its own number because the protocol is the
// interoperability contract and must not silently follow a simulation constant
// -- but a static_assert in world.cpp holds the two together, so raising one
// without the other fails the build rather than truncating a level on the wire.
inline constexpr u32 MAX_ENTITIES       = 2432;
inline constexpr u32 MAX_VFX            = 64;
inline constexpr u32 MAX_VISUAL_PARAMS  = 8;
inline constexpr u32 MAX_SECTOR_OFFSETS = 32;

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
// Whether other bodies collide with this one. Replicated because a client
// PREDICTS against it: it mirrors the bodies around it into its own world as
// colliders, and one that thought a corpse was still solid would be stopped by
// something the server walks straight through -- every tick, for as long as the
// body is there.
inline constexpr u32 SOLID           = 1u << 8;
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

// A server-side script changed THIS observer's entity since the last snapshot
// sent to it (docs/protocol.md §10.1.3). Per observer, deliberately: set
// globally it would excuse every client's mismatch on every scripted tick, and
// the signal it exists to carry would be worth nothing.
inline constexpr u16 SNAP_LUA_APPLIED = 1u << 1;

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
    // Ticks of swing remaining. Authoritative: the server decides when a swing
    // ends, and a client that lost this on every snapshot would draw its own
    // collider for two frames at a time and never see it travel.
    u8     attack_left     = 0;
    // Ticks until the next swing is allowed.
    //
    // Replicated for the same reason attack_left is: it is part of the state a
    // client predicts against -- it decides whether a press starts a swing at
    // all -- and it is in the prediction hash. A hashed field that reconcile
    // cannot restore is a divergence with no way back: the client would
    // mispredict every tick and never recover, because the correction carries
    // nothing to correct it with.
    u8     attack_cooldown_left = 0;
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

    // Which generation of this entity's movement parameters produced the state
    // above (docs/protocol.md 8.4, 10.1.2).
    //
    // Parameters are the RULES, not the state, and they travel on the reliable
    // channel while this travels on the unreliable one -- the two are not
    // ordered against each other, so a snapshot computed with new parameters
    // can arrive before the PARAM_UPDATE that describes them. Without this byte
    // the client cannot tell "I am one generation behind" from "the two builds
    // disagree", and those have opposite fixes.
    u8     param_epoch     = 0;

    u64    state_hash      = 0;
};

// ------------------------------------------------------- movement parameters
//
// docs/protocol.md 8.4. Control channel, reliable, because these are the RULES
// the next thousand ticks will be computed from rather than a position that
// reconciliation can correct. A client running different parameters from the
// server mispredicts every tick and never recovers.

namespace param_flag {
// Replaces every field rather than patching. Required on the first update a
// joining client receives, which has no parameter history to patch.
inline constexpr u8 FULL = 1u << 0;
}  // namespace param_flag

// MoveParams has 47 fields today. The cap is a decoder bound on hostile input,
// not a gameplay limit -- it exists so a two-byte count cannot ask for a
// gigabyte, the same reason as every other cap in this file.
inline constexpr u32 MAX_PARAM_FIELDS = 64;

struct ParamField {
    u16 id  = 0;    // index into the field table; see server/src/lua/params_table.hpp
    i64 raw = 0;    // fx raw, or a bool/u8 widened -- the table says which
};

// --- UI (docs/protocol.md §11) ------------------------------------------------

inline constexpr u32 UI_MAX_TEXT    = 128;   // bytes, per string
inline constexpr u32 UI_MAX_WIDGETS = 32;
inline constexpr u32 UI_MAX_OPTIONS = 32;

enum class UiWidgetKind : u8 {
    Label  = 0,   // static text; sends nothing
    Button = 1,   // sends value 1 when pressed
    Slider = 2,   // integer in [lo, hi]
    Select = 3,   // index into `options`
    Toggle = 4    // 0 or 1
};

// A fixed-size string, like everything else on this wire.
//
// Not a std::string: this header is included by core/, which allocates nothing
// in the tick path, and by the wasm build. The cap is the protocol's, so a
// hostile length cannot make the decoder reserve anything.
struct UiText {
    u16 len = 0;
    char text[UI_MAX_TEXT]{};
};

struct UiWidget {
    UiWidgetKind kind = UiWidgetKind::Label;
    u16    id = 0;          // opaque to the client; echoed back in UI_EVENT
    UiText label{};
    i32    value = 0;
    i32    lo = 0;
    i32    hi = 0;
    u16    option_count = 0;
    UiText options[UI_MAX_OPTIONS]{};
};

struct UiPanel {
    u32      ui_id = 0;
    UiText   title{};
    u16      count = 0;
    UiWidget widgets[UI_MAX_WIDGETS]{};
};

// Camera settings. Values are THOUSANDTHS of their unit -- see the table in
// docs/protocol.md §11.1. Not fx: a camera is never hashed and never predicted,
// so it does not need the simulation's number type, and a thousandth of a unit
// is finer than a pixel at any sane zoom.
// Headroom, not a fit. read_camera_table() fills this array by walking its own
// name table, so the bound that protects it is the LENGTH OF THAT LIST -- and a
// list that grew past 16 would have overrun the array with nothing to stop it.
// Sized well clear of the names so adding one is a one-line change again, and
// the guard there now says so rather than leaving it to arithmetic.
inline constexpr u32 CAMERA_MAX_FIELDS = 32;

struct CameraField {
    u8  id = 0;
    i32 milli = 0;
};

struct CameraSet {
    u16 count = 0;
    CameraField fields[CAMERA_MAX_FIELDS]{};
};

void encode_camera_set(ByteWriter& w, const CameraSet& c) noexcept;
bool decode_camera_set(ByteReader& r, CameraSet& out) noexcept;

// Which of a side's three texture fields (docs/protocol.md §11.1).
enum class SideField : u8 { Upper = 0, Middle = 1, Lower = 2 };

struct SideTexture {
    u16      side = 0;
    SideField field = SideField::Middle;
    UiText   name{};        // a texture name, reusing the capped-string type
};

void encode_side_texture(ByteWriter& w, const SideTexture& t) noexcept;
bool decode_side_texture(ByteReader& r, SideTexture& out) noexcept;

// When a cue change lands (docs/protocol.md §11.1): on the next bar of what is
// playing, when its loop ends, or at once.
enum class MusicAt : u8 { NextBar = 0, LoopEnd = 1, Now = 2 };

struct MusicCue {
    UiText  cue{};          // a cue name from the map's music manifest
    u16     fade_ms = 800;
    MusicAt at = MusicAt::NextBar;
};

void encode_music_cue(ByteWriter& w, const MusicCue& m) noexcept;
bool decode_music_cue(ByteReader& r, MusicCue& out) noexcept;

// What is drawn OVER everything: a fade to black, and letterbox bars.
//
// The same record shape as CameraSet, deliberately -- {u8 id, i32 milli},
// thousandths, unknown ids skipped -- so it inherits that message's forward
// compatibility rather than inventing a second convention for the same job. A
// newer server can send a field this client has never heard of and the client
// simply does not apply it.
//
// Values in thousandths for the same reason the camera's are: this is not
// simulation state, it needs a fixed scale rather than fx, and a thousandth of
// an opacity is finer than a screen can show.
//
// Colour is fixed black. That covers a fade and a letterbox, which is what a
// cutscene wants; the format is extensible, so a colour can be added later
// without a version bump or a second message.
inline constexpr u32 SCREEN_MAX_FIELDS = 16;

struct ScreenField {
    u8  id = 0;
    i32 milli = 0;
};

struct ScreenSet {
    u16 count = 0;
    ScreenField fields[SCREEN_MAX_FIELDS]{};
};

void encode_screen_set(ByteWriter& w, const ScreenSet& s) noexcept;
bool decode_screen_set(ByteReader& r, ScreenSet& out) noexcept;

struct UiEvent {
    u32 ui_id = 0;
    u16 widget_id = 0;
    i32 value = 0;
};

// Body only, like SECTOR_MOTION_SET and PARAM_UPDATE: the caller frames it.
void encode_ui_open(ByteWriter& w, const UiPanel& p) noexcept;
bool decode_ui_open(ByteReader& r, UiPanel& out) noexcept;
void encode_ui_event(ByteWriter& w, const UiEvent& e) noexcept;
bool decode_ui_event(ByteReader& r, UiEvent& out) noexcept;

struct ParamUpdate {
    Handle     handle{};
    u8         epoch = 0;
    u32        effective_tick = 0;
    u8         flags = 0;
    u16        count = 0;
    ParamField fields[MAX_PARAM_FIELDS]{};
};

// Body only: the caller supplies the control-channel length/type framing, the
// same split SECTOR_MOTION_SET uses.
void encode_param_update(ByteWriter& w, const ParamUpdate& p) noexcept;

// Returns false on truncation or an out-of-range count, leaving `p` untouched
// rather than partly filled -- a half-applied parameter set is worse than none,
// because it looks like it worked.
bool decode_param_update(ByteReader& r, ParamUpdate& p) noexcept;

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
    u8     solid           = 1;
};

// A scripted-discrete sector's current offset. Parametric movers are absent by
// construction -- the client computes theirs (docs/protocol.md 5.5).
struct SectorOffset {
    u32 sector = 0;
    fx3 offset{};
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
    u16            sector_offset_count = 0;
    SectorOffset   sector_offsets[MAX_SECTOR_OFFSETS]{};
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

    // `offsets` carries only scripted-discrete movers; parametric ones are
    // computed client-side and must not be sent (docs/protocol.md 5.5).
    void begin(const SnapshotHeader& h, const SelfBlock& self, i32 hp,
               const SectorOffset* offsets = nullptr, u16 offset_count = 0) noexcept;

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
