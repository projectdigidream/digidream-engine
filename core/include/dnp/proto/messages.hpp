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
#include "dnp/sim/weapon.hpp"
#include "dnp/geom/sector_fx.hpp"

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
// THE LEVEL NEXT DOOR, and where it sits relative to this one.
//
// A metamap composes levels in world space, and a corridor ending at a cell
// boundary is the same line as its neighbour's. Without this the observer sees
// their own cell and nothing else: a passage that leads into black, and then a
// level change that arrives like a cut. What makes a seam a seam is that the
// far side was already on screen.
//
// Sent immediately BEFORE that neighbour's MAP_DATA chunks, which is what makes
// this work without a level id on every chunk: the control channel is reliable
// and ordered, so the stream that follows is unambiguously this one's. The
// observer's OWN map is still sent with no selector at all, so a server with no
// metamap emits exactly the bytes it emitted before.
//
// The offset is the neighbour's cell origin MINUS this observer's, so a client
// draws it at a translation and never has to know where in the world it is
// standing. Purely renderer-side: it is not simulation state, the client does
// not collide with it, and crossing the seam is still the server's decision.
inline constexpr u8 MSG_NEIGHBOUR_MAP  = 0x28;
// That neighbour is no longer resident, or is no longer a neighbour. The client
// drops its geometry. Not an error to receive for a level never sent.
inline constexpr u8 MSG_NEIGHBOUR_DROP = 0x29;
// That neighbour is still a neighbour, and has MOVED relative to this observer.
//
// Which is what every crossing does to most of them: walking one cell east
// changes the offset of all nine and the membership of only three. Re-sending
// the geometry for a level the client already holds would make a seam the most
// expensive thing in the game -- nine maps and nine meshes rebuilt to walk
// through a doorway, which is precisely the loading screen a seam exists to
// abolish. Twenty bytes instead.
inline constexpr u8 MSG_NEIGHBOUR_MOVE = 0x2A;

// THE BODY IS NOW IN THIS LEVEL, and nothing else happened.
//
// Several levels can share one simulation (a region: every level on a
// metamap's grid). Walking from one into the next is then walking between two
// rooms -- same world, same body, same agent index, no WELCOME -- and what does
// change is which level's music plays, whose camera settings apply, and what
// every held level's offset is measured from. `{u16 level}`. Followed by the
// NEIGHBOUR_* messages that re-state the neighbourhood from there.
inline constexpr u8 MSG_HOME = 0x2B;

// SINGLE-PLAYER SAVE AND LOAD (docs/SAVING.md). Client to server, {u8 slot}.
// Answered with GAME_RESULT {u8 op (0 save, 1 load), u8 slot, u8 ok, u16 len,
// text}: refused, with the reason, unless the module enables saves and there is
// exactly one player.
inline constexpr u8 MSG_GAME_SAVE   = 0x2C;
inline constexpr u8 MSG_GAME_LOAD   = 0x2D;
inline constexpr u8 MSG_GAME_RESULT = 0x2E;
// A NEW GAME: the world from scratch, every kept level state forgotten, the
// player at the first level's spawn. Client to server, no body; answered with
// GAME_RESULT, op 2. Single player only.
inline constexpr u8 MSG_GAME_NEW    = 0x2F;
// What this client should play the moment it predicts its own action, so its
// own jump and shot are heard at once rather than a round trip later
// (docs/protocol.md §5.7). S->C, reliable.
inline constexpr u8 MSG_SOUND_PREDICT = 0x33;
// HUD values: what the HUD layout's server-known bindings read (docs/
// UI_LAYOUT.md 4, protocol.md 11.1). S->C, reliable, only when a value changes.
// u16 count, then count x (str name, u8 kind, value): kind 0 i32, 1 str, 2 clear.
inline constexpr u8  MSG_HUD_SET     = 0x34;
// The weapon table a client predicts its own weapon with
// (docs/WEAPON_PREDICTION.md): sim::encode_weapon_table's bytes, sent when a
// client has not got the current one. A client holding none predicts none.
inline constexpr u8  MSG_WEAPON_DEFS = 0x35;
// The sky the player's level is drawn under (docs/SKY.md): its kind, its
// pictures (texture names) and a bag of fields. Sent with WELCOME and HOME and
// whenever a script changes it. Not simulation state -- never hashed, never
// predicted.
inline constexpr u8  MSG_SKY_SET     = 0x36;
// A sector's look changes (docs/GRAPHS.md "sectors"): its floor or ceiling
// picture, its light (with a fade), or one of its effects. Kept per level and
// sent to whoever arrives, as a switch thrown is. Not simulation state.
inline constexpr u8  MSG_SECTOR_SET  = 0x37;
// The module's own keys (docs/KEYS.md): u8 n, then per action its name, its
// keyboard code (a KeyboardEvent.code, "KeyR") and its touch button's label
// (empty for none), each u16 length + bytes. Bit i of InputFrame::actions is
// action i. Sent once, after WELCOME.
inline constexpr u8  MSG_KEYS        = 0x38;
// MARKERS (docs/MARKERS.md): a small UI over something in the world -- a
// health bar over a monster, a sign over a door, a marker over a goal. Drawn
// from a template in the module's UI layout, never interacted with. Kept per
// level and sent to whoever arrives, as a sector's look is. Not simulation
// state. MARK_SET adds or replaces one (whole); MARK_CLEAR (u32 id) removes it.
inline constexpr u8  MSG_MARK_SET    = 0x39;
inline constexpr u8  MSG_MARK_CLEAR  = 0x3A;
inline constexpr u32 MAX_KEY_ACTIONS = 8;
inline constexpr u32 HUD_MAX_FIELDS  = 32;    // per message
inline constexpr u32 HUD_MAX_NAMES   = 64;    // per session
inline constexpr u32 HUD_NAME_MAX    = 24;
inline constexpr u32 HUD_STR_MAX     = 1024;   // a subtitle of several lines
inline constexpr u8  HUD_INT = 0, HUD_STR = 1, HUD_CLEAR = 2;

// Decoder-side caps. These bound work and memory on hostile input; they are not
// gameplay limits.
inline constexpr u32 MAX_INPUT_FRAMES   = 16;
// Must be at least sim::MAX_AGENTS + sim::MAX_PROPS, since every one of those
// can appear in a snapshot. Kept as its own number because the protocol is the
// interoperability contract and must not silently follow a simulation constant
// -- but a static_assert in world.cpp holds the two together, so raising one
// without the other fails the build rather than truncating a level on the wire.
inline constexpr u32 MAX_ENTITIES       = 8192;
inline constexpr u32 MAX_VFX            = 64;
inline constexpr u32 MAX_SOUNDS         = 32;
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
    // Looking up (+) or down (-), in 1/65536 of a turn: +16384 is straight up.
    // The simulation clamps it to MAX_PITCH. What a first-person player aims
    // with vertically; zero for a scheme that has no vertical aim.
    i16  pitch   = 0;
    // The weapon wanted, by its index in the WEAPON_DEFS table, or NO_WEAPON
    // (docs/WEAPON_PREDICTION.md). An INPUT, not a request on the side, so a
    // switch is predicted: the client and the server apply the same choice on
    // the same tick. Set for the one tick a key went down -- held, it would
    // undo every switch the machine makes by itself when a weapon runs dry.
    u8   weapon  = sim::NO_WEAPON;
    // The MODULE'S OWN KEYS (module.txt `key` lines, docs/KEYS.md): bit i is
    // the i-th declared action, held this tick. Nothing in the simulation reads
    // it; scripts and graphs do (e:actions()). Appended after `view`.
    u8   actions = 0;
    // THE TICK THIS CLIENT WAS DRAWING the others at when it sampled this frame
    // (lag compensation, docs/WEAPON_PREDICTION.md): view_byte(tick), and 0 for
    // none. One byte is enough because the server only ever rewinds a few
    // ticks; it rebuilds the whole tick from its own clock (view_tick).
    u8   view    = 0;
};

// A tick in one byte: its residue mod 255, plus one, so 0 can mean "none" --
// which a plain low byte could not, since every value of one is a tick.
constexpr u8 view_byte(u32 tick) noexcept { return static_cast<u8>(tick % 255u + 1u); }

// The most recent tick at or before `now` that `view` names, as an age in
// ticks (0..254), or -1 for a frame that names none.
constexpr i32 view_age(u8 view, u32 now) noexcept {
    if (view == 0) return -1;
    const u32 r = static_cast<u32>(view) - 1u;
    return static_cast<i32>((now % 255u + 255u - r) % 255u);
}
inline constexpr i16 MAX_PITCH = 14563;   // 80 degrees

constexpr bool operator==(const InputFrame& a, const InputFrame& b) noexcept {
    return a.buttons == b.buttons && a.move_x == b.move_x &&
           a.move_y == b.move_y && a.aim == b.aim && a.pitch == b.pitch &&
           a.weapon == b.weapon && a.view == b.view && a.actions == b.actions;
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
// WHICH AVATAR draws this player (design 4.1).
//
// Its own field rather than display_id's, which carries the KIND -- and kind
// zero has to keep meaning "a player", because the client branches on it to
// decide whether a body's collider comes from the catalogue or from the map's
// own replicated `body`, and whether it is a prop at all. An avatar number is a
// different question from what a thing is: two players are the same kind of
// thing and may look nothing alike.
//
// Delta-masked rather than sent once on appearance, because it can change: a
// class swap, a costume. Zero is the default avatar, which is what a server that
// never speaks gets.
inline constexpr u32 AVATAR          = 1u << 9;
// HOW FAR INTO ITS SWING another body is: its attack countdown (u8, ticks left;
// 0 not swinging). Sent while it changes, so a swing costs a byte a snapshot
// for as long as it lasts and nothing otherwise. What lets a client draw
// somebody ELSE's swing -- a monster's claw, another player's blade -- with the
// shape it already has from that body's parameters (design 4.1; the client's
// own swing is predicted and needs none of this).
inline constexpr u32 ATTACK          = 1u << 10;
// THE POSITION IS A JUMP, not a move: the body was teleported since the
// baseline, so a client draws it at the new place on its tick rather than
// sliding it there across the interpolation window. No payload; it always
// travels with POS.
inline constexpr u32 TELEPORTED      = 1u << 11;
// WHICH WEAPON STEP its swing is (u8 weapon, u8 phase, u8 step; weapon 0xFF:
// none, the swing is its parameters' shape). A player's `swing` step gives a
// shape of its own, which no parameter carries: with this, and the weapon
// table every client already holds, another client draws that swing as it
// is rather than as the weapon's. Sent when it changes; a monster's step
// shape is in its body's parameters and never needs it.
inline constexpr u32 ATTACK_STEP     = 1u << 12;
// WHICH PARTICLE KIND FOLLOWS IT (u16; 0 none): a missile's smoke, a
// fireball's flame, an aura (e:set_trail). The client keeps an emitter of that
// kind on the body while it draws it, so a trail is this field, sent when it
// changes, and not an event a tick. Not physics: not hashed, not predicted.
inline constexpr u32 TRAIL           = 1u << 13;
// WHAT ELSE A PLAYER WEARS, and how its animations are redefined (proto::Look):
// sprite layers drawn on the body, and states played in place of others. Server
// state as the avatar is, and delta-masked the same way.
inline constexpr u32 LOOK            = 1u << 14;
}  // namespace ent_field

namespace state_flag {
inline constexpr u16 GROUNDED  = 1u << 0;
inline constexpr u16 CLIMBING  = 1u << 1;
inline constexpr u16 HOVERING  = 1u << 2;
inline constexpr u16 FLYING    = 1u << 3;
inline constexpr u16 GRAPPLING = 1u << 4;
inline constexpr u16 ANCHORED  = 1u << 5;
// The body holds weapons, so SelfBlock carries the weapon machine's state.
inline constexpr u16 ARMED     = 1u << 6;
// Climbing on a ledge rather than a wall: hanging (MoveState::hanging).
inline constexpr u16 HANGING   = 1u << 7;
// Off a ledge by JUMP: its small hop, or the pull-up (MoveState::hopping).
inline constexpr u16 HOPPING   = 1u << 8;
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
// A PLAYER'S LOOK beyond its avatar (design 4.1; docs/protocol.md 5.2):
//
//   layer[i]   a catalogue record drawn ON the body, in the body's own state,
//              clock and facing -- armour, a weapon in hand. 0 is none. Drawn
//              in slot order, 0 nearest the body.
//   from/to    a state played in place of another: where the body (and every
//              layer) would play `from`, it plays `to`. An attack that looks
//              different with each weapon is one pair.
//
// On the wire: u8 mask of the layers present, a u16 kind per one present (low
// bit first), then u8 n (<= LOOK_REMAPS) and n x (u16 from, u16 to).
inline constexpr u32 LOOK_LAYERS = 8;
inline constexpr u32 LOOK_REMAPS = 8;
struct Look {
    u16 layer[LOOK_LAYERS]{};
    u8  remaps = 0;
    u16 from[LOOK_REMAPS]{};
    u16 to[LOOK_REMAPS]{};
};
void encode_look(ByteWriter& w, const Look& l) noexcept;
bool decode_look(ByteReader& r, Look& l) noexcept;

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
    fx     coyote{};                // MoveState::coyote_timer

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
    // Which avatar draws this player. As EntityBlock's, and here for the same
    // reason animation_state is: the local agent never appears in its own entity
    // stream, so without it a player would be the one body on the map that could
    // not see its own costume.
    u16    avatar          = 0;
    // And what else it wears (proto::Look), for the same reason.
    Look   look{};
    // The weapon machine (docs/WEAPON_PREDICTION.md): only when state_flags
    // has ARMED. Simulation state the client predicts and the hash covers, so
    // reconciliation has to be able to restore all of it.
    sim::WeaponState weapon{};
    // The observer's own animation state (design 4.1).
    //
    // SelfBlock's first purely COSMETIC field, and worth a sentence because the
    // distinction had never had to be made: this block is "what this observer
    // needs to know about itself", not "what the hash needs". Until now the two
    // overlapped completely, because everything per-self was simulation state.
    //
    // It is here because the local agent is the one body excluded from its own
    // entity stream, so a script that animates a player would otherwise be
    // animating the only thing on the map nobody can see. It is deliberately NOT
    // in World::agent_hash -- see the comment there -- because a script's choice
    // is not something the client can reproduce, and a hashed field with nothing
    // to reconcile it from is a permanent mismatch.
    u16    animation_state = 0;
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
// A panel's template: the name of one of the layout's `panels`
// (docs/UI_LAYOUT.md 5), [a-z0-9_], like a HUD name.
inline constexpr u32 UI_MAX_TEMPLATE = 24;

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
    // Presentation only: where and how the client draws it. Empty is "the
    // default panel", and encodes to exactly the bytes a panel had before
    // templates existed.
    u8       template_len = 0;
    char     template_name[UI_MAX_TEMPLATE]{};
    // UI_FLAG_*: trailing after the template (written with an empty template
    // when there is none). A panel with no flags is byte-for-byte what it was.
    u8       flags = 0;
};
// A PINNED panel has no ×: it stays until the script closes it -- a module's
// own weapon window, a score board (docs/GRAPHS.md "Panels").
inline constexpr u8 UI_FLAG_PINNED = 0x01;

/** [a-z0-9_], 1..UI_MAX_TEMPLATE -- the one rule for a template name, both ends. */
bool valid_template_name(const char* s, size_t n) noexcept;

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
    // On the wire, the side's NAME (geom::MapStorage::side_name): chunk key in
    // the high half, the index within its own map in the low. For a one-map
    // store that is the index. Held as the sender's own index until it is sent.
    u32      side = 0;
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

// THE SKY (docs/SKY.md). `kind` says how the pictures wrap the world:
//   None   -- no sky: sky surfaces show the default gradient, the background
//             the clear colour (what a level without one has always looked like)
//   Strip  -- one picture wrapped round the horizon `repeat` times, DOOM's sky
//   Panorama -- one equirectangular picture, the whole sphere
//   Cube   -- six pictures: east west north south up down (+x -x +y -y +z -z)
// Fields are the CameraSet record shape, {u8 id, i32 milli}, unknown ids
// skipped: 1 repeat (Strip), 2 yaw (turns), 3 height (Strip: the tangent of
// the elevation its top edge sits at).
enum class SkyKind : u8 { None = 0, Strip = 1, Panorama = 2, Cube = 3 };
inline constexpr u32 SKY_MAX_IMAGES = 6;
inline constexpr u32 SKY_MAX_FIELDS = 8;

struct SkySet {
    SkyKind kind = SkyKind::None;
    u8      images = 0;
    UiText  image[SKY_MAX_IMAGES]{};
    u16     count = 0;
    CameraField fields[SKY_MAX_FIELDS]{};
};

void encode_sky_set(ByteWriter& w, const SkySet& s) noexcept;
bool decode_sky_set(ByteReader& r, SkySet& out) noexcept;

// What a SECTOR_SET changes, and the change. One record per (sector, what,
// and for an effect its kind): the server keeps the latest of each.
enum class SectorWhat : u8 { Floor = 0, Ceiling = 1, Light = 2, Effect = 3 };

struct SectorSet {
    // On the wire, the sector's NAME (geom::MapStorage::sector_name): chunk key
    // high, index low. Held as the sender's own index until it is sent.
    u32        sector = 0;
    SectorWhat what = SectorWhat::Floor;
    UiText     texture{};      // Floor, Ceiling
    u8         light = 0;      // Light: 0..255, the map's own scale
    u16        fade_ms = 0;    // Light: how long the change takes
    bool       on = false;     // Effect: false takes the kind off the sector
    geom::SectorFx fx{};       // Effect: kind and settings (fx.sector unused)
};

void encode_sector_set(ByteWriter& w, const SectorSet& s) noexcept;
bool decode_sector_set(ByteReader& r, SectorSet& out) noexcept;

struct UiEvent {
    u32 ui_id = 0;
    u16 widget_id = 0;
    i32 value = 0;
};

// Body only, like SECTOR_MOTION_SET and PARAM_UPDATE: the caller frames it.
void encode_ui_open(ByteWriter& w, const UiPanel& p) noexcept;
bool decode_ui_open(ByteReader& r, UiPanel& out) noexcept;

// A MARKER (MSG_MARK_SET): what it hangs on, which template draws it, and the
// values its template's bindings read. The values are the HUD's kinds, int or
// text, by a name of [a-z0-9_].
inline constexpr u32 MARK_MAX_VALUES = 16;
inline constexpr u8  MARK_ON_ENTITY = 0;   // follows an entity's drawn body
inline constexpr u8  MARK_AT_POINT  = 1;   // a fixed place in the level's frame
inline constexpr u8  MARK_FLAG_DEBUG   = 0x01;  // drawn only with the debug overlay
inline constexpr u8  MARK_FLAG_THROUGH = 0x02;  // drawn through walls, not hidden by them
struct MarkValue {
    u8     name_len = 0;
    char   name[HUD_NAME_MAX]{};
    u8     kind = HUD_INT;                  // HUD_INT or HUD_STR
    i32    number = 0;
    UiText text{};
};
struct MarkSet {
    u32    id = 0;
    u8     anchor = MARK_ON_ENTITY;
    Handle handle{};                        // MARK_ON_ENTITY
    fx3    pos{};                           // MARK_AT_POINT
    u8     template_len = 0;
    char   template_name[UI_MAX_TEMPLATE]{};
    u8     flags = 0;
    fx     height{};                        // above the body's top, or the point
    fx     range{};                         // shown within this of the viewer; 0 always
    u8     count = 0;
    MarkValue values[MARK_MAX_VALUES]{};
};
void encode_mark_set(ByteWriter& w, const MarkSet& m) noexcept;
bool decode_mark_set(ByteReader& r, MarkSet& out) noexcept;
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
    u16    avatar          = 0;   // which avatar draws a player; 0 is the default
    u8     solid           = 1;
    u8     attack_left     = 0;   // ent_field::ATTACK
    u8     attack_weapon   = 0xFF; // ent_field::ATTACK_STEP
    u8     attack_phase    = 0xFF;
    u8     attack_step     = 0;
    u16    trail           = 0;    // ent_field::TRAIL
    Look   look{};                 // ent_field::LOOK
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

// A SOUND, fire-and-forget like a VfxEvent and under the same rule (R4): never
// the sole carrier of anything the simulation needs. docs/protocol.md §5.7.
//
// `sound` is the FNV-1a hash of the sound's NAME ("weapon.shotgun.fire") --
// the client hashes the names in the bank it loaded and plays the match, so
// neither side needs a table of numbers the other keeps in step. `source` is
// the entity that made it (a handle, 0 for none), which a FOLLOW sound tracks
// and which the client compares with itself for a PREDICTED one.
inline constexpr u8 SOUND_FOLLOW    = 1;   // moves with `source`
inline constexpr u8 SOUND_PREDICTED = 2;   // `source`'s own client already played it
struct SoundEvent {
    u32 sound  = 0;
    fx3 pos{};
    u32 source = 0;
    u8  flags  = 0;
};

// SOUND_PREDICT entries: on which of its own actions a client plays which
// sound. `param` is the trigger's: for a button, the ticks between two shots;
// for a landing, the fall speed that counts as one, in 1/16 world units a
// second. `shots` is how many the weapon has left, -1 for no limit -- the
// client counts it down itself between updates.
inline constexpr u8 PREDICT_JUMP = 1, PREDICT_LAND = 2, PREDICT_PRIMARY = 3,
                    PREDICT_PRIMARY_HELD = 4, PREDICT_SECONDARY = 5,
                    PREDICT_SECONDARY_HELD = 6, PREDICT_SWING = 7;
inline constexpr u32 MAX_PREDICT_SOUNDS = 16;
struct PredictSound {
    u8  trigger = 0;
    u32 sound   = 0;   // sound_hash
    u16 param   = 0;
    i16 shots   = -1;
};

// The name hash both sides agree on: 32-bit FNV-1a over the name's bytes.
constexpr u32 sound_hash(const char* s, size_t n) noexcept {
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= static_cast<u8>(s[i]); h *= 16777619u; }
    return h;
}

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
    // After the vfx, and only when there are any: a snapshot with no sound
    // ends where it always did.
    u16            sound_count = 0;
    SoundEvent     sounds[MAX_SOUNDS]{};
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
    // After every vfx: once a sound is written no vfx may follow.
    bool try_sound(const SoundEvent& s) noexcept;

    // Patches the two counts and returns the final byte count (0 if the header
    // itself did not fit).
    size_t finish() noexcept;

    u16 entities_written() const noexcept { return entity_count_; }
    u16 vfx_written() const noexcept { return vfx_count_; }

    // What is left of the datagram, so a caller can keep some back. A snapshot's
    // transients are written AFTER its entities because that is the order the
    // wire is read in, and entities on a populated level will happily use every
    // byte -- so reserving is the only way an event ever gets sent.
    size_t remaining() const noexcept { return w_.remaining(); }

    // What one encoded VfxEvent costs: kind (2) + a quantised vec3 (12) +
    // a brad (4) + param (4). Named so a caller can reserve N of them without
    // restating the format, which is how a reserve silently becomes wrong.
    static constexpr size_t VFX_BYTES = 22;
    // sound (4) + a quantised vec3 (12) + source (4) + flags (1).
    static constexpr size_t SOUND_BYTES = 21;

private:
    u8*        buf_;
    ByteWriter w_;
    size_t     entity_count_pos_ = 0;
    size_t     vfx_count_pos_    = 0;
    u16        entity_count_     = 0;
    u16        vfx_count_        = 0;
    bool       began_            = false;
    bool       vfx_started_      = false;
    size_t     sound_count_pos_  = 0;
    u16        sound_count_      = 0;
    bool       sound_started_    = false;
};

}  // namespace dnp::proto
