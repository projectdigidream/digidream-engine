//
// The metamap: where each level sits, and therefore what has to be in memory.
//
// Two pure questions, tested apart from any server, filesystem or map -- which
// is why they are pure. The parse takes text and answers cells and complaints;
// the policy takes cells and answers a resident set. Neither needs a level to
// exist, and a bug in either is otherwise only visible as "the world stutters"
// or "a map I walked into was not there".
//
#include <cstdio>
#include <cstring>
#include <string>

#include "world/metamap.hpp"

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

static MetaMap parse(const char* s) { return parse_metamap(s, std::strlen(s)); }

static void test_a_world_is_read() {
    std::printf("metamap: a laid-out world reads back\n");

    const MetaMap m = parse(
        "# the starter world\n"
        "pitch 64 64\n"
        "radius 1\n"
        "\n"
        "cell 0 0 hub        # where you begin\n"
        "cell 1 0 east\n"
        "cell 0 -1 south\n");

    for (const std::string& p : m.problems) std::printf("  unexpected: %s\n", p.c_str());
    CHECK(m.ok());
    CHECK(m.cells.size() == 3);
    CHECK(m.radius == 1);
    CHECK(m.pitch_x == fx::from_int(64));
    CHECK(m.pitch_y == fx::from_int(64));

    const MetaCell* e = m.find("east");
    CHECK(e != nullptr);
    CHECK(e && e->x == 1 && e->y == 0);
    // NEGATIVE coordinates, because a world has no reason to begin at a corner
    // and an author who starts in the middle of one should not have to
    // renumber every cell to add a row below.
    const MetaCell* s = m.find("south");
    CHECK(s && s->x == 0 && s->y == -1);
    CHECK(m.find("nowhere") == nullptr);
}

static void test_the_defaults_are_the_old_behaviour() {
    std::printf("metamap: a world that says nothing keeps its own coordinates\n");

    const MetaMap m = parse("cell 0 0 only\n");
    CHECK(m.ok());
    // No pitch declared is not a pitch of one: it means the cells are NOT
    // composed in world space, each map keeps its own coordinates, and the grid
    // is a residency hint and nothing more. A default of 1 would put every map
    // one unit from its neighbour, which is a world where they overlap.
    CHECK(m.pitch_x == fx::zero());
    CHECK(m.pitch_y == fx::zero());
    CHECK(m.radius == 1);

    LevelCell c;
    c.on_grid = true;
    c.x = 3;
    c.y = 2;
    const fx2 o = cell_origin(c, m.pitch_x, m.pitch_y);
    CHECK(o.x == fx::zero() && o.y == fx::zero());

    // And with one, a cell's origin is its coordinate times the pitch.
    const fx2 p = cell_origin(c, fx::from_int(64), fx::from_int(40));
    CHECK(p.x == fx::from_int(192));
    CHECK(p.y == fx::from_int(80));

    // A level the metamap never mentions sits at the origin whatever the pitch,
    // because it is not on the grid at all.
    LevelCell off;
    CHECK(cell_origin(off, fx::from_int(64), fx::from_int(64)).x == fx::zero());
}

static void test_every_mistake_is_reported() {
    std::printf("metamap: a file with mistakes in it says so, once per mistake\n");

    const MetaMap m = parse(
        "cell 0 0 hub\n"
        "cell 0 0 other\n"        // the cell is taken
        "cell 1 0 hub\n"          // the map is placed
        "cell x 0 bad\n"          // not a number
        "cell 2 0\n"              // no map
        "radius 99\n"             // out of range
        "pitch 0 64\n"            // not a distance
        "north 1\n");             // not a directive

    for (const std::string& p : m.problems) std::printf("    %s\n", p.c_str());
    CHECK(!m.ok());
    // SEVEN, one per mistake. A parser that stopped at the first one turns a
    // five-minute fix into seven separate runs, which is the whole reason this
    // collects rather than returns.
    CHECK(m.problems.size() == 7);
    // And it kept what it could read: the first cell is still placed.
    CHECK(m.cells.size() == 1);
    CHECK(m.find("hub") != nullptr);
    // The refused ones are NOT placed -- a refusal that still takes effect is
    // worse than no check at all.
    CHECK(m.find("other") == nullptr);
    CHECK(m.find("bad") == nullptr);
    CHECK(m.radius == 1);
    CHECK(m.pitch_x == fx::zero());

    // CONTROL: the same file without the mistakes is clean, so the count above
    // is the mistakes and not something the parser says about every line.
    const MetaMap good = parse("cell 0 0 hub\ncell 1 0 other\nradius 2\npitch 64 64\n");
    for (const std::string& p : good.problems) std::printf("  unexpected: %s\n", p.c_str());
    CHECK(good.ok());
    CHECK(good.radius == 2);
}

static void test_a_number_has_to_be_one() {
    std::printf("metamap: a coordinate that is not a number is refused, not read as zero\n");

    // atoi answers 0 for "north", which would place a map at the origin and
    // silently take the cell a real map is in. The whole token has to be the
    // number.
    CHECK(!parse("cell north 0 m\n").ok());
    CHECK(!parse("cell 1x 0 m\n").ok());
    CHECK(!parse("cell 1 0x m\n").ok());
    // NAMED, not read off the temporary: find() returns a pointer into the
    // MetaMap's own vector, and a pointer into an expression that has finished
    // is a dangling one -- which read back as a signed-coordinate bug.
    const MetaMap signed_ok = parse("cell -12 +3 m\n");
    CHECK(signed_ok.ok());
    const MetaCell* c = signed_ok.find("m");
    CHECK(c && c->x == -12 && c->y == 3);
}

// --------------------------------------------------------------- residency

static std::vector<LevelCell> row(int n) {
    std::vector<LevelCell> cells(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        cells[static_cast<size_t>(i)].on_grid = true;
        cells[static_cast<size_t>(i)].x = i;
        cells[static_cast<size_t>(i)].y = 0;
    }
    return cells;
}

static u32 count(const std::vector<bool>& v) {
    u32 n = 0;
    for (bool b : v) if (b) ++n;
    return n;
}

static void test_a_neighbour_is_resident() {
    std::printf("metamap: the cell you are in and the ones beside it\n");

    const std::vector<LevelCell> cells = row(6);
    std::vector<bool> occupied(6, false);
    occupied[3] = true;
    std::vector<bool> want;

    residency(cells, occupied, 1, want);
    CHECK(want.size() == 6);
    CHECK(want[2] && want[3] && want[4]);
    // AND NOT THE REST. A residency that loaded everything would satisfy every
    // assertion above and is exactly what this replaces.
    CHECK(!want[0] && !want[1] && !want[5]);
    CHECK(count(want) == 3);

    // Radius 2 reaches further, and radius 0 is only what is being played --
    // which is a legal answer and what a hub world wants.
    residency(cells, occupied, 2, want);
    CHECK(count(want) == 5);
    residency(cells, occupied, 0, want);
    CHECK(count(want) == 1 && want[3]);
}

static void test_two_players_are_two_neighbourhoods() {
    std::printf("metamap: two players in two places keep both neighbourhoods\n");

    const std::vector<LevelCell> cells = row(8);
    std::vector<bool> occupied(8, false);
    occupied[1] = true;
    occupied[6] = true;
    std::vector<bool> want;
    residency(cells, occupied, 1, want);

    CHECK(want[0] && want[1] && want[2]);
    CHECK(want[5] && want[6] && want[7]);
    CHECK(!want[3] && !want[4]);
    CHECK(count(want) == 6);
}

static void test_the_diagonal_counts() {
    std::printf("metamap: the cell through a corner is a neighbour too\n");

    // A SQUARE window, not a circle. A player walking diagonally reaches the
    // corner cell as fast as the one beside them, so a circular radius would
    // load it late -- which is the one moment a load is a stall.
    std::vector<LevelCell> cells(4);
    for (int i = 0; i < 4; ++i) cells[static_cast<size_t>(i)].on_grid = true;
    cells[0].x = 0; cells[0].y = 0;
    cells[1].x = 1; cells[1].y = 1;    // through the corner
    cells[2].x = 2; cells[2].y = 2;    // two corners away
    cells[3].x = 0; cells[3].y = 2;

    std::vector<bool> occupied(4, false);
    occupied[0] = true;
    std::vector<bool> want;
    residency(cells, occupied, 1, want);
    CHECK(want[0] && want[1]);
    CHECK(!want[2] && !want[3]);
}

static void test_a_level_off_the_grid_still_loads() {
    std::printf("metamap: a map the world does not place is loaded when entered\n");

    // Not an error and not a special case: a hub, a menu level, a map somebody
    // added without laying it out. It is resident while somebody is in it and
    // has no neighbours, which is the honest reading of "not placed".
    std::vector<LevelCell> cells(3);
    cells[0].on_grid = true; cells[0].x = 0; cells[0].y = 0;
    cells[1].on_grid = true; cells[1].x = 1; cells[1].y = 0;
    cells[2].on_grid = false;

    std::vector<bool> occupied(3, false);
    occupied[2] = true;
    std::vector<bool> want;
    residency(cells, occupied, 1, want);
    CHECK(want[2]);
    // It pulls in nothing, because it is nowhere.
    CHECK(!want[0] && !want[1]);

    // CONTROL: somebody in a PLACED cell does pull its neighbour in, so the
    // silence above is the level being unplaced rather than residency not
    // working at all.
    occupied[2] = false;
    occupied[0] = true;
    residency(cells, occupied, 1, want);
    CHECK(want[0] && want[1] && !want[2]);
}

static void test_nobody_anywhere_keeps_nothing() {
    std::printf("metamap: an empty server holds no map at all\n");

    const std::vector<LevelCell> cells = row(4);
    const std::vector<bool> occupied(4, false);
    std::vector<bool> want;
    residency(cells, occupied, 2, want);
    CHECK(count(want) == 0);
    // And the answer is WRITTEN, not left stale: a caller reading `want` after
    // the last player left must see the emptiness rather than the last answer.
    want.assign(4, true);
    residency(cells, occupied, 2, want);
    CHECK(count(want) == 0);
}

int main() {
    std::printf("== the metamap ==\n");
    test_a_world_is_read();
    test_the_defaults_are_the_old_behaviour();
    test_every_mistake_is_reported();
    test_a_number_has_to_be_one();
    test_a_neighbour_is_resident();
    test_two_players_are_two_neighbourhoods();
    test_the_diagonal_counts();
    test_a_level_off_the_grid_still_loads();
    test_nobody_anywhere_keeps_nothing();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
