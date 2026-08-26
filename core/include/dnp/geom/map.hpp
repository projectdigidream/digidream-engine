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

// An open edge: the sector simply ENDS here, and an entity that walks off it
// leaves the map and falls.
//
// Needed because "no neighbour" already means "solid wall", which is the right
// default for a room and exactly wrong for a floating platform -- without a way
// to say otherwise, every platform edge is an invisible wall. Marked rather
// than inferred: a side with no neighbour and no LEDGE is still an error worth
// catching, and inferring openness would silently turn a mis-authored map into
// a hole.
inline constexpr u8 LEDGE     = 1u << 3;
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

    // How far the solid slab extends BELOW the floor.
    //
    // Zero for a sector in a sealed room, where there is no "below the floor" to
    // be in. For a floating platform it is the thickness of the slab: solid from
    // floor_z - thickness up to floor_z, so a body falls past its side rather
    // than through it and cannot rise through its underside.
    //
    // Geometry, not decoration -- the renderer draws the skirt from this SAME
    // number, so what you see is exactly what you collide with. It first lived
    // in SectorRender on the assumption that thickness was cosmetic; collision
    // from below is what showed that it is not.
    fx  thickness{};

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

// ---------------------------------------------------------------------------
// Rendering attributes (design §3.1, §4).
//
// A SEPARATE view, deliberately. Textures, pegging and light level are not
// simulation state: two servers that disagree about a wall's texture still owe
// their clients identical physics, and a client that renders a sector black
// must still collide with it the same way. Keeping them out of MapData means
// physics cannot read them by accident -- not as a convention, but because the
// struct it is handed does not contain them, so reaching for `light_level`
// inside a collision routine does not compile.
//
// The parallel arrays are indexed by the SAME sector and side indices as the
// physics view, so nothing needs a second lookup table to correlate them.
// ---------------------------------------------------------------------------

inline constexpr u16 NO_TEXTURE = 0xFFFFu;

namespace peg_flag {
// Doom's pegging, and it earns its place here for a reason specific to this
// engine: sectors MOVE (§3.1.2). Pegging decides whether a wall texture's
// origin is anchored to the top of the segment or the bottom, which is what
// makes a door's texture slide up with the door instead of the door sliding
// past a texture nailed to the world. Without it every mover looks like a hole
// with a static pattern behind it.
inline constexpr u8 UPPER_UNPEGGED = 1u << 0;
inline constexpr u8 LOWER_UNPEGGED = 1u << 1;
}  // namespace peg_flag

// The three texture fields of a side, in Doom's sense.
//
// A side facing open space needs one texture. A side facing another sector
// needs up to three: the step UP to the neighbour's floor (lower), the step
// DOWN from the neighbour's ceiling (upper), and anything drawn across the
// opening itself (middle -- a grate, a window frame). Which fields are visible
// follows from the two sectors' heights and is computed at draw time, so a
// moving sector reveals and hides them without the map being edited.
struct SideRender {
    u16 upper  = NO_TEXTURE;
    u16 middle = NO_TEXTURE;
    u16 lower  = NO_TEXTURE;
    u8  peg_flags = 0;
    fx  x_offset{};   // texture alignment along the wall
    fx  y_offset{};
};

struct SectorRender {
    u16 floor_tex = NO_TEXTURE;
    u16 ceil_tex  = NO_TEXTURE;


    // 0 = dark, 255 = fully lit. Defaults to LIT, not to dark: a map that
    // authored no lighting should render visibly wrong rather than render
    // nothing at all, because a black screen looks like a broken renderer and a
    // flatly-lit one looks like exactly what it is.
    u8 light_level = 255;
};

inline constexpr u32 MAX_TEXTURE_NAME = 24;

// Texture NAMES, not texture data. The map says `BRICK1`; what BRICK1 looks
// like is the client's business, which is what lets one map be shipped to
// clients with entirely different asset sets and keeps image data out of a file
// that travels over the control channel on every join.
struct TextureName {
    char name[MAX_TEXTURE_NAME]{};   // NUL-padded, not NUL-terminated at capacity
    u8   len = 0;
};

// A point light (design §4).
//
// Purely visual, and in RenderData for the same structural reason textures are:
// physics is never handed one, so no collision rule can come to depend on where
// a lamp is. Sector light_level is the ambient FLOOR a surface is lit to; a
// point light adds on top of it with distance falloff and surface orientation,
// so a torch brightens a dark room rather than replacing its lighting.
struct MapLight {
    fx3 pos{};
    u8  r = 255;
    u8  g = 255;
    u8  b = 255;
    fx  radius{};      // world distance at which the contribution reaches zero
    fx  intensity{};   // scale on the added light, 1.0 being a full extra unit
};

struct RenderData {
    const MapLight*     lights       = nullptr;
    u32                 light_count  = 0;
    const SectorRender* sectors      = nullptr;
    u32                 sector_count = 0;
    const SideRender*   sides        = nullptr;
    u32                 side_count   = 0;
    const TextureName*  textures     = nullptr;
    u32                 texture_count = 0;

    const SectorRender& sector(u32 i) const noexcept { return sectors[i]; }
    const SideRender& side(u32 i) const noexcept { return sides[i]; }
    const TextureName& texture(u32 i) const noexcept { return textures[i]; }
    const MapLight& light(u32 i) const noexcept { return lights[i]; }
};

// A uniform broadphase grid over sector footprints.
//
// find_sector was a linear scan over every sector, which is fine for a test map
// and is O(sectors) on every step of every agent for a real one. Deferred until
// there were real map sizes to size it against, which is now.
//
// Each cell lists the sectors whose bounding box touches it, in ascending
// index order. A query narrows to one cell and then runs the SAME exact
// point_in_sector test on the candidates -- the grid decides who to ask, never
// what the answer is, so it cannot change a result. A property test asserts
// that against a linear scan over thousands of points.
//
// `usable` is false when the grid is absent or stale, and every query falls
// back to the full scan. That is what keeps a moving sector safe: horizontal
// motion rewrites footprints, and a grid that described where they used to be
// would be worse than none.
struct SectorGrid {
    bool       usable = false;
    fx2        min{};
    fx         cell{};
    u16        cols = 0;
    u16        rows = 0;
    const u32* start = nullptr;   // cols * rows + 1 entries, CSR style
    const u16* items = nullptr;
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

    SectorGrid grid{};

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

// Index of the sector containing `p`, ignoring height, or NO_SECTOR.
//
// Where footprints nest, the SMALLEST one wins -- the most specific answer
// available without knowing where a body is vertically. That is a genuine
// approximation and it is only correct for nesting; anything that knows the
// body's height should ask find_sector_at instead, which answers the question
// this one can only guess at.
//
// Kept for callers that have no height to offer: a renderer deciding whether a
// shadow lands on floor at all, and the nav graph asking which sectors touch.
i32 find_sector(const MapData& map, fx2 p) noexcept;

// The sector whose floor a body standing at `feet_z` is on.
//
// THE height-aware query, and the one the mover uses. Among the sectors
// containing `p`, it takes the HIGHEST floor that is at or below feet_z plus
// the step threshold -- the topmost surface the body could actually be standing
// on. If every floor is above that, the body is beneath all of them and the
// lowest is returned, which is the "fallen under a platform" case.
//
// This is what makes overlapping footprints mean something. A bridge over a
// path shares floor area with it, and which floor you are on is decided by
// where your feet are, not by which sector is smaller or was declared first.
// It subsumes the nesting rule too: standing on a pillar inside a hall, the
// pillar's floor is the highest one at or below your feet, so it wins without
// anything needing to know that one footprint contains the other.
i32 find_sector_at(const MapData& map, fx2 p, fx feet_z, fx body_height) noexcept;

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
