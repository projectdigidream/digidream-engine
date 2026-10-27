//
// Wire codec. Normative reference: docs/protocol.md.
//
// Encoders and decoders are written as mirror images and are tested by
// round-tripping randomised messages (core/tests/test_proto.cpp) rather than
// against hand-written byte strings. A codec whose two halves are wrong in the
// same way passes any test that only ever exercises both halves together, so
// the tests also assert byte-exact sizes and reject truncated and hostile input.
//
#include "dnp/proto/messages.hpp"

namespace dnp::proto {

// --------------------------------------------------------------- input bundle

void encode_input_bundle(ByteWriter& w, const InputBundle& b) noexcept {
    w.u8v(MSG_INPUT_BUNDLE);
    w.u32v(b.client_tick);
    w.u32v(b.ack_snapshot_tick);

    const u8 n = b.frame_count > MAX_INPUT_FRAMES
                     ? static_cast<u8>(MAX_INPUT_FRAMES)
                     : b.frame_count;
    w.u8v(n);
    for (u8 i = 0; i < n; ++i) {
        const InputFrame& f = b.frames[i];
        // Reserved bits are masked off on the way out rather than trusted. A
        // client that sets them is buggy, not malicious, but letting them onto
        // the wire would quietly consume bits the frozen protocol needs later.
        w.u16v(static_cast<u16>(f.buttons & ~button::RESERVED_MASK));
        w.i16v(f.move_x);
        w.i16v(f.move_y);
        w.bradv(f.aim);
        w.i16v(f.pitch);
        w.u8v(f.weapon);
        w.u8v(f.view);
        w.u8v(f.actions);
    }
}

bool decode_input_bundle(ByteReader& r, InputBundle& out) noexcept {
    if (r.u8v() != MSG_INPUT_BUNDLE) return false;
    out.client_tick       = r.u32v();
    out.ack_snapshot_tick = r.u32v();

    const u8 n = r.u8v();
    if (!r.ok() || n > MAX_INPUT_FRAMES) return false;
    out.frame_count = n;

    for (u8 i = 0; i < n; ++i) {
        InputFrame& f = out.frames[i];
        f.buttons = static_cast<u16>(r.u16v() & ~button::RESERVED_MASK);
        f.move_x  = r.i16v();
        f.move_y  = r.i16v();
        f.aim     = r.bradv();
        f.pitch   = r.i16v();
        f.weapon  = r.u8v();
        f.view    = r.u8v();
        f.actions = r.u8v();
    }
    return r.ok();
}

// -------------------------------------------------------------- entity blocks

void encode_look(ByteWriter& w, const Look& l) noexcept {
    u8 present = 0;
    for (u32 i = 0; i < LOOK_LAYERS; ++i) if (l.layer[i] != 0) present |= static_cast<u8>(1u << i);
    w.u8v(present);
    for (u32 i = 0; i < LOOK_LAYERS; ++i) if (l.layer[i] != 0) w.u16v(l.layer[i]);
    const u8 n = l.remaps > LOOK_REMAPS ? static_cast<u8>(LOOK_REMAPS) : l.remaps;
    w.u8v(n);
    for (u8 i = 0; i < n; ++i) { w.u16v(l.from[i]); w.u16v(l.to[i]); }
}

bool decode_look(ByteReader& r, Look& l) noexcept {
    l = Look{};
    const u8 present = r.u8v();
    for (u32 i = 0; i < LOOK_LAYERS; ++i) if (present & (1u << i)) l.layer[i] = r.u16v();
    // Every pair is read, the first LOOK_REMAPS kept: the stream stays in step
    // whatever a sender says.
    const u8 n = r.u8v();
    for (u8 i = 0; i < n; ++i) {
        const u16 f = r.u16v(), t = r.u16v();
        if (i < LOOK_REMAPS) { l.from[i] = f; l.to[i] = t; l.remaps = static_cast<u8>(i + 1); }
    }
    return r.ok();
}

namespace {

void encode_entity(ByteWriter& w, const EntityBlock& e) noexcept {
    w.u32v(e.handle.raw);
    w.varint(e.mask);

    // Fields follow in ascending bit order. The encoder and decoder must walk
    // the mask in the same direction or a message with two set bits decodes
    // into the wrong fields while a message with one set bit still passes.
    if (e.mask & ent_field::POS) w.vec3_q(e.pos);
    if (e.mask & ent_field::FACING) w.bradv(e.facing);
    if (e.mask & ent_field::ANIMATION_STATE) w.u16v(e.animation_state);
    if (e.mask & ent_field::VISUAL_PARAMS) {
        const u8 n = e.visual_count > MAX_VISUAL_PARAMS
                         ? static_cast<u8>(MAX_VISUAL_PARAMS)
                         : e.visual_count;
        w.varint(n);
        for (u8 i = 0; i < n; ++i) {
            w.u16v(e.visual[i].key);
            w.i32v(e.visual[i].value);
        }
    }
    if (e.mask & ent_field::ANCHOR) w.u32v(e.anchor.raw);
    if (e.mask & ent_field::CONTROLLER_MODE) w.u8v(e.controller_mode);
    if (e.mask & ent_field::DISPLAY_ID) w.u32v(e.display_id);
    if (e.mask & ent_field::AVATAR) w.u16v(e.avatar);
    if (e.mask & ent_field::SOLID) w.u8v(e.solid);
    if (e.mask & ent_field::ATTACK) w.u8v(e.attack_left);
    if (e.mask & ent_field::ATTACK_STEP) {
        w.u8v(e.attack_weapon); w.u8v(e.attack_phase); w.u8v(e.attack_step);
    }
    if (e.mask & ent_field::TRAIL) w.u16v(e.trail);
    if (e.mask & ent_field::LOOK) encode_look(w, e.look);
    // DESTROYED carries no payload.
}

bool decode_entity(ByteReader& r, EntityBlock& e) noexcept {
    e.handle.raw = r.u32v();
    const u64 mask = r.varint();
    if (!r.ok() || mask > 0xFFFFFFFFull) return false;
    e.mask = static_cast<u32>(mask);

    if (e.mask & ent_field::POS) e.pos = r.vec3_q();
    if (e.mask & ent_field::FACING) e.facing = r.bradv();
    if (e.mask & ent_field::ANIMATION_STATE) e.animation_state = r.u16v();
    if (e.mask & ent_field::VISUAL_PARAMS) {
        const u64 n = r.varint();
        if (!r.ok() || n > MAX_VISUAL_PARAMS) return false;
        e.visual_count = static_cast<u8>(n);
        for (u64 i = 0; i < n; ++i) {
            e.visual[i].key   = r.u16v();
            e.visual[i].value = r.i32v();
        }
    }
    if (e.mask & ent_field::ANCHOR) e.anchor.raw = r.u32v();
    if (e.mask & ent_field::CONTROLLER_MODE) e.controller_mode = r.u8v();
    if (e.mask & ent_field::DISPLAY_ID) e.display_id = r.u32v();
    if (e.mask & ent_field::AVATAR) e.avatar = r.u16v();
    if (e.mask & ent_field::SOLID) e.solid = r.u8v();
    if (e.mask & ent_field::ATTACK) e.attack_left = r.u8v();
    if (e.mask & ent_field::ATTACK_STEP) {
        e.attack_weapon = r.u8v(); e.attack_phase = r.u8v(); e.attack_step = r.u8v();
    }
    if (e.mask & ent_field::TRAIL) e.trail = r.u16v();
    if ((e.mask & ent_field::LOOK) && !decode_look(r, e.look)) return false;
    return r.ok();
}

void encode_vfx(ByteWriter& w, const VfxEvent& v) noexcept {
    w.u16v(v.kind);
    w.vec3_q(v.pos);
    w.bradv(v.dir);
    w.u32v(v.param);
}

bool decode_vfx(ByteReader& r, VfxEvent& v) noexcept {
    v.kind  = r.u16v();
    v.pos   = r.vec3_q();
    v.dir   = r.bradv();
    v.param = r.u32v();
    return r.ok();
}

void encode_self(ByteWriter& w, const SelfBlock& s) noexcept {
    w.vec3_raw(s.pos);
    w.vec3_raw(s.vel);
    w.bradv(s.facing);
    w.u8v(s.controller_mode);
    w.u16v(s.state_flags);
    w.u32v(s.anchor.raw);
    w.u8v(s.jumps_used);
    w.u8v(s.attack_left);
    w.u8v(s.attack_cooldown_left);
    w.u8v(s.dashes_used);
    w.i32v(s.sector);
    w.fxraw(s.dash_timer);
    w.fxraw(s.dash_cooldown);
    w.fxraw(s.hover_fuel);
    w.fxraw(s.flight_fuel);
    w.fxraw(s.coyote);
    w.u8v(s.move_state);
    w.u16v(s.state_ticks);
    {
        u8 n = physics::MAX_MOVE_METERS;
        while (n > 0 && s.meters[n - 1] == fx::zero()) --n;
        w.u8v(n);
        for (u8 i = 0; i < n; ++i) w.fxraw(s.meters[i]);
        u8 mask = 0;
        for (u32 a = 0; a < physics::MOVE_AXES; ++a) mask = static_cast<u8>(mask | (s.moves[a].move ? 1u << a : 0u));
        w.u8v(mask);
        for (u32 a = 0; a < physics::MOVE_AXES; ++a) {
            if (!s.moves[a].move) continue;
            w.u8v(s.moves[a].move);
            w.u16v(s.moves[a].left);
            w.fxraw(s.moves[a].dir.x);
            w.fxraw(s.moves[a].dir.y);
        }
    }
    if (s.state_flags & state_flag::GRAPPLING) {
        w.vec3_raw(s.grapple_anchor);
        w.fxraw(s.grapple_dist);
    }
    if (s.state_flags & state_flag::ARMED) {
        const sim::WeaponState& k = s.weapon;
        w.u32v(k.have);
        w.u8v(k.current); w.u8v(k.phase); w.u8v(k.armed); w.u8v(k.swing_phase);
        w.u8v(k.prev_buttons);
        w.u32v(k.entered); w.u32v(k.done);
        w.u32v(k.held_since[0]); w.u32v(k.held_since[1]);
        for (i32 v : k.vars) w.i32v(v);
        for (u16 a : k.ammo) w.u16v(a);
        // The cooldowns, up to the last one set: nearly always none.
        u8 n = sim::MAX_WEAPON_COOLDOWNS;
        while (n > 0 && k.cool[n - 1] == 0) --n;
        w.u8v(n);
        for (u8 i = 0; i < n; ++i) w.i32v(k.cool[i]);
        // The step whose swing is its latest (WeaponState::swung_*).
        w.u8v(k.swung_weapon); w.u8v(k.swung_phase); w.u8v(k.swung_step);
    }
    w.u16v(s.avatar);
    encode_look(w, s.look);
    // The observer's OWN animation state (design 4.1). Ungated: the local agent
    // is the one body that never appears in its own entity stream, so without
    // this it is the only thing on the map whose scripted animation nobody can
    // see. NOT hashed and not predicted -- see SelfBlock's own comment.
    w.u16v(s.animation_state);
    // Which generation of the movement parameters this state was computed with
    // (docs/protocol.md 8.4). Ungated, unlike the grapple block: a client needs
    // it on EVERY snapshot, because its whole job is to answer "were we running
    // the same rules?" and the answer matters most when it is no.
    w.u8v(s.param_epoch);
    w.u64v(s.state_hash);
}

void decode_self(ByteReader& r, SelfBlock& s) noexcept {
    s.pos             = r.vec3_raw();
    s.vel             = r.vec3_raw();
    s.facing          = r.bradv();
    s.controller_mode = r.u8v();
    s.state_flags     = r.u16v();
    s.anchor.raw      = r.u32v();
    s.jumps_used      = r.u8v();
    s.attack_left     = r.u8v();
    s.attack_cooldown_left = r.u8v();
    s.dashes_used     = r.u8v();
    s.sector          = r.i32v();
    s.dash_timer      = r.fxraw();
    s.dash_cooldown   = r.fxraw();
    s.hover_fuel      = r.fxraw();
    s.flight_fuel     = r.fxraw();
    s.coyote          = r.fxraw();
    s.move_state      = r.u8v();
    s.state_ticks     = r.u16v();
    {
        // More than the core holds is read past, not kept: no server sends it.
        const u8 n = r.u8v();
        for (u8 i = 0; i < n; ++i) {
            const fx v = r.fxraw();
            if (i < physics::MAX_MOVE_METERS) s.meters[i] = v;
        }
        const u8 mask = r.u8v();
        for (u32 a = 0; a < physics::MOVE_AXES; ++a) {
            if (!(mask & (1u << a))) continue;
            s.moves[a].move = r.u8v();
            s.moves[a].left = r.u16v();
            s.moves[a].dir.x = r.fxraw();
            s.moves[a].dir.y = r.fxraw();
        }
    }
    if (s.state_flags & state_flag::GRAPPLING) {
        s.grapple_anchor = r.vec3_raw();
        s.grapple_dist   = r.fxraw();
    }
    if (s.state_flags & state_flag::ARMED) {
        sim::WeaponState& k = s.weapon;
        k.have = r.u32v();
        k.current = r.u8v(); k.phase = r.u8v(); k.armed = r.u8v(); k.swing_phase = r.u8v();
        k.prev_buttons = r.u8v();
        k.entered = r.u32v(); k.done = r.u32v();
        k.held_since[0] = r.u32v(); k.held_since[1] = r.u32v();
        for (i32& v : k.vars) v = r.i32v();
        for (u16& a : k.ammo) a = r.u16v();
        // More than the core holds is read past, not kept: no server sends it.
        const u8 n = r.u8v();
        for (u8 i = 0; i < n; ++i) {
            const i32 v = r.i32v();
            if (i < sim::MAX_WEAPON_COOLDOWNS) k.cool[i] = v;
        }
        k.swung_weapon = r.u8v(); k.swung_phase = r.u8v(); k.swung_step = r.u8v();
    }
    s.avatar          = r.u16v();
    decode_look(r, s.look);
    s.animation_state = r.u16v();
    s.param_epoch     = r.u8v();
    s.state_hash      = r.u64v();
}

}  // namespace

// ------------------------------------------------------- movement parameters

void encode_param_update(ByteWriter& w, const ParamUpdate& p) noexcept {
    const u16 n = p.count > MAX_PARAM_FIELDS ? static_cast<u16>(MAX_PARAM_FIELDS) : p.count;
    w.u32v(p.handle.raw);
    w.u8v(p.epoch);
    w.u32v(p.effective_tick);
    w.u8v(p.flags);
    w.u16v(n);
    for (u16 i = 0; i < n; ++i) {
        w.u16v(p.fields[i].id);
        w.i64v(p.fields[i].raw);
    }
}

bool decode_param_update(ByteReader& r, ParamUpdate& p) noexcept {
    ParamUpdate out;
    out.handle.raw     = r.u32v();
    out.epoch          = r.u8v();
    out.effective_tick = r.u32v();
    out.flags          = r.u8v();
    const u16 n        = r.u16v();
    if (!r.ok()) return false;

    // Bounded BEFORE it is used as a loop count. This arrives from the network.
    if (n > MAX_PARAM_FIELDS) return false;

    for (u16 i = 0; i < n; ++i) {
        out.fields[i].id  = r.u16v();
        out.fields[i].raw = r.i64v();
    }
    if (!r.ok()) return false;

    // Committed only once every byte is known good, so a truncated message
    // leaves the caller's parameters alone instead of half-updated.
    out.count = n;
    p = out;
    return true;
}

// ------------------------------------------------------------ SnapshotWriter

SnapshotWriter::SnapshotWriter(u8* buf, size_t capacity) noexcept
    : buf_(buf), w_(buf, capacity) {}

void SnapshotWriter::begin(const SnapshotHeader& h, const SelfBlock& self, i32 hp,
                           const SectorOffset* offsets, u16 offset_count) noexcept {
    w_.u8v(MSG_SNAPSHOT);
    w_.u32v(h.server_tick);
    w_.u32v(h.baseline_tick);
    w_.u32v(h.last_input_tick);
    w_.i8v(h.input_margin);
    w_.u16v(h.flags);

    encode_self(w_, self);
    w_.i32v(hp);   // PrivateBlock: hp only; the stat dict rides the control channel

    // Scripted-discrete sector offsets come BEFORE entities: geometry the
    // rider's own collision depends on matters more than a distant entity, so
    // if the budget runs short it is entities that get dropped.
    const u16 n = offset_count > MAX_SECTOR_OFFSETS ? static_cast<u16>(MAX_SECTOR_OFFSETS)
                                                    : offset_count;
    w_.u16v(n);
    for (u16 i = 0; i < n; ++i) {
        w_.u32v(offsets[i].sector);
        w_.vec3_raw(offsets[i].offset);
    }

    // Reserve the entity count. It is fixed-width precisely so it can be patched
    // here once the budget has been filled (docs/protocol.md 5).
    entity_count_pos_ = w_.mark();
    w_.u16v(0);

    began_ = w_.ok();
}

bool SnapshotWriter::try_entity(const EntityBlock& e) noexcept {
    if (!began_ || vfx_started_ || entity_count_ == 0xFFFF) return false;
    const size_t m = w_.mark();
    encode_entity(w_, e);
    // ROOM FOR THE TAIL: finish() writes the vfx count (a u16) after the
    // entities, and an entity that fills the datagram to within a byte of the
    // end leaves it nowhere to go -- finish() then fails and the server sends
    // NOTHING that tick, every tick, for as long as the sizes land that way.
    if (!w_.ok() || (!vfx_started_ && w_.remaining() < 2)) {
        w_.rollback(m);   // leaves the buffer exactly as it was
        return false;
    }
    ++entity_count_;
    return true;
}

bool SnapshotWriter::try_vfx(const VfxEvent& v) noexcept {
    if (!began_ || sound_started_) return false;
    if (!vfx_started_) {
        const size_t m = w_.mark();
        vfx_count_pos_ = m;
        w_.u16v(0);
        if (!w_.ok()) { w_.rollback(m); return false; }
        vfx_started_ = true;
    }
    if (vfx_count_ == 0xFFFF) return false;
    const size_t m = w_.mark();
    encode_vfx(w_, v);
    if (!w_.ok()) {
        w_.rollback(m);
        return false;
    }
    ++vfx_count_;
    return true;
}

bool SnapshotWriter::try_sound(const SoundEvent& s) noexcept {
    if (!began_) return false;
    if (!sound_started_) {
        // The vfx count has to be in place before anything follows it.
        const size_t m0 = w_.mark();
        if (!vfx_started_) {
            vfx_count_pos_ = w_.mark();
            w_.u16v(0);
        }
        sound_count_pos_ = w_.mark();
        w_.u16v(0);
        if (!w_.ok()) { w_.rollback(m0); return false; }
        vfx_started_ = true;
        sound_started_ = true;
    }
    if (sound_count_ >= MAX_SOUNDS) return false;
    const size_t m = w_.mark();
    w_.u32v(s.sound);
    w_.vec3_q(s.pos);
    w_.u32v(s.source);
    w_.u8v(s.flags);
    if (!w_.ok()) { w_.rollback(m); return false; }
    ++sound_count_;
    return true;
}

size_t SnapshotWriter::finish() noexcept {
    if (!began_) return 0;
    if (!vfx_started_) {
        vfx_count_pos_ = w_.mark();
        w_.u16v(0);
        if (!w_.ok()) return 0;
        vfx_started_ = true;
    }
    buf_[entity_count_pos_]     = static_cast<u8>(entity_count_ & 0xFF);
    buf_[entity_count_pos_ + 1] = static_cast<u8>((entity_count_ >> 8) & 0xFF);
    buf_[vfx_count_pos_]        = static_cast<u8>(vfx_count_ & 0xFF);
    buf_[vfx_count_pos_ + 1]    = static_cast<u8>((vfx_count_ >> 8) & 0xFF);
    if (sound_started_) {
        buf_[sound_count_pos_]     = static_cast<u8>(sound_count_ & 0xFF);
        buf_[sound_count_pos_ + 1] = static_cast<u8>((sound_count_ >> 8) & 0xFF);
    }
    return w_.size();
}

// ------------------------------------------------------------ decode_snapshot

bool decode_snapshot(ByteReader& r, DecodedSnapshot& out) noexcept {
    if (r.u8v() != MSG_SNAPSHOT) return false;

    out.header.server_tick     = r.u32v();
    out.header.baseline_tick   = r.u32v();
    out.header.last_input_tick = r.u32v();
    out.header.input_margin    = r.i8v();
    out.header.flags           = r.u16v();

    decode_self(r, out.self);
    out.hp = r.i32v();
    if (!r.ok()) return false;

    const u16 ocount = r.u16v();
    if (!r.ok() || ocount > MAX_SECTOR_OFFSETS) return false;
    out.sector_offset_count = ocount;
    for (u16 i = 0; i < ocount; ++i) {
        out.sector_offsets[i].sector = r.u32v();
        out.sector_offsets[i].offset = r.vec3_raw();
    }
    if (!r.ok()) return false;

    const u16 ecount = r.u16v();
    if (!r.ok() || ecount > MAX_ENTITIES) return false;
    out.entity_count = ecount;
    for (u16 i = 0; i < ecount; ++i) {
        if (!decode_entity(r, out.entities[i])) return false;
    }

    const u16 vcount = r.u16v();
    if (!r.ok() || vcount > MAX_VFX) return false;
    out.vfx_count = vcount;
    for (u16 i = 0; i < vcount; ++i) {
        if (!decode_vfx(r, out.vfx[i])) return false;
    }

    // Sounds, if the snapshot has any (docs/protocol.md §5.7).
    out.sound_count = 0;
    if (r.ok() && r.remaining() > 0) {
        const u16 scount = r.u16v();
        if (!r.ok() || scount > MAX_SOUNDS) return false;
        out.sound_count = scount;
        for (u16 i = 0; i < scount; ++i) {
            SoundEvent& s = out.sounds[i];
            s.sound  = r.u32v();
            s.pos    = r.vec3_q();
            s.source = r.u32v();
            s.flags  = r.u8v();
        }
    }

    return r.ok();
}


// --- UI (docs/protocol.md section 11) ----------------------------------------
//
// A panel is not simulation state: it is never hashed, never in a snapshot, and
// has no bearing on the determinism gate. It is here rather than in the server
// because the CLIENT has to decode it, and the client's decoder is this one.

static void encode_text(ByteWriter& w, const UiText& t) noexcept {
    const u16 n = t.len > UI_MAX_TEXT ? static_cast<u16>(UI_MAX_TEXT) : t.len;
    w.u16v(n);
    w.bytes(reinterpret_cast<const u8*>(t.text), n);
}

// Refuses rather than truncates. A length past the cap is not a long string, it
// is a malformed message, and quietly keeping the first 128 bytes of one would
// leave the reader mid-field with everything after it misaligned.
static bool decode_text(ByteReader& r, UiText& out) noexcept {
    const u16 n = r.u16v();
    if (!r.ok() || n > UI_MAX_TEXT) return false;
    const u8* p = r.bytes(n);
    if (p == nullptr) return false;
    out.len = n;
    for (u16 i = 0; i < n; ++i) out.text[i] = static_cast<char>(p[i]);
    if (n < UI_MAX_TEXT) out.text[n] = '\0';
    return true;
}

void encode_ui_open(ByteWriter& w, const UiPanel& p) noexcept {
    const u16 n = p.count > UI_MAX_WIDGETS ? static_cast<u16>(UI_MAX_WIDGETS) : p.count;
    w.u32v(p.ui_id);
    encode_text(w, p.title);
    w.u16v(n);
    for (u16 i = 0; i < n; ++i) {
        const UiWidget& g = p.widgets[i];
        const u16 opts = g.option_count > UI_MAX_OPTIONS ? static_cast<u16>(UI_MAX_OPTIONS)
                                                        : g.option_count;
        w.u8v(static_cast<u8>(g.kind));
        w.u16v(g.id);
        encode_text(w, g.label);
        w.i32v(g.value);
        w.i32v(g.lo);
        w.i32v(g.hi);
        w.u16v(opts);
        for (u16 k = 0; k < opts; ++k) encode_text(w, g.options[k]);
    }
    // TRAILING and optional, so a panel without one is byte-for-byte what it
    // was, and a reader from before templates stops at the widgets and draws
    // the default panel.
    if (p.template_len > 0 || p.flags != 0) {
        const u16 n = p.template_len > UI_MAX_TEMPLATE ? static_cast<u16>(UI_MAX_TEMPLATE) : p.template_len;
        w.u16v(n);
        w.bytes(reinterpret_cast<const u8*>(p.template_name), n);
        // Flags after it, only when set: an empty template then says "none".
        if (p.flags != 0) w.u8v(p.flags);
    }
}

bool valid_template_name(const char* s, size_t n) noexcept {
    if (n == 0 || n > UI_MAX_TEMPLATE) return false;
    for (size_t i = 0; i < n; ++i) {
        const char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}

bool decode_ui_open(ByteReader& r, UiPanel& out) noexcept {
    UiPanel p;
    p.ui_id = r.u32v();
    if (!decode_text(r, p.title)) return false;
    const u16 n = r.u16v();
    if (!r.ok() || n > UI_MAX_WIDGETS) return false;
    for (u16 i = 0; i < n; ++i) {
        UiWidget& g = p.widgets[i];
        const u8 kind = r.u8v();
        // An unknown kind is refused, not skipped: its fields are not
        // self-describing, so there is no way to step over one and stay aligned.
        if (!r.ok() || kind > static_cast<u8>(UiWidgetKind::Toggle)) return false;
        g.kind = static_cast<UiWidgetKind>(kind);
        g.id = r.u16v();
        if (!decode_text(r, g.label)) return false;
        g.value = r.i32v();
        g.lo = r.i32v();
        g.hi = r.i32v();
        const u16 opts = r.u16v();
        if (!r.ok() || opts > UI_MAX_OPTIONS) return false;
        g.option_count = opts;
        for (u16 k = 0; k < opts; ++k) if (!decode_text(r, g.options[k])) return false;
    }
    if (!r.ok()) return false;
    // The template, when the message goes on. A byte or two that is not a whole
    // name is a message cut short, not "no template", and is refused.
    if (r.remaining() > 0) {
        const u16 tn = r.u16v();
        if (!r.ok() || tn > UI_MAX_TEMPLATE) return false;
        if (tn > 0) {
            const u8* tp = r.bytes(tn);
            if (tp == nullptr || !valid_template_name(reinterpret_cast<const char*>(tp), tn)) return false;
            p.template_len = static_cast<u8>(tn);
            for (u16 i = 0; i < tn; ++i) p.template_name[i] = static_cast<char>(tp[i]);
        }
        // Flags, when the message goes on. An empty template with no flags
        // after it is a message cut short, and refused.
        if (r.remaining() > 0) {
            p.flags = r.u8v();
            if (!r.ok()) return false;
        } else if (tn == 0) {
            return false;
        }
    }
    p.count = n;
    out = p;
    return true;
}

// --- markers (docs/MARKERS.md) -------------------------------------------------

namespace {
bool valid_value_name(const u8* s, size_t n) noexcept {
    if (n == 0 || n > HUD_NAME_MAX) return false;
    for (size_t i = 0; i < n; ++i) {
        const u8 ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}
}  // namespace

void encode_mark_set(ByteWriter& w, const MarkSet& m) noexcept {
    w.u32v(m.id);
    w.u8v(m.anchor);
    if (m.anchor == MARK_AT_POINT) w.vec3_raw(m.pos);
    else w.u32v(m.handle.raw);
    const u16 tn = m.template_len > UI_MAX_TEMPLATE ? static_cast<u16>(UI_MAX_TEMPLATE) : m.template_len;
    w.u16v(tn);
    w.bytes(reinterpret_cast<const u8*>(m.template_name), tn);
    w.u8v(m.flags);
    w.fxraw(m.height);
    w.fxraw(m.range);
    const u8 n = m.count > MARK_MAX_VALUES ? static_cast<u8>(MARK_MAX_VALUES) : m.count;
    w.u8v(n);
    for (u8 i = 0; i < n; ++i) {
        const MarkValue& v = m.values[i];
        const u16 nn = v.name_len > HUD_NAME_MAX ? static_cast<u16>(HUD_NAME_MAX) : v.name_len;
        w.u16v(nn);
        w.bytes(reinterpret_cast<const u8*>(v.name), nn);
        w.u8v(v.kind);
        if (v.kind == HUD_STR) encode_text(w, v.text);
        else w.i32v(v.number);
    }
}

// Refused whole, as UI_OPEN is: an anchor, a value kind or a name it does not
// know is not something a reader can step over and stay aligned.
bool decode_mark_set(ByteReader& r, MarkSet& out) noexcept {
    MarkSet m;
    m.id = r.u32v();
    m.anchor = r.u8v();
    if (!r.ok() || m.anchor > MARK_AT_POINT) return false;
    if (m.anchor == MARK_AT_POINT) m.pos = r.vec3_raw();
    else m.handle.raw = r.u32v();
    const u16 tn = r.u16v();
    if (!r.ok() || tn > UI_MAX_TEMPLATE) return false;
    const u8* tp = r.bytes(tn);
    if (tp == nullptr || !valid_template_name(reinterpret_cast<const char*>(tp), tn)) return false;
    m.template_len = static_cast<u8>(tn);
    for (u16 i = 0; i < tn; ++i) m.template_name[i] = static_cast<char>(tp[i]);
    m.flags = r.u8v();
    m.height = r.fxraw();
    m.range = r.fxraw();
    const u8 n = r.u8v();
    if (!r.ok() || n > MARK_MAX_VALUES) return false;
    for (u8 i = 0; i < n; ++i) {
        MarkValue& v = m.values[i];
        const u16 nn = r.u16v();
        if (!r.ok() || nn > HUD_NAME_MAX) return false;
        const u8* np = r.bytes(nn);
        if (np == nullptr || !valid_value_name(np, nn)) return false;
        v.name_len = static_cast<u8>(nn);
        for (u16 k = 0; k < nn; ++k) v.name[k] = static_cast<char>(np[k]);
        v.kind = r.u8v();
        if (!r.ok() || v.kind > HUD_STR) return false;
        if (v.kind == HUD_STR) { if (!decode_text(r, v.text)) return false; }
        else v.number = r.i32v();
    }
    if (!r.ok()) return false;
    m.count = n;
    out = m;
    return true;
}

void encode_camera_set(ByteWriter& w, const CameraSet& c) noexcept {
    const u16 n = c.count > CAMERA_MAX_FIELDS ? static_cast<u16>(CAMERA_MAX_FIELDS) : c.count;
    w.u16v(n);
    for (u16 i = 0; i < n; ++i) {
        w.u8v(c.fields[i].id);
        w.i32v(c.fields[i].milli);
    }
}

bool decode_camera_set(ByteReader& r, CameraSet& out) noexcept {
    CameraSet c;
    const u16 n = r.u16v();
    if (!r.ok() || n > CAMERA_MAX_FIELDS) return false;
    for (u16 i = 0; i < n; ++i) {
        c.fields[i].id = r.u8v();
        c.fields[i].milli = r.i32v();
    }
    if (!r.ok()) return false;
    c.count = n;
    out = c;
    return true;
}

// The same codec as CameraSet, on purpose: one convention for "a small bag of
// named thousandths", so a reader who has understood one has understood both.
void encode_screen_set(ByteWriter& w, const ScreenSet& s) noexcept {
    const u16 n = s.count > SCREEN_MAX_FIELDS ? static_cast<u16>(SCREEN_MAX_FIELDS) : s.count;
    w.u16v(n);
    for (u16 i = 0; i < n; ++i) {
        w.u8v(s.fields[i].id);
        w.i32v(s.fields[i].milli);
    }
}

bool decode_screen_set(ByteReader& r, ScreenSet& out) noexcept {
    ScreenSet s;
    const u16 n = r.u16v();
    if (!r.ok() || n > SCREEN_MAX_FIELDS) return false;
    for (u16 i = 0; i < n; ++i) {
        s.fields[i].id = r.u8v();
        s.fields[i].milli = r.i32v();
    }
    if (!r.ok()) return false;
    s.count = n;
    out = s;
    return true;
}

void encode_sky_set(ByteWriter& w, const SkySet& s) noexcept {
    w.u8v(static_cast<u8>(s.kind));
    const u8 n = s.images > SKY_MAX_IMAGES ? static_cast<u8>(SKY_MAX_IMAGES) : s.images;
    w.u8v(n);
    for (u8 i = 0; i < n; ++i) encode_text(w, s.image[i]);
    const u16 f = s.count > SKY_MAX_FIELDS ? static_cast<u16>(SKY_MAX_FIELDS) : s.count;
    w.u16v(f);
    for (u16 i = 0; i < f; ++i) {
        w.u8v(s.fields[i].id);
        w.i32v(s.fields[i].milli);
    }
}

bool decode_sky_set(ByteReader& r, SkySet& out) noexcept {
    SkySet s;
    const u8 k = r.u8v();
    // An unknown kind is refused: a picture wrapped the wrong way round the
    // world is a stranger result than the sky staying as it was.
    if (!r.ok() || k > static_cast<u8>(SkyKind::Cube)) return false;
    s.kind = static_cast<SkyKind>(k);
    const u8 n = r.u8v();
    if (!r.ok() || n > SKY_MAX_IMAGES) return false;
    for (u8 i = 0; i < n; ++i) if (!decode_text(r, s.image[i])) return false;
    s.images = n;
    const u16 f = r.u16v();
    if (!r.ok() || f > SKY_MAX_FIELDS) return false;
    for (u16 i = 0; i < f; ++i) {
        s.fields[i].id = r.u8v();
        s.fields[i].milli = r.i32v();
    }
    if (!r.ok()) return false;
    s.count = f;
    out = s;
    return true;
}

void encode_sector_set(ByteWriter& w, const SectorSet& s) noexcept {
    w.u32v(s.sector);
    w.u8v(static_cast<u8>(s.what));
    switch (s.what) {
        case SectorWhat::Floor:
        case SectorWhat::Ceiling: encode_text(w, s.texture); break;
        case SectorWhat::Light: w.u8v(s.light); w.u16v(s.fade_ms); break;
        case SectorWhat::Effect: {
            w.u8v(s.fx.kind);
            w.u8v(s.on ? 1 : 0);
            const u8 n = s.on ? (s.fx.count > geom::MAX_FX_PARAMS ? geom::MAX_FX_PARAMS : s.fx.count) : 0;
            w.u8v(n);
            for (u8 i = 0; i < n; ++i) { w.u8v(s.fx.params[i].key); w.i64v(s.fx.params[i].raw); }
            break;
        }
    }
}

bool decode_sector_set(ByteReader& r, SectorSet& out) noexcept {
    SectorSet s;
    s.sector = r.u32v();
    const u8 what = r.u8v();
    // An unknown change is refused: drawn as some other one, it would be wrong.
    if (!r.ok() || what > static_cast<u8>(SectorWhat::Effect)) return false;
    s.what = static_cast<SectorWhat>(what);
    switch (s.what) {
        case SectorWhat::Floor:
        case SectorWhat::Ceiling:
            if (!decode_text(r, s.texture)) return false;
            break;
        case SectorWhat::Light:
            s.light = r.u8v();
            s.fade_ms = r.u16v();
            break;
        case SectorWhat::Effect: {
            s.fx.kind = r.u8v();
            s.on = r.u8v() != 0;
            const u8 n = r.u8v();
            if (!r.ok() || s.fx.kind < 1 || s.fx.kind > geom::FX_KIND_COUNT || n > geom::MAX_FX_PARAMS) return false;
            for (u8 i = 0; i < n; ++i) {
                s.fx.params[i].key = r.u8v();
                s.fx.params[i].raw = r.i64v();
                // A setting this kind does not take is refused, as the map loader does.
                if (!r.ok() || !geom::fx_takes(s.fx.kind, s.fx.params[i].key)) return false;
            }
            s.fx.count = n;
            break;
        }
    }
    if (!r.ok()) return false;
    out = s;
    return true;
}

void encode_side_texture(ByteWriter& w, const SideTexture& t) noexcept {
    w.u32v(t.side);
    w.u8v(static_cast<u8>(t.field));
    encode_text(w, t.name);
}

bool decode_side_texture(ByteReader& r, SideTexture& out) noexcept {
    SideTexture t;
    t.side = r.u32v();
    const u8 f = r.u8v();
    // An unknown field is refused rather than defaulted: writing a texture to
    // the wrong band of a wall is a stranger result than nothing happening.
    if (!r.ok() || f > static_cast<u8>(SideField::Lower)) return false;
    t.field = static_cast<SideField>(f);
    if (!decode_text(r, t.name)) return false;
    out = t;
    return true;
}

void encode_music_cue(ByteWriter& w, const MusicCue& m) noexcept {
    encode_text(w, m.cue);
    w.u16v(m.fade_ms);
    w.u8v(static_cast<u8>(m.at));
}

bool decode_music_cue(ByteReader& r, MusicCue& out) noexcept {
    MusicCue m;
    if (!decode_text(r, m.cue)) return false;
    m.fade_ms = r.u16v();
    const u8 at = r.u8v();
    // An unknown moment is refused rather than defaulted, as a side field is:
    // the sender and this reader disagree about the protocol, and a cue that
    // lands at some other time than asked is the quieter of the two failures.
    if (!r.ok() || at > static_cast<u8>(MusicAt::Now)) return false;
    m.at = static_cast<MusicAt>(at);
    out = m;
    return true;
}

void encode_ui_event(ByteWriter& w, const UiEvent& e) noexcept {
    w.u32v(e.ui_id);
    w.u16v(e.widget_id);
    w.i32v(e.value);
}

bool decode_ui_event(ByteReader& r, UiEvent& out) noexcept {
    UiEvent e;
    e.ui_id = r.u32v();
    e.widget_id = r.u16v();
    e.value = r.i32v();
    if (!r.ok()) return false;
    out = e;
    return true;
}

}  // namespace dnp::proto
