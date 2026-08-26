#pragma once
//
// Mutable map storage, and the staged shape-edit commit of design §3.1.1.
//
// MapData is a read-only view; this owns the arrays behind it. Fixed capacity
// and no allocation, because map data arrives over the network and an
// attacker-chosen count must never become an allocation size.
//
// The edit model is the one the design specifies, and the reason for it is
// worth stating: re-triangulating, rebuilding collision data and rebuilding the
// local nav graph are expensive, and a script that moves four corners of a
// sector should pay for that once, not four times. So edits are STAGED and a
// single explicit commit applies them atomically.
//
// Between a staged edit and its commit, every reader still sees the last valid
// triangulation. That is not a convenience -- it is what makes the intermediate
// state safe to observe at all, and §3.1.1 requires it be documented so script
// authors do not build on undefined geometry.
//
#include "dnp/geom/map.hpp"
#include "dnp/geom/triangulate.hpp"

namespace dnp::geom {

inline constexpr u32 MAX_MAP_POINTS  = 4096;
inline constexpr u32 MAX_MAP_SECTORS = 512;
inline constexpr u32 MAX_MAP_SIDES   = 4096;
inline constexpr u32 MAX_MAP_TRI_IDX = 12288;   // 3 per triangle
inline constexpr u32 MAX_MAP_SPAWNS  = 64;
inline constexpr u32 MAX_MAP_RAILS      = 64;
inline constexpr u32 MAX_MAP_WAYPOINTS  = 512;
inline constexpr u32 MAX_MAP_RAIL_LINKS = 64;
inline constexpr u32 MAX_SPAWN_TAG   = 32;
inline constexpr u32 MAX_MAP_TEXTURES   = 256;
inline constexpr u32 MAX_MAP_LIGHTS     = 128;

// A named landing point. Portals name one of these in the join handshake
// (design §3.3), so each portal can deliver its arrivals somewhere specific
// rather than every portal dumping players at one generic map spawn.
struct SpawnPoint {
    char tag[MAX_SPAWN_TAG]{};   // NUL-padded, not NUL-terminated at capacity
    u8   tag_len = 0;
    fx3  pos{};
};

enum class CommitStatus : u8 {
    Ok = 0,
    NoStagedEdits,
    BadSector,
    TriangulationFailed,   // shape rejected; the previous mesh is retained
    OutOfTriangleSpace,
};

class MapStorage {
public:
    void clear() noexcept;

    // --- construction (map load, or a test fixture) ---
    // Returns the sector index, or MAX_MAP_SECTORS on failure.
    u32 add_sector(const fx2* pts, u32 n, fx floor_z, fx ceil_z, u16 tag) noexcept;
    bool add_side(u32 sector, u16 local_a, u16 local_b, i32 neighbor, u8 flags) noexcept;

    // Triangulate a sector from its current points. Called by the map compiler
    // at bake time and by commit_shape() at runtime -- the same code, so a
    // baked mesh and a rebuilt one cannot disagree.
    CommitStatus retriangulate(u32 sector) noexcept;

    // --- runtime shape editing (design §3.1.1) ---

    // Stage a new position for one of a sector's points. Cheap: nothing is
    // rebuilt, and readers continue to see the previous shape.
    bool stage_point(u32 sector, u16 local_index, fx2 pos) noexcept;

    // Apply every staged edit for `sector` at once: move the points, rebuild
    // the mesh. On failure nothing changes and the previous triangulation
    // stands, so a script cannot leave the world in a half-edited state.
    CommitStatus commit_shape(u32 sector) noexcept;

    bool has_staged_edits(u32 sector) const noexcept;
    void discard_staged_edits(u32 sector) noexcept;

    // --- rails and wires (design §3.4) ---
    //
    // Built in the same order every time, like everything else the map is made
    // of: rails are appended, and a rail's waypoints must be added before the
    // next rail is started. Interleaving them would leave a rail's waypoint
    // range non-contiguous, and nothing downstream could iterate it.
    u32 add_rail(u16 tag, u8 flags) noexcept;

    // `anchor` is the sector the waypoint rides, or NO_SECTOR for a world-fixed
    // one. The caller decides -- the map compiler infers it from the sector the
    // point falls in, and stores the answer, so the runtime never has to guess.
    bool add_waypoint(u32 rail, fx3 pos, i32 anchor) noexcept;

    bool add_rail_link(u32 rail, u16 waypoint, u32 to_rail, u16 to_waypoint) noexcept;

    u32 rail_count() const noexcept { return rail_count_; }
    u32 waypoint_count() const noexcept { return waypoint_count_; }
    u32 rail_link_count() const noexcept { return rail_link_count_; }

    // Live waypoint position, written by sector motion each tick. Deliberately
    // the same shape as point_mut(): a rail on a moving platform is the corner
    // points of that platform, one level up.
    RailWaypoint& waypoint_mut(u32 i) noexcept { return waypoints_[i]; }

    // --- spawns ---
    bool add_spawn(const char* tag, u32 tag_len, fx3 pos) noexcept;
    u32 spawn_count() const noexcept { return spawn_count_; }
    const SpawnPoint& spawn(u32 i) const noexcept { return spawns_[i]; }

    // Spawn matching `tag`, else the first spawn, else the origin. An
    // unmatched tag MUST NOT reject the join (design §3.3): a player who
    // cannot land anywhere is a worse outcome than one who lands in the wrong
    // place.
    fx3 spawn_for_tag(const char* tag, u32 tag_len) const noexcept;

    // --- access ---
    // --- rendering attributes (design §4) ---
    //
    // Written by the map compiler and the loader; read by the renderer. Held in
    // the same store because they arrive in the same file, exposed through a
    // separate view because physics must not see them.

    // Intern a texture name, returning its id. Repeated names collapse to one
    // id, so a map that paints two hundred walls with BRICK1 carries the string
    // once. Returns NO_TEXTURE when the table is full or the name is too long.
    u16 intern_texture(const char* name, u32 len) noexcept;
    u32 texture_count() const noexcept { return texture_count_; }
    const TextureName& texture(u32 i) const noexcept { return textures_[i]; }

    // Returns false when the map is full. Lights are cosmetic, so running out
    // is not fatal -- but it is reported, because a map that silently drops
    // half its lighting looks like a renderer bug.
    bool add_light(const MapLight& l) noexcept;
    u32 light_count() const noexcept { return light_count_; }
    const MapLight& light(u32 i) const noexcept { return lights_[i]; }

    SectorRender& sector_render_mut(u32 i) noexcept { return sector_render_[i]; }
    SideRender&   side_render_mut(u32 i) noexcept { return side_render_[i]; }

    MapData view() const noexcept;

    // The parallel rendering view. Nothing in core/physics or core/sim takes
    // one of these, and scripts/check_render_isolation.sh keeps it that way.
    RenderData render_view() const noexcept;
    u32 sector_count() const noexcept { return sector_count_; }
    Sector& sector_mut(u32 i) noexcept { return sectors_[i]; }
    Side& side_mut(u32 i) noexcept { return sides_[i]; }

    // Direct point access, for sector motion writing base+offset each tick.
    // Deliberately NOT a general editing path: changing a point's position
    // without re-triangulating is only safe when the change cannot alter which
    // vertices are ears, which is true of a translation and of nothing else.
    // Shape edits go through stage_point/commit_shape.
    fx2& point_mut(u32 i) noexcept { return points_[i]; }

    // Content hash of the geometry. Part of the world state, so a client whose
    // map has drifted from the server's is detectable rather than mysterious.
    u64 hash() const noexcept;

private:
    fx2    points_[MAX_MAP_POINTS]{};
    Sector sectors_[MAX_MAP_SECTORS]{};
    Side   sides_[MAX_MAP_SIDES]{};
    u16    tris_[MAX_MAP_TRI_IDX]{};

    u32 point_count_ = 0;
    u32 sector_count_ = 0;
    u32 side_count_ = 0;
    u32 tri_index_count_ = 0;

    // Staged edits, held out of line so the live arrays are never partially
    // written. One pending position per point is enough: staging the same point
    // twice before a commit simply means the later value wins.
    struct Staged {
        bool active = false;
        fx2  pos{};
    };
    Staged staged_[MAX_MAP_POINTS]{};
    bool   sector_dirty_[MAX_MAP_SECTORS]{};

    SpawnPoint spawns_[MAX_MAP_SPAWNS]{};
    u32        spawn_count_ = 0;

    // Rendering attributes, parallel to sectors_ and sides_ and never merged
    // into them. See RenderData in map.hpp for why the separation is
    // structural rather than a naming convention.
    SectorRender sector_render_[MAX_MAP_SECTORS]{};
    SideRender   side_render_[MAX_MAP_SIDES]{};
    TextureName  textures_[MAX_MAP_TEXTURES]{};
    u32          texture_count_ = 0;
    MapLight     lights_[MAX_MAP_LIGHTS]{};
    u32          light_count_ = 0;

    Rail         rails_[MAX_MAP_RAILS]{};
    RailWaypoint waypoints_[MAX_MAP_WAYPOINTS]{};
    RailLink     rail_links_[MAX_MAP_RAIL_LINKS]{};
    u32          rail_count_ = 0;
    u32          waypoint_count_ = 0;
    u32          rail_link_count_ = 0;
};

}  // namespace dnp::geom
