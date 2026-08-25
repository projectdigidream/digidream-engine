#include "dnp/sim/testmap.hpp"

#include "dnp/sim/constants.hpp"

namespace dnp::sim {

using namespace dnp::geom;

namespace {

constexpr fx F(i64 n) { return fx::from_int(n); }

// Each sector owns its own copy of its corner points. Sharing indices between
// adjacent sectors would be smaller, but it would also mean a Phase 3 edit that
// moves one sector's corner silently drags its neighbour's with it.
const fx2 POINTS[] = {
    // sector 0
    {F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)},
    // sector 1
    {F(40), F(0)}, {F(60), F(0)}, {F(60), F(40)}, {F(40), F(40)},
    // sector 2
    {F(60), F(0)}, {F(80), F(0)}, {F(80), F(40)}, {F(60), F(40)},
};

const Side SIDES[] = {
    // sector 0
    {0, 1, NO_SECTOR, 0},
    {1, 2, 1, 0},                        // east: onto the step
    {2, 3, NO_SECTOR, 0},
    {3, 0, NO_SECTOR, side_flag::CLIMBABLE},

    // sector 1
    {4, 5, NO_SECTOR, 0},
    {5, 6, 2, 0},                        // east: onto the high platform
    {6, 7, NO_SECTOR, 0},
    {7, 4, 0, 0},                        // west: back down

    // sector 2
    {8, 9, NO_SECTOR, 0},
    {9, 10, NO_SECTOR, side_flag::GRAPPLE},
    {10, 11, NO_SECTOR, 0},
    {11, 8, 1, 0},
};

// Baked triangles. Every sector here is a quad, so two triangles each.
//
// These are exactly what the runtime triangulator emits for these polygons --
// NOT merely a valid triangulation of them. A quad has two valid
// triangulations and the obvious hand-written choice (0,1,2 / 0,2,3) is the
// other one, so this was wrong until the assertion in test_triangulate.cpp
// caught it. Had it shipped, a sector would have visibly changed shape the
// first time update_shape() rebuilt it, long after the cause.
const u16 TRIS[] = {
     3,  0,  1,    1,  2,  3,
     7,  4,  5,    5,  6,  7,
    11,  8,  9,    9, 10, 11,
};

const Sector SECTORS[] = {
    //  pt   n  side n  floor              ceil     tag  tri  n
    {    0,  4,   0,  4, F(0),              F(40),   1,    0,  2},
    {    4,  4,   4,  4, STEP_HEIGHT,       F(40),   2,    6,  2},   // exactly walkable
    {    8,  4,   8,  4, F(4),              F(40),   3,   12,  2},   // needs a jump
};

const MapData MAP{
    POINTS, sizeof(POINTS) / sizeof(POINTS[0]),
    SECTORS, sizeof(SECTORS) / sizeof(SECTORS[0]),
    SIDES,   sizeof(SIDES) / sizeof(SIDES[0]),
    TRIS,    sizeof(TRIS) / sizeof(TRIS[0]),
};

}  // namespace

const MapData& test_map() noexcept { return MAP; }

}  // namespace dnp::sim
