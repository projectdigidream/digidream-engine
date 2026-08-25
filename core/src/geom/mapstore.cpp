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
    if (local_a >= s.point_count || local_b >= s.point_count) return false;

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
    const TriangulateStatus st = triangulate(&points_[s.first_point], s.point_count, tl);
    if (st != TriangulateStatus::Ok) return CommitStatus::TriangulationFailed;

    // Reuse the sector's existing range when it still fits. Triangle count is a
    // pure function of point count for a simple polygon, so a shape edit that
    // keeps the point count -- which is the common case -- never needs new
    // space and never fragments the array.
    const u32 need = static_cast<u32>(tl.count) * 3;
    u32 base;
    if (s.tri_count > 0 && static_cast<u32>(s.tri_count) * 3 >= need) {
        base = s.first_tri;
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
    return m;
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
        h.u32v(s.tag);
        h.u32v(s.tri_count);
        for (u32 t = 0; t < static_cast<u32>(s.tri_count) * 3; ++t) h.u32v(tris_[s.first_tri + t]);
    }
    for (u32 i = 0; i < side_count_; ++i) {
        const Side& sd = sides_[i];
        h.u32v(sd.a); h.u32v(sd.b); h.i32v(sd.neighbor); h.byte(sd.flags);
    }
    h.u32v(spawn_count_);
    for (u32 i = 0; i < spawn_count_; ++i) {
        const SpawnPoint& sp = spawns_[i];
        h.byte(sp.tag_len);
        for (u32 k = 0; k < sp.tag_len; ++k) h.byte(static_cast<unsigned char>(sp.tag[k]));
        h.vec3(sp.pos);
    }
    return h.value();
}

}  // namespace dnp::geom
