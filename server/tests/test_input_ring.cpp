//
// The server's input buffer (server/src/session.hpp), on its own.
//
// Its failures all look the same from a controller -- "the player walks off on
// their own and will not respond" -- and have had three different causes, each
// fixed in a comment before it was fixed in code. So these drive it with the
// sequences that caused them and ask what take() hands the simulation.
//
#include <cstdio>

#include "session.hpp"

using namespace dnp;
using namespace dnp::server;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static const u16 EAST = 1, WEST = 2;   // just buttons, to tell frames apart

// One frame per bundle, as simple as the protocol allows.
static void send(Session& s, u32 tick, u16 buttons, u32 world_tick) {
    proto::InputBundle b;
    b.client_tick = tick;
    b.ack_snapshot_tick = 0;
    b.frame_count = 1;
    b.frames[0].buttons = buttons;
    s.ingest(b, world_tick);
}

static Session* fresh() {
    static Session pool[8];
    static int next = 0;
    Session* s = &pool[next++];
    s->reset();
    return s;
}

static void test_in_order() {
    std::printf("input ring: frames come out in the order of their ticks\n");
    Session& s = *fresh();
    for (u32 t = 100; t < 110; ++t) send(s, t, static_cast<u16>(t), 100);
    bool ordered = true;
    for (u32 t = 100; t < 110; ++t) ordered = ordered && s.take().buttons == t;
    CHECK(ordered);
    CHECK(s.clock_resyncs == 0);
}

static void test_a_straggler_is_not_a_clock_change() {
    std::printf("input ring: a late frame is dropped, not taken for a new clock\n");
    Session& s = *fresh();
    for (u32 t = 100; t < 140; ++t) { send(s, t, EAST, 100); s.take(); }
    send(s, 120, WEST, 140);   // twenty ticks late: a straggler
    CHECK(s.clock_resyncs == 0);
    send(s, 140, EAST, 140);
    CHECK(s.take().buttons == EAST);
}

// THE JAM. The ring is re-seeded by a bundle still in flight from before a
// level change, stamped in the thousands; the client's new predictor counts
// from zero. Without the resync every new frame is "too old", and take()
// repeats the last one for ever.
static void test_a_clock_that_restarts_is_followed() {
    std::printf("input ring: a client clock that restarts is followed, not ignored\n");
    Session& s = *fresh();
    s.reset_input();                         // the level change
    send(s, 5000, EAST, 5000);               // in flight from the old world
    CHECK(s.take().buttons == EAST);
    // The new predictor, from zero, holding west.
    u32 west = 0;
    for (u32 t = 0; t < 40; ++t) {
        send(s, t, WEST, 5000 + t);
        if (s.take().buttons == WEST) ++west;
    }
    CHECK(s.clock_resyncs >= 1);
    // Nearly every tick obeys the new input: at most a tick of repetition.
    CHECK(west >= 38);
}

static void test_control_the_jam_is_real() {
    std::printf("input ring: CONTROL -- a restart smaller than the ring is not followed\n");
    // The same shape, but the "restart" is only 100 ticks back: inside the
    // ring, so the frames are read as stale and dropped. This is the behaviour
    // the resync overrides, and without it the test above cannot fail.
    Session& s = *fresh();
    s.reset_input();
    send(s, 5000, EAST, 5000);
    s.take();
    u32 west = 0;
    for (u32 t = 4900; t < 4920; ++t) {
        send(s, t, WEST, 5000);
        if (s.take().buttons == WEST) ++west;
    }
    CHECK(west == 0);
    CHECK(s.clock_resyncs == 0);
}

static void test_a_gap_that_will_not_fill_is_given_up() {
    std::printf("input ring: a gap nobody will fill is skipped after a while\n");
    Session& s = *fresh();
    send(s, 100, EAST, 100);
    s.take();
    send(s, 200, WEST, 100);                 // 99 ticks never arrive
    int starved = 0;
    for (int k = 0; k < 60 && s.take().buttons != WEST; ++k) ++starved;
    CHECK(starved <= static_cast<int>(STARVE_GIVE_UP) + 2);
    CHECK(s.clock_resyncs == 1);
}

int main() {
    test_in_order();
    test_a_straggler_is_not_a_clock_change();
    test_a_clock_that_restarts_is_followed();
    test_control_the_jam_is_real();
    test_a_gap_that_will_not_fill_is_given_up();
    std::printf("\ninput ring: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
