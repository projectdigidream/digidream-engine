#include "dnp/geom/mapfile.hpp"

namespace dnp::geom {

bool save_map(const MapStorage& store, ByteWriter& w) noexcept {
    const MapData m = store.view();

    w.u32v(MAP_MAGIC);
    w.u16v(MAP_VERSION);
    w.u16v(0);   // flags, reserved

    w.u32v(m.point_count);
    w.u32v(m.sector_count);
    w.u32v(m.side_count);
    w.u32v(m.tri_index_count);
    w.u32v(store.spawn_count());

    for (u32 i = 0; i < m.point_count; ++i) {
        w.fxraw(m.point(i).x);
        w.fxraw(m.point(i).y);
    }

    for (u32 i = 0; i < m.sector_count; ++i) {
        const Sector& s = m.sector(i);
        w.u16v(s.first_point);
        w.u16v(s.point_count);
        w.u16v(s.first_side);
        w.u16v(s.side_count);
        w.fxraw(s.floor_z);
        w.fxraw(s.ceil_z);
        w.u16v(s.tag);
        w.u32v(s.first_tri);
        w.u16v(s.tri_count);
    }

    for (u32 i = 0; i < m.side_count; ++i) {
        const Side& sd = m.side(i);
        w.u16v(sd.a);
        w.u16v(sd.b);
        w.i32v(sd.neighbor);
        w.u8v(sd.flags);
    }

    for (u32 i = 0; i < m.tri_index_count; ++i) w.u16v(m.tris[i]);

    for (u32 i = 0; i < store.spawn_count(); ++i) {
        const SpawnPoint& sp = store.spawn(i);
        w.u8v(sp.tag_len);
        for (u32 k = 0; k < sp.tag_len; ++k) w.u8v(static_cast<u8>(sp.tag[k]));
        w.fxraw(sp.pos.x);
        w.fxraw(sp.pos.y);
        w.fxraw(sp.pos.z);
    }

    // Trailing content hash. Also what WELCOME's map_hash reports, so a client
    // running different geometry from the server is a detectable condition
    // rather than an unexplained series of collision disagreements.
    w.u64v(store.hash());
    return w.ok();
}

MapLoadStatus load_map(ByteReader& r, MapStorage& store) noexcept {
    store.clear();

    if (r.u32v() != MAP_MAGIC) { store.clear(); return MapLoadStatus::BadMagic; }
    const u16 version = r.u16v();
    const u16 flags = r.u16v();
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
    if (version != MAP_VERSION) { store.clear(); return MapLoadStatus::BadVersion; }

    // Reserved flags MUST be zero. Ignoring them would be the friendlier-looking
    // choice, but a set bit means the writer used a feature this build does not
    // know about -- and loading the file anyway is guessing at its meaning.
    // Refusing also leaves no "don't care" bytes, so no single-bit corruption
    // can slip through unnoticed.
    if (flags != 0) { store.clear(); return MapLoadStatus::BadVersion; }

    const u32 point_count = r.u32v();
    const u32 sector_count = r.u32v();
    const u32 side_count = r.u32v();
    const u32 tri_index_count = r.u32v();
    const u32 spawn_count = r.u32v();
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    // Bound every count BEFORE reading anything sized by it. These are
    // attacker-chosen numbers.
    if (point_count > MAX_MAP_POINTS || sector_count > MAX_MAP_SECTORS ||
        side_count > MAX_MAP_SIDES || tri_index_count > MAX_MAP_TRI_IDX ||
        spawn_count > MAX_MAP_SPAWNS) {
        store.clear();
        return MapLoadStatus::TooLarge;
    }

    // Sectors are rebuilt through add_sector/add_side rather than written into
    // the arrays directly, so a loaded map goes through exactly the same
    // construction path as one built in code -- including triangulation. The
    // baked triangles are then compared against what the triangulator produces,
    // below.
    struct PendingSector {
        u16 first_point, point_count, first_side, side_count, tag, tri_count;
        u32 first_tri;
        fx floor_z, ceil_z;
    };
    PendingSector pend[MAX_MAP_SECTORS];
    fx2 pts[MAX_MAP_POINTS];

    for (u32 i = 0; i < point_count; ++i) {
        pts[i].x = r.fxraw();
        pts[i].y = r.fxraw();
    }
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    for (u32 i = 0; i < sector_count; ++i) {
        PendingSector& s = pend[i];
        s.first_point = r.u16v();
        s.point_count = r.u16v();
        s.first_side = r.u16v();
        s.side_count = r.u16v();
        s.floor_z = r.fxraw();
        s.ceil_z = r.fxraw();
        s.tag = r.u16v();
        s.first_tri = r.u32v();
        s.tri_count = r.u16v();
    }
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    struct PendingSide { u16 a, b; i32 neighbor; u8 flags; };
    PendingSide psides[MAX_MAP_SIDES];
    for (u32 i = 0; i < side_count; ++i) {
        psides[i].a = r.u16v();
        psides[i].b = r.u16v();
        psides[i].neighbor = r.i32v();
        psides[i].flags = r.u8v();
    }
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    // The baked triangles are READ AND CHECKED, not discarded.
    //
    // Recomputing them and ignoring the file's copy would work -- the
    // triangulator is deterministic -- but it would also mean a map baked by a
    // mapc built from different core code loads silently and only diverges once
    // something calls update_shape(). Shipping them and comparing turns that
    // into an immediate, named failure, which is what docs/protocol.md 8.1
    // claims they are for.
    u16 baked[MAX_MAP_TRI_IDX];
    for (u32 i = 0; i < tri_index_count; ++i) baked[i] = r.u16v();
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    // --- validate every index before anything can dereference one ----------
    for (u32 i = 0; i < sector_count; ++i) {
        const PendingSector& s = pend[i];
        if (static_cast<u32>(s.first_point) + s.point_count > point_count) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
        if (static_cast<u32>(s.first_side) + s.side_count > side_count) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
        if (s.point_count < 3 || s.point_count > MAX_POLY_POINTS) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
    }
    for (u32 i = 0; i < side_count; ++i) {
        const PendingSide& sd = psides[i];
        if (sd.a >= point_count || sd.b >= point_count) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
        if (sd.neighbor != NO_SECTOR &&
            (sd.neighbor < 0 || static_cast<u32>(sd.neighbor) >= sector_count)) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
    }

    // --- rebuild ------------------------------------------------------------
    for (u32 i = 0; i < sector_count; ++i) {
        const PendingSector& s = pend[i];
        const u32 idx = store.add_sector(&pts[s.first_point], s.point_count,
                                         s.floor_z, s.ceil_z, s.tag);
        if (idx != i) { store.clear(); return MapLoadStatus::BadIndex; }

        for (u16 k = 0; k < s.side_count; ++k) {
            const PendingSide& sd = psides[s.first_side + k];
            // Sides are stored with absolute point indices; add_side takes
            // sector-local ones.
            const u16 la = static_cast<u16>(sd.a - s.first_point);
            const u16 lb = static_cast<u16>(sd.b - s.first_point);
            if (la >= s.point_count || lb >= s.point_count) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
            if (!store.add_side(i, la, lb, sd.neighbor, sd.flags)) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
        }
    }

    // Compare what this build produced against what the file carries.
    {
        const MapData built = store.view();
        if (built.tri_index_count != tri_index_count) {
            store.clear();
            return MapLoadStatus::TriangleMismatch;
        }
        for (u32 i = 0; i < sector_count; ++i) {
            const Sector& got = built.sector(i);
            if (got.first_tri != pend[i].first_tri || got.tri_count != pend[i].tri_count) {
                store.clear();
                return MapLoadStatus::TriangleMismatch;
            }
        }
        for (u32 i = 0; i < tri_index_count; ++i) {
            if (built.tris[i] != baked[i]) {
                store.clear();
                return MapLoadStatus::TriangleMismatch;
            }
        }
    }

    for (u32 i = 0; i < spawn_count; ++i) {
        const u8 len = r.u8v();
        if (!r.ok() || len > MAX_SPAWN_TAG) { store.clear(); return MapLoadStatus::Truncated; }
        char tag[MAX_SPAWN_TAG];
        for (u32 k = 0; k < len; ++k) tag[k] = static_cast<char>(r.u8v());
        const fx x = r.fxraw();
        const fx y = r.fxraw();
        const fx z = r.fxraw();
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
        store.add_spawn(tag, len, fx3{x, y, z});
    }

    const u64 want_hash = r.u64v();
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    // The rebuilt map is hashed and compared against the file's own hash. This
    // catches corruption, but more usefully it catches a file whose BAKED
    // triangles disagree with what this build's triangulator produces -- which
    // would mean the map compiler and the runtime had drifted apart, and every
    // update_shape() would silently reshape the sector.
    if (store.hash() != want_hash) { store.clear(); return MapLoadStatus::HashMismatch; }

    return MapLoadStatus::Ok;
}

const char* map_load_status_text(MapLoadStatus s) noexcept {
    switch (s) {
        case MapLoadStatus::Ok: return "ok";
        case MapLoadStatus::BadMagic: return "not a DNPM map file";
        case MapLoadStatus::BadVersion: return "unsupported map version";
        case MapLoadStatus::Truncated: return "truncated";
        case MapLoadStatus::TooLarge: return "exceeds a map capacity limit";
        case MapLoadStatus::BadIndex: return "an index is out of range";
        case MapLoadStatus::HashMismatch: return "content hash mismatch";
        case MapLoadStatus::TriangleMismatch:
            return "baked triangles disagree with this build's triangulator";
    }
    return "unknown";
}

}  // namespace dnp::geom
