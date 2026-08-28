//
// Wire codec tests.
//
// Round-tripping alone is not enough: a codec whose encoder and decoder are
// wrong in the SAME way round-trips perfectly. So these also pin exact encoded
// sizes (which is what the datagram budget is computed from), feed truncated and
// hostile input, and check that the budget-filling encoder rolls back cleanly.
//
#include <cstdio>

#include "dnp/math/rng.hpp"
#include "dnp/physics/params_table.hpp"
#include "dnp/proto/messages.hpp"

using namespace dnp;
using namespace dnp::proto;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

// ------------------------------------------------------------------ primitives

static void test_bytes() {
    std::printf("bytes: little-endian round trip, bounds safety\n");
    u8 buf[64];
    {
        ByteWriter w(buf, sizeof(buf));
        w.u8v(0x12); w.u16v(0x3456); w.u32v(0x789ABCDE); w.u64v(0x0123456789ABCDEFull);
        w.i16v(-2); w.i32v(-3); w.i64v(-4);
        CHECK(w.ok());
        CHECK(w.size() == 1 + 2 + 4 + 8 + 2 + 4 + 8);

        // Byte order is written out explicitly, so pin it rather than assume it.
        CHECK(buf[0] == 0x12);
        CHECK(buf[1] == 0x56 && buf[2] == 0x34);

        ByteReader r(buf, w.size());
        CHECK(r.u8v() == 0x12);
        CHECK(r.u16v() == 0x3456);
        CHECK(r.u32v() == 0x789ABCDE);
        CHECK(r.u64v() == 0x0123456789ABCDEFull);
        CHECK(r.i16v() == -2);
        CHECK(r.i32v() == -3);
        CHECK(r.i64v() == -4);
        CHECK(r.ok() && r.at_end());
    }

    // Reading past the end poisons the cursor instead of running off the buffer.
    {
        ByteReader r(buf, 2);
        r.u32v();
        CHECK(!r.ok());
        r.u8v();
        CHECK(!r.ok());   // stays poisoned
    }

    // Writing past capacity fails cleanly and stays failed.
    {
        u8 small[4];
        ByteWriter w(small, sizeof(small));
        w.u32v(1);
        CHECK(w.ok());
        w.u8v(2);
        CHECK(!w.ok());
        CHECK(w.size() == 4);   // nothing partial was written
    }
}

static void test_varint() {
    std::printf("bytes: varint round trip and hostile-input caps\n");
    const u64 values[] = {0, 1, 127, 128, 300, 16383, 16384, 0xFFFFFFFFull,
                          0xFFFFFFFFFFFFFFFFull};
    for (u64 v : values) {
        u8 buf[16];
        ByteWriter w(buf, sizeof(buf));
        w.varint(v);
        CHECK(w.ok());
        ByteReader r(buf, w.size());
        CHECK(r.varint() == v);
        CHECK(r.ok());
    }
    { u8 b[16]; ByteWriter w(b, sizeof(b)); w.varint(127);   CHECK(w.size() == 1); }
    { u8 b[16]; ByteWriter w(b, sizeof(b)); w.varint(128);   CHECK(w.size() == 2); }
    { u8 b[16]; ByteWriter w(b, sizeof(b)); w.varint(16384); CHECK(w.size() == 3); }

    // An unterminated run of continuation bytes must be rejected, not chased.
    {
        u8 evil[32];
        for (auto& b : evil) b = 0x80;
        ByteReader r(evil, sizeof(evil));
        r.varint();
        CHECK(!r.ok());
    }
}

static void test_quantisation() {
    std::printf("bytes: fxq quantisation is symmetric and bounded\n");
    CHECK(fx_to_fxq(fx::zero()) == 0);
    CHECK(fxq_to_fx(fx_to_fxq(fx::one())) == fx::one());

    // Mirror symmetry must survive quantisation, for the same reason multiply
    // truncates toward zero (docs/physics-contract.md rule 4).
    Rng rng(0xC0DE);
    for (int i = 0; i < 20000; ++i) {
        const fx v = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30);
        CHECK(fx_to_fxq(-v) == -fx_to_fxq(v));
        // Error is bounded by one quantisation step.
        const fx back = fxq_to_fx(fx_to_fxq(v));
        CHECK(fx_abs(back - v).raw < (static_cast<i64>(1) << FXQ_SHIFT));
    }

    // Out-of-range positions saturate rather than wrapping to the far side of
    // the map, which is the failure an unchecked cast would produce.
    CHECK(fx_to_fxq(fx::from_int(10000000)) == static_cast<i32>(FXQ_MAX_RAW));
    CHECK(fx_to_fxq(fx::from_int(-10000000)) == static_cast<i32>(FXQ_MIN_RAW));

    // Input axes, protocol R1. The clamp is deliberately symmetric: Q1.15 could
    // represent -32768, but using it would make full-left stronger than
    // full-right and break mirror symmetry at exactly the input the player holds
    // most often.
    CHECK(axis_from_fx(fx::zero()) == 0);
    CHECK(axis_from_fx(fx::one()) == 32767);
    CHECK(axis_from_fx(-fx::one()) == -32767);
    CHECK(axis_from_fx(fx::from_int(5)) == 32767);     // clamped
    CHECK(axis_from_fx(fx::from_int(-5)) == -32767);   // and symmetrically
    for (int i = 0; i < 10000; ++i) {
        const fx v = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 33);
        CHECK(axis_from_fx(-v) == -axis_from_fx(v));
    }
    // Sweep the saturation boundary itself, which the random draws above will
    // essentially never hit.
    for (i64 d = -4; d <= 4; ++d) {
        const fx v = fx::from_raw(fx::ONE + d);
        CHECK(axis_from_fx(-v) == -axis_from_fx(v));
    }
}

// ---------------------------------------------------------------- input bundle

static void test_input_bundle() {
    std::printf("input: round trip, size, reserved bits, truncation\n");
    Rng rng(77);
    for (int iter = 0; iter < 2000; ++iter) {
        InputBundle in;
        in.client_tick       = rng.next_u32();
        in.ack_snapshot_tick = rng.next_u32();
        in.frame_count       = static_cast<u8>(rng.next_below(MAX_INPUT_FRAMES) + 1);
        for (u8 i = 0; i < in.frame_count; ++i) {
            in.frames[i].buttons =
                static_cast<u16>(rng.next_u32() & ~button::RESERVED_MASK);
            in.frames[i].move_x = static_cast<i16>(rng.next_u32());
            in.frames[i].move_y = static_cast<i16>(rng.next_u32());
            in.frames[i].aim    = brad(rng.next_u32());
        }

        u8 buf[512];
        ByteWriter w(buf, sizeof(buf));
        encode_input_bundle(w, in);
        CHECK(w.ok());
        CHECK(w.size() == 1u + 4 + 4 + 1 + 10u * in.frame_count);

        InputBundle out;
        ByteReader r(buf, w.size());
        CHECK(decode_input_bundle(r, out));
        CHECK(out.client_tick == in.client_tick);
        CHECK(out.ack_snapshot_tick == in.ack_snapshot_tick);
        CHECK(out.frame_count == in.frame_count);
        for (u8 i = 0; i < in.frame_count; ++i) CHECK(out.frames[i] == in.frames[i]);
    }

    // Reserved bits are stripped on encode rather than trusted, so they cannot
    // silently consume bits the frozen protocol will need later.
    {
        InputBundle in;
        in.frame_count = 1;
        in.frames[0].buttons = 0xFFFF;
        u8 buf[64];
        ByteWriter w(buf, sizeof(buf));
        encode_input_bundle(w, in);
        InputBundle out;
        ByteReader r(buf, w.size());
        CHECK(decode_input_bundle(r, out));
        CHECK((out.frames[0].buttons & button::RESERVED_MASK) == 0);
        CHECK(out.frames[0].buttons == 0x07FF);
    }

    // Every truncation of a valid bundle must be rejected, never half-accepted.
    {
        InputBundle in;
        in.client_tick = 5;
        in.frame_count = 4;
        u8 buf[128];
        ByteWriter w(buf, sizeof(buf));
        encode_input_bundle(w, in);
        for (size_t len = 0; len < w.size(); ++len) {
            InputBundle out;
            ByteReader r(buf, len);
            CHECK(!decode_input_bundle(r, out));
        }
    }

    // A frame count beyond the cap is rejected before it can index out of range.
    {
        u8 buf[16] = {MSG_INPUT_BUNDLE, 0, 0, 0, 0, 0, 0, 0, 0, 255};
        InputBundle out;
        ByteReader r(buf, sizeof(buf));
        CHECK(!decode_input_bundle(r, out));
    }
}

// -------------------------------------------------------------------- snapshot

static EntityBlock make_entity(Rng& rng) {
    EntityBlock e;
    e.handle = Handle::make(rng.next_below(0xFFFFFF), static_cast<u8>(rng.next_below(256)));
    e.mask   = ent_field::POS | ent_field::FACING | ent_field::ANIMATION_STATE;
    e.pos    = fx3{fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30),
                   fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30),
                   fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30)};
    e.facing = brad(rng.next_u32());
    e.animation_state = static_cast<u16>(rng.next_u32());
    return e;
}

static void test_snapshot_roundtrip() {
    std::printf("snapshot: round trip across all field combinations\n");
    Rng rng(31337);

    for (int iter = 0; iter < 500; ++iter) {
        SnapshotHeader h;
        h.server_tick     = rng.next_u32();
        h.baseline_tick   = rng.next_u32();
        h.last_input_tick = rng.next_u32();
        h.input_margin    = static_cast<i8>(rng.next_u32());
        h.flags           = static_cast<u16>(rng.next_u32() & 1);

        SelfBlock self;
        self.pos = fx3{fx::from_raw(static_cast<i64>(rng.next_u64()) >> 20),
                       fx::from_raw(static_cast<i64>(rng.next_u64()) >> 20),
                       fx::from_raw(static_cast<i64>(rng.next_u64()) >> 20)};
        self.vel = fx3{fx::from_raw(static_cast<i64>(rng.next_u64()) >> 20),
                       fx::from_raw(static_cast<i64>(rng.next_u64()) >> 20),
                       fx::from_raw(static_cast<i64>(rng.next_u64()) >> 20)};
        self.facing      = brad(rng.next_u32());
        self.state_flags = static_cast<u16>(rng.next_u32() & 0x3F);
        self.anchor      = Handle::make(rng.next_below(1000), 3);
        self.jumps_used  = static_cast<u8>(rng.next_below(4));
        self.state_hash  = rng.next_u64();
        const i32 hp = static_cast<i32>(rng.next_u32());

        u8 buf[MAX_DATAGRAM_BYTES];
        SnapshotWriter sw(buf, sizeof(buf));
        sw.begin(h, self, hp);

        const int want = static_cast<int>(rng.next_below(12));
        EntityBlock sent[12];
        int written = 0;
        for (int i = 0; i < want; ++i) {
            sent[i] = make_entity(rng);
            // Exercise every optional field, not just the common three.
            if (i % 3 == 0) {
                sent[i].mask |= ent_field::VISUAL_PARAMS;
                sent[i].visual_count = static_cast<u8>(rng.next_below(MAX_VISUAL_PARAMS) + 1);
                for (u8 v = 0; v < sent[i].visual_count; ++v) {
                    sent[i].visual[v].key   = static_cast<u16>(rng.next_u32());
                    sent[i].visual[v].value = static_cast<i32>(rng.next_u32());
                }
            }
            if (i % 4 == 0) {
                sent[i].mask |= ent_field::ANCHOR | ent_field::CONTROLLER_MODE;
                sent[i].anchor = Handle::make(rng.next_below(500), 1);
                sent[i].controller_mode = static_cast<u8>(rng.next_below(5));
            }
            if (i % 5 == 0) {
                sent[i].mask |= ent_field::DISPLAY_ID;
                sent[i].display_id = rng.next_u32();
            }
            if (sw.try_entity(sent[i])) ++written;
        }

        VfxEvent vsent[4];
        int vwritten = 0;
        const int vwant = static_cast<int>(rng.next_below(4));
        for (int i = 0; i < vwant; ++i) {
            vsent[i].kind  = static_cast<u16>(rng.next_u32());
            vsent[i].pos   = fx3{fx::from_int(1), fx::from_int(2), fx::from_int(3)};
            vsent[i].dir   = brad(rng.next_u32());
            vsent[i].param = rng.next_u32();
            if (sw.try_vfx(vsent[i])) ++vwritten;
        }

        const size_t n = sw.finish();
        CHECK(n > 0 && n <= sizeof(buf));

        DecodedSnapshot got;
        ByteReader r(buf, n);
        CHECK(decode_snapshot(r, got));
        CHECK(r.at_end());   // no trailing garbage, no under-read

        CHECK(got.header.server_tick == h.server_tick);
        CHECK(got.header.baseline_tick == h.baseline_tick);
        CHECK(got.header.last_input_tick == h.last_input_tick);
        CHECK(got.header.input_margin == h.input_margin);
        CHECK(got.header.flags == h.flags);
        CHECK(got.hp == hp);

        // Self state is full precision: it must survive EXACTLY, because
        // reconciliation compares it against the client's own arithmetic.
        CHECK(got.self.pos == self.pos);
        CHECK(got.self.vel == self.vel);
        CHECK(got.self.facing == self.facing);
        CHECK(got.self.anchor == self.anchor);
        CHECK(got.self.state_hash == self.state_hash);
        CHECK(got.self.jumps_used == self.jumps_used);

        CHECK(got.entity_count == written);
        for (int i = 0; i < written; ++i) {
            CHECK(got.entities[i].handle == sent[i].handle);
            CHECK(got.entities[i].mask == sent[i].mask);
            CHECK(got.entities[i].facing == sent[i].facing);
            CHECK(got.entities[i].animation_state == sent[i].animation_state);
            // Position is quantised, so compare within one step, not exactly.
            CHECK(fx_abs(got.entities[i].pos.x - sent[i].pos.x).raw <= (1LL << FXQ_SHIFT));
            if (sent[i].mask & ent_field::VISUAL_PARAMS) {
                CHECK(got.entities[i].visual_count == sent[i].visual_count);
                for (u8 v = 0; v < sent[i].visual_count; ++v) {
                    CHECK(got.entities[i].visual[v].key == sent[i].visual[v].key);
                    CHECK(got.entities[i].visual[v].value == sent[i].visual[v].value);
                }
            }
            if (sent[i].mask & ent_field::DISPLAY_ID) {
                CHECK(got.entities[i].display_id == sent[i].display_id);
            }
        }
        CHECK(got.vfx_count == vwritten);
    }
}

static void test_snapshot_budget() {
    std::printf("snapshot: budget is enforced structurally, rollback is clean\n");
    Rng rng(4242);

    SnapshotHeader h;
    SelfBlock self;

    // Offer far more entities than can fit and confirm the encoder stops at the
    // budget rather than overrunning it -- this is protocol R10, made structural.
    u8 buf[MAX_DATAGRAM_BYTES];
    SnapshotWriter sw(buf, sizeof(buf));
    sw.begin(h, self, 100);
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 500; ++i) {
        if (sw.try_entity(make_entity(rng))) ++accepted;
        else ++rejected;
    }
    const size_t n = sw.finish();

    CHECK(n <= MAX_DATAGRAM_BYTES);
    CHECK(rejected > 0);           // the budget was actually reached
    CHECK(accepted == sw.entities_written());
    std::printf("       %d entities fit in %zu bytes (%d rejected)\n", accepted, n, rejected);

    // The measured budget in docs/protocol.md 5.4 predicts ~45 entities. If this
    // moves a lot, that section is stale and the priority scheme needs revisiting.
    CHECK(accepted >= 35 && accepted <= 60);

    // A rejected entity must leave the buffer byte-identical, or the snapshot
    // would carry a half-written entity that decodes as garbage.
    DecodedSnapshot got;
    ByteReader r(buf, n);
    CHECK(decode_snapshot(r, got));
    CHECK(r.at_end());
    CHECK(got.entity_count == accepted);
}

static void test_snapshot_hostile() {
    std::printf("snapshot: truncated and hostile input is rejected\n");

    SnapshotHeader h;
    SelfBlock self;
    u8 buf[MAX_DATAGRAM_BYTES];
    SnapshotWriter sw(buf, sizeof(buf));
    sw.begin(h, self, 7);
    Rng rng(9);
    for (int i = 0; i < 5; ++i) sw.try_entity(make_entity(rng));
    const size_t n = sw.finish();

    // Every prefix must be rejected. None may read out of bounds -- if any did,
    // the sanitiser build would catch it here.
    for (size_t len = 0; len < n; ++len) {
        DecodedSnapshot got;
        ByteReader r(buf, len);
        CHECK(!decode_snapshot(r, got));
    }

    // A wrong leading type byte is rejected immediately.
    {
        u8 bad[64] = {0x00};
        DecodedSnapshot got;
        ByteReader r(bad, sizeof(bad));
        CHECK(!decode_snapshot(r, got));
    }

    // An entity count larger than the decoder's cap must be refused before it
    // is used as an index bound.
    {
        u8 bad[MAX_DATAGRAM_BYTES];
        SnapshotWriter w2(bad, sizeof(bad));
        w2.begin(h, self, 0);
        const size_t sz = w2.finish();
        // Overwrite the entity count with something absurd.
        const size_t count_pos = sz - 4;
        bad[count_pos]     = 0xFF;
        bad[count_pos + 1] = 0xFF;
        DecodedSnapshot got;
        ByteReader r(bad, sz);
        CHECK(!decode_snapshot(r, got));
    }
}

// PARAM_UPDATE (docs/protocol.md 8.4). Parameters are the RULES, so the
// failure mode of getting this wrong is not a correctable position error --
// it is every subsequent tick computed differently. That makes "a truncated
// message must leave the previous parameters ALONE" the property worth
// hammering, more than the round trip.
static void test_param_update() {
    std::printf("params: round trip, and a bad message never half-applies\n");

    ParamUpdate sent;
    sent.handle = Handle::make(7, 3);
    sent.epoch = 42;
    sent.effective_tick = 123456;
    sent.flags = param_flag::FULL;
    sent.count = 3;
    sent.fields[0] = ParamField{0, fx::from_int(8).raw};
    sent.fields[1] = ParamField{17, fx::from_frac(3, 2).raw};
    sent.fields[2] = ParamField{46, -1};

    u8 buf[1024];
    ByteWriter w(buf, sizeof(buf));
    encode_param_update(w, sent);
    CHECK(w.ok());

    ParamUpdate got;
    ByteReader r(buf, w.size());
    CHECK(decode_param_update(r, got));
    CHECK(got.handle == sent.handle);
    CHECK(got.epoch == 42);
    CHECK(got.effective_tick == 123456);
    CHECK(got.flags == param_flag::FULL);
    CHECK(got.count == 3);
    for (u16 i = 0; i < 3; ++i) {
        CHECK(got.fields[i].id == sent.fields[i].id);
        CHECK(got.fields[i].raw == sent.fields[i].raw);
    }

    // Every truncation must be refused, and must not disturb the destination.
    // A parameter set that is half old and half new is worse than one that is
    // entirely stale, because the stale one is at least self-consistent and the
    // epoch names it.
    for (size_t cut = 0; cut < w.size(); ++cut) {
        ParamUpdate dest = sent;
        dest.epoch = 99;
        ByteReader tr(buf, cut);
        const bool ok = decode_param_update(tr, dest);
        CHECK(!ok);
        CHECK(dest.epoch == 99);   // untouched
    }

    // A hostile count must be rejected before it is used to index anything.
    {
        u8 bad[64];
        ByteWriter bw(bad, sizeof(bad));
        bw.u32v(1); bw.u8v(0); bw.u32v(0); bw.u8v(0);
        bw.u16v(static_cast<u16>(MAX_PARAM_FIELDS + 1));
        ParamUpdate dest;
        dest.epoch = 55;
        ByteReader br(bad, bw.size());
        CHECK(!decode_param_update(br, dest));
        CHECK(dest.epoch == 55);
    }

    // The count is clamped on encode rather than writing past the array.
    {
        ParamUpdate over;
        over.count = static_cast<u16>(MAX_PARAM_FIELDS + 10);
        u8 ob[4096];
        ByteWriter ow(ob, sizeof(ob));
        encode_param_update(ow, over);
        ParamUpdate back;
        ByteReader orr(ob, ow.size());
        CHECK(decode_param_update(orr, back));
        CHECK(back.count == MAX_PARAM_FIELDS);
    }
}

// The parameter table is addressed by WIRE id, so a mistake in it is a protocol
// bug rather than a local one: the server would encode `jump_impulse` and the
// client would apply it to `friction`.
static void test_param_table() {
    std::printf("params: every field round-trips through its wire id\n");

    using namespace dnp::physics;

    // Every field is reachable, distinct, and survives a set/get round trip.
    // Distinctness is the one that matters: two entries sharing an offset -- a
    // copy-paste slip in a 29-line table -- would otherwise pass everything.
    for (u16 i = 0; i < PARAM_COUNT; ++i) {
        MoveParams p;
        const i64 probe = (PARAM_TABLE[i].type == ParamType::Fx)
                              ? fx::from_frac(static_cast<i64>(i) + 1, 7).raw
                              : static_cast<i64>(i % 2 == 0 ? 1 : 0);
        CHECK(param_set_raw(p, i, probe));
        CHECK(param_get_raw(p, i) == (PARAM_TABLE[i].type == ParamType::Fx
                                          ? probe
                                          : (probe != 0 ? 1 : 0)));

        // Writing field i must not disturb any other field.
        MoveParams fresh;
        u32 collided = 0;
        for (u16 j = 0; j < PARAM_COUNT; ++j) {
            if (j == i) continue;
            if (param_get_raw(p, j) != param_get_raw(fresh, j)) ++collided;
        }
        CHECK(collided == 0);
    }

    // Names are unique and resolve back to their own id.
    for (u16 i = 0; i < PARAM_COUNT; ++i) {
        CHECK(param_id_by_name(PARAM_TABLE[i].name) == static_cast<i32>(i));
    }
    CHECK(param_id_by_name("no_such_param") == -1);
    CHECK(param_id_by_name(nullptr) == -1);

    // An unknown id is version skew, not corruption: a newer server may name a
    // field this build does not have. Ignore that field, keep the rest.
    {
        MoveParams p;
        const MoveParams before = p;
        CHECK(!param_set_raw(p, PARAM_COUNT, 999));
        CHECK(!param_set_raw(p, 65535, 999));
        for (u16 j = 0; j < PARAM_COUNT; ++j) {
            CHECK(param_get_raw(p, j) == param_get_raw(before, j));
        }
    }

    // u8 fields clamp rather than truncate: 300 jumps is 255, never 44.
    {
        MoveParams p;
        const i32 id = param_id_by_name("max_jumps");
        CHECK(id >= 0);
        CHECK(param_set_raw(p, static_cast<u16>(id), 300));
        CHECK(param_get_raw(p, static_cast<u16>(id)) == 255);
        CHECK(param_set_raw(p, static_cast<u16>(id), -5));
        CHECK(param_get_raw(p, static_cast<u16>(id)) == 0);
    }

    std::printf("       %u fields, %zu bytes of MoveParams, all ids distinct\n",
                PARAM_COUNT, sizeof(MoveParams));
}

static void test_fuzz() {
    // Anything reachable from the network is reachable from an attacker. The
    // decoders' contract is that ANY byte string either decodes or is rejected
    // -- never reads out of bounds, never indexes past a cap, never loops
    // forever. Under a sanitiser build this is where that gets proven; without
    // one it still catches hangs and cap violations.
    std::printf("fuzz: arbitrary and mutated input must never misbehave\n");
    Rng rng(0xF0F0);

    u8 buf[MAX_DATAGRAM_BYTES];

    // Pure random bytes. Almost all are rejected at the type byte, so this is
    // shallow -- but it is free and it covers the entry path.
    for (int iter = 0; iter < 20000; ++iter) {
        const size_t len = static_cast<size_t>(rng.next_below(300)) + 1;
        for (size_t i = 0; i < len; ++i) buf[i] = static_cast<u8>(rng.next_u32());
        DecodedSnapshot snap;
        ByteReader r1(buf, len);
        (void)decode_snapshot(r1, snap);
        InputBundle in;
        ByteReader r2(buf, len);
        (void)decode_input_bundle(r2, in);
    }

    // Mutated VALID messages, which get much deeper into the parsers: a good
    // header followed by corrupted counts, masks and lengths.
    for (int iter = 0; iter < 20000; ++iter) {
        SnapshotHeader h;
        SelfBlock self;
        SnapshotWriter sw(buf, sizeof(buf));
        sw.begin(h, self, 0);
        for (int i = 0; i < 6; ++i) sw.try_entity(make_entity(rng));
        VfxEvent v;
        sw.try_vfx(v);
        size_t n = sw.finish();

        const int mutations = static_cast<int>(rng.next_below(6)) + 1;
        for (int m = 0; m < mutations; ++m) {
            buf[rng.next_below(static_cast<u32>(n))] = static_cast<u8>(rng.next_u32());
        }
        // Sometimes truncate as well, so length and content are both suspect.
        if ((rng.next_u32() & 3) == 0) n = rng.next_below(static_cast<u32>(n) + 1);

        DecodedSnapshot snap;
        ByteReader r(buf, n);
        if (decode_snapshot(r, snap)) {
            // If it claims success, the caps must still hold -- a decoder that
            // returns true with an out-of-range count is worse than one that
            // crashes, because the crash would at least be noticed.
            CHECK(snap.entity_count <= MAX_ENTITIES);
            CHECK(snap.vfx_count <= MAX_VFX);
            for (u16 i = 0; i < snap.entity_count; ++i) {
                CHECK(snap.entities[i].visual_count <= MAX_VISUAL_PARAMS);
            }
        }
    }
}


// Small helpers for the UI tests: UiText is a fixed buffer, not a std::string.
static void set_text(UiText& t, const char* s) {
    u16 n = 0;
    while (s[n] != '\0' && n < UI_MAX_TEXT) { t.text[n] = s[n]; ++n; }
    t.len = n;
    if (n < UI_MAX_TEXT) t.text[n] = '\0';
}
static bool same_text(const UiText& t, const char* s) {
    u16 n = 0;
    while (s[n] != '\0') ++n;
    if (t.len != n) return false;
    for (u16 i = 0; i < n; ++i) if (t.text[i] != s[i]) return false;
    return true;
}

// --- UI (docs/protocol.md section 11) ----------------------------------------
//
// A panel is the first thing on this wire that carries TEXT, and text is where
// a decoder gets hurt: a length field the sender controls, read into a fixed
// buffer, on data that arrives from the network.

static void test_ui_panel_round_trip() {
    std::printf("ui: a panel survives the wire\n");

    UiPanel p;
    p.ui_id = 7;
    set_text(p.title, "Debug");
    p.count = 3;
    p.widgets[0].kind = UiWidgetKind::Label;
    p.widgets[0].id = 1;
    set_text(p.widgets[0].label, "Player");
    p.widgets[1].kind = UiWidgetKind::Slider;
    p.widgets[1].id = 2;
    set_text(p.widgets[1].label, "Speed");
    p.widgets[1].value = 8; p.widgets[1].lo = 1; p.widgets[1].hi = 40;
    p.widgets[2].kind = UiWidgetKind::Select;
    p.widgets[2].id = 3;
    set_text(p.widgets[2].label, "Map");
    p.widgets[2].option_count = 2;
    set_text(p.widgets[2].options[0], "MAP01");
    set_text(p.widgets[2].options[1], "MAP02");

    u8 buf[2048];
    ByteWriter w(buf, sizeof(buf));
    encode_ui_open(w, p);
    CHECK(w.ok());

    ByteReader r(buf, w.size());
    UiPanel got;
    CHECK(decode_ui_open(r, got));
    CHECK(got.ui_id == 7);
    CHECK(same_text(got.title, "Debug"));
    CHECK(got.count == 3);
    CHECK(got.widgets[1].kind == UiWidgetKind::Slider);
    CHECK(got.widgets[1].value == 8 && got.widgets[1].lo == 1 && got.widgets[1].hi == 40);
    CHECK(same_text(got.widgets[1].label, "Speed"));
    CHECK(got.widgets[2].option_count == 2);
    CHECK(same_text(got.widgets[2].options[1], "MAP02"));
}

static void test_ui_event_round_trip() {
    std::printf("ui: an event survives the wire\n");
    UiEvent e{42, 9, -3};
    u8 buf[64];
    ByteWriter w(buf, sizeof(buf));
    encode_ui_event(w, e);
    ByteReader r(buf, w.size());
    UiEvent got;
    CHECK(decode_ui_event(r, got));
    CHECK(got.ui_id == 42 && got.widget_id == 9 && got.value == -3);
}

// The decoder runs on bytes from the network, so every length in the message is
// a number an attacker picks. Each of these is a way to be handed one.
static void test_ui_rejects_hostile_input() {
    std::printf("ui: hostile lengths are refused, not truncated\n");

    // A string longer than the cap. Refused rather than clamped: clamping keeps
    // the first 128 bytes and leaves the reader mid-field, so everything after
    // it decodes as garbage that passed.
    {
        u8 buf[64];
        ByteWriter w(buf, sizeof(buf));
        w.u32v(1);
        w.u16v(UI_MAX_TEXT + 1);      // title length
        ByteReader r(buf, w.size());
        UiPanel got;
        CHECK(!decode_ui_open(r, got));
    }
    // More widgets than the panel can hold.
    {
        u8 buf[64];
        ByteWriter w(buf, sizeof(buf));
        w.u32v(1);
        w.u16v(0);                     // empty title
        w.u16v(UI_MAX_WIDGETS + 1);
        ByteReader r(buf, w.size());
        UiPanel got;
        CHECK(!decode_ui_open(r, got));
    }
    // A widget kind the client does not know. Refused rather than skipped,
    // because its fields are not self-describing and there is no way to step
    // over one and stay aligned.
    {
        u8 buf[64];
        ByteWriter w(buf, sizeof(buf));
        w.u32v(1);
        w.u16v(0);
        w.u16v(1);
        w.u8v(200);
        ByteReader r(buf, w.size());
        UiPanel got;
        CHECK(!decode_ui_open(r, got));
    }
    // A string whose length runs off the end of the buffer.
    {
        u8 buf[16];
        ByteWriter w(buf, sizeof(buf));
        w.u32v(1);
        w.u16v(64);                    // claims 64 bytes of title, has none
        ByteReader r(buf, w.size());
        UiPanel got;
        CHECK(!decode_ui_open(r, got));
    }
    // And a truncated event.
    {
        u8 buf[3]{};
        ByteReader r(buf, sizeof(buf));
        UiEvent got;
        CHECK(!decode_ui_event(r, got));
    }
}

// Every prefix of a valid panel must be refused rather than accepted as a
// shorter one. This is the shape of a message cut short in transit, and the
// version of it that "works" is a decoder reading past what arrived.
static void test_ui_truncation_at_every_length() {
    std::printf("ui: every truncation of a valid panel is refused\n");

    UiPanel p;
    p.ui_id = 3;
    set_text(p.title, "Shop");
    p.count = 2;
    p.widgets[0].kind = UiWidgetKind::Button;
    set_text(p.widgets[0].label, "Buy");
    p.widgets[1].kind = UiWidgetKind::Toggle;
    set_text(p.widgets[1].label, "Noclip");

    u8 buf[1024];
    ByteWriter w(buf, sizeof(buf));
    encode_ui_open(w, p);
    const size_t full = w.size();

    for (size_t n = 0; n < full; ++n) {
        ByteReader r(buf, n);
        UiPanel got;
        CHECK(!decode_ui_open(r, got));
        if (g_fails) break;
    }
    // And the whole thing still decodes, so the loop above is not passing
    // because the message is unreadable at every length including its own.
    ByteReader r(buf, full);
    UiPanel got;
    CHECK(decode_ui_open(r, got));
    CHECK(got.count == 2);
}

int main() {
    test_ui_panel_round_trip();
    test_ui_event_round_trip();
    test_ui_rejects_hostile_input();
    test_ui_truncation_at_every_length();
    std::printf("== core/proto test suite ==\n");
    test_bytes();
    test_varint();
    test_quantisation();
    test_input_bundle();
    test_snapshot_roundtrip();
    test_snapshot_budget();
    test_snapshot_hostile();
    test_param_update();
    test_param_table();
    test_fuzz();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
