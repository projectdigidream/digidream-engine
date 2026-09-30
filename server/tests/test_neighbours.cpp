//
// What a client holds, and the messages that make it hold what it should.
//
// Every crossing bug this replaces was the server and the client disagreeing
// about what the client had, so these walk the sequences that produced them --
// out and back, a level left and re-entered, a change of level with no grid --
// and check the one thing that matters: a level the server believes held was
// sent as a map and never dropped since, and nothing is sent twice.
//
#include <cstdio>
#include <vector>

#include "world/neighbours.hpp"

using namespace dnp;
using namespace dnp::server::world;

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

// A CLIENT, modelled by the rule alone: it holds a level from the first byte
// of its map until a drop. Every message is checked against it, so a MOVE for
// something it does not hold or a MAP for something it does is caught at the
// message rather than as a count at the end.
struct Client {
    std::vector<u32> holds;   // including the home
    u32 home = 0;
    int moves_unheld = 0, maps_held = 0, drops_unheld = 0, maps = 0, welcome_lies = 0;

    bool has(u32 li) const {
        for (u32 h : holds) if (h == li) return true;
        return false;
    }
    void erase(u32 li) {
        for (size_t k = holds.size(); k-- > 0;) if (holds[k] == li) holds.erase(holds.begin() + static_cast<long>(k));
    }
    void welcome(u32 li, bool map_follows) {
        if (!map_follows && !has(li)) ++welcome_lies;   // told it has a map it does not
        if (map_follows && has(li)) ++maps_held;        // sent a second copy
        if (map_follows) { holds.push_back(li); ++maps; }
        home = li;
    }
    void apply(const std::vector<NeighbourMessage>& ms) {
        for (const auto& m : ms) {
            switch (m.say) {
                case NeighbourSay::Drop: if (!has(m.level)) ++drops_unheld; erase(m.level); break;
                case NeighbourSay::Move: if (!has(m.level)) ++moves_unheld; break;
                case NeighbourSay::Map:  if (has(m.level)) ++maps_held; holds.push_back(m.level); ++maps; break;
            }
        }
    }
};

// A 3x3 grid of levels, indices row-major, radius 1, pitch 1 (raw).
static std::vector<WantedLevel> around(u32 home, bool grid = true) {
    std::vector<WantedLevel> w;
    if (!grid) return w;
    const i32 hx = static_cast<i32>(home % 3), hy = static_cast<i32>(home / 3);
    for (u32 li = 0; li < 9; ++li) {
        const i32 x = static_cast<i32>(li % 3), y = static_cast<i32>(li / 3);
        if (li == home) continue;
        if (x - hx > 1 || hx - x > 1 || y - hy > 1 || hy - y > 1) continue;
        w.push_back(WantedLevel{li, x - hx, y - hy});
    }
    return w;
}

struct Server {
    std::vector<HeldLevel> held;
    bool has_home = false;
    u32 level = 0;
    bool grid = true;

    // The same order place_in_level uses: rehome, WELCOME, diff.
    void place(Client& c, u32 li) {
        const bool follows = !rehome(held, has_home, level, li);
        has_home = true;
        level = li;
        c.welcome(li, follows);
        c.apply(diff_neighbours(held, around(li, grid), li));
    }
};

static bool clean(const Client& c) {
    return c.moves_unheld == 0 && c.maps_held == 0 && c.drops_unheld == 0 && c.welcome_lies == 0;
}

// Held on the server == held on the client, minus the client's home.
static bool agree(const Server& s, const Client& c) {
    size_t n = 0;
    for (u32 h : c.holds) {
        if (h == c.home) continue;
        ++n;
        bool found = false;
        for (const auto& x : s.held) if (x.level == h) found = true;
        if (!found) return false;
    }
    return n == s.held.size();
}

static void test_join_sends_everything_once() {
    std::printf("neighbours: a join sends its map and its neighbours, once each\n");
    Server s; Client c;
    s.place(c, 4);   // the middle of the grid
    CHECK(clean(c));
    CHECK(c.maps == 9);
    CHECK(agree(s, c));
    // And a second pass with nothing changed says nothing.
    CHECK(diff_neighbours(s.held, around(4), 4).empty());
}

static void test_a_crossing_sends_only_the_new_edge() {
    std::printf("neighbours: a crossing re-sends nothing the client holds\n");
    Server s; Client c;
    s.place(c, 3);            // west-middle: holds 0 1 3 4 6 7
    const int before = c.maps;
    s.place(c, 4);            // east one: the map is held, only 2 5 8 are new
    CHECK(clean(c));
    CHECK(c.maps - before == 3);
    CHECK(agree(s, c));
}

static void test_out_and_back() {
    std::printf("neighbours: out and back, many times, costs only the far edge\n");
    // From the centre to the west drops the east column, which is out of
    // reach, and coming back sends it again. The home maps never travel.
    Server s; Client c;
    s.place(c, 3);
    s.place(c, 4);
    const int settled = c.maps;
    for (int k = 0; k < 20; ++k) { s.place(c, 3); s.place(c, 4); }
    CHECK(clean(c));
    CHECK(c.maps - settled == 20 * 3);
    CHECK(agree(s, c));
}

static void test_the_level_left_is_placed() {
    std::printf("neighbours: the level just left is told where it now is\n");
    Server s; Client c;
    s.place(c, 3);
    std::vector<HeldLevel> held = s.held;
    const bool follows = !rehome(held, true, 3, 4);
    CHECK(!follows);
    const auto ms = diff_neighbours(held, around(4), 4);
    bool moved = false;
    for (const auto& m : ms)
        if (m.level == 3) moved = m.say == NeighbourSay::Move && m.dx == -1 && m.dy == 0;
    CHECK(moved);
    // CONTROL: the old home is in `held` unplaced after rehome, so the diff
    // cannot have skipped it as "already where it was said to be".
    std::vector<HeldLevel> h2 = s.held;
    rehome(h2, true, 3, 4);
    bool unplaced = false;
    for (const auto& x : h2) if (x.level == 3) unplaced = !x.placed;
    CHECK(unplaced);
}

static void test_far_jump_drops_what_is_out_of_reach() {
    std::printf("neighbours: a jump across the grid drops, then re-sends, correctly\n");
    Server s; Client c;
    s.place(c, 0);            // corner: holds 0 1 3 4
    s.place(c, 8);            // opposite corner: 0 and 1 and 3 drop, 4 stays
    CHECK(clean(c));
    CHECK(agree(s, c));
    CHECK(!c.has(0));
    s.place(c, 0);            // back: 0 was dropped, so its map follows again
    CHECK(clean(c));
    CHECK(c.has(0));
    CHECK(agree(s, c));
}

static void test_no_grid_holds_nothing_for_ever() {
    std::printf("neighbours: with no grid, a level left is dropped rather than hoarded\n");
    Server s; Client c; s.grid = false;
    for (u32 li = 0; li < 9; ++li) s.place(c, li);
    CHECK(clean(c));
    CHECK(s.held.empty());
    CHECK(c.holds.size() == 1);
    // CONTROL: without the diff after a level change -- the early return this
    // replaced -- the client would hold every level it had visited.
    std::vector<HeldLevel> held;
    bool had = false; u32 from = 0;
    for (u32 li = 0; li < 9; ++li) { rehome(held, had, from, li); had = true; from = li; }
    CHECK(held.size() == 8);
}

static void test_respawn_in_place() {
    std::printf("neighbours: a respawn in the same level sends no map\n");
    Server s; Client c;
    s.place(c, 4);
    const int before = c.maps;
    s.place(c, 4);
    CHECK(clean(c));
    CHECK(c.maps == before);
}

static void test_control_the_detector_can_fail() {
    std::printf("neighbours: CONTROL -- the client model catches a server that lies\n");
    // Claim the map is held when it was never sent: the exact failure the old
    // skip-send had, three times.
    Client c;
    c.welcome(4, false);
    CHECK(c.welcome_lies == 1);
    // And a MOVE for something never sent.
    Client d;
    d.apply({NeighbourMessage{NeighbourSay::Move, 7, 0, 0}});
    CHECK(d.moves_unheld == 1);
    // And a crossing that forgets the level left -- rehome without the push --
    // leaves the server re-sending a map the client still holds.
    Server s; Client e;
    s.place(e, 3);
    s.held.erase(s.held.begin(), s.held.end());   // server forgets everything
    s.has_home = false;                           // and its home
    s.place(e, 4);
    CHECK(!clean(e));
}

int main() {
    test_join_sends_everything_once();
    test_a_crossing_sends_only_the_new_edge();
    test_out_and_back();
    test_the_level_left_is_placed();
    test_far_jump_drops_what_is_out_of_reach();
    test_no_grid_holds_nothing_for_ever();
    test_respawn_in_place();
    test_control_the_detector_can_fail();
    std::printf("\nneighbours: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
