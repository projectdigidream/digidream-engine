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

// THE SIZE OF ONE MAP, raised fourfold for worlds sewn out of several.
//
// The numbers below were sized for a converted Doom level, and they are not
// generous for one: MAP15 is 307 sectors and 2361 points against 512 and 4096.
// A seamless world composed of cells wants each cell to be a place rather than
// a room, and a cell that cannot hold one and a half of Doom's larger levels is
// not a place.
//
// THEY SCALE TOGETHER, and have to. A sector needs at least three points and in
// practice takes about eight, so a sector budget the point budget cannot feed
// is a number that can never be reached -- 2048 sectors at MAP15's ratio is
// about 15,700 points, which is what 16384 is for. Triangle indices follow the
// points at three apiece.
//
// WHAT THIS COSTS. A MapStorage is a fixed arena, so the whole increase is paid
// at once, resident, per loaded map -- which is what makes loading levels on
// demand a requirement rather than an optimisation: thirty-two of these is no
// longer sixty megabytes.
//
// WHAT IT DOES NOT CHANGE. Every index into these arrays is already wide
// enough: a side's point indices are u16 and 16384 is well inside one, the
// triangle list is u16 over points, the broadphase holds u16 sector indices,
// and a sector's own point count is capped by MAX_POLY_POINTS long before any
// of this. Nothing in the file format, the wire or either hash mentions a
// capacity, so a map baked before this reads back identically.
inline constexpr u32 MAX_MAP_POINTS  = 16384;
inline constexpr u32 MAX_MAP_SECTORS = 2048;
inline constexpr u32 MAX_MAP_SIDES   = 16384;
inline constexpr u32 MAX_MAP_TRI_IDX = 49152;   // 3 per triangle
inline constexpr u32 MAX_MAP_SPAWNS  = 64;
inline constexpr u32 MAX_MAP_RAILS      = 64;
inline constexpr u32 MAX_MAP_WAYPOINTS  = 512;
inline constexpr u32 MAX_MAP_RAIL_LINKS = 64;
// Camera paths (design §4.1). Far smaller than the rail budget on purpose: a
// map has a handful of scripted shots, not a network of track.
inline constexpr u32 MAX_MAP_PATHS       = 16;
inline constexpr u32 MAX_MAP_PATH_POINTS = 256;
inline constexpr u32 MAX_SPAWN_TAG   = 32;
// ENOUGH FOR A WORLD, not for a room.
//
// 256 is generous for one converted Doom level, which uses about sixty, and is
// nothing like enough for several composed into one map: nine of them name 564
// distinct textures and all thirty-two name 604, because the sets overlap far
// less than the wall count suggests. A texture id is a u16 with 0xFFFF reserved,
// so the type has always had room; what it cost was the name table, and a
// thousand names is 25 KB against a two-megabyte arena.
inline constexpr u32 MAX_MAP_TEXTURES   = 1024;
inline constexpr u32 MAX_MAP_LIGHTS     = 128;
inline constexpr u32 MAX_MAP_HOLES      = 2048;

// The broadphase grid. 48x48 cells over the map's bounding box, and a cap on
// total membership: a sector belongs to every cell its bounding box touches, so
// one enormous sector could otherwise claim the whole grid. Overflowing sets
// the grid unusable rather than truncating it, and every query falls back to
// the linear scan -- slower, and still exactly right.
inline constexpr u32 GRID_DIM       = 48;
inline constexpr u32 GRID_CELLS     = GRID_DIM * GRID_DIM;
inline constexpr u32 MAX_GRID_ITEMS = 98304;

// A named landing point. Portals name one of these in the join handshake
// (design §3.3), so each portal can deliver its arrivals somewhere specific
// rather than every portal dumping players at one generic map spawn.
struct SpawnPoint {
    char tag[MAX_SPAWN_TAG]{};   // NUL-padded, not NUL-terminated at capacity
    u8   tag_len = 0;
    fx3  pos{};
};

// SEVERAL MAPS IN ONE STORE, added and removed while the world runs.
//
// A metamap lays levels side by side in world space. Rather than a simulation
// per level and a hand-over at every boundary, a store can hold several of
// them at once -- each copied in at its cell's origin, into slots that never
// move while it is resident -- so one World simulates all of them and a body
// walks from one into the next like walking between two rooms.
//
// THE JOIN IS THE MAPS' OWN OPEN EDGES. Where a side marked LEDGE in one chunk
// is exactly the reverse of a LEDGE in another (same two points, after the
// origins are applied), the two are joined as neighbours: the one rule the
// engine already has for "these sectors meet here". Nothing else is inferred,
// so two maps join only where their authors made them meet.
//
// Indices are the store's own. They depend on what else is resident and in
// what order it arrived, so they mean nothing to anybody holding a different
// set of chunks; the key given to add_chunk is the identity that travels.
inline constexpr u32 MAX_CHUNKS = 64;
inline constexpr u32 NO_CHUNK   = 0xFFFFFFFFu;

struct ChunkRange {
    u32 first = 0;
    u32 count = 0;
};

struct Chunk {
    bool live = false;
    u32  key = 0;          // the caller's name for it: a level index
    fx2  origin{};         // added to every point of the source map
    ChunkRange points, sectors, sides, tris, holes, rails, waypoints, rail_links;
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

    // Add a hole to the sector most recently created.
    //
    // Must be called before the next add_sector: a hole's points sit
    // immediately after its sector's outer loop, so that the triangulator can
    // be handed one base pointer and relative offsets. Adding out of order
    // would put another sector's points between them.
    //
    // Winding is normalised to the opposite of the outer loop, which is what
    // makes the shoelace sum subtract the hole and the bridge produce a simple
    // polygon.
    bool add_hole(u32 sector, const fx2* pts, u32 n) noexcept;

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

    // --- camera paths (design §4.1) ---
    //
    // Same append-then-fill discipline as rails, and separate arrays for the
    // reason map.hpp gives: a path is cosmetic and a rail is simulated, and the
    // contiguity rule that protects the second should not be loosened for the
    // first.
    //
    // `curve_length` is the arc length of the CURVE through these points, which
    // only the client knows how to draw -- tools/mapc computes it at bake and
    // passes it in here.
    u32 add_path(u16 tag, u8 flags, fx curve_length) noexcept;
    bool add_path_point(u32 path, fx3 pos, i32 anchor) noexcept;

    u32 path_count() const noexcept { return path_count_; }
    u32 path_point_count() const noexcept { return path_point_count_; }

    PathPoint& path_point_mut(u32 i) noexcept { return path_points_[i]; }

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

    // The scripts this map needs. Order is preserved and meaningful: a map's
    // own data file is written against the library it declares first.
    //
    // Duplicates collapse, so a map that names the same library twice loads it
    // once -- the alternative is a script whose top level runs twice, which for
    // anything that registers handlers is a silent double-registration.
    bool add_script(const char* name, u32 len) noexcept;
    u32 script_count() const noexcept { return script_count_; }
    const ScriptName& script(u32 i) const noexcept { return scripts_[i]; }

    // The body proportions this map is built for.
    //
    // A property of the MAP, not of the command line, because it is a fact
    // about the geometry: a Doom map converted at 1/48 has 56-unit-tall
    // doorways, and the engine's default 2.0 body is 96 of those units and
    // simply does not fit. Carrying it here means a converted map works when
    // you load it, rather than when you remember two flags -- and a wrong
    // value is visible in the map source instead of absent from a shell
    // history.
    //
    // Defaults to the engine's own defaults, so every map authored before this
    // existed means exactly what it meant before.
    fx body_radius() const noexcept { return body_radius_; }
    fx body_height() const noexcept { return body_height_; }

    // How many texture pixels cover one world unit.
    //
    // A fact about the geometry, like the body, and stated for the same reason:
    // a converted Doom map is at 1/48 and Doom's textures are one pixel per map
    // unit, so its walls want 48 pixels to the unit. The renderer defaulted to
    // 32 and drew every texture on every converted level half again too large.
    // That is not something a viewer can be expected to notice as a scale
    // error -- it reads as "the textures are placed wrong".
    //
    // Part of the map hash, like the body: a client drawing at a different
    // scale from the one the map was built for is looking at a different map.
    fx texture_scale() const noexcept { return texture_scale_; }
    void set_texture_scale(fx pixels_per_unit) noexcept {
        if (pixels_per_unit > fx::zero()) texture_scale_ = pixels_per_unit;
    }
    // Assigns BOTH, unconditionally. An earlier version skipped non-positive
    // values as a convenience for the map compiler, which meant a flipped bit
    // in the file's body field read back as "the default" and matched the hash
    // of any map that used the defaults -- the one kind of corruption the fuzz
    // test could not see. A caller that wants to change one field passes the
    // other's current value; the loader applies what the file says.
    void set_body(fx radius, fx height) noexcept {
        body_radius_ = radius;
        body_height_ = height;
    }

    SectorRender& sector_render_mut(u32 i) noexcept { return sector_render_[i]; }
    SideRender&   side_render_mut(u32 i) noexcept { return side_render_[i]; }

    MapData view() const noexcept;

    // The parallel rendering view. Nothing in core/physics or core/sim takes
    // one of these, and scripts/check_render_isolation.sh keeps it that way.
    RenderData render_view() const noexcept;
    u32 sector_count() const noexcept { return sector_count_; }
    Sector& sector_mut(u32 i) noexcept { return sectors_[i]; }
    Side& side_mut(u32 i) noexcept { return sides_[i]; }

    // --- broadphase ---
    //
    // Rebuilt from the current geometry. Called after construction, and again
    // by sector motion whenever a footprint actually moves.
    void rebuild_grid() noexcept;

    // True when a point has been written since the grid was last built. Sector
    // motion rewrites footprints, and a grid describing where they used to be
    // would be worse than none -- so the grid is marked unusable the moment a
    // point moves, and every query falls back to the scan until it is rebuilt.
    bool grid_dirty() const noexcept { return grid_dirty_; }

    // Direct point access, for sector motion writing base+offset each tick.
    // Deliberately NOT a general editing path: changing a point's position
    // without re-triangulating is only safe when the change cannot alter which
    // vertices are ears, which is true of a translation and of nothing else.
    // Shape edits go through stage_point/commit_shape.
    fx2& point_mut(u32 i) noexcept { grid_dirty_ = true; return points_[i]; }

    // --- chunks: several maps in one store (see MAX_CHUNKS above) ---
    //
    // Copies `src`'s geometry and rails into free slots, moved by `origin`,
    // and joins its open edges to any resident chunk's that exactly meet them.
    // Returns the chunk's slot, or NO_CHUNK if the key is already resident,
    // the store holds a map that was not built from chunks, or there is no
    // room. On failure nothing changes.
    //
    // Rebuilds the broadphase. Callers holding a MapData view must take a
    // fresh one: the counts it carries have changed.
    u32 add_chunk(u32 key, const MapStorage& src, fx2 origin) noexcept;

    // Frees the chunk's slots and puts back every ledge that was joined to it.
    // Its sectors become empty (no points, no sides), so nothing finds them.
    // False if no chunk has that key.
    bool remove_chunk(u32 key) noexcept;

    const Chunk* find_chunk(u32 key) const noexcept;
    const Chunk& chunk_slot(u32 slot) const noexcept { return chunks_[slot]; }
    // The key of the chunk a sector belongs to, or NO_CHUNK.
    u32 chunk_of_sector(u32 sector) const noexcept;
    // How many edge pairs are joined between chunks right now.
    u32 stitch_count() const noexcept;

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
    PolyLoop     holes_[MAX_MAP_HOLES]{};
    u32          hole_count_ = 0;

    MapLight     lights_[MAX_MAP_LIGHTS]{};
    u32          light_count_ = 0;

    // physics::MoveParams' own defaults, restated rather than included: core's
    // geometry layer does not depend on its physics layer, and a map file must
    // decode to the same numbers whatever the controller happens to default to.
    ScriptName   scripts_[MAX_MAP_SCRIPTS]{};
    u32          script_count_ = 0;

    // 32 is what the renderer used before maps could say, so a map that says
    // nothing is unchanged.
    fx texture_scale_ = fx::from_int(32);
    fx body_radius_ = fx::from_frac(1, 2);
    fx body_height_ = fx::from_int(2);

    u32  grid_start_[GRID_CELLS + 1]{};
    u16  grid_items_[MAX_GRID_ITEMS]{};
    fx2  grid_min_{};
    fx   grid_cell_{};
    u16  grid_cols_ = 0;
    u16  grid_rows_ = 0;
    bool grid_built_ = false;
    bool grid_dirty_ = true;

    Rail         rails_[MAX_MAP_RAILS]{};
    RailWaypoint waypoints_[MAX_MAP_WAYPOINTS]{};
    RailLink     rail_links_[MAX_MAP_RAIL_LINKS]{};
    u32          rail_count_ = 0;
    u32          waypoint_count_ = 0;
    u32          rail_link_count_ = 0;

    Chunk     chunks_[MAX_CHUNKS]{};
    bool      chunked_ = false;
    // The lowest start at which `need` slots of one array are free of every
    // resident chunk's range, or `cap` + 1 when there is none.
    u32 alloc_range(ChunkRange Chunk::*which, u32 need, u32 cap) const noexcept;
    void recount() noexcept;

    Path      paths_[MAX_MAP_PATHS]{};
    PathPoint path_points_[MAX_MAP_PATH_POINTS]{};
    u32       path_count_ = 0;
    u32       path_point_count_ = 0;
};

}  // namespace dnp::geom
