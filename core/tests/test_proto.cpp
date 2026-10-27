//
// Wire codec tests.
//
// Round-tripping alone is not enough: a codec whose encoder and decoder are
// wrong in the SAME way round-trips perfectly. So these also pin exact encoded
// sizes (which is what the datagram budget is computed from), feed truncated and
// hostile input, and check that the budget-filling encoder rolls back cleanly.
//
#include <cstdio>
#include <initializer_list>
#include <cstring>

#include "dnp/math/rng.hpp"
#include "dnp/physics/params_table.hpp"
#include "dnp/physics/param_text.hpp"
#include "dnp/proto/messages.hpp"

using namespace dnp;
using namespace dnp::proto;

// EACH TEST ITS OWN FRAME. Inlined into main they all share one, and under the
// sanitizers (scripts/check_sanitizers.sh: -O1, every local its own padded
// slot) main grew to 12 MB of stack -- past the 8 MB a process gets -- and
// died before its first check. Kept out of line, the deepest is one test's.
#define TEST_FN static __attribute__((noinline))

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

TEST_FN void test_bytes() {
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

TEST_FN void test_varint() {
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

TEST_FN void test_quantisation() {
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

TEST_FN void test_view_tick() {
    std::printf("input: the drawn tick in one byte comes back as its age\n");
    // Every age the rewind could want, at clocks either side of a wrap.
    for (u32 now : {0u, 1u, 254u, 255u, 256u, 509u, 510u, 100000u, 4294967295u}) {
        for (u32 age = 0; age < 200 && age <= now; ++age) {
            CHECK(view_age(view_byte(now - age), now) == static_cast<i32>(age));
        }
    }
    // CONTROL: 0 is "none", and no tick encodes to it.
    CHECK(view_age(0, 1234) == -1);
    bool zero = false;
    for (u32 t = 0; t < 1000; ++t) zero = zero || view_byte(t) == 0;
    CHECK(!zero);
}

TEST_FN void test_input_bundle() {
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
            // The vertical aim, every value -- the wire carries it whole and
            // the simulation clamps it, so a clamp here would hide a dropped
            // high byte.
            in.frames[i].pitch  = static_cast<i16>(rng.next_u32());
            // The weapon asked for, every byte value, NO_WEAPON included.
            in.frames[i].weapon = static_cast<u8>(rng.next_u32());
            in.frames[i].view   = static_cast<u8>(rng.next_u32());
            in.frames[i].actions = static_cast<u8>(rng.next_u32());
        }

        u8 buf[512];
        ByteWriter w(buf, sizeof(buf));
        encode_input_bundle(w, in);
        CHECK(w.ok());
        // Fifteen bytes a frame: buttons, two axes, the aim, the pitch, the
        // weapon wanted, the tick being drawn, the module's own keys.
        CHECK(w.size() == 1u + 4 + 4 + 1 + 15u * in.frame_count);

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

TEST_FN void test_snapshot_roundtrip() {
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
        self.move_state  = static_cast<u8>(rng.next_u32());
        self.state_ticks = static_cast<u16>(rng.next_u32());
        // Meters: some, with zeros among and after them, so the trailing-zero
        // trim is exercised and a zero inside the run is kept.
        for (u32 k = 0; k < physics::MAX_MOVE_METERS; ++k)
            self.meters[k] = (rng.next_u32() % 3 == 0) ? fx::zero() : fx::from_raw(static_cast<i64>(rng.next_u64()) >> 24);
        self.asked = static_cast<u8>(rng.next_u32() % 10);
        self.wall_contact = static_cast<u8>(rng.next_u32() & 1);
        // Layered moves: each axis present or not, apart.
        for (u32 k = 0; k < physics::MOVE_AXES; ++k) {
            if (rng.next_u32() % 2) continue;
            self.moves[k].move = static_cast<u8>(1 + rng.next_u32() % 8);
            self.moves[k].left = static_cast<u16>(rng.next_u32());
            self.moves[k].dir = fx2{fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30), fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30)};
        }
        // Every flag, ARMED included, so the weapon block is written on about
        // half the iterations and its absence is exercised on the rest.
        self.state_flags = static_cast<u16>(rng.next_u32() & 0x7F);
        // The wall's normal, sent only while climbing: set either way, so the
        // decode must drop it when the flag is off.
        self.climb_normal = fx2{fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30),
                                fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30)};
        if (self.state_flags & state_flag::ARMED) {
            sim::WeaponState& k = self.weapon;
            k.have = rng.next_u32();
            k.current = static_cast<u8>(rng.next_u32()); k.phase = static_cast<u8>(rng.next_u32());
            k.armed = static_cast<u8>(rng.next_u32()); k.swing_phase = static_cast<u8>(rng.next_u32());
            k.prev_buttons = static_cast<u8>(rng.next_u32() & 3);
            k.entered = rng.next_u32(); k.done = rng.next_u32();
            k.held_since[0] = rng.next_u32(); k.held_since[1] = rng.next_u32();
            for (i32& v : k.vars) v = static_cast<i32>(rng.next_u32());
            for (u16& a : k.ammo) a = static_cast<u16>(rng.next_u32());
            // Cooldowns up to a random last one, so every length is written.
            const u32 cools = rng.next_u32() % (sim::MAX_WEAPON_COOLDOWNS + 1);
            for (u32 c = 0; c < cools; ++c) k.cool[c] = static_cast<i32>(rng.next_u32());
            k.swung_weapon = static_cast<u8>(rng.next_u32()); k.swung_phase = static_cast<u8>(rng.next_u32());
            k.swung_step = static_cast<u8>(rng.next_u32());
        }
        self.anchor      = Handle::make(rng.next_below(1000), 3);
        self.jumps_used  = static_cast<u8>(rng.next_below(4));
        self.animation_state = static_cast<u16>(rng.next_u32());
        self.avatar = static_cast<u16>(rng.next_u32());
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
            // Which avatar draws a player -- its own field, so its own coverage.
            if (i % 6 == 0) {
                sent[i].mask |= ent_field::AVATAR;
                sent[i].avatar = static_cast<u16>(rng.next_u32());
            }
            // How far into its swing: a byte, while it changes.
            if (i % 4 == 1) {
                sent[i].mask |= ent_field::ATTACK;
                sent[i].attack_left = static_cast<u8>(1 + rng.next_below(255));
            }
            // And which weapon step it is: three bytes, its own bit.
            if (i % 3 == 2) {
                sent[i].mask |= ent_field::ATTACK_STEP;
                sent[i].attack_weapon = static_cast<u8>(rng.next_u32());
                sent[i].attack_phase = static_cast<u8>(rng.next_u32());
                sent[i].attack_step = static_cast<u8>(rng.next_u32());
            }
            // And the particle kind that follows it.
            if (i % 7 == 3) {
                sent[i].mask |= ent_field::TRAIL;
                sent[i].trail = static_cast<u16>(1 + rng.next_below(65535));
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
        CHECK(got.self.animation_state == self.animation_state);
        CHECK(got.self.avatar == self.avatar);
        CHECK(got.self.move_state == self.move_state && got.self.state_ticks == self.state_ticks);
        {
            bool same = true;
            for (u32 k = 0; k < physics::MAX_MOVE_METERS; ++k) same = same && got.self.meters[k] == self.meters[k];
            same = same && got.self.asked == self.asked && got.self.wall_contact == self.wall_contact;
            // CONTROL inside: off a wall it is not sent, and comes back nothing.
            same = same && ((self.state_flags & state_flag::CLIMBING)
                                ? got.self.climb_normal.x == self.climb_normal.x && got.self.climb_normal.y == self.climb_normal.y
                                : got.self.climb_normal.x == fx::zero() && got.self.climb_normal.y == fx::zero());
            for (u32 k = 0; k < physics::MOVE_AXES; ++k)
                same = same && got.self.moves[k].move == self.moves[k].move && got.self.moves[k].left == self.moves[k].left &&
                       got.self.moves[k].dir.x == self.moves[k].dir.x && got.self.moves[k].dir.y == self.moves[k].dir.y;
            CHECK(same);
        }
        if (self.state_flags & state_flag::ARMED) {
            const sim::WeaponState& a = got.self.weapon;
            const sim::WeaponState& b = self.weapon;
            bool same = a.have == b.have && a.current == b.current && a.phase == b.phase &&
                        a.armed == b.armed && a.swing_phase == b.swing_phase &&
                        a.prev_buttons == b.prev_buttons && a.entered == b.entered &&
                        a.done == b.done && a.held_since[0] == b.held_since[0] &&
                        a.held_since[1] == b.held_since[1] &&
                        a.swung_weapon == b.swung_weapon && a.swung_phase == b.swung_phase &&
                        a.swung_step == b.swung_step;
            for (u32 v = 0; v < sim::MAX_WEAPON_VARS; ++v) same = same && a.vars[v] == b.vars[v];
            for (u32 p = 0; p < sim::MAX_AMMO_POOLS; ++p) same = same && a.ammo[p] == b.ammo[p];
            for (u32 c = 0; c < sim::MAX_WEAPON_COOLDOWNS; ++c) same = same && a.cool[c] == b.cool[c];
            CHECK(same);
        }

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
                CHECK(got.entities[i].avatar == sent[i].avatar);
            }
            if (sent[i].mask & ent_field::ATTACK) CHECK(got.entities[i].attack_left == sent[i].attack_left);
            else CHECK(got.entities[i].attack_left == 0);   // CONTROL: absent means nothing was read
            if (sent[i].mask & ent_field::ATTACK_STEP) {
                CHECK(got.entities[i].attack_weapon == sent[i].attack_weapon &&
                      got.entities[i].attack_phase == sent[i].attack_phase &&
                      got.entities[i].attack_step == sent[i].attack_step);
            } else {
                CHECK(got.entities[i].attack_weapon == 0xFF);   // CONTROL: absent, none
            }
            if (sent[i].mask & ent_field::TRAIL) CHECK(got.entities[i].trail == sent[i].trail);
            else CHECK(got.entities[i].trail == 0);             // CONTROL: absent, none
        }
        CHECK(got.vfx_count == vwritten);
    }
}

// SOUNDS (docs/protocol.md §5.7): after the vfx, and only when there are any.
TEST_FN void test_snapshot_sounds() {
    std::printf("snapshot: sounds after the vfx, absent when there are none\n");
    Rng rng(4242);
    for (int iter = 0; iter < 200; ++iter) {
        SnapshotHeader h;
        h.server_tick = rng.next_u32();
        SelfBlock self;
        self.facing = brad(rng.next_u32());

        // The same snapshot twice: once with sounds and once without.
        u8 a[MAX_DATAGRAM_BYTES], b[MAX_DATAGRAM_BYTES];
        SnapshotWriter wa(a, sizeof(a)), wb(b, sizeof(b));
        wa.begin(h, self, 100);
        wb.begin(h, self, 100);
        const int vwant = static_cast<int>(rng.next_below(3));
        for (int i = 0; i < vwant; ++i) {
            VfxEvent v;
            v.kind = static_cast<u16>(rng.next_below(50));
            v.param = rng.next_u32();
            wa.try_vfx(v);
            wb.try_vfx(v);
        }
        const int swant = 1 + static_cast<int>(rng.next_below(5));
        SoundEvent sent[6];
        for (int i = 0; i < swant; ++i) {
            sent[i].sound  = rng.next_u32();
            sent[i].pos    = fx3{fx::from_int(static_cast<i64>(rng.next_below(200)) - 100),
                                 fx::from_int(static_cast<i64>(rng.next_below(200)) - 100),
                                 fx::from_frac(static_cast<i64>(rng.next_below(96)), 48)};
            sent[i].source = rng.next_u32();
            sent[i].flags  = static_cast<u8>(rng.next_below(4));
            CHECK(wa.try_sound(sent[i]));
        }
        // CONTROL: no vfx may follow a sound -- the reader would take it for one.
        VfxEvent late;
        CHECK(!wa.try_vfx(late));
        const size_t na = wa.finish(), nb = wb.finish();
        CHECK(na == nb + 2 + static_cast<size_t>(swant) * SnapshotWriter::SOUND_BYTES);

        DecodedSnapshot got;
        ByteReader ra(a, na);
        CHECK(decode_snapshot(ra, got));
        CHECK(got.vfx_count == vwant);
        CHECK(got.sound_count == swant);
        for (int i = 0; i < swant && i < got.sound_count; ++i) {
            CHECK(got.sounds[i].sound == sent[i].sound);
            CHECK(got.sounds[i].source == sent[i].source);
            CHECK(got.sounds[i].flags == sent[i].flags);
            CHECK(got.sounds[i].pos.x == sent[i].pos.x && got.sounds[i].pos.y == sent[i].pos.y);
        }
        // Without sounds the snapshot ends where it always did, and says none.
        ByteReader rb(b, nb);
        DecodedSnapshot none;
        CHECK(decode_snapshot(rb, none));
        CHECK(none.sound_count == 0);
    }
    // The name hash, pinned: the JS client computes the same number
    // (client/src/proto.js soundHash) and a drift between them is silence.
    CHECK(sound_hash("weapon.shotgun.fire", 19) == 0x31b9dc0du);
    std::printf("  sound_hash(\"weapon.shotgun.fire\") = %08x\n", sound_hash("weapon.shotgun.fire", 19));
}

TEST_FN void test_snapshot_budget() {
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

TEST_FN void test_snapshot_hostile() {
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
TEST_FN void test_param_update() {
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
// A parameter as a module's `player` line writes it (docs/PLAYER.md).
TEST_FN void test_param_text() {
    std::printf("params: text to parameters, exactly\n");
    using namespace dnp::physics;
    ParamText t;
    const char* err = "";
    CHECK(parse_param_text("jump_impulse", "9.5", t, err) && t.id == param_id_by_name("jump_impulse") &&
          t.raw == fx::from_frac(19, 2).raw);
    // No float in the path: 0.1 is from_frac(1, 10) to the last bit.
    CHECK(parse_param_text("friction", "0.1", t, err) && t.raw == fx::from_frac(1, 10).raw);
    CHECK(parse_param_text("gravity_scale", "-0.25", t, err) && t.raw == -fx::from_frac(1, 4).raw);
    CHECK(parse_param_text("hover_enabled", "on", t, err) && t.raw == 1);
    CHECK(parse_param_text("climb_enabled", "no", t, err) && t.raw == 0);
    CHECK(parse_param_text("max_jumps", "3", t, err) && t.raw == 3);
    // And it lands in the field the name says.
    MoveParams p;
    CHECK(parse_param_text("dash_speed", "12.25", t, err));
    param_set_raw(p, t.id, t.raw);
    CHECK(p.dash_speed == fx::from_frac(49, 4));
    // CONTROLS: each is refused, with a reason.
    static const char* const BAD[][2] = {
        {"jump_height", "9"}, {"jump_impulse", "fast"}, {"jump_impulse", "1e3"}, {"jump_impulse", ""},
        {"hover_enabled", "maybe"}, {"max_jumps", "256"}, {"max_jumps", "1.5"}, {"max_jumps", "-1"}};
    for (const auto& bad : BAD) {
        err = "";
        const bool ok = parse_param_text(bad[0], bad[1], t, err);
        if (ok || *err == '\0') std::printf("  accepted %s %s\n", bad[0], bad[1]);
        CHECK(!ok && *err != '\0');
    }
}

TEST_FN void test_param_table() {
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

TEST_FN void test_fuzz() {
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

TEST_FN void test_ui_panel_round_trip() {
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

TEST_FN void test_ui_event_round_trip() {
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

TEST_FN void test_music_cue_round_trip() {
    std::printf("music: a cue change survives the wire, and an unknown moment does not\n");
    MusicCue m;
    const char* name = "fight";
    for (u32 i = 0; name[i]; ++i) m.cue.text[i] = name[i];
    m.cue.len = 5;
    m.fade_ms = 1200;
    m.at = MusicAt::LoopEnd;
    u8 buf[160];
    ByteWriter w(buf, sizeof(buf));
    encode_music_cue(w, m);
    CHECK(w.ok());
    ByteReader r(buf, w.size());
    MusicCue got;
    CHECK(decode_music_cue(r, got));
    CHECK(got.cue.len == 5 && std::memcmp(got.cue.text, "fight", 5) == 0);
    CHECK(got.fade_ms == 1200 && got.at == MusicAt::LoopEnd);

    // CONTROL: the last byte is the moment, and 3 is not one.
    buf[w.size() - 1] = 3;
    ByteReader r2(buf, w.size());
    CHECK(!decode_music_cue(r2, got));
}

// SelfBlock's animation_state is the observer's own, and the reason it needs its
// own check is that a field an encoder silently drops round-trips perfectly on
// the default. So this asserts the BYTES move, not just that a zero came back.
TEST_FN void test_self_block_carries_its_animation_state() {
    std::printf("self: the observer's own animation state reaches the wire\n");

    SnapshotHeader h{};
    h.server_tick = 7;

    const auto encode = [&](u16 anim, u8* out, size_t cap) {
        SelfBlock self{};
        self.animation_state = anim;
        SnapshotWriter sw(out, cap);
        sw.begin(h, self, 100);
        return sw.finish();
    };

    u8 buf_a[MAX_DATAGRAM_BYTES];
    u8 buf_b[MAX_DATAGRAM_BYTES];
    const size_t na = encode(0, buf_a, sizeof(buf_a));
    const size_t nb = encode(9, buf_b, sizeof(buf_b));
    CHECK(na > 0 && nb > 0);

    // CONTROL. Two snapshots differing ONLY in this field must differ on the
    // wire. An encoder that never wrote it would still decode 0 == 0 and pass a
    // round-trip on the default, which is exactly how a dropped field hides.
    CHECK(na == nb);
    bool differ = false;
    for (size_t i = 0; i < na && i < nb; ++i) if (buf_a[i] != buf_b[i]) differ = true;
    CHECK(differ);

    ByteReader r(buf_b, nb);
    DecodedSnapshot got;
    CHECK(decode_snapshot(r, got));
    CHECK(got.self.animation_state == 9);

    ByteReader r0(buf_a, na);
    DecodedSnapshot zero;
    CHECK(decode_snapshot(r0, zero));
    CHECK(zero.self.animation_state == 0);
}

// CameraSet had no test at all. It is the one message whose field ids are
// positional across two languages with no shared header, so the two properties
// worth pinning are that a full record survives and that an id this build has
// never heard of survives with it -- that second one is what lets a newer
// server configure an older client, and it was documented but never checked.
TEST_FN void test_camera_set_round_trip() {
    std::printf("camera: a full record survives, and so does a field id we do not know\n");
    CameraSet c;
    c.count = static_cast<u16>(CAMERA_MAX_FIELDS);
    for (u16 i = 0; i < c.count; ++i) {
        c.fields[i].id = static_cast<u8>(i);
        // Alternating sign: milli is an i32 and a zig-zag varint is where a
        // sign is lost, so a table of positives would not notice.
        c.fields[i].milli = (i % 2 == 0) ? static_cast<i32>(i) * 1000
                                         : -static_cast<i32>(i) * 1000;
    }
    u8 buf[256];
    ByteWriter w(buf, sizeof(buf));
    encode_camera_set(w, c);
    CHECK(w.ok());
    ByteReader r(buf, w.size());
    CameraSet got;
    CHECK(decode_camera_set(r, got));
    CHECK(got.count == CAMERA_MAX_FIELDS);
    for (u16 i = 0; i < got.count; ++i) {
        CHECK(got.fields[i].id == c.fields[i].id);
        CHECK(got.fields[i].milli == c.fields[i].milli);
    }

    // An id far past anything this build names. docs/protocol.md §11.1 promises
    // it is carried rather than refused -- the SKIPPING happens where the client
    // applies, not here, and a decoder that rejected it would make that promise
    // unkeepable.
    CameraSet u;
    u.count = 1;
    u.fields[0].id = 200;
    u.fields[0].milli = -7;
    ByteWriter w2(buf, sizeof(buf));
    encode_camera_set(w2, u);
    ByteReader r2(buf, w2.size());
    CHECK(decode_camera_set(r2, got));
    CHECK(got.count == 1 && got.fields[0].id == 200 && got.fields[0].milli == -7);

    // CONTROL: one field past the cap is refused, not clamped. Clamping would
    // leave the reader mid-record and everything after it decodes as garbage
    // that passed -- the same reason the UI lengths above are refused.
    ByteWriter w3(buf, sizeof(buf));
    w3.u16v(CAMERA_MAX_FIELDS + 1);
    for (u32 i = 0; i <= CAMERA_MAX_FIELDS; ++i) { w3.u8v(0); w3.i32v(0); }
    ByteReader r3(buf, w3.size());
    CHECK(!decode_camera_set(r3, got));
}

TEST_FN void test_screen_set_round_trip() {
    std::printf("screen: the curtain and the bars survive the wire\n");
    ScreenSet sc;
    sc.count = 4;
    for (u16 i = 0; i < 4; ++i) { sc.fields[i].id = static_cast<u8>(i); sc.fields[i].milli = 250 * (i + 1); }
    u8 buf[128];
    ByteWriter w(buf, sizeof(buf));
    encode_screen_set(w, sc);
    CHECK(w.ok());
    ByteReader r(buf, w.size());
    ScreenSet got;
    CHECK(decode_screen_set(r, got));
    CHECK(got.count == 4);
    for (u16 i = 0; i < 4; ++i) {
        CHECK(got.fields[i].id == i && got.fields[i].milli == 250 * (i + 1));
    }

    // CONTROL: over the cap is refused rather than clamped, as CameraSet's is.
    ByteWriter w2(buf, sizeof(buf));
    w2.u16v(SCREEN_MAX_FIELDS + 1);
    for (u32 i = 0; i <= SCREEN_MAX_FIELDS; ++i) { w2.u8v(0); w2.i32v(0); }
    ByteReader r2(buf, w2.size());
    CHECK(!decode_screen_set(r2, got));
}

// The sky (docs/SKY.md): a kind, six pictures at most, fields as CameraSet's.
TEST_FN void test_sky_set_round_trip() {
    std::printf("sky: a cube's six pictures and its fields survive the wire\n");
    SkySet s;
    s.kind = SkyKind::Cube;
    s.images = 6;
    const char* faces[6] = {"px", "nx", "py", "ny", "pz", "nz"};
    for (int i = 0; i < 6; ++i) set_text(s.image[i], faces[i]);
    s.count = 2;
    s.fields[0] = {2, 250};
    s.fields[1] = {9, -7};      // a field this reader may not know: carried, not refused
    u8 buf[1024];
    ByteWriter w(buf, sizeof(buf));
    encode_sky_set(w, s);
    CHECK(w.ok());
    ByteReader r(buf, w.size());
    SkySet got;
    CHECK(decode_sky_set(r, got));
    CHECK(got.kind == SkyKind::Cube && got.images == 6 && got.count == 2);
    CHECK(got.image[5].len == 2 && std::memcmp(got.image[5].text, "nz", 2) == 0);
    CHECK(got.fields[0].id == 2 && got.fields[0].milli == 250 && got.fields[1].milli == -7);

    // CONTROL: an unknown kind and a seventh picture are refused, not guessed at.
    for (int bad = 0; bad < 2; ++bad) {
        ByteWriter w2(buf, sizeof(buf));
        w2.u8v(bad == 0 ? 4 : 1);
        w2.u8v(bad == 0 ? 0 : 7);
        w2.u16v(0);
        ByteReader r2(buf, w2.size());
        CHECK(!decode_sky_set(r2, got));
    }
}

// A sector's look (SECTOR_SET): each kind of change round-trips, and what the
// renderer could not draw honestly is refused.
// A pinned panel's flag rides after the template, with an empty one when
// there is none; a panel without flags is byte-for-byte what it was.
TEST_FN void test_ui_pinned() {
    std::printf("ui: a pinned panel's flag survives the wire, and costs nothing when unset\n");
    UiPanel p;
    p.ui_id = 7;
    set_text(p.title, "Arms");
    u8 a[256], b[256];
    ByteWriter wa(a, sizeof(a)); encode_ui_open(wa, p);
    p.flags = UI_FLAG_PINNED;
    ByteWriter wb(b, sizeof(b)); encode_ui_open(wb, p);
    CHECK(wb.size() == wa.size() + 3);            // u16 empty template + the flags byte
    UiPanel got;
    ByteReader rb(b, wb.size());
    CHECK(decode_ui_open(rb, got) && got.flags == UI_FLAG_PINNED && got.template_len == 0);
    ByteReader ra(a, wa.size());
    CHECK(decode_ui_open(ra, got) && got.flags == 0);   // CONTROL: unset, none
    // A cut-short empty template (no flags after it) is refused.
    ByteReader rc(b, wb.size() - 1);
    CHECK(!decode_ui_open(rc, got));
}

TEST_FN void test_mark_set_round_trip() {
    std::printf("markers: on an entity and at a point, with their values, survive the wire\n");
    static u8 buf[2048];
    auto trip = [&](const MarkSet& m, MarkSet& got) {
        ByteWriter w(buf, sizeof(buf));
        encode_mark_set(w, m);
        ByteReader r(buf, w.size());
        return w.ok() && decode_mark_set(r, got);
    };
    auto named = [](MarkValue& v, const char* name) {
        v.name_len = static_cast<u8>(std::strlen(name));
        std::memcpy(v.name, name, v.name_len);
    };
    MarkSet bar;
    bar.id = 7;
    bar.anchor = MARK_ON_ENTITY;
    bar.handle = Handle::make(41, 3);
    std::memcpy(bar.template_name, "health", 6);
    bar.template_len = 6;
    bar.flags = MARK_FLAG_DEBUG;
    bar.height = fx::from_frac(1, 4);
    bar.range = fx::from_int(30);
    bar.count = 2;
    named(bar.values[0], "hp");     bar.values[0].number = 35;
    named(bar.values[1], "hp_max"); bar.values[1].number = 60;
    MarkSet got;
    CHECK(trip(bar, got) && got.id == 7 && got.anchor == MARK_ON_ENTITY && got.handle.raw == bar.handle.raw &&
          got.template_len == 6 && std::memcmp(got.template_name, "health", 6) == 0 &&
          got.flags == MARK_FLAG_DEBUG && got.height == fx::from_frac(1, 4) && got.range == fx::from_int(30) &&
          got.count == 2 && got.values[1].name_len == 6 && got.values[1].number == 60);
    MarkSet sign;
    sign.id = 8;
    sign.anchor = MARK_AT_POINT;
    sign.pos = fx3{fx::from_int(-3), fx::from_frac(5, 2), fx::from_int(1)};
    std::memcpy(sign.template_name, "label", 5);
    sign.template_len = 5;
    sign.count = 1;
    named(sign.values[0], "text");
    sign.values[0].kind = HUD_STR;
    set_text(sign.values[0].text, "The Old Mill");
    CHECK(trip(sign, got) && got.anchor == MARK_AT_POINT && got.pos.x == fx::from_int(-3) &&
          got.pos.y == fx::from_frac(5, 2) && got.values[0].kind == HUD_STR &&
          got.values[0].text.len == 12 && std::memcmp(got.values[0].text.text, "The Old Mill", 12) == 0);
    // Every cut is refused, never half-read.
    ByteWriter w(buf, sizeof(buf));
    encode_mark_set(w, sign);
    const size_t n = w.size();
    int accepted = 0;
    for (size_t k = 0; k < n; ++k) {
        ByteReader r(buf, k);
        MarkSet x;
        if (decode_mark_set(r, x)) ++accepted;
    }
    CHECK(accepted == 0);
    // CONTROLS: what it does not know is refused whole -- an anchor, a value
    // kind, a name or a template it cannot read.
    const auto refuses = [&](auto mutate) {
        MarkSet m = bar;
        mutate(m);
        ByteWriter bw(buf, sizeof(buf));
        encode_mark_set(bw, m);
        ByteReader r(buf, bw.size());
        MarkSet x;
        return !decode_mark_set(r, x);
    };
    CHECK(refuses([](MarkSet& m) { m.anchor = 2; }));
    CHECK(refuses([](MarkSet& m) { m.values[0].kind = 2; }));
    CHECK(refuses([](MarkSet& m) { m.values[0].name[0] = 'H'; }));
    CHECK(refuses([](MarkSet& m) { m.template_name[0] = '-'; }));
    CHECK(refuses([](MarkSet& m) { m.template_len = 0; }));
    CHECK(!refuses([](MarkSet&) {}));   // and the unmutated one is not
}

TEST_FN void test_sector_set_round_trip() {
    std::printf("sector set: pictures, light with a fade, and effects survive the wire\n");
    u8 buf[512];
    auto trip = [&](const SectorSet& s, SectorSet& got) {
        ByteWriter w(buf, sizeof(buf));
        encode_sector_set(w, s);
        ByteReader r(buf, w.size());
        return w.ok() && decode_sector_set(r, got);
    };
    SectorSet tex;
    tex.sector = 0x00030007;
    tex.what = SectorWhat::Ceiling;
    set_text(tex.texture, "F_SKY1");
    SectorSet got;
    CHECK(trip(tex, got) && got.sector == 0x00030007 && got.what == SectorWhat::Ceiling &&
          got.texture.len == 6 && std::memcmp(got.texture.text, "F_SKY1", 6) == 0);
    SectorSet light;
    light.what = SectorWhat::Light;
    light.light = 40;
    light.fade_ms = 2500;
    CHECK(trip(light, got) && got.light == 40 && got.fade_ms == 2500);
    SectorSet fog;
    fog.what = SectorWhat::Effect;
    fog.on = true;
    fog.fx.kind = 5;
    fog.fx.count = 2;
    fog.fx.params[0] = {1, fx::from_frac(1, 2).raw};     // density
    fog.fx.params[1] = {8, 0x203040};                    // color
    CHECK(trip(fog, got) && got.on && got.fx.kind == 5 && got.fx.count == 2 &&
          got.fx.params[1].raw == 0x203040);
    SectorSet off = fog;
    off.on = false;
    CHECK(trip(off, got) && !got.on && got.fx.count == 0);
    // CONTROLS: an unknown change, an unknown effect, and a setting the
    // effect does not take (fog has no `speed`) are each refused.
    {
        ByteWriter w(buf, sizeof(buf));
        w.u32v(1); w.u8v(4);
        ByteReader r(buf, w.size());
        CHECK(!decode_sector_set(r, got));
    }
    {
        ByteWriter w(buf, sizeof(buf));
        w.u32v(1); w.u8v(3); w.u8v(9); w.u8v(1); w.u8v(0);
        ByteReader r(buf, w.size());
        CHECK(!decode_sector_set(r, got));
    }
    {
        SectorSet bad = fog;
        bad.fx.count = 1;
        bad.fx.params[0] = {2, 0};    // speed
        CHECK(!trip(bad, got));
    }
}

// The decoder runs on bytes from the network, so every length in the message is
// a number an attacker picks. Each of these is a way to be handed one.
TEST_FN void test_ui_rejects_hostile_input() {
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
TEST_FN void test_ui_truncation_at_every_length() {
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

// THE TEMPLATE (docs/UI_LAYOUT.md 5): trailing and optional. Without one a
// panel's bytes must be what they were before templates existed, or every
// client in the field draws garbage; with one, every cut is still refused
// except the single one a trailing field cannot tell apart -- the end of the
// widgets, which reads as the panel without its template.
__attribute__((noinline)) static void test_ui_template() {
    std::printf("ui: a template is trailing, optional, and checked\n");
    // STATIC and never reassigned from a temporary: a UiPanel is ~137 KB of
    // fixed arrays, main's frame was already near the 8 MB stack under
    // AddressSanitizer, and every `= UiPanel{}` is one more on it. (Each
    // decode below overwrites `got` whole on success; a refusal leaves it be.)
    static UiPanel p;
    static UiPanel got;
    p.ui_id = 5;
    set_text(p.title, "Shop");
    p.count = 1;
    p.widgets[0].kind = UiWidgetKind::Button;
    p.widgets[0].id = 1;
    set_text(p.widgets[0].label, "Buy");

    u8 plain[512];
    ByteWriter w0(plain, sizeof(plain));
    encode_ui_open(w0, p);
    const size_t plain_n = w0.size();

    const char* name = "shop_2";
    p.template_len = 6;
    for (int i = 0; i < 6; ++i) p.template_name[i] = name[i];
    u8 buf[512];
    ByteWriter w(buf, sizeof(buf));
    encode_ui_open(w, p);
    const size_t full = w.size();
    CHECK(full == plain_n + 2 + 6);
    bool prefix_same = true;
    for (size_t i = 0; i < plain_n; ++i) prefix_same = prefix_same && buf[i] == plain[i];
    CHECK(prefix_same);   // the template only ever APPENDS

    {
        ByteReader r(buf, full);
        CHECK(decode_ui_open(r, got));
        CHECK(got.template_len == 6 && std::memcmp(got.template_name, "shop_2", 6) == 0);
        CHECK(got.count == 1);
    }
    {
        ByteReader r(plain, plain_n);
        CHECK(decode_ui_open(r, got));
        CHECK(got.template_len == 0);   // CONTROL: no template, none decoded
    }
    // Every cut inside the template is refused.
    for (size_t n = plain_n + 1; n < full; ++n) {
        ByteReader r(buf, n);
        CHECK(!decode_ui_open(r, got));
        if (g_fails) break;
    }
    // A name the client could not look up in a JSON object is refused.
    {
        u8 bad[512];
        std::memcpy(bad, buf, full);
        bad[plain_n + 2] = 'S';         // upper case
        ByteReader r(bad, full);
        CHECK(!decode_ui_open(r, got));
    }
    {
        u8 bad[512];
        std::memcpy(bad, plain, plain_n);
        bad[plain_n] = static_cast<u8>(UI_MAX_TEMPLATE + 1);
        bad[plain_n + 1] = 0;
        for (u32 i = 0; i <= UI_MAX_TEMPLATE; ++i) bad[plain_n + 2 + i] = 'a';
        ByteReader r(bad, plain_n + 2 + UI_MAX_TEMPLATE + 1);
        CHECK(!decode_ui_open(r, got));   // 25 bytes: one over
    }
    CHECK(valid_template_name("a", 1) && !valid_template_name("", 0) && !valid_template_name("a-b", 3));
}

int main() {
    test_ui_template();
    test_ui_panel_round_trip();
    test_ui_event_round_trip();
    test_music_cue_round_trip();
    test_self_block_carries_its_animation_state();
    test_camera_set_round_trip();
    test_screen_set_round_trip();
    test_sky_set_round_trip();
    test_sector_set_round_trip();
    test_mark_set_round_trip();
    test_ui_pinned();
    test_ui_rejects_hostile_input();
    test_ui_truncation_at_every_length();
    std::printf("== core/proto test suite ==\n");
    test_bytes();
    test_varint();
    test_quantisation();
    test_view_tick();
    test_input_bundle();
    test_snapshot_roundtrip();
    test_snapshot_sounds();
    test_snapshot_budget();
    test_snapshot_hostile();
    test_param_update();
    test_param_table();
    test_param_text();
    test_fuzz();
    // ------------------------------------------- a full snapshot still carries events
    //
    // The wire's order is entities then transients, so the transients are
    // written LAST -- and on a populated level the entities will use every
    // byte. Doom's MAP01 places thirty-one monsters beside thirty-one props
    // against a datagram that holds about forty-six entity blocks, so there was
    // nothing left and every tracer, spray and explosion was dropped. It looks
    // exactly like the effect not working.
    //
    // The two do not fail alike, which is what decides who yields: an entity
    // that does not fit is re-sent next snapshot and the client carries the old
    // one forward, while an event that does not fit is gone -- it happened at a
    // moment that has passed. So the caller reserves room, and this is the
    // property that reserve exists for.
    // A SNAPSHOT FILLED WITH ENTITIES TO THE LAST BYTE STILL FINISHES. The vfx
    // count goes after the entities; when one filled the buffer to within a
    // byte, finish() had nowhere for it, returned 0, and the server sent
    // nothing that tick -- every tick, once the sizes landed that way. Every
    // capacity from an empty snapshot's size to 60 bytes more: several land
    // exactly there. (The stack's snapshot-order test, 250 monsters, is the
    // one that caught it in a real server.)
    {
        std::printf("proto: a snapshot packed with entities to the last byte is still sent\n");
        // From an empty snapshot's own size up: below it nothing could be sent.
        size_t empty = 0;
        {
            u8 big[512];
            SnapshotWriter w(big, sizeof big);
            SnapshotHeader h{};
            w.begin(h, SelfBlock{}, 100);
            empty = w.finish();
        }
        u32 bad = 0, tried = 0;
        for (size_t cap = empty; cap <= empty + 60; ++cap) {
            u8 buf[512];
            SnapshotWriter w(buf, cap);
            SnapshotHeader h{};
            w.begin(h, SelfBlock{}, 100);
            for (u32 i = 0; i < 100; ++i) {
                EntityBlock e;
                e.handle = Handle::make(i + 1, 0);
                e.mask = ent_field::POS;
                if (!w.try_entity(e)) break;
            }
            ++tried;
            if (w.finish() == 0) ++bad;
        }
        std::printf("  %u of %u capacities finished (an empty snapshot is %zu bytes)\n", tried - bad, tried, empty);
        CHECK(empty > 0 && bad == 0);
    }
    {
        std::printf("proto: a snapshot full of entities still carries its events\n");
        u8 buf[MAX_DATAGRAM_BYTES];

        // How many entities fit when nothing is reserved -- the number the
        // reserve is measured against.
        u32 alone = 0;
        {
            SnapshotWriter w(buf, sizeof buf);
            SnapshotHeader h{};
            h.server_tick = 7;
            w.begin(h, SelfBlock{}, 100);
            for (u32 i = 0; i < 200; ++i) {
                EntityBlock e;
                e.handle = Handle::make(i + 1, 0);
                e.mask = ent_field::POS | ent_field::FACING;
                e.pos = fx3{fx::from_int(static_cast<i64>(i)), fx::zero(), fx::zero()};
                if (!w.try_entity(e)) break;
                ++alone;
            }
            w.finish();
        }
        std::printf("  a datagram holds %u entity blocks with nothing held back\n", alone);
        CHECK(alone > 20);

        // Now the same, with room kept back for eight events, and the events
        // written after -- which is the order the wire is read in.
        SnapshotWriter w(buf, sizeof buf);
        SnapshotHeader h{};
        h.server_tick = 7;
        w.begin(h, SelfBlock{}, 100);
        // One event of slack: the loop below stops when the budget is AT MOST
        // the reserve, and the last entity written can leave it a few bytes
        // under. Without the slack, eight reserved fits seven.
        const size_t reserve = 9 * SnapshotWriter::VFX_BYTES + 2;
        u32 written = 0;
        for (u32 i = 0; i < 200; ++i) {
            if (w.remaining() <= reserve) break;
            EntityBlock e;
            e.handle = Handle::make(i + 1, 0);
            e.mask = ent_field::POS | ent_field::FACING;
            e.pos = fx3{fx::from_int(static_cast<i64>(i)), fx::zero(), fx::zero()};
            if (!w.try_entity(e)) break;
            ++written;
        }
        u32 events = 0;
        for (u32 i = 0; i < 8; ++i) {
            VfxEvent v;
            v.kind = 11;
            v.pos = fx3{fx::from_int(1), fx::from_int(2), fx::from_int(3)};
            v.param = 512;
            if (!w.try_vfx(v)) break;
            ++events;
        }
        const size_t n = w.finish();
        std::printf("  with eight reserved: %u entities and %u events in %zu bytes\n",
                    written, events, n);
        // ALL EIGHT. That is the whole point -- and the control is the count
        // above: if the reserve cost nothing, `alone` would equal `written` and
        // the reserve would be doing nothing.
        CHECK(events == 8);
        CHECK(written < alone);
        CHECK(n <= MAX_DATAGRAM_BYTES);

        // And it all decodes, so the reserve did not corrupt the layout.
        DecodedSnapshot got{};
        ByteReader r(buf, n);
        CHECK(decode_snapshot(r, got));
        CHECK(r.at_end());
        CHECK(got.vfx_count == events);
        CHECK(got.entity_count == written);
    }

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
