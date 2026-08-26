#include "dnp/geom/mapfile.hpp"

namespace dnp::geom {

namespace {
// On-disk record sizes, so the loader can seek past a section without
// materialising it.
constexpr size_t POINT_BYTES = 16;   // two fxraw
constexpr size_t SIDE_BYTES  = 9;    // u16 + u16 + i32 + u8
constexpr size_t WAYPOINT_BYTES = 28;   // i32 + three fxraw
constexpr size_t RAIL_BYTES     = 7;    // u16 + u16 + u16 + u8
constexpr size_t LINK_BYTES     = 8;    // four u16
}  // namespace

bool save_map(const MapStorage& store, ByteWriter& w) noexcept {
    const MapData m = store.view();
    const RenderData rd = store.render_view();

    w.u32v(MAP_MAGIC);
    w.u16v(MAP_VERSION);
    w.u16v(0);   // flags, reserved

    w.u32v(m.point_count);
    w.u32v(m.sector_count);
    w.u32v(m.side_count);
    w.u32v(m.tri_index_count);
    w.u32v(store.spawn_count());
    w.u32v(store.rail_count());
    w.u32v(store.waypoint_count());
    w.u32v(store.rail_link_count());
    w.u32v(store.texture_count());

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
        w.fxraw(s.thickness);
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

    // Rails: waypoints hold the position they were AUTHORED at, never a live
    // one. A waypoint riding a moving platform is somewhere different on every
    // tick, and baking that would make the file -- and its hash -- depend on
    // when it was written.
    for (u32 i = 0; i < m.waypoint_count; ++i) {
        const RailWaypoint& p = m.waypoint(i);
        w.i32v(p.anchor);
        w.fxraw(p.base.x);
        w.fxraw(p.base.y);
        w.fxraw(p.base.z);
    }
    for (u32 i = 0; i < m.rail_count; ++i) {
        const Rail& r = m.rail(i);
        w.u16v(r.first_waypoint);
        w.u16v(r.waypoint_count);
        w.u16v(r.tag);
        w.u8v(r.flags);
    }
    for (u32 i = 0; i < m.rail_link_count; ++i) {
        const RailLink& l = m.rail_link(i);
        w.u16v(l.rail);
        w.u16v(l.waypoint);
        w.u16v(l.to_rail);
        w.u16v(l.to_waypoint);
    }

    // Rendering attributes, last: a reader that only wants geometry -- the
    // server's collision code, the nav graph, a diff tool -- can stop at the
    // section it cares about, and the file is laid out so that costs a seek
    // rather than a parse.
    for (u32 i = 0; i < store.texture_count(); ++i) {
        const TextureName& t = store.texture(i);
        w.u8v(t.len);
        for (u32 k = 0; k < t.len; ++k) w.u8v(static_cast<u8>(t.name[k]));
    }
    for (u32 i = 0; i < m.sector_count; ++i) {
        const SectorRender& r = rd.sector(i);
        w.u16v(r.floor_tex);
        w.u16v(r.ceil_tex);
        w.u8v(r.light_level);
    }
    for (u32 i = 0; i < m.side_count; ++i) {
        const SideRender& r = rd.side(i);
        w.u16v(r.upper);
        w.u16v(r.middle);
        w.u16v(r.lower);
        w.u8v(r.peg_flags);
        w.fxraw(r.x_offset);
        w.fxraw(r.y_offset);
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
    const u32 rail_count = r.u32v();
    const u32 waypoint_count = r.u32v();
    const u32 rail_link_count = r.u32v();
    const u32 texture_count = r.u32v();
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    // Bound every count BEFORE reading anything sized by it. These are
    // attacker-chosen numbers.
    if (point_count > MAX_MAP_POINTS || sector_count > MAX_MAP_SECTORS ||
        side_count > MAX_MAP_SIDES || tri_index_count > MAX_MAP_TRI_IDX ||
        spawn_count > MAX_MAP_SPAWNS || rail_count > MAX_MAP_RAILS ||
        waypoint_count > MAX_MAP_WAYPOINTS || rail_link_count > MAX_MAP_RAIL_LINKS ||
        texture_count > MAX_MAP_TEXTURES) {
        store.clear();
        return MapLoadStatus::TooLarge;
    }

    // The arrays are walked by SEEKING through the source buffer rather than
    // copied into scratch first. Buffering them would need ~140 KB of locals --
    // more than the whole wasm stack, and unreasonable anywhere. Each section's
    // offset is recorded once and revisited as needed.
    const size_t points_at = r.pos();
    if (!r.skip(static_cast<size_t>(point_count) * POINT_BYTES)) {
        store.clear(); return MapLoadStatus::Truncated;
    }

    // Sector records are small enough to hold: 512 of them is ~20 KB.
    struct PendingSector {
        u16 first_point, point_count, first_side, side_count, tag, tri_count;
        u32 first_tri;
        fx floor_z, ceil_z, thickness;
    };
    PendingSector pend[MAX_MAP_SECTORS];
    for (u32 i = 0; i < sector_count; ++i) {
        PendingSector& s = pend[i];
        s.first_point = r.u16v();
        s.point_count = r.u16v();
        s.first_side = r.u16v();
        s.side_count = r.u16v();
        s.floor_z = r.fxraw();
        s.ceil_z = r.fxraw();
        s.thickness = r.fxraw();
        s.tag = r.u16v();
        s.first_tri = r.u32v();
        s.tri_count = r.u16v();
    }
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    const size_t sides_at = r.pos();
    if (!r.skip(static_cast<size_t>(side_count) * SIDE_BYTES)) {
        store.clear(); return MapLoadStatus::Truncated;
    }
    const size_t tris_at = r.pos();
    if (!r.skip(static_cast<size_t>(tri_index_count) * 2)) {
        store.clear(); return MapLoadStatus::Truncated;
    }
    const size_t spawns_at = r.pos();

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
    r.seek(sides_at);
    for (u32 i = 0; i < side_count; ++i) {
        const u16 a = r.u16v();
        const u16 b = r.u16v();
        const i32 nb = r.i32v();
        (void)r.u8v();
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
        if (a >= point_count || b >= point_count) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
        if (nb != NO_SECTOR && (nb < 0 || static_cast<u32>(nb) >= sector_count)) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
    }

    // --- rebuild ------------------------------------------------------------
    //
    // Sectors go through add_sector/add_side rather than being written into the
    // arrays directly, so a loaded map takes exactly the same construction path
    // as one built in code -- including triangulation.
    for (u32 i = 0; i < sector_count; ++i) {
        const PendingSector& s = pend[i];

        fx2 poly[MAX_POLY_POINTS];   // at most 64 points: ~1 KB
        r.seek(points_at + static_cast<size_t>(s.first_point) * POINT_BYTES);
        for (u16 k = 0; k < s.point_count; ++k) {
            poly[k].x = r.fxraw();
            poly[k].y = r.fxraw();
        }
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

        const u32 idx = store.add_sector(poly, s.point_count, s.floor_z, s.ceil_z, s.tag);
        if (idx != i) { store.clear(); return MapLoadStatus::BadIndex; }
        // Thickness is not an add_sector parameter: it is meaningful only for a
        // sector with an open edge, and threading it through every construction
        // site would put a zero in most of them.
        store.sector_mut(idx).thickness = s.thickness;

        r.seek(sides_at + static_cast<size_t>(s.first_side) * SIDE_BYTES);
        for (u16 k = 0; k < s.side_count; ++k) {
            const u16 a = r.u16v();
            const u16 b = r.u16v();
            const i32 nb = r.i32v();
            const u8 fl = r.u8v();
            if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
            // Sides are stored with absolute point indices; add_side takes
            // sector-local ones.
            const u16 la = static_cast<u16>(a - s.first_point);
            const u16 lb = static_cast<u16>(b - s.first_point);
            if (la >= s.point_count || lb >= s.point_count) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
            if (!store.add_side(i, la, lb, nb, fl)) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
        }
    }

    // --- the baked triangles are CHECKED, not discarded ---------------------
    //
    // Recomputing them and ignoring the file's copy would work -- the
    // triangulator is deterministic -- but a map baked by a mapc built from
    // different core code would then load silently and only diverge once
    // something called update_shape(). Comparing turns that into an immediate,
    // named failure, which is what docs/protocol.md 8.1 claims they are for.
    {
        const MapData built = store.view();
        if (built.tri_index_count != tri_index_count) {
            store.clear(); return MapLoadStatus::TriangleMismatch;
        }
        for (u32 i = 0; i < sector_count; ++i) {
            const Sector& got = built.sector(i);
            if (got.first_tri != pend[i].first_tri || got.tri_count != pend[i].tri_count) {
                store.clear(); return MapLoadStatus::TriangleMismatch;
            }
        }
        r.seek(tris_at);
        for (u32 i = 0; i < tri_index_count; ++i) {
            if (r.u16v() != built.tris[i]) {
                store.clear(); return MapLoadStatus::TriangleMismatch;
            }
        }
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
    }

    r.seek(spawns_at);
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

    // --- rails ---------------------------------------------------------------
    //
    // Read after the spawns, in the order they were written. Waypoints come
    // first because a rail's range has to be validated against a count that is
    // already known.
    const size_t waypoints_at = r.pos();
    if (!r.skip(static_cast<size_t>(waypoint_count) * WAYPOINT_BYTES)) {
        store.clear(); return MapLoadStatus::Truncated;
    }
    const size_t rails_at = r.pos();
    if (!r.skip(static_cast<size_t>(rail_count) * RAIL_BYTES)) {
        store.clear(); return MapLoadStatus::Truncated;
    }
    const size_t links_at = r.pos();
    if (!r.skip(static_cast<size_t>(rail_link_count) * LINK_BYTES)) {
        store.clear(); return MapLoadStatus::Truncated;
    }

    {
        // Every rail's waypoint range, validated before a single one is added.
        struct PendingRail { u16 first, count, tag; u8 flags; };
        PendingRail rails[MAX_MAP_RAILS];
        r.seek(rails_at);
        u32 covered = 0;
        for (u32 i = 0; i < rail_count; ++i) {
            rails[i].first = r.u16v();
            rails[i].count = r.u16v();
            rails[i].tag = r.u16v();
            rails[i].flags = r.u8v();
            if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
            if (static_cast<u32>(rails[i].first) + rails[i].count > waypoint_count) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
            // Ranges are contiguous and in order, because that is the only way
            // add_waypoint() can rebuild them. A file claiming otherwise was
            // not written by this format's writer.
            if (rails[i].first != covered) { store.clear(); return MapLoadStatus::BadIndex; }
            covered += rails[i].count;
        }
        if (covered != waypoint_count) { store.clear(); return MapLoadStatus::BadIndex; }

        r.seek(waypoints_at);
        u32 next_wp = 0;
        for (u32 i = 0; i < rail_count; ++i) {
            const u32 idx = store.add_rail(rails[i].tag, rails[i].flags);
            if (idx != i) { store.clear(); return MapLoadStatus::BadIndex; }
            for (u16 k = 0; k < rails[i].count; ++k) {
                const i32 anchor = r.i32v();
                const fx x = r.fxraw();
                const fx y = r.fxraw();
                const fx z = r.fxraw();
                if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
                if (anchor != NO_SECTOR &&
                    (anchor < 0 || static_cast<u32>(anchor) >= sector_count)) {
                    store.clear(); return MapLoadStatus::BadIndex;
                }
                if (!store.add_waypoint(i, fx3{x, y, z}, anchor)) {
                    store.clear(); return MapLoadStatus::BadIndex;
                }
                ++next_wp;
            }
        }
        if (next_wp != waypoint_count) { store.clear(); return MapLoadStatus::BadIndex; }

        r.seek(links_at);
        for (u32 i = 0; i < rail_link_count; ++i) {
            const u16 from_rail = r.u16v();
            const u16 from_wp = r.u16v();
            const u16 to_rail = r.u16v();
            const u16 to_wp = r.u16v();
            if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
            // add_rail_link validates every index against the rails it now
            // holds, so a junction naming a waypoint that does not exist is a
            // rejected file rather than an out-of-range read at the first fork.
            if (!store.add_rail_link(from_rail, from_wp, to_rail, to_wp)) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
        }
    }

    // --- rendering attributes ------------------------------------------------
    //
    // Read last, and read with the same suspicion as everything else: a texture
    // id is an index like any other, and a side naming texture 9000 must be a
    // rejected file rather than an out-of-range read the first time that wall
    // comes on screen. Rendering data is not simulation data, but it is still
    // attacker-supplied.
    for (u32 i = 0; i < texture_count; ++i) {
        const u8 len = r.u8v();
        if (!r.ok() || len == 0 || len > MAX_TEXTURE_NAME) {
            store.clear(); return MapLoadStatus::Truncated;
        }
        char name[MAX_TEXTURE_NAME];
        for (u32 k = 0; k < len; ++k) name[k] = static_cast<char>(r.u8v());
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
        // Interning must reproduce the file's own ids exactly. If two entries
        // in the table are identical the intern collapses them, the ids shift,
        // and every side below this point silently refers to the wrong texture
        // -- so a file with duplicate names is malformed, not merely wasteful.
        if (store.intern_texture(name, len) != i) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
    }

    const auto texture_ok = [&](u16 t) {
        return t == NO_TEXTURE || t < texture_count;
    };

    for (u32 i = 0; i < sector_count; ++i) {
        const u16 floor_tex = r.u16v();
        const u16 ceil_tex = r.u16v();
        const u8 light = r.u8v();
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
        if (!texture_ok(floor_tex) || !texture_ok(ceil_tex)) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
        SectorRender& sr = store.sector_render_mut(i);
        sr.floor_tex = floor_tex;
        sr.ceil_tex = ceil_tex;
        sr.light_level = light;
    }

    for (u32 i = 0; i < side_count; ++i) {
        const u16 upper = r.u16v();
        const u16 middle = r.u16v();
        const u16 lower = r.u16v();
        const u8 peg = r.u8v();
        const fx xo = r.fxraw();
        const fx yo = r.fxraw();
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
        if (!texture_ok(upper) || !texture_ok(middle) || !texture_ok(lower)) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
        SideRender& sr = store.side_render_mut(i);
        sr.upper = upper;
        sr.middle = middle;
        sr.lower = lower;
        sr.peg_flags = peg;
        sr.x_offset = xo;
        sr.y_offset = yo;
    }

    const u64 want_hash = r.u64v();
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    // Finally, the rebuilt map is hashed and compared against the file's own.
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
