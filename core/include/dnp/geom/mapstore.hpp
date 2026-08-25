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
inline constexpr u32 MAX_SPAWN_TAG   = 32;

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
    MapData view() const noexcept;
    u32 sector_count() const noexcept { return sector_count_; }
    Sector& sector_mut(u32 i) noexcept { return sectors_[i]; }
    Side& side_mut(u32 i) noexcept { return sides_[i]; }

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
};

}  // namespace dnp::geom
