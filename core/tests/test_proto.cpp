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

int main() {
    std::printf("== core/proto test suite ==\n");
    test_bytes();
    test_varint();
    test_quantisation();
    test_input_bundle();
    test_snapshot_roundtrip();
    test_snapshot_budget();
    test_snapshot_hostile();
    test_fuzz();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
