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
constexpr size_t PATH_POINT_BYTES = 28; // i32 + three fxraw, as a waypoint
constexpr size_t PATH_BYTES       = 15; // u16 + u16 + u16 + u8 + fxraw
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
    w.u32v(store.path_count());
    w.u32v(store.path_point_count());
    w.u32v(store.texture_count());
    w.u32v(store.light_count());
    w.u32v(store.script_count());

    // The body this map is built for. Two more fields in a fixed header rather
    // than an optional trailing block: a map that means the engine's defaults
    // says so explicitly, which is the difference between "this map wants a
    // 2.0 body" and "this map is old".
    w.fxraw(store.body_radius());
    w.fxraw(store.body_height());
    w.fxraw(store.texture_scale());

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
        w.u16v(s.hole_count);
        for (u16 h = 0; h < s.hole_count; ++h) {
            const PolyLoop& L = m.hole(s.first_hole + h);
            w.u16v(L.first);
            w.u16v(L.count);
        }
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

    // Camera paths: points first, then the paths that index them, so the loader
    // can validate every range against a count it already has. Same order and
    // same authored-not-live rule as the rails above.
    for (u32 i = 0; i < m.path_point_count; ++i) {
        const PathPoint& p = m.path_point(i);
        w.i32v(p.anchor);
        w.fxraw(p.base.x);
        w.fxraw(p.base.y);
        w.fxraw(p.base.z);
    }
    for (u32 i = 0; i < m.path_count; ++i) {
        const Path& p = m.path(i);
        w.u16v(p.first_point);
        w.u16v(p.point_count);
        w.u16v(p.tag);
        w.u8v(p.flags);
        w.fxraw(p.curve_length);
    }

    // Rendering attributes, last: a reader that only wants geometry -- the
    // server's collision code, the nav graph, a diff tool -- can stop at the
    // section it cares about, and the file is laid out so that costs a seek
    // rather than a parse.
    for (u32 i = 0; i < store.light_count(); ++i) {
        const MapLight& l = store.light(i);
        w.fxraw(l.pos.x); w.fxraw(l.pos.y); w.fxraw(l.pos.z);
        w.u8v(l.r); w.u8v(l.g); w.u8v(l.b);
        w.fxraw(l.radius);
        w.fxraw(l.intensity);
    }
    for (u32 i = 0; i < store.texture_count(); ++i) {
        const TextureName& t = store.texture(i);
        w.u8v(t.len);
        for (u32 k = 0; k < t.len; ++k) w.u8v(static_cast<u8>(t.name[k]));
    }
    for (u32 i = 0; i < store.script_count(); ++i) {
        const ScriptName& sc = store.script(i);
        w.u8v(sc.len);
        for (u32 k = 0; k < sc.len; ++k) w.u8v(static_cast<u8>(sc.name[k]));
    }
    // One flat's style. Written for both flats of every sector even when it is
    // the default, because a fixed-width record is what lets the loader read
    // this section without a per-sector length.
    const auto write_style = [&](const SurfaceStyle& st) {
        w.u32v(st.angle);
        w.fxraw(st.off_x);
        w.fxraw(st.off_y);
        w.fxraw(st.scale);
        w.u8v(st.alpha);
        w.u8v(st.anchor);
    };
    for (u32 i = 0; i < m.sector_count; ++i) {
        const SectorRender& r = rd.sector(i);
        w.u16v(r.floor_tex);
        w.u16v(r.ceil_tex);
        w.u8v(r.light_level);
        write_style(r.floor);
        write_style(r.ceil);
    }
    for (u32 i = 0; i < m.side_count; ++i) {
        const SideRender& r = rd.side(i);
        w.u16v(r.upper);
        w.u16v(r.middle);
        w.u16v(r.lower);
        w.u8v(r.peg_flags);
        w.fxraw(r.x_offset);
        w.fxraw(r.y_offset);
        w.u8v(r.alpha);
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
    if (version < MAP_MIN_VERSION || version > MAP_VERSION) {
        store.clear(); return MapLoadStatus::BadVersion;
    }

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
    const u32 path_count = r.u32v();
    const u32 path_point_count = r.u32v();
    const u32 texture_count = r.u32v();
    const u32 light_count = r.u32v();
    const u32 script_count = r.u32v();
    const fx body_radius = r.fxraw();
    const fx body_height = r.fxraw();
    const fx texture_scale = r.fxraw();
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    // Structural, like the counts below: a body with no size is not a body, and
    // an entity with a zero height stands in every ceiling at once. Bounded
    // above too -- these are attacker-chosen numbers, and a body larger than
    // the map is a way to make collision do arbitrary work.
    if (body_radius <= fx::zero() || body_height <= fx::zero() ||
        body_radius > fx::from_int(64) || body_height > fx::from_int(64)) {
        store.clear();
        return MapLoadStatus::BadIndex;
    }
    // Same reasoning: an attacker-chosen number that divides a UV. Zero would
    // make every texture infinitely large, and the upper bound is generous --
    // Doom's own is 48 and a 4K wall texture on a one-unit wall is 4096.
    if (texture_scale <= fx::zero() || texture_scale > fx::from_int(8192)) {
        store.clear();
        return MapLoadStatus::BadIndex;
    }

    // Bound every count BEFORE reading anything sized by it. These are
    // attacker-chosen numbers.
    if (point_count > MAX_MAP_POINTS || sector_count > MAX_MAP_SECTORS ||
        side_count > MAX_MAP_SIDES || tri_index_count > MAX_MAP_TRI_IDX ||
        spawn_count > MAX_MAP_SPAWNS || rail_count > MAX_MAP_RAILS ||
        waypoint_count > MAX_MAP_WAYPOINTS || rail_link_count > MAX_MAP_RAIL_LINKS ||
        path_count > MAX_MAP_PATHS || path_point_count > MAX_MAP_PATH_POINTS ||
        texture_count > MAX_MAP_TEXTURES || light_count > MAX_MAP_LIGHTS ||
        script_count > MAX_MAP_SCRIPTS) {
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
        u16 hole_count;
        // The FILE OFFSET of this sector's hole table, not the table itself.
        //
        // pend[] is a local array of MAX_MAP_SECTORS entries, so every byte
        // here is multiplied by 512. Inlining 64 hole ranges would make it
        // 130 KB of stack -- twice the whole wasm stack -- to hold data the
        // rebuild pass re-reads by seeking anyway.
        size_t holes_at;
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
        s.hole_count = r.u16v();
        if (!r.ok() || s.hole_count > MAX_POLY_HOLES) {
            store.clear(); return MapLoadStatus::TooLarge;
        }
        s.holes_at = r.pos();
        if (!r.skip(static_cast<size_t>(s.hole_count) * 4)) {
            store.clear(); return MapLoadStatus::Truncated;
        }
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
        // A hole's range is relative to its sector's first point and must lie
        // beyond the outer loop and inside the map. An out-of-range hole would
        // otherwise be triangulated against whatever points followed it.
        r.seek(s.holes_at);
        for (u16 h = 0; h < s.hole_count; ++h) {
            const u16 hf = r.u16v();
            const u16 hl = r.u16v();
            if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
            if (hl < 3 || hl > MAX_POLY_POINTS) { store.clear(); return MapLoadStatus::BadIndex; }
            if (hf < s.point_count) { store.clear(); return MapLoadStatus::BadIndex; }
            if (static_cast<u32>(s.first_point) + hf + hl > point_count) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
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

        // Holes go in immediately, before the next sector: their points must be
        // contiguous with their sector's.
        for (u16 h = 0; h < s.hole_count; ++h) {
            r.seek(s.holes_at + static_cast<size_t>(h) * 4);
            const u16 hf = r.u16v();
            const u16 hl = r.u16v();
            if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

            fx2 hpoly[MAX_POLY_POINTS];
            r.seek(points_at + static_cast<size_t>(s.first_point + hf) * POINT_BYTES);
            for (u16 k = 0; k < hl; ++k) {
                hpoly[k].x = r.fxraw();
                hpoly[k].y = r.fxraw();
            }
            if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
            if (!store.add_hole(idx, hpoly, hl)) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
        }

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
            // Bounded by the sector's WHOLE point span, holes included. A
            // hole's rim has sides too -- they are what a body walking into a
            // pit collides with -- and their local indices continue past the
            // outer loop. Checking only the outer loop rejected every map with
            // a hole in it, as BadIndex, from the loader rather than the
            // compiler that had just written it.
            u32 span = s.point_count;
            for (u16 h = 0; h < s.hole_count; ++h) {
                span += store.view().sector(i).hole_count > h
                    ? store.view().hole(store.view().sector(i).first_hole + h).count : 0;
            }
            if (la >= span || lb >= span) {
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

    // --- camera paths --------------------------------------------------------
    //
    // Validated exactly as the rails above are, and on separate arrays, so a
    // malformed path can never reach the ranges the simulation reads.
    const size_t path_points_at = r.pos();
    if (!r.skip(static_cast<size_t>(path_point_count) * PATH_POINT_BYTES)) {
        store.clear(); return MapLoadStatus::Truncated;
    }
    const size_t paths_at = r.pos();
    if (!r.skip(static_cast<size_t>(path_count) * PATH_BYTES)) {
        store.clear(); return MapLoadStatus::Truncated;
    }
    // Where the next section begins. The rail block above gets away without
    // this because links are its last subsection; here the last thing read is
    // the points, which sit BEFORE the paths, so the position has to be put
    // back deliberately rather than left wherever the reads ended.
    const size_t after_paths = r.pos();

    {
        struct PendingPath { u16 first, count, tag; u8 flags; fx length; };
        PendingPath paths[MAX_MAP_PATHS];
        r.seek(paths_at);
        u32 covered = 0;
        for (u32 i = 0; i < path_count; ++i) {
            paths[i].first = r.u16v();
            paths[i].count = r.u16v();
            paths[i].tag = r.u16v();
            paths[i].flags = r.u8v();
            paths[i].length = r.fxraw();
            if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
            if (static_cast<u32>(paths[i].first) + paths[i].count > path_point_count) {
                store.clear(); return MapLoadStatus::BadIndex;
            }
            if (paths[i].first != covered) { store.clear(); return MapLoadStatus::BadIndex; }
            covered += paths[i].count;
            // A negative arc length is not a path anything can ride: it divides
            // the wrong way in every duration a script computes from it. Zero is
            // allowed -- a path whose points coincide is degenerate, not hostile.
            if (paths[i].length < fx::zero()) { store.clear(); return MapLoadStatus::BadIndex; }
        }
        if (covered != path_point_count) { store.clear(); return MapLoadStatus::BadIndex; }

        r.seek(path_points_at);
        u32 next_pp = 0;
        for (u32 i = 0; i < path_count; ++i) {
            const u32 idx = store.add_path(paths[i].tag, paths[i].flags, paths[i].length);
            if (idx != i) { store.clear(); return MapLoadStatus::BadIndex; }
            for (u16 k = 0; k < paths[i].count; ++k) {
                const i32 anchor = r.i32v();
                const fx x = r.fxraw();
                const fx y = r.fxraw();
                const fx z = r.fxraw();
                if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
                if (anchor != NO_SECTOR &&
                    (anchor < 0 || static_cast<u32>(anchor) >= sector_count)) {
                    store.clear(); return MapLoadStatus::BadIndex;
                }
                if (!store.add_path_point(i, fx3{x, y, z}, anchor)) {
                    store.clear(); return MapLoadStatus::BadIndex;
                }
                ++next_pp;
            }
        }
        if (next_pp != path_point_count) { store.clear(); return MapLoadStatus::BadIndex; }
    }
    r.seek(after_paths);

    // --- rendering attributes ------------------------------------------------
    //
    // Read last, and read with the same suspicion as everything else: a texture
    // id is an index like any other, and a side naming texture 9000 must be a
    // rejected file rather than an out-of-range read the first time that wall
    // comes on screen. Rendering data is not simulation data, but it is still
    // attacker-supplied.
    for (u32 i = 0; i < light_count; ++i) {
        MapLight l;
        l.pos = fx3{r.fxraw(), r.fxraw(), r.fxraw()};
        l.r = r.u8v(); l.g = r.u8v(); l.b = r.u8v();
        l.radius = r.fxraw();
        l.intensity = r.fxraw();
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
        if (!store.add_light(l)) { store.clear(); return MapLoadStatus::TooLarge; }
    }

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

    for (u32 i = 0; i < script_count; ++i) {
        const u8 len = r.u8v();
        if (!r.ok() || len == 0 || len > MAX_SCRIPT_NAME) {
            store.clear(); return MapLoadStatus::Truncated;
        }
        char name[MAX_SCRIPT_NAME];
        for (u32 k = 0; k < len; ++k) name[k] = static_cast<char>(r.u8v());
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
        // Re-validated here and not merely trusted from the bake. The server
        // appends these to the map's own directory, so a name that could climb
        // out of it would let a FILE choose what the host runs -- and the file
        // on disk is not necessarily the one mapc wrote. add_script refuses an
        // unsafe name; refusing the whole map is the only honest response,
        // because a map missing its actions is a building with no doors.
        if (!store.add_script(name, len)) { store.clear(); return MapLoadStatus::BadIndex; }
    }

    const auto texture_ok = [&](u16 t) {
        return t == NO_TEXTURE || t < texture_count;
    };

    // A file older than the styles leaves them at their defaults, which is
    // exactly how it was always drawn. See MAP_MIN_VERSION.
    const bool has_styles = version >= 12;
    const bool has_anchor = version >= 13;
    const auto read_style = [&](SurfaceStyle& st) {
        if (!has_styles) return true;
        st.angle = r.u32v();
        st.off_x = r.fxraw();
        st.off_y = r.fxraw();
        st.scale = r.fxraw();
        st.alpha = r.u8v();
        if (has_anchor) st.anchor = r.u8v();
        // A scale of zero or less is a division, and the file on disk is not
        // necessarily the one mapc wrote. REFUSED rather than repaired: a
        // repair would silently turn a flipped bit back into the default, and
        // the default is the one value this hash does not fold in -- so
        // clamping here was a genuine don't-care byte, and the format's own
        // bit-flip test caught it within a minute of being written.
        return st.scale.raw > 0;
    };

    for (u32 i = 0; i < sector_count; ++i) {
        const u16 floor_tex = r.u16v();
        const u16 ceil_tex = r.u16v();
        const u8 light = r.u8v();
        SectorRender& sr = store.sector_render_mut(i);
        sr.floor = SurfaceStyle{};
        sr.ceil = SurfaceStyle{};
        const bool style_ok = read_style(sr.floor) && read_style(sr.ceil);
        if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }
        if (!style_ok) { store.clear(); return MapLoadStatus::BadIndex; }
        if (!texture_ok(floor_tex) || !texture_ok(ceil_tex)) {
            store.clear(); return MapLoadStatus::BadIndex;
        }
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
        const u8 alpha = has_styles ? r.u8v() : 255;
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
        sr.alpha = alpha;
    }

    const u64 want_hash = r.u64v();
    if (!r.ok()) { store.clear(); return MapLoadStatus::Truncated; }

    // BEFORE the hash, because the body is part of the hashed content: applying
    // it afterwards hashes a map with the engine's defaults against a file
    // written with the map's own, and every converted map is rejected as
    // corrupt.
    store.set_body(body_radius, body_height);
    store.set_texture_scale(texture_scale);

    // Finally, the rebuilt map is hashed and compared against the file's own.
    if (store.hash() != want_hash) { store.clear(); return MapLoadStatus::HashMismatch; }

    // The broadphase is derived from the geometry, so it is REBUILT rather than
    // stored: two peers that parsed the same file build the same grid, and
    // there is no third thing in the file that could disagree with the first
    // two. It is also not in the hash, for the same reason -- it is not data.
    store.rebuild_grid();

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
