//
// Effects over the movement-parameter table (design §6, §7.2).
//
// The property under test is not "an effect changes a value" -- that is one
// line and could hardly fail. It is that applying and removing an effect leaves
// NO RESIDUE, which the obvious implementation does not give you.
//
// In fixed point, multiply truncates toward zero. So `x1.5` then `/1.5` on the
// live value does not return where it started, and a buff that ticks on and off
// through a match walks the parameter steadily downward. The player ends up
// slower than they began and nothing in the game can explain it.
//
// This file therefore implements the naive version too, and requires it to
// FAIL. An anti-drift test that only tests the good implementation cannot tell
// you whether drift was ever possible.
//
#include <cstdio>

#include "dnp/physics/params_table.hpp"
#include "lua/effects.hpp"

using namespace dnp;
using namespace dnp::physics;
using namespace dnp::server::lua;

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

static void test_apply_and_remove_leaves_no_residue() {
    std::printf("effects: 10,000 apply/remove cycles land EXACTLY on the base\n");

    static EffectStack fx_stack;
    fx_stack.clear();

    MoveParams base;
    const i32 speed = param_id_by_name("move_speed");
    CHECK(speed >= 0);
    // Both the base AND the multiplier have to be values that actually round.
    //
    // Two earlier versions of this test were vacuous. move_speed's default is
    // 8, whose raw is a whole multiple of 2^32, so `x1.5` and even `x1/3`
    // multiply and divide back EXACTLY -- the shift discards nothing. A drift
    // test that cannot observe drift passes against the very implementation it
    // exists to rule out. An awkward base and an awkward factor are what make
    // the two designs distinguishable.
    param_set_raw(base, static_cast<u16>(speed), fx::from_frac(25, 3).raw);
    const i64 want = param_get_raw(base, static_cast<u16>(speed));
    fx_stack.set_base(0, base);

    const fx buff = fx::from_frac(1, 7);

    for (int i = 0; i < 10000; ++i) {
        const u32 id = fx_stack.add(0, static_cast<u16>(speed), EffectOp::Mul, buff, 0);
        CHECK(id != 0 || i > 0);
        const MoveParams boosted = fx_stack.compute(0);
        // The effect really did something, or the loop is measuring nothing.
        if (i == 0) CHECK(param_get_raw(boosted, static_cast<u16>(speed)) < want);
        fx_stack.remove(id);
    }

    const MoveParams after = fx_stack.compute(0);
    const i64 got = param_get_raw(after, static_cast<u16>(speed));
    std::printf("       base=%lld after 10,000 cycles=%lld (drift %lld)\n",
                static_cast<long long>(want), static_cast<long long>(got),
                static_cast<long long>(got - want));
    CHECK(got == want);
}

// THE control: the implementation that was NOT chosen must fail the same test.
static void test_the_naive_implementation_drifts() {
    std::printf("effects: CONTROL -- multiplying the live value instead DOES drift\n");

    MoveParams live;
    const u16 speed = static_cast<u16>(param_id_by_name("move_speed"));
    // The same awkward base and factor as the test above -- see the note there.
    param_set_raw(live, speed, fx::from_frac(25, 3).raw);
    const i64 want = param_get_raw(live, speed);

    const fx buff = fx::from_frac(1, 7);
    for (int i = 0; i < 10000; ++i) {
        // Apply: scale the live value.
        param_set_raw(live, speed, (fx::from_raw(param_get_raw(live, speed)) * buff).raw);
        // Remove: scale it back. Exactly the inverse in real arithmetic, and
        // not the inverse in fixed point, because multiply truncates.
        param_set_raw(live, speed, (fx::from_raw(param_get_raw(live, speed)) / buff).raw);
    }

    const i64 got = param_get_raw(live, speed);
    std::printf("       base=%lld after 10,000 cycles=%lld (drift %lld)\n",
                static_cast<long long>(want), static_cast<long long>(got),
                static_cast<long long>(got - want));
    // If this ever stops failing, fixed-point multiply has stopped truncating
    // and the anti-drift design above is no longer load-bearing.
    CHECK(got != want);
    CHECK(got < want);   // and it drifts DOWNWARD, as truncation toward zero implies
}

static void test_order_is_insertion_order_and_the_engine_decides_nothing() {
    std::printf("effects: modifiers apply in insertion order; the engine picks no winner\n");

    static EffectStack s;
    s.clear();
    MoveParams base;
    const u16 speed = static_cast<u16>(param_id_by_name("move_speed"));
    s.set_base(0, base);

    // Two effects that both scale. Design §6 gives the engine no business
    // deciding which "wins": they compose, and a game wanting strongest-wins
    // implements that in Lua.
    s.add(0, speed, EffectOp::Mul, fx::from_frac(1, 2), 0);
    s.add(0, speed, EffectOp::Mul, fx::from_frac(1, 2), 0);
    const i64 quartered = param_get_raw(s.compute(0), speed);
    CHECK(quartered == (fx::from_raw(param_get_raw(base, speed)) *
                        fx::from_frac(1, 2) * fx::from_frac(1, 2)).raw);

    // Set is last-write-wins, and composes with what came after it.
    s.clear();
    s.set_base(0, base);
    s.add(0, speed, EffectOp::Set, fx::from_int(20), 0);
    s.add(0, speed, EffectOp::Add, fx::from_int(5), 0);
    CHECK(param_get_raw(s.compute(0), speed) == fx::from_int(25).raw);
}

static void test_expiry_and_cleanup() {
    std::printf("effects: timed effects retire, and a despawn takes its effects with it\n");

    static EffectStack s;
    s.clear();
    MoveParams base;
    const u16 speed = static_cast<u16>(param_id_by_name("move_speed"));
    s.set_base(0, base);
    s.set_base(1, base);

    s.add(0, speed, EffectOp::Mul, fx::from_int(2), 100);   // expires at tick 100
    s.add(0, speed, EffectOp::Mul, fx::from_int(2), 0);     // permanent
    CHECK(s.count() == 2);

    CHECK(!s.expire(99));
    CHECK(s.count() == 2);
    CHECK(s.expire(100));
    CHECK(s.count() == 1);
    // The permanent one survives, and the parameter is recomputed from base --
    // not from the doubled value the expired effect had produced.
    CHECK(param_get_raw(s.compute(0), speed) ==
          (fx::from_raw(param_get_raw(base, speed)) * fx::from_int(2)).raw);

    // Effects are per agent.
    s.add(1, speed, EffectOp::Mul, fx::from_int(3), 0);
    CHECK(param_get_raw(s.compute(1), speed) ==
          (fx::from_raw(param_get_raw(base, speed)) * fx::from_int(3)).raw);
    s.clear_agent(0);
    CHECK(param_get_raw(s.compute(0), speed) == param_get_raw(base, speed));
    CHECK(param_get_raw(s.compute(1), speed) !=
          param_get_raw(base, speed));   // agent 1 untouched
}

static void test_the_table_is_bounded_and_says_so() {
    std::printf("effects: the table is bounded and reports what it refused\n");

    static EffectStack s;
    s.clear();
    MoveParams base;
    s.set_base(0, base);
    const u16 speed = static_cast<u16>(param_id_by_name("move_speed"));

    for (u32 i = 0; i < MAX_EFFECTS; ++i) {
        CHECK(s.add(0, speed, EffectOp::Add, fx::zero(), 0) != 0);
    }
    // Full: refused, counted, never silently dropped (R10's rule again).
    CHECK(s.add(0, speed, EffectOp::Add, fx::zero(), 0) == 0);
    CHECK(s.dropped() == 1);

    // An unknown field is refused rather than writing somewhere arbitrary.
    s.clear();
    s.set_base(0, base);
    CHECK(s.add(0, PARAM_COUNT, EffectOp::Add, fx::zero(), 0) == 0);
    CHECK(s.add(0, 60000, EffectOp::Add, fx::zero(), 0) == 0);
}


// An effect on a field whose "raw" is a PLAIN INTEGER, not a Q32.32 raw.
//
// param_get_raw does not return one kind of number: for an Fx field it is a
// fixed-point raw, and for a U8 or a Bool it is the value itself -- 16 means
// sixteen. The effect stack did fx arithmetic on both, which was wrong in two
// directions at once and had been since effects existed. Nothing set an integer
// parameter until a debug panel did, and then:
//
//   Set attack_ticks to 10  ->  wrote fx(10)'s raw, 42949672960, which
//   param_set_raw clamps to 255. A swing meant to last a sixth of a second
//   lasted four and a quarter seconds and its sweep advanced a 255th of a turn
//   per tick -- which reads, correctly, as a wedge that has stopped moving.
//
//   Multiply max_jumps by 1.5  ->  fx::from_raw(2) is 4.7e-10, so two jumps
//   became zero.
static void test_integer_parameters_take_the_number_not_the_raw() {
    std::printf("effects: a U8 field is set to the NUMBER, not to a fixed-point raw\n");

    static EffectStack fx_stack;
    fx_stack.clear();

    MoveParams base;
    const i32 ticks = param_id_by_name("attack_ticks");
    const i32 jumps = param_id_by_name("max_jumps");
    CHECK(ticks >= 0 && jumps >= 0);
    fx_stack.set_base(0, base);

    // SET. The number asked for, not 255 -- which is what a clamped fx raw
    // gives for any value at all, so "it clamped" and "it worked" are
    // indistinguishable unless the value asked for is small.
    fx_stack.add(0, static_cast<u16>(ticks), EffectOp::Set, fx::from_int(10), 0);
    MoveParams out = fx_stack.compute(0);
    std::printf("       set attack_ticks=10 -> %u\n", out.attack_ticks);
    CHECK(out.attack_ticks == 10);

    // MUL. Two jumps and a half gives three, truncating toward zero like every
    // other product in this engine.
    fx_stack.clear();
    fx_stack.set_base(0, base);
    fx_stack.add(0, static_cast<u16>(jumps), EffectOp::Mul, fx::from_frac(3, 2), 0);
    out = fx_stack.compute(0);
    std::printf("       max_jumps %u x1.5 -> %u\n", base.max_jumps, out.max_jumps);
    CHECK(out.max_jumps == static_cast<u8>((base.max_jumps * 3) / 2));

    // ADD, including downward.
    fx_stack.clear();
    fx_stack.set_base(0, base);
    fx_stack.add(0, static_cast<u16>(ticks), EffectOp::Add, fx::from_int(-6), 0);
    out = fx_stack.compute(0);
    CHECK(out.attack_ticks == static_cast<u8>(base.attack_ticks - 6));

    // The clamp still applies to a value that genuinely is out of range: a
    // script asking for 300 gets 255, not 44.
    fx_stack.clear();
    fx_stack.set_base(0, base);
    fx_stack.add(0, static_cast<u16>(ticks), EffectOp::Set, fx::from_int(300), 0);
    out = fx_stack.compute(0);
    CHECK(out.attack_ticks == 255);

    // And an Fx field is UNCHANGED by all of this -- the two domains must not
    // have been merged into one wrong answer.
    const i32 speed = param_id_by_name("move_speed");
    fx_stack.clear();
    fx_stack.set_base(0, base);
    fx_stack.add(0, static_cast<u16>(speed), EffectOp::Set, fx::from_frac(25, 2), 0);
    out = fx_stack.compute(0);
    CHECK(out.move_speed == fx::from_frac(25, 2));
    // Which a fraction proves and a whole number would not: 12.5 cannot survive
    // being routed through the integer path.
    CHECK(out.move_speed != fx::from_int(12));
}

int main() {
    std::printf("== server/lua effects suite ==\n");
    test_apply_and_remove_leaves_no_residue();
    test_integer_parameters_take_the_number_not_the_raw();
    test_the_naive_implementation_drifts();
    test_order_is_insertion_order_and_the_engine_decides_nothing();
    test_expiry_and_cleanup();
    test_the_table_is_bounded_and_says_so();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
