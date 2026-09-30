#pragma once
//
// THE METAMAP: where each level sits in the world, and therefore what has to be
// in memory at once.
//
// A level costs about 1.8 MB resident -- a sim::World, a MapStorage and a
// NavGraph -- plus a Lua VM and the bytes clients are sent. Loading all
// thirty-two levels of a converted game at startup is sixty megabytes and
// thirty-two nav builds before anybody has moved, for a world in which a player
// can be in exactly one of them. The empty ones already cost almost nothing per
// TICK (Server::tick steps them idle); they cost their whole footprint anyway.
//
// So a level is loaded when it is needed and dropped when it is not, and this
// is what answers "needed". A neighbour is loaded WITH its level rather than
// when it is entered, because the moment it is entered is the moment a load
// would be a stall -- the player is standing in the exit.
//
// A GRID rather than a declared adjacency list, and that is the whole design:
// adjacency is a relation and a grid is a coordinate, so a world laid out on
// one cannot have an edge that goes one way, cannot name a neighbour that does
// not exist, and needs nothing rewritten when a map is added beside an existing
// one. The price is that a world which is genuinely a graph -- a hub with six
// doors to six unrelated places -- does not fit, and for that the answer is a
// radius of zero and a level list, not a second structure here.
//
// It does NOT replace the level ORDER. `entity:change_map` still takes an index
// into the list the operator loaded, and `next` still means the next one. A
// grid says what is near; the order says what comes after. A linear game is a
// row, and then the two agree.
//
// PURE, and deliberately: the parse takes text and the policy takes numbers, so
// both can be tested without a server, a filesystem or a map. Everything that
// touches the disk is the caller's.
//
#include <cstddef>
#include <string>
#include <vector>

#include "dnp/math/fixed.hpp"
#include "dnp/math/vec.hpp"

namespace dnp::server::world {

using namespace dnp;

// How far a world may see. A radius of 1 is the eight cells around you, which
// is nine levels resident; 2 is twenty-five, which is most of a small game.
// Capped because the number comes from a file and the cost is quadratic in the
// window and linear in memory.
inline constexpr u32 MAX_METAMAP_RADIUS = 4;

struct MetaCell {
    std::string map;     // the level's name, as the server knows it: a basename
    i32 x = 0;
    i32 y = 0;
};

struct MetaMap {
    std::vector<MetaCell> cells;

    // THE GRID PITCH, in world units: how far apart two neighbouring cells'
    // origins are. A cell at (2, -1) therefore starts at (2 * pitch_x,
    // -1 * pitch_y), and that is what lets two maps authored in their own
    // coordinates be laid beside each other and line up.
    //
    // Declared rather than derived from the maps' extents, because a grid needs
    // ONE pitch: cells sized by their contents would not align, and a world
    // where the seams do not meet is not a world, it is a set of rooms with
    // gaps. A map smaller than the pitch leaves space around it; a map larger
    // than it overlaps its neighbour, which the loader reports.
    //
    // Zero -- the default -- means the cells are not composed in world space at
    // all. The grid is then purely a residency hint: neighbours are kept loaded
    // so a transition does not stall, and each map keeps its own coordinates.
    // That is the honest state of things until the seam is built, and it is
    // also what a hub world wants for ever.
    fx pitch_x = fx::zero();
    fx pitch_y = fx::zero();

    // How many cells out from an occupied one is kept resident. Zero is a legal
    // and useful answer: it means "load what is being played and nothing else",
    // which is what a hub world with unrelated exits wants.
    u32 radius = 1;

    // What it refused, one string per problem, with the line number in it.
    //
    // COLLECTED rather than thrown or returned as a bool, for the same reason
    // the map compiler collects: an author who has made three mistakes wants to
    // be told three times, and a parser that stops at the first one turns a
    // five-minute fix into five separate runs.
    std::vector<std::string> problems;

    bool ok() const noexcept { return problems.empty(); }

    // The cell a named level sits in, or nullptr.
    const MetaCell* find(const std::string& name) const noexcept;
};

// Parse the metamap text. Never throws; a file with mistakes in it yields every
// cell it could read plus a problem per mistake.
MetaMap parse_metamap(const char* text, size_t len);

// Where one level is. `on_grid` false means the metamap does not mention it,
// which is not an error: an unplaced level is loaded when it is entered and
// dropped when it is left, with no neighbours.
struct LevelCell {
    i32  x = 0;
    i32  y = 0;
    bool on_grid = false;
};

// Where a cell's own origin sits in world space. Zero for a level that is not
// on the grid, and zero for every level when the metamap declares no pitch --
// which is the same answer and means the same thing: this map's coordinates
// ARE world coordinates.
fx2 cell_origin(const LevelCell& c, fx pitch_x, fx pitch_y) noexcept;

// WHICH CELL A WORLD COORDINATE FALLS IN.
//
// FLOORED, not truncated, and the difference is every cell left of or below the
// origin: C++ integer division rounds toward zero, so a body one unit into cell
// -1 would be said to be in cell 0 -- and a player walking west would be handed
// back to the level they were leaving, on every tick, for ever.
//
// A pitch of zero means the cells are not composed in world space, and the
// answer is cell zero: there is one frame and everything is in it.
i32 cell_index(fx v, fx pitch) noexcept;

// WHICH LEVELS MUST BE RESIDENT.
//
// A level is wanted if somebody is in it, or if it is within `radius` cells of
// a level somebody is in. `want` is resized to match and every entry written,
// so a caller cannot read a stale answer for a level that has since been added.
//
// ponytail: O(levels^2) over the occupied ones. The list is capped at 64 and
// this runs when somebody changes level, not per tick; a grid index is the
// upgrade if a world ever gets big enough to notice.
void residency(const std::vector<LevelCell>& cells,
               const std::vector<bool>& occupied,
               u32 radius,
               std::vector<bool>& want);

}  // namespace dnp::server::world
