#pragma once
//
// Sector geometry: the minimum the physics core needs to resolve collision.
//
// A sector is a simple polygon footprint with a floor and a ceiling, and its
// edges are sides that either face another sector or are solid walls. That is
// the Doom model of design §3.1, reduced to the parts §7.1's collision rules
// actually read.
//
// Per-point heights, stacked sectors, baked triangulation, runtime motion and
// tags are Phase 3. They extend this representation; they do not replace it, so
// the collision code written against it stays valid.
//
// MapData is a VIEW over externally-owned arrays. The core never allocates: a
// baked map is mapped or loaded once by the caller, and both the server and the
// wasm client point at their own copy of identical bytes.
//
#include "dnp/math/fixed.hpp"
#include "dnp/math/vec.hpp"

namespace dnp::geom {

using namespace dnp;

inline constexpr i32 NO_SECTOR = -1;

namespace side_flag {
inline constexpr u8 CLIMBABLE = 1u << 0;   // design §3.1, read by the climb state
inline constexpr u8 GRAPPLE   = 1u << 1;   // grapple-attachable, design §7.1.1
inline constexpr u8 BLOCKING  = 1u << 2;   // solid regardless of the neighbour
}  // namespace side_flag

struct Side {
    u16 a = 0;                 // point index
    u16 b = 0;                 // point index
    i32 neighbor = NO_SECTOR;  // sector on the far side, or NO_SECTOR for a wall
    u8  flags = 0;
};

struct Sector {
    u16 first_point = 0;
    u16 point_count = 0;
    u16 first_side  = 0;
    u16 side_count  = 0;
    fx  floor_z{};
    fx  ceil_z{};
    u16 tag = 0;

    // Baked triangle range, into MapData::tris. Indices are absolute into the
    // point array, so a mesh can be uploaded without a per-sector rebase.
    u32 first_tri = 0;
    u16 tri_count = 0;
};

// Rails and wires (design §3.4).
//
// A rail is a polyline an entity is carried along; a wire is the same polyline
// with the rider hanging under it. The distinction is one flag, because the
// difference lives entirely in the controller (§7.4, Phase 6), not in the
// geometry.
//
// Waypoints are SECTOR-RELATIVE, and that is the whole design. A waypoint holds
// the position it was authored at plus the sector it rides; the live position
// is that base plus the sector's current motion offset, written back exactly
// the way a moving sector's corner points are. So a rail bolted to a moving
// platform simply moves, and nothing anywhere has to special-case the
// combination -- which is what §3.4 asks for and what an absolute-coordinate
// rail could not give without a special case in every reader.
namespace rail_flag {
inline constexpr u8 LOOP = 1u << 0;   // the last waypoint joins back to the first
inline constexpr u8 WIRE = 1u << 1;   // the rider hangs below instead of riding on top
}  // namespace rail_flag

struct RailWaypoint {
    i32 anchor = NO_SECTOR;   // sector it rides, or NO_SECTOR for world-fixed
    fx3 base{};               // as authored
    fx3 pos{};                // live: base + the anchor's current offset
};

struct Rail {
    u16 first_waypoint = 0;
    u16 waypoint_count = 0;
    u16 tag = 0;
    u8  flags = 0;
};

// A junction (design §7.4: "junction/branch selection is Lua; movement along
// the chosen branch is native"). Leaving `rail` at `waypoint` may continue onto
// `to_rail` at `to_waypoint`. Directed, and a waypoint may carry several, which
// is what makes it a branch rather than a join.
struct RailLink {
    u16 rail = 0;
    u16 waypoint = 0;      // index into the rail's own waypoint range, not absolute
    u16 to_rail = 0;
    u16 to_waypoint = 0;
};

struct MapData {
    const fx2*    points       = nullptr;
    u32           point_count  = 0;
    const Sector* sectors      = nullptr;
    u32           sector_count = 0;
    const Side*   sides        = nullptr;
    u32           side_count   = 0;
    const u16*    tris         = nullptr;
    u32           tri_index_count = 0;

    const Rail*         rails            = nullptr;
    u32                 rail_count       = 0;
    const RailWaypoint* waypoints        = nullptr;
    u32                 waypoint_count   = 0;
    const RailLink*     rail_links       = nullptr;
    u32                 rail_link_count  = 0;

    const fx2& point(u32 i) const noexcept { return points[i]; }
    const Sector& sector(u32 i) const noexcept { return sectors[i]; }
    const Side& side(u32 i) const noexcept { return sides[i]; }
    const Rail& rail(u32 i) const noexcept { return rails[i]; }
    const RailWaypoint& waypoint(u32 i) const noexcept { return waypoints[i]; }
    const RailLink& rail_link(u32 i) const noexcept { return rail_links[i]; }
};

// Is `p` inside sector `s`?
//
// Crossing-number test written with cross products in 128-bit, so there is no
// division and no rounding anywhere: whether a point is inside a sector must be
// a bit-exact predicate. If it were approximate, a player standing exactly on a
// sector boundary could be judged inside on the server and outside on the
// client, and the two simulations would then disagree about which floor they
// are standing on -- a divergence no amount of position correction repairs.
bool point_in_sector(const MapData& map, u32 s, fx2 p) noexcept;

// Index of the sector containing `p`, or NO_SECTOR.
//
// Linear scan. A broadphase grid arrives in Phase 3 with real map sizes; adding
// one now would be optimising against a guess.
i32 find_sector(const MapData& map, fx2 p) noexcept;

// Nearest point to `p` on the segment a-b.
fx2 closest_point_on_segment(fx2 a, fx2 b, fx2 p) noexcept;

// Squared distance from `p` to the segment a-b, at raw scale 2^64.
// Returned un-shifted so a radius comparison never needs a square root.
u128 distance_sq_to_segment(fx2 a, fx2 b, fx2 p) noexcept;

// Does the segment p0->p1 cross the segment a->b? If so, `t_out` is the
// fraction along p0->p1 at which it does.
//
// This is what makes CCD a real guarantee rather than a finer sampling rate.
// Sub-sampling a fast move can never be sufficient on its own: whatever step
// size is chosen, a step larger than the agent's radius can straddle a wall,
// being outside it at both sampled points and inside at neither. Testing the
// PATH removes the sampling question entirely.
//
// Exact: the classification is done with 128-bit cross products and no
// division, and the single divide happens only once a crossing is certain.
bool segment_intersect(fx2 p0, fx2 p1, fx2 a, fx2 b, fx& t_out) noexcept;

// Outward normal of a side, as seen from `from_sector`. Unit length.
// Zero-length sides yield a zero vector rather than garbage.
fx2 side_normal(const MapData& map, u32 side_index, u32 from_sector) noexcept;

// Signed area doubled, in 128-bit. Positive for counter-clockwise winding.
// Used to normalise winding at load time so normals point consistently outward.
i128 signed_area_2x(const MapData& map, u32 s) noexcept;

}  // namespace dnp::geom
