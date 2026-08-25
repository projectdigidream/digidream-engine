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
    }
    return r.ok();
}

// -------------------------------------------------------------- entity blocks

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
    w.u8v(s.dashes_used);
    w.i32v(s.sector);
    w.fxraw(s.dash_timer);
    w.fxraw(s.dash_cooldown);
    w.fxraw(s.hover_fuel);
    w.fxraw(s.flight_fuel);
    if (s.state_flags & state_flag::GRAPPLING) {
        w.vec3_raw(s.grapple_anchor);
        w.fxraw(s.grapple_dist);
    }
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
    s.dashes_used     = r.u8v();
    s.sector          = r.i32v();
    s.dash_timer      = r.fxraw();
    s.dash_cooldown   = r.fxraw();
    s.hover_fuel      = r.fxraw();
    s.flight_fuel     = r.fxraw();
    if (s.state_flags & state_flag::GRAPPLING) {
        s.grapple_anchor = r.vec3_raw();
        s.grapple_dist   = r.fxraw();
    }
    s.state_hash      = r.u64v();
}

}  // namespace

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
    if (!w_.ok()) {
        w_.rollback(m);   // leaves the buffer exactly as it was
        return false;
    }
    ++entity_count_;
    return true;
}

bool SnapshotWriter::try_vfx(const VfxEvent& v) noexcept {
    if (!began_) return false;
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

    return r.ok();
}

}  // namespace dnp::proto
