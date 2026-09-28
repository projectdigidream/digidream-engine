//
// Baked map format tests.
//
// The loader parses bytes that arrive over the control channel, which means it
// parses bytes from whoever is on the other end. So round-tripping is only half
// the job: the other half is that every malformed, truncated or index-corrupt
// input is REJECTED rather than loaded into a state that reads out of bounds at
// the first collision query.
//
#include <cstdio>

#include "dnp/geom/mapfile.hpp"
#include "dnp/physics/collide.hpp"
#include "dnp/math/rng.hpp"
#include "dnp/sim/testmap.hpp"

using namespace dnp;
using namespace dnp::geom;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static fx F(i64 n) { return fx::from_int(n); }

// Builds the same three-sector layout the hardcoded test map describes.
static void build_reference(MapStorage& s) {
    s.clear();
    const fx2 a[] = {{F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)}};
    const fx2 b[] = {{F(40), F(0)}, {F(60), F(0)}, {F(60), F(40)}, {F(40), F(40)}};
    const fx2 c[] = {{F(60), F(0)}, {F(80), F(0)}, {F(80), F(40)}, {F(60), F(40)}};

    s.add_sector(a, 4, F(0), F(40), 1);
    s.add_sector(b, 4, fx::from_frac(1, 2), F(40), 2);
    s.add_sector(c, 4, F(4), F(40), 3);

    s.add_side(0, 0, 1, NO_SECTOR, 0);
    s.add_side(0, 1, 2, 1, 0);
    s.add_side(0, 2, 3, NO_SECTOR, 0);
    s.add_side(0, 3, 0, NO_SECTOR, side_flag::CLIMBABLE);

    s.add_side(1, 0, 1, NO_SECTOR, 0);
    s.add_side(1, 1, 2, 2, 0);
    s.add_side(1, 2, 3, NO_SECTOR, 0);
    s.add_side(1, 3, 0, 0, 0);

    s.add_side(2, 0, 1, NO_SECTOR, 0);
    s.add_side(2, 1, 2, NO_SECTOR, side_flag::GRAPPLE);
    s.add_side(2, 2, 3, NO_SECTOR, 0);
    s.add_side(2, 3, 0, 1, 0);

    s.add_spawn("start", 5, fx3{F(10), F(10), F(2)});
    s.add_spawn("ledge", 5, fx3{F(50), F(20), F(2)});

    // Rendering attributes, so the round-trip actually carries some. A fixture
    // that leaves them all at their defaults would round-trip a file full of
    // zeroes and prove nothing about the section.
    const u16 wall = s.intern_texture("WALL1", 5);
    const u16 floor = s.intern_texture("FLOOR1", 6);
    const u16 step = s.intern_texture("STEP1", 5);
    s.sector_render_mut(0).floor_tex = floor;
    s.sector_render_mut(0).ceil_tex = wall;
    s.sector_render_mut(0).light_level = 190;
    s.sector_render_mut(1).light_level = 220;
    s.sector_render_mut(2).floor_tex = step;
    // A floating slab: thickness is geometry, so it round-trips with the
    // sector rather than with its textures.
    s.sector_mut(1).thickness = fx::from_frac(5, 2);
    s.side_render_mut(0).middle = wall;
    s.side_render_mut(1).lower = step;
    s.side_render_mut(1).peg_flags = peg_flag::LOWER_UNPEGGED;
    s.side_render_mut(3).middle = wall;
    s.side_render_mut(3).x_offset = fx::from_frac(3, 2);

    // HOW THE FLATS ARE PAINTED, on the fixture every other test in this file
    // shares -- so the round trip, the physics-separation check and the
    // bit-flip sweep all cover them without a fourth fixture to keep in step.
    // Deliberately all four attributes on one flat and a different one on
    // another: a writer that emitted the floor's style twice would round-trip
    // perfectly against a fixture that painted only floors.
    s.sector_render_mut(0).floor.angle = 0x20000000u;         // an eighth turn
    s.sector_render_mut(0).floor.off_x = fx::from_frac(5, 4);
    s.sector_render_mut(0).floor.off_y = fx::from_int(-3);
    s.sector_render_mut(0).floor.scale = fx::from_frac(5, 2);
    s.sector_render_mut(0).floor.alpha = 128;
    s.sector_render_mut(1).ceil.angle = 0xC0000000u;          // three eighths
    s.sector_render_mut(2).ceil.scale = fx::from_frac(1, 4);
    s.side_render_mut(2).alpha = 200;
    s.side_render_mut(3).y_offset = fx::from_int(-4);
    s.side_render_mut(9).upper = step;
    s.side_render_mut(9).peg_flags = peg_flag::UPPER_UNPEGGED;
}

// A MAP WRITTEN BEFORE FLAT STYLES EXISTED STILL LOADS, AND IS THE SAME MAP.
//
// The format has never had a version window before: every earlier bump changed
// what a map MEANS, so every earlier bump rewrote every map. This one did not.
// Everything version 12 added is a rendering attribute whose absence has one
// correct reading, so a version 11 file is a version 12 file with every style
// at its default -- and its CONTENT HASH is unchanged, which is the load-
// bearing half. modules/doom2/assets/music/index.json is keyed by that hash,
// and a re-bake of all thirty-two levels to gain a floor rotation nobody used
// would silently unmap thirty-two soundtracks.
//
// Built by DOWNGRADING a file this writer produced rather than by checking in
// an old one: a fixture recorded once drifts, and this way the two layouts are
// compared as they are actually written today.
static void test_a_map_written_before_styles_still_loads() {
    std::printf("mapfile: a map from before flat styles is the same map\n");

    // STATIC, both of them. A MapStorage is a fixed-size arena measured in
    // megabytes, and four of them plus two file buffers overflowed the stack
    // under the sanitizer -- which is the whole reason the other fixtures in
    // this file are static too.
    static MapStorage src;
    src.clear();
    build_reference(src);
    // Defaults everywhere, because defaults are all a version 11 file can say.
    for (u32 i = 0; i < src.view().sector_count; ++i) {
        src.sector_render_mut(i).floor = SurfaceStyle{};
        src.sector_render_mut(i).ceil = SurfaceStyle{};
    }
    for (u32 i = 0; i < src.view().side_count; ++i) src.side_render_mut(i).alpha = 255;

    static u8 buf[8192];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));
    const size_t n = w.size();

    // The two render sections are the last thing before the trailing hash, so
    // the bytes ahead of them are byte-identical between the two versions.
    constexpr size_t SECTOR_12 = 5 + 2 * 29, SECTOR_11 = 5;
    constexpr size_t SIDE_12 = 24, SIDE_11 = 23;
    const u32 sectors = src.view().sector_count;
    const u32 sides = src.view().side_count;
    const size_t tail = sectors * SECTOR_12 + sides * SIDE_12 + 8;
    CHECK(n > tail);
    const size_t head = n - tail;

    static u8 old[8192];
    size_t m = 0;
    for (size_t i = 0; i < head; ++i) old[m++] = buf[i];
    old[4] = 11; old[5] = 0;
    size_t at = head;
    for (u32 i = 0; i < sectors; ++i, at += SECTOR_12) {
        for (size_t k = 0; k < SECTOR_11; ++k) old[m++] = buf[at + k];
    }
    for (u32 i = 0; i < sides; ++i, at += SIDE_12) {
        for (size_t k = 0; k < SIDE_11; ++k) old[m++] = buf[at + k];
    }
    for (size_t k = 0; k < 8; ++k) old[m++] = buf[at + k];   // the hash

    static MapStorage dst;
    ByteReader r(old, m);
    const MapLoadStatus st = load_map(r, dst);
    CHECK(st == MapLoadStatus::Ok);
    // THE HASH IS THE CLAIM. load_map already compares it against the one the
    // file carries, so reaching Ok says the recomputed hash matched -- and this
    // says it matched the one the NEW writer produces for the same map.
    CHECK(dst.hash() == src.hash());
    for (u32 i = 0; i < dst.view().sector_count; ++i) {
        CHECK(dst.render_view().sector(i).floor.is_default());
        CHECK(dst.render_view().sector(i).ceil.is_default());
    }
    for (u32 i = 0; i < dst.view().side_count; ++i) {
        CHECK(dst.render_view().side(i).alpha == 255);
    }

    // CONTROL: the window has a bottom. A file one version older than the
    // oldest supported is refused, or "we accept 11 and 12" is really "we
    // accept anything" and the version field means nothing.
    // `dst` again rather than a third arena: load_map clears its destination
    // before it reads a byte, so a refused load leaves nothing behind.
    old[4] = 10;
    ByteReader r10(old, m);
    CHECK(load_map(r10, dst) == MapLoadStatus::BadVersion);

    // And so is one newer than this writer knows how to produce.
    old[4] = 13;
    ByteReader r13(old, m);
    CHECK(load_map(r13, dst) == MapLoadStatus::BadVersion);
}

static void test_roundtrip() {
    std::printf("mapfile: save/load round-trips exactly\n");

    MapStorage src;
    build_reference(src);

    u8 buf[8192];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));
    CHECK(w.size() > 0);

    MapStorage dst;
    ByteReader r(buf, w.size());
    const MapLoadStatus st = load_map(r, dst);
    if (st != MapLoadStatus::Ok) {
        std::printf("  load failed: %s\n", map_load_status_text(st));
    }
    CHECK(st == MapLoadStatus::Ok);

    // The hash covers points, sectors, sides, triangles and spawns, so equality
    // here is a strong statement -- and it is the same value WELCOME reports as
    // map_hash, which is what makes a client running different geometry a
    // detectable condition rather than an unexplained series of disagreements.
    CHECK(dst.hash() == src.hash());

    const MapData a = src.view();
    const MapData b = dst.view();
    CHECK(a.point_count == b.point_count);
    CHECK(a.sector_count == b.sector_count);
    CHECK(a.side_count == b.side_count);
    CHECK(a.tri_index_count == b.tri_index_count);
    for (u32 i = 0; i < a.point_count; ++i) CHECK(a.point(i) == b.point(i));
    for (u32 i = 0; i < a.side_count; ++i) {
        CHECK(a.side(i).a == b.side(i).a);
        CHECK(a.side(i).b == b.side(i).b);
        CHECK(a.side(i).neighbor == b.side(i).neighbor);
        CHECK(a.side(i).flags == b.side(i).flags);
    }
    for (u32 i = 0; i < a.sector_count; ++i) {
        CHECK(a.sector(i).floor_z == b.sector(i).floor_z);
        CHECK(a.sector(i).ceil_z == b.sector(i).ceil_z);
        CHECK(a.sector(i).tag == b.sector(i).tag);
        CHECK(a.sector(i).tri_count == b.sector(i).tri_count);
        CHECK(a.sector(i).thickness == b.sector(i).thickness);
        CHECK(a.sector(i).hole_count == b.sector(i).hole_count);
    }
    CHECK(dst.spawn_count() == 2);

    // The render section, field by field. Interning must reproduce the same
    // ids, or every side below the table would silently point at a different
    // texture -- which is exactly the failure a hash comparison alone would
    // catch but could not name.
    CHECK(dst.texture_count() == src.texture_count());
    for (u32 i = 0; i < dst.texture_count(); ++i) {
        CHECK(dst.texture(i).len == src.texture(i).len);
        for (u32 k = 0; k < dst.texture(i).len; ++k) {
            CHECK(dst.texture(i).name[k] == src.texture(i).name[k]);
        }
    }
    const RenderData ra = src.render_view();
    const RenderData rb = dst.render_view();
    CHECK(ra.sector_count == rb.sector_count);
    CHECK(ra.side_count == rb.side_count);
    const auto same_style = [&](const SurfaceStyle& x, const SurfaceStyle& y) {
        CHECK(x.angle == y.angle);
        CHECK(x.off_x == y.off_x);
        CHECK(x.off_y == y.off_y);
        CHECK(x.scale == y.scale);
        CHECK(x.alpha == y.alpha);
    };
    for (u32 i = 0; i < ra.sector_count && i < rb.sector_count; ++i) {
        CHECK(ra.sector(i).floor_tex == rb.sector(i).floor_tex);
        CHECK(ra.sector(i).ceil_tex == rb.sector(i).ceil_tex);
        CHECK(ra.sector(i).light_level == rb.sector(i).light_level);
        same_style(ra.sector(i).floor, rb.sector(i).floor);
        same_style(ra.sector(i).ceil, rb.sector(i).ceil);
    }
    for (u32 i = 0; i < ra.side_count && i < rb.side_count; ++i) {
        CHECK(ra.side(i).upper == rb.side(i).upper);
        CHECK(ra.side(i).middle == rb.side(i).middle);
        CHECK(ra.side(i).lower == rb.side(i).lower);
        CHECK(ra.side(i).peg_flags == rb.side(i).peg_flags);
        CHECK(ra.side(i).x_offset == rb.side(i).x_offset);
        CHECK(ra.side(i).y_offset == rb.side(i).y_offset);
        CHECK(ra.side(i).alpha == rb.side(i).alpha);
    }
}

// Rendering data must not be able to change what the simulation does.
//
// The structural guarantee is that MapData does not contain it, so physics
// cannot read it even by mistake. This asserts the consequence: two maps
// identical except for every rendering attribute must collide identically.
// If this ever fails, something has reached across the separation.
static void test_render_does_not_affect_physics() {
    MapStorage plain;
    build_reference(plain);
    // Strip every rendering attribute back to its default.
    for (u32 i = 0; i < plain.view().sector_count; ++i) plain.sector_render_mut(i) = SectorRender{};
    for (u32 i = 0; i < plain.view().side_count; ++i) plain.side_render_mut(i) = SideRender{};

    MapStorage painted;
    build_reference(painted);

    const MapData a = plain.view();
    const MapData b = painted.view();

    // Walk a grid of positions across the whole map and compare the two answers
    // the simulation actually asks of geometry.
    int probes = 0;
    for (int x = -4; x <= 84; x += 2) {
        for (int y = -4; y <= 44; y += 2) {
            const fx2 p{F(x), F(y)};
            CHECK(find_sector(a, p) == find_sector(b, p));
            ++probes;
        }
    }
    CHECK(probes > 1000);

    for (u32 s = 0; s < a.sector_count; ++s) {
        const Sector& sec = a.sector(s);
        for (u16 k = 0; k < sec.side_count; ++k) {
            const u32 si = sec.first_side + k;
            for (int fz = 0; fz <= 6; ++fz) {
                CHECK(physics::side_is_passable(a, si, s, F(fz), F(2)) ==
                      physics::side_is_passable(b, si, s, F(fz), F(2)));
            }
        }
    }

    // And the two maps really ARE different, or every comparison above is
    // vacuous -- it would pass just as well if build_reference had painted
    // nothing at all.
    CHECK(plain.hash() != painted.hash());
    std::printf("mapfile: rendering attributes change nothing the simulation reads\n");
}

static void test_matches_hardcoded_map() {
    // The hardcoded test map is a temporary bridge until MAP_DATA transfer
    // lands. While both exist they must describe the same world, or the server
    // and client would be colliding against different geometry the moment one
    // switched over.
    std::printf("mapfile: the built map matches the hardcoded one\n");

    MapStorage built;
    build_reference(built);

    const MapData a = built.view();
    const MapData b = dnp::sim::test_map();

    CHECK(a.point_count == b.point_count);
    CHECK(a.sector_count == b.sector_count);
    CHECK(a.side_count == b.side_count);
    for (u32 i = 0; i < b.point_count && i < a.point_count; ++i) CHECK(a.point(i) == b.point(i));
    for (u32 i = 0; i < b.sector_count && i < a.sector_count; ++i) {
        CHECK(a.sector(i).floor_z == b.sector(i).floor_z);
        CHECK(a.sector(i).ceil_z == b.sector(i).ceil_z);
        CHECK(a.sector(i).tag == b.sector(i).tag);
        CHECK(a.sector(i).tri_count == b.sector(i).tri_count);
        for (u16 k = 0; k < b.sector(i).tri_count * 3; ++k) {
            CHECK(a.tris[a.sector(i).first_tri + k] == b.tris[b.sector(i).first_tri + k]);
        }
    }
    for (u32 i = 0; i < b.side_count && i < a.side_count; ++i) {
        CHECK(a.side(i).a == b.side(i).a);
        CHECK(a.side(i).b == b.side(i).b);
        CHECK(a.side(i).neighbor == b.side(i).neighbor);
        CHECK(a.side(i).flags == b.side(i).flags);
    }
}

// Holes get their OWN fixture rather than being added to build_reference.
//
// That reference is shared with the test comparing it against the hardcoded
// map in core/src/sim -- which has no hole -- so a hole in it made a geometry
// difference look like a round-trip failure. Three assertions in two tests, one
// cause, and none of them pointing at it.
static void test_holes_round_trip() {
    std::printf("mapfile: a sector's holes survive save and load\n");

    MapStorage src;
    src.clear();
    const fx2 room[] = {{F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)}};
    const u32 s0 = src.add_sector(room, 4, F(10), F(60), 7);
    const fx2 pit[] = {{F(14), F(14)}, {F(26), F(14)}, {F(26), F(26)}, {F(14), F(26)}};
    CHECK(src.add_hole(s0, pit, 4));
    src.add_side(s0, 0, 1, NO_SECTOR, 0);
    src.add_side(s0, 1, 2, NO_SECTOR, 0);
    src.add_side(s0, 2, 3, NO_SECTOR, 0);
    src.add_side(s0, 3, 0, NO_SECTOR, 0);

    // The room is an annulus now, so it meshes as one: 8 triangles, not 2.
    CHECK(src.view().sector(0).tri_count == 8);
    CHECK(src.view().sector(0).hole_count == 1);

    static u8 buf[1 << 16];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));

    MapStorage dst;
    ByteReader r(buf, w.size());
    CHECK(load_map(r, dst) == MapLoadStatus::Ok);
    CHECK(dst.hash() == src.hash());

    const MapData a = src.view();
    const MapData b = dst.view();
    CHECK(a.point_count == b.point_count);
    CHECK(b.sector(0).hole_count == 1);
    CHECK(b.sector(0).tri_count == 8);
    CHECK(a.hole(a.sector(0).first_hole).first == b.hole(b.sector(0).first_hole).first);
    CHECK(a.hole(a.sector(0).first_hole).count == b.hole(b.sector(0).first_hole).count);

    // And the hole is real to the SIMULATION, not merely stored: a point over
    // it is not in the sector at all. That is the whole reason holes exist --
    // nesting a second sector leaves the room's floor covering the pit, and a
    // body walks over it.
    CHECK(!point_in_sector(b, 0, fx2{F(20), F(20)}));
    CHECK(point_in_sector(b, 0, fx2{F(5), F(20)}));

    // Area subtracts the hole: 40*40 - 12*12 = 1456.
    const i128 want = static_cast<i128>(fx::from_int(1456).raw) * fx::one().raw * 2;
    CHECK(signed_area_2x(b, 0) == want);
}

static void test_rejects_bad_input() {
    std::printf("mapfile: malformed input is rejected, never partially loaded\n");

    MapStorage src;
    build_reference(src);
    u8 buf[8192];
    ByteWriter w(buf, sizeof(buf));
    save_map(src, w);
    const size_t n = w.size();

    MapStorage dst;

    // Every truncation must be refused, and must leave the store empty rather
    // than half-populated -- a partly-loaded map is worse than none because it
    // looks usable.
    for (size_t len = 0; len < n; ++len) {
        ByteReader r(buf, len);
        const MapLoadStatus st = load_map(r, dst);
        CHECK(st != MapLoadStatus::Ok);
        CHECK(dst.sector_count() == 0);
    }

    // Wrong magic.
    {
        u8 bad[64] = {0};
        ByteReader r(bad, sizeof(bad));
        CHECK(load_map(r, dst) == MapLoadStatus::BadMagic);
    }

    // Wrong version.
    {
        u8 bad[8192];
        for (size_t i = 0; i < n; ++i) bad[i] = buf[i];
        bad[4] = 99;   // version low byte
        ByteReader r(bad, n);
        CHECK(load_map(r, dst) == MapLoadStatus::BadVersion);
    }

    // Absurd counts must be refused BEFORE anything sized by them is read.
    {
        u8 bad[8192];
        for (size_t i = 0; i < n; ++i) bad[i] = buf[i];
        bad[8] = 0xFF; bad[9] = 0xFF; bad[10] = 0xFF; bad[11] = 0xFF;   // point_count
        ByteReader r(bad, n);
        const MapLoadStatus st = load_map(r, dst);
        CHECK(st == MapLoadStatus::TooLarge || st == MapLoadStatus::Truncated);
        CHECK(dst.sector_count() == 0);
    }

    // Reserved flags must be refused rather than ignored: a set bit means the
    // writer used a feature this build does not understand.
    {
        u8 bad[8192];
        for (size_t i = 0; i < n; ++i) bad[i] = buf[i];
        bad[6] = 1;
        ByteReader r(bad, n);
        CHECK(load_map(r, dst) == MapLoadStatus::BadVersion);
    }

    // NO single flipped bit may load silently. Every byte of the file is either
    // structural (and rejected) or hashed (and caught) -- there are no
    // don't-care bytes, which is exactly why the reserved field is validated
    // rather than skipped.
    {
        Rng rng(9);
        int loaded_silently = 0;
        for (int trial = 0; trial < 5000; ++trial) {
            u8 bad[8192];
            for (size_t i = 0; i < n; ++i) bad[i] = buf[i];
            const u32 at = rng.next_below(static_cast<u32>(n));
            bad[at] = static_cast<u8>(bad[at] ^ (1u << rng.next_below(8)));
            ByteReader r(bad, n);
            if (load_map(r, dst) == MapLoadStatus::Ok && dst.hash() == src.hash()) {
                ++loaded_silently;
            }
        }
        CHECK(loaded_silently == 0);
    }
}

static void test_fuzz() {
    std::printf("mapfile: arbitrary bytes never misbehave\n");
    Rng rng(0xBEEF);
    MapStorage dst;
    u8 buf[2048];

    for (int trial = 0; trial < 20000; ++trial) {
        const size_t len = rng.next_below(600) + 1;
        for (size_t i = 0; i < len; ++i) buf[i] = static_cast<u8>(rng.next_u32());
        // Give a third of them a valid magic so the parser is entered rather
        // than bailing on the first four bytes.
        if ((trial % 3) == 0 && len >= 4) {
            buf[0] = 0x44; buf[1] = 0x4E; buf[2] = 0x50; buf[3] = 0x4D;
            if (len >= 6) { buf[4] = 1; buf[5] = 0; }
        }
        ByteReader r(buf, len);
        const MapLoadStatus st = load_map(r, dst);
        if (st == MapLoadStatus::Ok) {
            // If it claims success, every index must be in range -- a loader
            // that returns true with a bad index is worse than one that
            // crashes, because the crash would at least be noticed.
            const MapData m = dst.view();
            for (u32 i = 0; i < m.side_count; ++i) {
                CHECK(m.side(i).a < m.point_count);
                CHECK(m.side(i).b < m.point_count);
            }
            for (u32 i = 0; i < m.sector_count; ++i) {
                CHECK(static_cast<u32>(m.sector(i).first_point) + m.sector(i).point_count <= m.point_count);
            }
        }
    }
}

// The body proportions a map declares survive the file, and the hash notices
// when they do not.
//
// Round-tripping is the easy half. The half worth asserting is that the body is
// part of the CONTENT: a map built for a 1.167-tall player and one built for
// the default 2.0 are different maps, and a client that parsed the second while
// the server parsed the first would mispredict every doorway. That is exactly
// what WELCOME.map_hash exists to catch.
static void test_body_round_trips_and_is_hashed() {
    std::printf("mapfile: a map's body proportions survive the file and enter its hash\n");

    MapStorage src;
    build_reference(src);
    const u64 default_hash = src.hash();

    // Doom's player at 1/48: 16 and 56 units.
    src.set_body(fx::from_frac(1, 3), fx::from_frac(7, 6));
    CHECK(src.hash() != default_hash);

    u8 buf[8192];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));

    MapStorage dst;
    ByteReader r(buf, w.size());
    CHECK(load_map(r, dst) == MapLoadStatus::Ok);
    CHECK(dst.body_radius() == fx::from_frac(1, 3));
    CHECK(dst.body_height() == fx::from_frac(7, 6));
    CHECK(dst.hash() == src.hash());

    // CONTROL: a map that says nothing keeps the engine's defaults, so every
    // map authored before this field existed means what it always meant.
    MapStorage plain;
    build_reference(plain);
    u8 buf2[8192];
    ByteWriter w2(buf2, sizeof(buf2));
    CHECK(save_map(plain, w2));
    MapStorage dst2;
    ByteReader r2(buf2, w2.size());
    CHECK(load_map(r2, dst2) == MapLoadStatus::Ok);
    CHECK(dst2.body_radius() == fx::from_frac(1, 2));
    CHECK(dst2.body_height() == fx::from_int(2));
    CHECK(dst2.hash() == default_hash);
}

// The scripts a map declares survive the file, and cannot name a path.
//
// The server appends each of these to the map's own directory, so a name that
// could climb out of it would let a FILE choose what the host runs. Checked at
// bake time by add_script and again by the loader, because the file on disk is
// not necessarily the one mapc wrote -- and the loader is the side an attacker
// gets to choose the bytes for.
static void test_declared_scripts_round_trip_and_stay_inside_the_map() {
    std::printf("mapfile: a map's declared scripts survive the file, and cannot be paths\n");

    // static, like every other fixture here: a MapStorage is ~460 KB of fixed
    // arrays and this test wants five of them, which is more stack than a test
    // binary has -- ASan found it as a stack overflow rather than as a crash in
    // whichever field happened to land past the guard page.
    static MapStorage src;
    build_reference(src);
    const u64 bare_hash = src.hash();

    CHECK(src.add_script("doom.lua", 8));
    CHECK(src.add_script("map01.lua", 9));
    CHECK(src.script_count() == 2);
    // Order is preserved: a map's own file is written against the library it
    // names first, so loading them the other way round would run doom.load
    // before doom exists.
    CHECK(src.script(0).len == 8 && src.script(0).name[0] == 'd');
    CHECK(src.script(1).len == 9 && src.script(1).name[0] == 'm');
    // Declaring the same library twice loads it once. Running a script's top
    // level a second time re-registers everything it registered.
    CHECK(src.add_script("doom.lua", 8));
    CHECK(src.script_count() == 2);
    // And they are part of what the file says.
    CHECK(src.hash() != bare_hash);

    u8 buf[8192];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));
    static MapStorage dst;
    ByteReader r(buf, w.size());
    CHECK(load_map(r, dst) == MapLoadStatus::Ok);
    CHECK(dst.script_count() == 2);
    CHECK(dst.hash() == src.hash());

    // Every shape of "somewhere else" is refused.
    static MapStorage bad;
    build_reference(bad);
    CHECK(!bad.add_script("../secrets.lua", 14));
    CHECK(!bad.add_script("/etc/passwd", 11));
    CHECK(!bad.add_script("sub/dir.lua", 11));
    CHECK(!bad.add_script("C:\\evil.lua", 11));
    CHECK(!bad.add_script(".hidden", 7));
    CHECK(!bad.add_script("", 0));
    CHECK(bad.script_count() == 0);

    // CONTROL: the same call with a plain name succeeds. Without it, "add_script
    // returned false" would be satisfied by a function that always did.
    CHECK(bad.add_script("plain.lua", 9));
    CHECK(bad.script_count() == 1);

    // And a FILE carrying a path is refused by the loader, not merely by the
    // compiler that was supposed to have prevented it. Written by hand into a
    // valid map's bytes, because the only way to produce one is to not use
    // add_script.
    {
        static MapStorage one;
        build_reference(one);
        CHECK(one.add_script("aaaaaaaa", 8));
        u8 b2[8192];
        ByteWriter w2(b2, sizeof(b2));
        CHECK(save_map(one, w2));
        // Find the name in the serialised bytes and turn it into a path.
        bool patched = false;
        for (size_t i = 0; i + 8 <= w2.size() && !patched; ++i) {
            bool all_a = true;
            for (u32 k = 0; k < 8; ++k) all_a = all_a && b2[i + k] == 'a';
            if (!all_a) continue;
            b2[i] = '.'; b2[i + 1] = '.'; b2[i + 2] = '/';
            patched = true;
        }
        CHECK(patched);
        static MapStorage out;
        ByteReader r2(b2, w2.size());
        const MapLoadStatus st = load_map(r2, out);
        CHECK(st != MapLoadStatus::Ok);
    }
}

int main() {
    std::printf("== core/geom map file suite ==\n");
    test_roundtrip();
    test_matches_hardcoded_map();
    test_holes_round_trip();
    test_body_round_trips_and_is_hashed();
    test_declared_scripts_round_trip_and_stay_inside_the_map();
    test_rejects_bad_input();
    test_render_does_not_affect_physics();
    test_fuzz();
    test_a_map_written_before_styles_still_loads();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
