#pragma once
//
// A small hand-built map, compiled into the core.
//
// TEMPORARY, and deliberately so. Phase 3 brings the baked map format, the
// edit-time triangulation and MAP_DATA transfer over the control channel; until
// then the server and the client need geometry they are guaranteed to agree on,
// and "both construct it from the same code" is the only way to guarantee that
// without a loader. When the real format lands, this becomes a fixture used by
// tests rather than something the server runs on.
//
// Laid out to exercise the collision rules rather than to be a level:
//
//     sector 0            sector 1          sector 2
//   x 0..40 floor 0    x 40..60          x 60..80
//   west wall is       floor +STEP        floor 4.0
//   CLIMBABLE          (walkable up)      (too high: a wall)
//                                         east wall is GRAPPLE
//
#include "dnp/geom/map.hpp"

namespace dnp::sim {

// The shared instance. Identical on every target because it is built from the
// same constants, which is the property the client's prediction depends on.
const geom::MapData& test_map() noexcept;

}  // namespace dnp::sim
