#include "dnp/geom/mapstore.hpp"

#include "dnp/sim/state_hash.hpp"

namespace dnp::geom {

void MapStorage::clear() noexcept {
    point_count_ = 0;
    sector_count_ = 0;
    side_count_ = 0;
    tri_index_count_ = 0;
    for (u32 i = 0; i < MAX_MAP_POINTS; ++i) staged_[i] = Staged{};
    for (u32 i = 0; i < MAX_MAP_SECTORS; ++i) sector_dirty_[i] = false;
    spawn_count_ = 0;
    rail_count_ = 0;
    waypoint_count_ = 0;
    rail_link_count_ = 0;
    path_count_ = 0;
    path_point_count_ = 0;
    texture_count_ = 0;
    light_count_ = 0;
    fx_count_ = 0;
    hole_count_ = 0;
    script_count_ = 0;
    body_radius_ = fx::from_frac(1, 2);
    body_height_ = fx::from_int(2);
    grid_built_ = false;
    grid_dirty_ = true;
    grid_cols_ = 0;
    grid_rows_ = 0;
    for (u32 i = 0; i < MAX_MAP_SECTORS; ++i) sector_render_[i] = SectorRender{};
    for (u32 i = 0; i < MAX_MAP_SIDES; ++i) side_render_[i] = SideRender{};
    for (u32 i = 0; i < MAX_CHUNKS; ++i) chunks_[i] = Chunk{};
    chunked_ = false;
}

bool MapStorage::add_hole(u32 sector, const fx2* pts, u32 n) noexcept {
    if (sector != sector_count_ - 1) return false;   // only the newest sector
    if (n < 3 || n > MAX_POLY_POINTS) return false;
    if (hole_count_ >= MAX_MAP_HOLES) return false;
    if (point_count_ + n > MAX_MAP_POINTS) return false;

    Sector& s = sectors_[sector];
    // Points must be contiguous with the sector's, so nothing may have been
    // appended since -- a side does not add points, but another sector does.
    if (s.first_point + s.point_count != point_count_ &&
        s.hole_count == 0) return false;

    const u16 rel = static_cast<u16>(point_count_ - s.first_point);
    // Opposite winding to the outer loop. The outer is normalised
    // counter-clockwise by add_sector, so a hole is stored clockwise.
    const bool ccw = polygon_area_2x(pts, n) > 0;
    for (u32 i = 0; i < n; ++i) {
        points_[point_count_ + i] = pts[ccw ? (n - 1 - i) : i];
    }
    point_count_ += n;

    if (s.hole_count == 0) s.first_hole = static_cast<u16>(hole_count_);
    holes_[hole_count_].first = rel;
    holes_[hole_count_].count = static_cast<u16>(n);
    ++hole_count_;
    ++s.hole_count;

    // Re-triangulate: add_sector already meshed the outer loop, and that mesh
    // covers the hole. Leaving it would give a sector whose COLLISION excludes
    // the hole and whose MESH does not -- a pit you fall into with a floor
    // drawn over it.
    if (retriangulate(sector) != CommitStatus::Ok) {
        // Undo, so a hole that cannot be triangulated leaves the sector exactly
        // as it was rather than meshed-without-it.
        --hole_count_;
        --s.hole_count;
        point_count_ -= n;
        retriangulate(sector);
        return false;
    }
    return true;
}

bool MapStorage::add_light(const MapLight& l) noexcept {
    if (light_count_ >= MAX_MAP_LIGHTS) return false;
    lights_[light_count_++] = l;
    return true;
}

bool MapStorage::add_effect(const SectorFx& e) noexcept {
    if (fx_count_ >= MAX_MAP_FX || e.count > MAX_FX_PARAMS) return false;
    fx_[fx_count_++] = e;
    return true;
}

bool MapStorage::add_script(const char* name, u32 len) noexcept {
    if (!script_name_is_safe(name, len)) return false;

    for (u32 i = 0; i < script_count_; ++i) {
        if (scripts_[i].len != len) continue;
        bool same = true;
        for (u32 k = 0; k < len; ++k) {
            if (scripts_[i].name[k] != name[k]) { same = false; break; }
        }
        if (same) return true;   // already declared; not an error
    }
    if (script_count_ >= MAX_MAP_SCRIPTS) return false;

    ScriptName& s = scripts_[script_count_++];
    for (u32 k = 0; k < len; ++k) s.name[k] = name[k];
    s.len = static_cast<u8>(len);
    return true;
}

u16 MapStorage::intern_texture(const char* name, u32 len) noexcept {
    if (len == 0 || len > MAX_TEXTURE_NAME) return NO_TEXTURE;

    // Linear scan. A map has at most MAX_MAP_TEXTURES distinct names and this
    // runs once at load, so the index a hash table would buy is not worth the
    // second structure to keep consistent.
    for (u32 i = 0; i < texture_count_; ++i) {
        if (textures_[i].len != len) continue;
        bool same = true;
        for (u32 k = 0; k < len; ++k) {
            if (textures_[i].name[k] != name[k]) { same = false; break; }
        }
        if (same) return static_cast<u16>(i);
    }

    if (texture_count_ >= MAX_MAP_TEXTURES) return NO_TEXTURE;
    TextureName& t = textures_[texture_count_];
    t = TextureName{};
    for (u32 k = 0; k < len; ++k) t.name[k] = name[k];
    t.len = static_cast<u8>(len);
    return static_cast<u16>(texture_count_++);
}

u32 MapStorage::add_rail(u16 tag, u8 flags) noexcept {
    if (rail_count_ >= MAX_MAP_RAILS) return MAX_MAP_RAILS;
    Rail& r = rails_[rail_count_];
    r = Rail{};
    r.first_waypoint = static_cast<u16>(waypoint_count_);
    r.waypoint_count = 0;
    r.tag = tag;
    r.flags = flags;
    return rail_count_++;
}

bool MapStorage::add_waypoint(u32 rail, fx3 pos, i32 anchor) noexcept {
    if (rail >= rail_count_) return false;
    // Only the rail most recently started may grow. A waypoint range has to be
    // contiguous, and appending to an earlier rail would silently claim the
    // next one's waypoints.
    if (rail != rail_count_ - 1) return false;
    if (waypoint_count_ >= MAX_MAP_WAYPOINTS) return false;

    RailWaypoint& w = waypoints_[waypoint_count_++];
    w = RailWaypoint{};
    w.anchor = anchor;
    w.base = pos;
    // Live from the moment it exists. A map with no moving sectors then needs
    // no per-tick pass at all, and a reader never sees an unresolved waypoint.
    w.pos = pos;
    ++rails_[rail].waypoint_count;
    return true;
}

bool MapStorage::add_rail_link(u32 rail, u16 waypoint, u32 to_rail, u16 to_waypoint) noexcept {
    if (rail_link_count_ >= MAX_MAP_RAIL_LINKS) return false;
    if (rail >= rail_count_ || to_rail >= rail_count_) return false;
    if (waypoint >= rails_[rail].waypoint_count) return false;
    if (to_waypoint >= rails_[to_rail].waypoint_count) return false;
    RailLink& l = rail_links_[rail_link_count_++];
    l.rail = static_cast<u16>(rail);
    l.waypoint = waypoint;
    l.to_rail = static_cast<u16>(to_rail);
    l.to_waypoint = to_waypoint;
    return true;
}

u32 MapStorage::add_path(u16 tag, u8 flags, fx curve_length) noexcept {
    if (path_count_ >= MAX_MAP_PATHS) return MAX_MAP_PATHS;
    Path& p = paths_[path_count_];
    p = Path{};
    p.first_point = static_cast<u16>(path_point_count_);
    p.point_count = 0;
    p.tag = tag;
    p.flags = flags;
    p.curve_length = curve_length;
    return path_count_++;
}

bool MapStorage::add_path_point(u32 path, fx3 pos, i32 anchor) noexcept {
    if (path >= path_count_) return false;
    // Same rule as add_waypoint, for the same reason: only the most recently
    // started path may grow, or its point range stops being contiguous.
    if (path != path_count_ - 1) return false;
    if (path_point_count_ >= MAX_MAP_PATH_POINTS) return false;

    PathPoint& pp = path_points_[path_point_count_++];
    pp = PathPoint{};
    pp.anchor = anchor;
    pp.base = pos;
    pp.pos = pos;   // live from the moment it exists, as a waypoint is
    ++paths_[path].point_count;
    return true;
}

bool MapStorage::add_spawn(const char* tag, u32 tag_len, fx3 pos) noexcept {
    if (spawn_count_ >= MAX_MAP_SPAWNS) return false;
    if (tag_len > MAX_SPAWN_TAG) return false;
    SpawnPoint& sp = spawns_[spawn_count_++];
    sp = SpawnPoint{};
    for (u32 i = 0; i < tag_len; ++i) sp.tag[i] = tag[i];
    sp.tag_len = static_cast<u8>(tag_len);
    sp.pos = pos;
    return true;
}

fx3 MapStorage::spawn_for_tag(const char* tag, u32 tag_len) const noexcept {
    for (u32 i = 0; i < spawn_count_; ++i) {
        const SpawnPoint& sp = spawns_[i];
        if (sp.tag_len != tag_len) continue;
        bool same = true;
        for (u32 k = 0; k < tag_len; ++k) {
            if (sp.tag[k] != tag[k]) { same = false; break; }
        }
        if (same) return sp.pos;
    }
    // Fall back rather than fail. See the header.
    if (spawn_count_ > 0) return spawns_[0].pos;
    return fx3::zero();
}

bool MapStorage::has_spawn_tag(const char* tag, u32 tag_len) const noexcept {
    for (u32 i = 0; i < spawn_count_; ++i) {
        const SpawnPoint& sp = spawns_[i];
        if (sp.tag_len != tag_len) continue;
        bool same = true;
        for (u32 k = 0; k < tag_len; ++k) {
            if (sp.tag[k] != tag[k]) { same = false; break; }
        }
        if (same) return true;
    }
    return false;
}

u32 MapStorage::add_sector(const fx2* pts, u32 n, fx floor_z, fx ceil_z, u16 tag) noexcept {
    if (sector_count_ >= MAX_MAP_SECTORS) return MAX_MAP_SECTORS;
    if (n < 3 || n > MAX_POLY_POINTS) return MAX_MAP_SECTORS;
    if (point_count_ + n > MAX_MAP_POINTS) return MAX_MAP_SECTORS;

    Sector& s = sectors_[sector_count_];
    s = Sector{};
    s.first_point = static_cast<u16>(point_count_);
    s.point_count = static_cast<u16>(n);
    // first_side is assigned lazily, on this sector's FIRST add_side. Capturing
    // it here instead would silently require that every sector's sides be added
    // before the next sector exists -- and a caller that adds all the sectors
    // and then all the sides, which is the natural order, would have every
    // sector after the first reject its sides and end up with no walls at all.
    s.first_side = 0;
    s.side_count = 0;
    s.floor_z = floor_z;
    s.ceil_z = ceil_z;
    s.tag = tag;

    for (u32 i = 0; i < n; ++i) points_[point_count_ + i] = pts[i];
    point_count_ += n;

    const u32 idx = sector_count_++;
    retriangulate(idx);
    return idx;
}

bool MapStorage::add_side(u32 sector, u16 local_a, u16 local_b, i32 neighbor, u8 flags) noexcept {
    if (sector >= sector_count_ || side_count_ >= MAX_MAP_SIDES) return false;
    Sector& s = sectors_[sector];
    // Bounded by the sector's WHOLE point span, outer loop plus holes -- a
    // hole's edges are sides too, and their local indices continue past the
    // outer loop exactly as add_hole laid the points out.
    u32 span = s.point_count;
    for (u16 h = 0; h < s.hole_count; ++h) span += holes_[s.first_hole + h].count;
    if (local_a >= span || local_b >= span) return false;

    // A Sector addresses its sides as a contiguous range, so one sector's sides
    // must be added consecutively -- but WHICH sector is being filled need not
    // be the most recently created one.
    if (s.side_count == 0) {
        s.first_side = static_cast<u16>(side_count_);
    } else if (static_cast<u32>(s.first_side) + s.side_count != side_count_) {
        return false;
    }

    Side& sd = sides_[side_count_++];
    sd.a = static_cast<u16>(s.first_point + local_a);
    sd.b = static_cast<u16>(s.first_point + local_b);
    sd.neighbor = neighbor;
    sd.flags = flags;
    ++s.side_count;
    return true;
}

CommitStatus MapStorage::retriangulate(u32 sector) noexcept {
    if (sector >= sector_count_) return CommitStatus::BadSector;
    Sector& s = sectors_[sector];

    TriangleList tl;
    const TriangulateStatus st =
        s.hole_count == 0
            ? triangulate(&points_[s.first_point], s.point_count, tl)
            : triangulate_with_holes(&points_[s.first_point], s.point_count,
                                     &holes_[s.first_hole], s.hole_count, tl);
    if (st != TriangulateStatus::Ok) return CommitStatus::TriangulationFailed;

    // Reuse the sector's existing range when it still fits. Triangle count is a
    // pure function of point count for a simple polygon, so a shape edit that
    // keeps the point count -- which is the common case -- never needs new
    // space and never fragments the array.
    const u32 need = static_cast<u32>(tl.count) * 3;
    u32 base;
    if (s.tri_count > 0 && static_cast<u32>(s.tri_count) * 3 >= need) {
        base = s.first_tri;
    } else if (s.tri_count > 0 &&
               s.first_tri + static_cast<u32>(s.tri_count) * 3 == tri_index_count_ &&
               s.first_tri + need <= MAX_MAP_TRI_IDX) {
        // The sector's range is the topmost, so it can simply GROW rather than
        // being abandoned and re-allocated past itself. Without this, adding a
        // hole to a sector that add_sector had just meshed left the outer
        // loop's triangles stranded -- counted, saved, transferred, and
        // referenced by nothing.
        base = s.first_tri;
        tri_index_count_ = s.first_tri + need;
    } else {
        if (tri_index_count_ + need > MAX_MAP_TRI_IDX) return CommitStatus::OutOfTriangleSpace;
        base = tri_index_count_;
        tri_index_count_ += need;
    }

    for (u32 i = 0; i < need; ++i) {
        // Rebase to absolute point indices so a renderer can draw the whole map
        // from one vertex buffer.
        tris_[base + i] = static_cast<u16>(s.first_point + tl.indices[i]);
    }
    s.first_tri = base;
    s.tri_count = tl.count;
    return CommitStatus::Ok;
}

bool MapStorage::stage_point(u32 sector, u16 local_index, fx2 pos) noexcept {
    if (sector >= sector_count_) return false;
    const Sector& s = sectors_[sector];
    if (local_index >= s.point_count) return false;

    const u32 gi = static_cast<u32>(s.first_point) + local_index;
    staged_[gi].active = true;
    staged_[gi].pos = pos;
    sector_dirty_[sector] = true;
    return true;
}

bool MapStorage::has_staged_edits(u32 sector) const noexcept {
    return sector < sector_count_ && sector_dirty_[sector];
}

void MapStorage::discard_staged_edits(u32 sector) noexcept {
    if (sector >= sector_count_) return;
    const Sector& s = sectors_[sector];
    for (u16 i = 0; i < s.point_count; ++i) staged_[s.first_point + i] = Staged{};
    sector_dirty_[sector] = false;
}

CommitStatus MapStorage::commit_shape(u32 sector) noexcept {
    if (sector >= sector_count_) return CommitStatus::BadSector;
    if (!sector_dirty_[sector]) return CommitStatus::NoStagedEdits;

    Sector& s = sectors_[sector];

    // Triangulate the PROPOSED shape before touching the live points, so a
    // rejected shape leaves the world exactly as it was. Committing first and
    // rolling back afterwards would mean a failure window in which the sector's
    // points and its mesh disagree -- and anything that queried collision in
    // that window would get an answer derived from geometry that never existed.
    fx2 proposed[MAX_POLY_POINTS];
    for (u16 i = 0; i < s.point_count; ++i) {
        const u32 gi = static_cast<u32>(s.first_point) + i;
        proposed[i] = staged_[gi].active ? staged_[gi].pos : points_[gi];
    }

    TriangleList tl;
    if (triangulate(proposed, s.point_count, tl) != TriangulateStatus::Ok) {
        discard_staged_edits(sector);
        return CommitStatus::TriangulationFailed;
    }

    const u32 need = static_cast<u32>(tl.count) * 3;
    u32 base;
    if (s.tri_count > 0 && static_cast<u32>(s.tri_count) * 3 >= need) {
        base = s.first_tri;
    } else {
        if (tri_index_count_ + need > MAX_MAP_TRI_IDX) {
            discard_staged_edits(sector);
            return CommitStatus::OutOfTriangleSpace;
        }
        base = tri_index_count_;
        tri_index_count_ += need;
    }

    // Past this point nothing can fail, so the update is atomic from any
    // reader's perspective.
    for (u16 i = 0; i < s.point_count; ++i) points_[s.first_point + i] = proposed[i];
    for (u32 i = 0; i < need; ++i) tris_[base + i] = static_cast<u16>(s.first_point + tl.indices[i]);
    s.first_tri = base;
    s.tri_count = tl.count;

    discard_staged_edits(sector);
    return CommitStatus::Ok;
}

void MapStorage::rebuild_grid() noexcept {
    grid_built_ = false;
    grid_dirty_ = false;
    grid_cols_ = 0;
    grid_rows_ = 0;
    if (sector_count_ == 0 || point_count_ == 0) return;

    // Bounding box of every sector's outer loop. The same box as every point
    // in a map read from a file -- a hole lies inside its sector's outer loop
    // -- and in a store built from chunks it leaves out the slots a removed
    // chunk left behind, whose contents are whatever it was.
    bool any = false;
    fx2 lo{}, hi{};
    for (u32 s = 0; s < sector_count_; ++s) {
        const Sector& sec = sectors_[s];
        for (u16 k = 0; k < sec.point_count; ++k) {
            const fx2& q = points_[sec.first_point + k];
            if (!any) { lo = q; hi = q; any = true; continue; }
            if (q.x < lo.x) lo.x = q.x;
            if (q.y < lo.y) lo.y = q.y;
            if (q.x > hi.x) hi.x = q.x;
            if (q.y > hi.y) hi.y = q.y;
        }
    }
    if (!any) return;

    // One cell size for both axes, so a cell is square and a query needs one
    // divisor. Rounded UP, so GRID_DIM cells always cover the box -- rounding
    // down leaves the far edge outside the grid, where a query would find no
    // candidates and report empty space in the middle of a map.
    const i64 span_x = hi.x.raw - lo.x.raw;
    const i64 span_y = hi.y.raw - lo.y.raw;
    const i64 span = span_x > span_y ? span_x : span_y;
    if (span <= 0) return;
    const i64 cell = (span + GRID_DIM - 1) / GRID_DIM + 1;

    grid_min_ = lo;
    grid_cell_ = fx::from_raw(cell);
    grid_cols_ = static_cast<u16>(span_x / cell + 1);
    grid_rows_ = static_cast<u16>(span_y / cell + 1);
    const u32 cells = static_cast<u32>(grid_cols_) * grid_rows_;
    if (cells == 0 || cells > GRID_CELLS) return;

    // Counting sort into CSR: count per cell, prefix sum, then fill. Two passes
    // over the sectors rather than a per-cell vector, because the core does not
    // allocate.
    for (u32 i = 0; i <= cells; ++i) grid_start_[i] = 0;

    u32 total = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 s = 0; s < sector_count_; ++s) {
            const Sector& sec = sectors_[s];
            if (sec.point_count == 0) continue;
            fx2 slo = points_[sec.first_point];
            fx2 shi = slo;
            for (u16 k = 1; k < sec.point_count; ++k) {
                const fx2& q = points_[sec.first_point + k];
                if (q.x < slo.x) slo.x = q.x;
                if (q.y < slo.y) slo.y = q.y;
                if (q.x > shi.x) shi.x = q.x;
                if (q.y > shi.y) shi.y = q.y;
            }
            i64 x0 = (slo.x.raw - lo.x.raw) / cell;
            i64 x1 = (shi.x.raw - lo.x.raw) / cell;
            i64 y0 = (slo.y.raw - lo.y.raw) / cell;
            i64 y1 = (shi.y.raw - lo.y.raw) / cell;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 >= grid_cols_) x1 = grid_cols_ - 1;
            if (y1 >= grid_rows_) y1 = grid_rows_ - 1;

            for (i64 cy = y0; cy <= y1; ++cy) {
                for (i64 cx = x0; cx <= x1; ++cx) {
                    const u32 c = static_cast<u32>(cy) * grid_cols_ + static_cast<u32>(cx);
                    if (pass == 0) {
                        ++grid_start_[c + 1];
                        if (++total > MAX_GRID_ITEMS) return;   // unusable; scan instead
                    } else {
                        grid_items_[grid_start_[c]++] = static_cast<u16>(s);
                    }
                }
            }
        }
        if (pass == 0) {
            for (u32 i = 0; i < cells; ++i) grid_start_[i + 1] += grid_start_[i];
            // grid_start_[c] is now the write cursor for cell c, and the fill
            // pass advances it. Ascending sector order is preserved within each
            // cell, which is what makes a tie between two candidates resolve
            // the same way on every target.
        }
    }

    // The fill pass left each cursor at the END of its cell, which is exactly
    // the start of the next -- so shifting by one restores the offsets.
    for (u32 i = cells; i > 0; --i) grid_start_[i] = grid_start_[i - 1];
    grid_start_[0] = 0;
    grid_built_ = true;
}

// --- chunks -------------------------------------------------------------------

const Chunk* MapStorage::find_chunk(u32 key) const noexcept {
    for (u32 i = 0; i < MAX_CHUNKS; ++i) {
        if (chunks_[i].live && chunks_[i].key == key) return &chunks_[i];
    }
    return nullptr;
}

u32 MapStorage::chunk_of_sector(u32 sector) const noexcept {
    for (u32 i = 0; i < MAX_CHUNKS; ++i) {
        const Chunk& c = chunks_[i];
        if (c.live && sector >= c.sectors.first && sector < c.sectors.first + c.sectors.count) return c.key;
    }
    return NO_CHUNK;
}

u32 MapStorage::stitch_count() const noexcept {
    u32 n = 0;
    for (u32 i = 0; i < side_count_; ++i) if (sides_[i].flags & side_flag::STITCHED) ++n;
    return n / 2;
}

u32 MapStorage::alloc_range(ChunkRange Chunk::*which, u32 need, u32 cap) const noexcept {
    if (need == 0) return 0;
    // Candidates: the start of the array and the end of every resident range.
    // The lowest that fits wins, so the layout depends only on the order chunks
    // were added and removed -- the same sequence gives the same slots.
    u32 best = cap + 1;
    for (u32 ci = 0; ci <= MAX_CHUNKS; ++ci) {
        u32 p = 0;
        if (ci < MAX_CHUNKS) {
            const Chunk& c = chunks_[ci];
            if (!c.live) continue;
            p = (c.*which).first + (c.*which).count;
        }
        if (p + need > cap || p >= best) continue;
        bool clear_of_all = true;
        for (u32 k = 0; k < MAX_CHUNKS && clear_of_all; ++k) {
            const Chunk& o = chunks_[k];
            if (!o.live) continue;
            const ChunkRange& r = o.*which;
            if (r.count && p < r.first + r.count && r.first < p + need) clear_of_all = false;
        }
        if (clear_of_all) best = p;
    }
    return best;
}

void MapStorage::recount() noexcept {
    u32 np = 0, ns = 0, nd = 0, nt = 0, nh = 0, nr = 0, nw = 0, nl = 0;
    const auto end = [](const ChunkRange& r) { return r.first + r.count; };
    for (u32 i = 0; i < MAX_CHUNKS; ++i) {
        const Chunk& c = chunks_[i];
        if (!c.live) continue;
        if (end(c.points) > np) np = end(c.points);
        if (end(c.sectors) > ns) ns = end(c.sectors);
        if (end(c.sides) > nd) nd = end(c.sides);
        if (end(c.tris) > nt) nt = end(c.tris);
        if (end(c.holes) > nh) nh = end(c.holes);
        if (end(c.rails) > nr) nr = end(c.rails);
        if (end(c.waypoints) > nw) nw = end(c.waypoints);
        if (end(c.rail_links) > nl) nl = end(c.rail_links);
    }
    point_count_ = np; sector_count_ = ns; side_count_ = nd; tri_index_count_ = nt;
    hole_count_ = nh; rail_count_ = nr; waypoint_count_ = nw; rail_link_count_ = nl;
}

u32 MapStorage::sector_name(i32 sector) const noexcept {
    if (sector < 0) return NO_NAME;
    const u32 s = static_cast<u32>(sector);
    if (!chunked_) return s < sector_count_ ? s : NO_NAME;
    for (u32 i = 0; i < MAX_CHUNKS; ++i) {
        const Chunk& c = chunks_[i];
        if (c.live && s >= c.sectors.first && s < c.sectors.first + c.sectors.count) {
            return (c.key << 16) | (s - c.sectors.first);
        }
    }
    return NO_NAME;
}

i32 MapStorage::sector_by_name(u32 name) const noexcept {
    if (name == NO_NAME) return NO_SECTOR;
    if (!chunked_) return name < sector_count_ ? static_cast<i32>(name) : NO_SECTOR;
    const Chunk* c = find_chunk(name >> 16);
    const u32 local = name & 0xFFFFu;
    if (!c || local >= c->sectors.count) return NO_SECTOR;
    return static_cast<i32>(c->sectors.first + local);
}

u32 MapStorage::side_name(i32 side) const noexcept {
    if (side < 0) return NO_NAME;
    const u32 s = static_cast<u32>(side);
    if (!chunked_) return s < side_count_ ? s : NO_NAME;
    for (u32 i = 0; i < MAX_CHUNKS; ++i) {
        const Chunk& c = chunks_[i];
        if (c.live && s >= c.sides.first && s < c.sides.first + c.sides.count) {
            return (c.key << 16) | (s - c.sides.first);
        }
    }
    return NO_NAME;
}

i32 MapStorage::side_by_name(u32 name) const noexcept {
    if (name == NO_NAME) return -1;
    if (!chunked_) return name < side_count_ ? static_cast<i32>(name) : -1;
    const Chunk* c = find_chunk(name >> 16);
    const u32 local = name & 0xFFFFu;
    if (!c || local >= c->sides.count) return -1;
    return static_cast<i32>(c->sides.first + local);
}

u32 MapStorage::add_chunk(u32 key, const MapStorage& src, fx2 origin) noexcept {
    if (key > MAX_CHUNK_KEY || find_chunk(key)) return NO_CHUNK;
    // A store is either one map or a set of chunks. Mixing them would leave
    // the map's own content with no range to free and nothing to stitch by.
    if (!chunked_ && (sector_count_ != 0 || point_count_ != 0)) return NO_CHUNK;
    u32 slot = NO_CHUNK;
    for (u32 i = 0; i < MAX_CHUNKS; ++i) if (!chunks_[i].live) { slot = i; break; }
    if (slot == NO_CHUNK) return NO_CHUNK;

    Chunk c;
    c.key = key;
    c.origin = origin;
    struct Want { ChunkRange Chunk::*which; u32 need; u32 cap; };
    const Want wants[] = {
        {&Chunk::points,     src.point_count_,     MAX_MAP_POINTS},
        {&Chunk::sectors,    src.sector_count_,    MAX_MAP_SECTORS},
        {&Chunk::sides,      src.side_count_,      MAX_MAP_SIDES},
        {&Chunk::tris,       src.tri_index_count_, MAX_MAP_TRI_IDX},
        {&Chunk::holes,      src.hole_count_,      MAX_MAP_HOLES},
        {&Chunk::rails,      src.rail_count_,      MAX_MAP_RAILS},
        {&Chunk::waypoints,  src.waypoint_count_,  MAX_MAP_WAYPOINTS},
        {&Chunk::rail_links, src.rail_link_count_, MAX_MAP_RAIL_LINKS},
    };
    for (const Want& w : wants) {
        const u32 at = alloc_range(w.which, w.need, w.cap);
        if (at > w.cap) return NO_CHUNK;
        c.*(w.which) = ChunkRange{at, w.need};
    }
    // Every index a sector, side or rail holds must fit the u16 it is kept in,
    // and a sector's or side's index within its own map must fit its name.
    if (src.sector_count_ > 0x10000u || src.side_count_ > 0x10000u ||
        c.points.first + c.points.count > 0x10000u || c.sides.first + c.sides.count > 0x10000u ||
        c.holes.first + c.holes.count > 0x10000u || c.waypoints.first + c.waypoints.count > 0x10000u) {
        return NO_CHUNK;
    }

    const u32 P = c.points.first, S = c.sectors.first, D = c.sides.first, T = c.tris.first;
    const u32 H = c.holes.first, R = c.rails.first, W = c.waypoints.first, L = c.rail_links.first;
    const auto sector_ref = [S](i32 s) { return s < 0 ? s : static_cast<i32>(static_cast<u32>(s) + S); };

    for (u32 i = 0; i < src.point_count_; ++i) {
        points_[P + i] = fx2{src.points_[i].x + origin.x, src.points_[i].y + origin.y};
        staged_[P + i] = Staged{};
    }
    for (u32 i = 0; i < src.sector_count_; ++i) {
        Sector s = src.sectors_[i];
        s.first_point = static_cast<u16>(s.first_point + P);
        s.first_side  = static_cast<u16>(s.first_side + D);
        s.first_tri   = s.first_tri + T;
        s.first_hole  = static_cast<u16>(s.first_hole + H);
        sectors_[S + i] = s;
        sector_dirty_[S + i] = false;
        sector_render_[S + i] = SectorRender{};
    }
    for (u32 i = 0; i < src.side_count_; ++i) {
        Side d = src.sides_[i];
        d.a = static_cast<u16>(d.a + P);
        d.b = static_cast<u16>(d.b + P);
        d.neighbor = sector_ref(d.neighbor);
        d.flags = static_cast<u8>(d.flags & ~side_flag::STITCHED);
        sides_[D + i] = d;
        side_render_[D + i] = SideRender{};
    }
    for (u32 i = 0; i < src.tri_index_count_; ++i) tris_[T + i] = static_cast<u16>(src.tris_[i] + P);
    for (u32 i = 0; i < src.hole_count_; ++i) holes_[H + i] = src.holes_[i];   // relative already
    for (u32 i = 0; i < src.rail_count_; ++i) {
        Rail r = src.rails_[i];
        r.first_waypoint = static_cast<u16>(r.first_waypoint + W);
        rails_[R + i] = r;
    }
    for (u32 i = 0; i < src.waypoint_count_; ++i) {
        RailWaypoint w = src.waypoints_[i];
        w.anchor = sector_ref(w.anchor);
        w.base.x = w.base.x + origin.x; w.base.y = w.base.y + origin.y;
        w.pos.x  = w.pos.x + origin.x;  w.pos.y  = w.pos.y + origin.y;
        waypoints_[W + i] = w;
    }
    for (u32 i = 0; i < src.rail_link_count_; ++i) {
        RailLink l = src.rail_links_[i];
        l.rail    = static_cast<u16>(l.rail + R);
        l.to_rail = static_cast<u16>(l.to_rail + R);
        rail_links_[L + i] = l;
    }

    c.live = true;
    chunks_[slot] = c;
    chunked_ = true;
    recount();

    // THE JOIN. Every open edge of the new chunk against every open edge of
    // the others, by exact endpoints reversed. Load-time work, and open edges
    // are few: a room has none, and a map's boundary has one per doorway or
    // ground tile along it.
    for (u32 si = S; si < S + c.sectors.count; ++si) {
        const Sector& sec = sectors_[si];
        for (u32 j = sec.first_side; j < static_cast<u32>(sec.first_side) + sec.side_count; ++j) {
            Side& mine = sides_[j];
            if (!(mine.flags & side_flag::LEDGE) || mine.neighbor != NO_SECTOR) continue;
            const fx2 a = points_[mine.a], b = points_[mine.b];
            bool joined = false;
            for (u32 ci = 0; ci < MAX_CHUNKS && !joined; ++ci) {
                const Chunk& o = chunks_[ci];
                if (!o.live || ci == slot) continue;
                for (u32 oi = o.sectors.first; oi < o.sectors.first + o.sectors.count && !joined; ++oi) {
                    const Sector& os = sectors_[oi];
                    for (u32 k = os.first_side; k < static_cast<u32>(os.first_side) + os.side_count; ++k) {
                        Side& theirs = sides_[k];
                        if (!(theirs.flags & side_flag::LEDGE) || theirs.neighbor != NO_SECTOR) continue;
                        if (!(points_[theirs.a] == b && points_[theirs.b] == a)) continue;
                        mine.neighbor = static_cast<i32>(oi);
                        theirs.neighbor = static_cast<i32>(si);
                        mine.flags = static_cast<u8>((mine.flags & ~side_flag::LEDGE) | side_flag::STITCHED);
                        theirs.flags = static_cast<u8>((theirs.flags & ~side_flag::LEDGE) | side_flag::STITCHED);
                        joined = true;
                        break;
                    }
                }
            }
        }
    }
    rebuild_grid();
    return slot;
}

bool MapStorage::remove_chunk(u32 key) noexcept {
    Chunk* c = nullptr;
    for (u32 i = 0; i < MAX_CHUNKS; ++i) if (chunks_[i].live && chunks_[i].key == key) c = &chunks_[i];
    if (!c) return false;

    // Put back every ledge on the far side of a join. The partner is the side
    // of the neighbouring sector that names this one across the same edge.
    for (u32 si = c->sectors.first; si < c->sectors.first + c->sectors.count; ++si) {
        const Sector& sec = sectors_[si];
        for (u32 j = sec.first_side; j < static_cast<u32>(sec.first_side) + sec.side_count; ++j) {
            const Side& mine = sides_[j];
            if (!(mine.flags & side_flag::STITCHED) || mine.neighbor < 0) continue;
            const Sector& os = sectors_[static_cast<u32>(mine.neighbor)];
            for (u32 k = os.first_side; k < static_cast<u32>(os.first_side) + os.side_count; ++k) {
                Side& theirs = sides_[k];
                if (!(theirs.flags & side_flag::STITCHED) || theirs.neighbor != static_cast<i32>(si)) continue;
                if (!(points_[theirs.a] == points_[mine.b] && points_[theirs.b] == points_[mine.a])) continue;
                theirs.neighbor = NO_SECTOR;
                theirs.flags = static_cast<u8>((theirs.flags & ~side_flag::STITCHED) | side_flag::LEDGE);
                break;
            }
        }
    }
    // Empty, not merely forgotten: a sector with no points is found by no
    // lookup and skipped by the broadphase, and a rail link naming no rail
    // matches no query.
    for (u32 i = c->sectors.first; i < c->sectors.first + c->sectors.count; ++i) sectors_[i] = Sector{};
    for (u32 i = c->sides.first; i < c->sides.first + c->sides.count; ++i) sides_[i] = Side{};
    for (u32 i = c->rails.first; i < c->rails.first + c->rails.count; ++i) rails_[i] = Rail{};
    for (u32 i = c->waypoints.first; i < c->waypoints.first + c->waypoints.count; ++i) waypoints_[i] = RailWaypoint{};
    for (u32 i = c->rail_links.first; i < c->rail_links.first + c->rail_links.count; ++i) {
        rail_links_[i] = RailLink{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    }
    *c = Chunk{};
    recount();
    rebuild_grid();
    return true;
}

MapData MapStorage::view() const noexcept {
    MapData m;
    m.points = points_;
    m.point_count = point_count_;
    m.sectors = sectors_;
    m.sector_count = sector_count_;
    m.sides = sides_;
    m.side_count = side_count_;
    m.tris = tris_;
    m.tri_index_count = tri_index_count_;
    m.rails = rails_;
    m.rail_count = rail_count_;
    m.waypoints = waypoints_;
    m.waypoint_count = waypoint_count_;
    m.rail_links = rail_links_;
    m.rail_link_count = rail_link_count_;
    m.paths = paths_;
    m.path_count = path_count_;
    m.path_points = path_points_;
    m.path_point_count = path_point_count_;
    m.holes = holes_;
    m.hole_loop_count = hole_count_;

    // Only offered when it is both built and current. A stale grid is never
    // handed out, so no query can silently consult one.
    if (grid_built_ && !grid_dirty_) {
        m.grid.usable = true;
        m.grid.min = grid_min_;
        m.grid.cell = grid_cell_;
        m.grid.cols = grid_cols_;
        m.grid.rows = grid_rows_;
        m.grid.start = grid_start_;
        m.grid.items = grid_items_;
    }
    return m;
}

RenderData MapStorage::render_view() const noexcept {
    RenderData r;
    r.lights = lights_;
    r.light_count = light_count_;
    r.sectors = sector_render_;
    r.sector_count = sector_count_;
    r.sides = side_render_;
    r.side_count = side_count_;
    r.textures = textures_;
    r.texture_count = texture_count_;
    return r;
}

u64 MapStorage::hash() const noexcept {
    StateHasher h;
    h.u32v(point_count_);
    h.u32v(sector_count_);
    h.u32v(side_count_);
    for (u32 i = 0; i < point_count_; ++i) h.vec2(points_[i]);
    for (u32 i = 0; i < sector_count_; ++i) {
        const Sector& s = sectors_[i];
        h.u32v(s.first_point); h.u32v(s.point_count);
        h.u32v(s.first_side);  h.u32v(s.side_count);
        h.fxv(s.floor_z);      h.fxv(s.ceil_z);
        h.fxv(s.thickness);
        h.u32v(s.hole_count);
        for (u16 k = 0; k < s.hole_count; ++k) {
            h.u32v(holes_[s.first_hole + k].first);
            h.u32v(holes_[s.first_hole + k].count);
        }
        h.u32v(s.tag);
        h.u32v(s.tri_count);
        for (u32 t = 0; t < static_cast<u32>(s.tri_count) * 3; ++t) h.u32v(tris_[s.first_tri + t]);
    }
    for (u32 i = 0; i < side_count_; ++i) {
        const Side& sd = sides_[i];
        h.u32v(sd.a); h.u32v(sd.b); h.i32v(sd.neighbor); h.byte(sd.flags);
    }
    // Rails are hashed by their AUTHORED positions, never their live ones. The
    // live position of a waypoint on a moving platform changes every tick, and
    // a map hash that changed every tick would make WELCOME.map_hash useless
    // for the one thing it exists for: telling a client its geometry differs
    // from the server's.
    // The scripts the map asks for. Hashed because they are part of what the
    // file SAYS -- a map that names a different action library is a different
    // map, and WELCOME.map_hash exists to notice exactly that.
    h.u32v(script_count_);
    for (u32 i = 0; i < script_count_; ++i) {
        h.u32v(scripts_[i].len);
        for (u8 k = 0; k < scripts_[i].len; ++k) h.byte(static_cast<unsigned char>(scripts_[i].name[k]));
    }

    // The body the map is built for. Hashed for the same reason the textures
    // below are: WELCOME.map_hash answers "did we parse the same file", and a
    // client whose player is a different size from the server's has a different
    // file -- one that would mispredict every wall it slid along.
    h.fxv(body_radius_);
    h.fxv(body_height_);
    // Hashed like the body, and for the same reason: a client drawing at a
    // different texture scale from the one the map was built for is looking at
    // a different map. Also because the file format's fuzz test asserts there
    // are no don't-care bytes -- a field written and not hashed is a bit that
    // can be flipped without anything noticing, which is what it caught.
    h.fxv(texture_scale_);

    h.u32v(rail_count_);
    h.u32v(waypoint_count_);
    h.u32v(rail_link_count_);
    for (u32 i = 0; i < rail_count_; ++i) {
        const Rail& r = rails_[i];
        h.u32v(r.first_waypoint); h.u32v(r.waypoint_count);
        h.u32v(r.tag); h.byte(r.flags);
    }
    for (u32 i = 0; i < waypoint_count_; ++i) {
        h.i32v(waypoints_[i].anchor);
        h.vec3(waypoints_[i].base);
    }
    for (u32 i = 0; i < rail_link_count_; ++i) {
        const RailLink& l = rail_links_[i];
        h.u32v(l.rail); h.u32v(l.waypoint); h.u32v(l.to_rail); h.u32v(l.to_waypoint);
    }

    // Camera paths, by their AUTHORED positions -- the same rule as waypoints
    // above, and for the same reason.
    //
    // Hashed at all because WELCOME.map_hash answers "did we parse the same
    // file", and the client is the ONLY reader of these: if its points differed
    // from the server's, nothing else in the system would ever notice. That
    // makes the hash the entire agreement, not a backstop to one.
    //
    // curve_length is hashed with them because it is a field in the file. The
    // format's fuzz test asserts there are no don't-care bytes, so a number
    // written and not hashed is a bit an attacker can flip in silence -- and
    // this one sets how long a cutscene runs.
    h.u32v(path_count_);
    h.u32v(path_point_count_);
    for (u32 i = 0; i < path_count_; ++i) {
        const Path& p = paths_[i];
        h.u32v(p.first_point); h.u32v(p.point_count);
        h.u32v(p.tag); h.byte(p.flags);
        h.fxv(p.curve_length);
    }
    for (u32 i = 0; i < path_point_count_; ++i) {
        h.i32v(path_points_[i].anchor);
        h.vec3(path_points_[i].base);
    }

    // Rendering attributes ARE hashed, even though they never reach the
    // simulation. WELCOME.map_hash answers one question -- "did we parse the
    // same file" -- and a client whose walls are textured differently from the
    // server's has a different file. Excluding them would make that the one
    // kind of map divergence the check cannot see, and it costs nothing here:
    // this hash feeds no determinism gate, so a retexture cannot move a golden
    // vector.
    h.u32v(light_count_);
    for (u32 i = 0; i < light_count_; ++i) {
        const MapLight& l = lights_[i];
        h.vec3(l.pos);
        h.byte(l.r); h.byte(l.g); h.byte(l.b);
        h.fxv(l.radius); h.fxv(l.intensity);
    }
    h.u32v(texture_count_);
    for (u32 i = 0; i < texture_count_; ++i) {
        h.byte(textures_[i].len);
        for (u32 k = 0; k < textures_[i].len; ++k) {
            h.byte(static_cast<unsigned char>(textures_[i].name[k]));
        }
    }
    // A FLAT'S STYLE IS FOLDED IN ONLY WHEN IT SAYS SOMETHING.
    //
    // Not an optimisation. This hash answers "did we parse the same file", and
    // a map's identity should not change because it was saved by a newer
    // writer: a level with no rotated floors is the same level before and after
    // the format learned about rotation, and its hash is what
    // modules/doom2/assets/music/index.json is keyed by.
    //
    // It costs nothing in coverage. The format's own fuzz test asserts there
    // are no don't-care bytes, and there are none here either: "default" has
    // exactly one encoding, so flipping any bit of a default style produces a
    // non-default one, which IS folded in and caught.
    const auto fold = [&](const SurfaceStyle& st) {
        if (st.is_default()) return;
        h.u32v(st.angle);
        h.fxv(st.off_x); h.fxv(st.off_y); h.fxv(st.scale);
        h.byte(st.alpha);
        h.byte(st.anchor);
    };
    for (u32 i = 0; i < sector_count_; ++i) {
        const SectorRender& r = sector_render_[i];
        h.u32v(r.floor_tex); h.u32v(r.ceil_tex); h.byte(r.light_level);
        fold(r.floor);
        fold(r.ceil);
    }
    for (u32 i = 0; i < side_count_; ++i) {
        const SideRender& r = side_render_[i];
        h.u32v(r.upper); h.u32v(r.middle); h.u32v(r.lower);
        h.byte(r.peg_flags);
        h.fxv(r.x_offset); h.fxv(r.y_offset);
        if (r.alpha != 255) h.byte(r.alpha);
    }

    h.u32v(spawn_count_);
    for (u32 i = 0; i < spawn_count_; ++i) {
        const SpawnPoint& sp = spawns_[i];
        h.byte(sp.tag_len);
        for (u32 k = 0; k < sp.tag_len; ++k) h.byte(static_cast<unsigned char>(sp.tag[k]));
        h.vec3(sp.pos);
    }
    // SECTOR EFFECTS, folded in only when there are some -- the flat styles'
    // rule above, for their reason: a map baked before effects existed, or with
    // none, keeps the hash it had.
    if (fx_count_ > 0) {
        h.u32v(fx_count_);
        for (u32 i = 0; i < fx_count_; ++i) {
            const SectorFx& e = fx_[i];
            h.u32v(e.sector); h.byte(e.kind); h.byte(e.count);
            for (u8 k = 0; k < e.count; ++k) { h.byte(e.params[k].key); h.i64v(e.params[k].raw); }
        }
    }
    return h.value();
}

}  // namespace dnp::geom
