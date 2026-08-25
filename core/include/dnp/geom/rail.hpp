#pragma once
//
// Rail and wire queries (design §3.4).
//
// A rail is a polyline; these are the questions a controller asks of one. The
// per-tick movement itself is §7.4 and arrives in Phase 6 -- what lives here is
// the geometry, so that when the controller lands it has nothing to invent.
//
// Every query reads a waypoint's LIVE position, which sector motion keeps up to
// date. That is why none of this needs to know a rail might be moving: by the
// time anything here runs, a rail on a lift is already where the lift is.
//
#include "dnp/geom/map.hpp"

namespace dnp::geom {

// Segments in a rail: one fewer than its waypoints, or one per waypoint when it
// loops. A rail with fewer than two waypoints has none, and every query below
// degrades to its first waypoint rather than reading past the end.
u32 rail_segment_count(const MapData& map, u32 rail) noexcept;

// Total length along the polyline, following the loop closure when present.
fx rail_length(const MapData& map, u32 rail) noexcept;

struct RailPoint {
    fx3 pos{};
    u32 segment = 0;    // which segment the distance fell in
    fx  t{};            // position within that segment, in [0, 1]
    bool clamped = false;   // the distance ran off the end of a non-looping rail
};

// Position at `along` units from the rail's first waypoint.
//
// A looping rail wraps; a non-looping one CLAMPS and says so. Clamping silently
// would make "the rider reached the end" indistinguishable from "the rider is
// somewhere in the middle", and the end of a rail is exactly where a controller
// has to hand back to standard physics (§7.1.1's shared detach).
RailPoint rail_point_at(const MapData& map, u32 rail, fx along) noexcept;

struct RailNearest {
    fx3  pos{};
    fx   distance_along{};
    u32  segment = 0;
    u128 dist_sq = 0;   // squared distance from the query point, at raw scale
};

// Closest point on the rail to `p`, and how far along it that is.
//
// This is how an entity attaches: Lua decides a rail is grabbable, and the
// native side works out where on it the entity actually joins. Returned
// un-rooted so a range test never needs a square root.
RailNearest rail_nearest(const MapData& map, u32 rail, fx3 p) noexcept;

// The `nth` junction leaving `rail` at `waypoint`, or -1.
//
// Enumerated rather than resolved, because choosing between branches is Lua's
// decision (design §7.4) and the engine must not quietly pick one. A waypoint
// with two links is a fork; the engine will report both and take neither.
i32 rail_link_at(const MapData& map, u32 rail, u16 waypoint, u32 nth) noexcept;
u32 rail_link_count_at(const MapData& map, u32 rail, u16 waypoint) noexcept;

}  // namespace dnp::geom
