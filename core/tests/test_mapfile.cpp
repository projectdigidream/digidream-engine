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
#include <cstring>
#include <tuple>

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
    s.sector_render_mut(2).floor.anchor = 1;
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
    // A style is 30 bytes at 13 and 29 at 12; a side gained its alpha at 12.
    constexpr size_t STYLE_13 = 30, STYLE_12 = 29;
    constexpr size_t SECTOR_13 = 5 + 2 * STYLE_13;
    constexpr size_t SIDE_13 = 24;
    const u32 sectors = src.view().sector_count;
    const u32 sides = src.view().side_count;
    const size_t tail = sectors * SECTOR_13 + sides * SIDE_13 + 8;
    CHECK(n > tail);
    const size_t head = n - tail;

    static u8 old[8192];
    static MapStorage dst;

    // Rewrite the two render sections at an older layout, keeping everything
    // ahead of them byte for byte -- which is what makes this a test of the
    // WINDOW rather than of a fixture somebody recorded once. (A map with no
    // sector effects is written as 13, so there is no version-14 field to
    // drop: see test_sector_effects.)
    CHECK(buf[4] == 13 && buf[5] == 0);
    const auto downgrade = [&](u16 version, size_t style_bytes, size_t side_bytes) {
        size_t m = 0;
        for (size_t i = 0; i < head; ++i) old[m++] = buf[i];
        old[4] = static_cast<u8>(version); old[5] = 0;
        size_t at = head;
        for (u32 i = 0; i < sectors; ++i, at += SECTOR_13) {
            for (size_t k = 0; k < 5; ++k) old[m++] = buf[at + k];   // tex, tex, light
            for (size_t flat = 0; flat < 2; ++flat) {
                for (size_t k = 0; k < style_bytes; ++k) {
                    old[m++] = buf[at + 5 + flat * STYLE_13 + k];
                }
            }
        }
        for (u32 i = 0; i < sides; ++i, at += SIDE_13) {
            for (size_t k = 0; k < side_bytes; ++k) old[m++] = buf[at + k];
        }
        for (size_t k = 0; k < 8; ++k) old[m++] = buf[at + k];       // the hash
        return m;
    };

    // Version 11 knew no styles at all, and its sides carried no alpha.
    // Version 12 knew the styles and not the anchor. Both must load, and both
    // must hash to what this writer produces for the same map.
    // Version 13 knew everything but effects (and so has no fx_count at all).
    const u16 versions[3] = {11, 12, 13};
    const size_t style_len[3] = {0, STYLE_12, STYLE_13};
    const size_t side_len[3] = {23, 24, 24};
    for (int v = 0; v < 3; ++v) {
        const size_t len = downgrade(versions[v], style_len[v], side_len[v]);
        ByteReader r(old, len);
        const MapLoadStatus st = load_map(r, dst);
        CHECK(st == MapLoadStatus::Ok);
        // THE HASH IS THE CLAIM. load_map already compares it against the one
        // the file carries, so reaching Ok says the recomputed hash matched --
        // and this says it matched the one the NEW writer produces.
        CHECK(dst.hash() == src.hash());
        for (u32 i = 0; i < dst.view().sector_count; ++i) {
            CHECK(dst.render_view().sector(i).floor.is_default());
            CHECK(dst.render_view().sector(i).ceil.is_default());
        }
        for (u32 i = 0; i < dst.view().side_count; ++i) {
            CHECK(dst.render_view().side(i).alpha == 255);
        }
    }

    // CONTROL: the window has a bottom. A file one version older than the
    // oldest supported is refused, or "we accept 11 through 13" is really "we
    // accept anything" and the version field means nothing.
    // `dst` again rather than a third arena: load_map clears its destination
    // before it reads a byte, so a refused load leaves nothing behind.
    const size_t len = downgrade(11, 0, 23);
    old[4] = 10;
    ByteReader r10(old, len);
    CHECK(load_map(r10, dst) == MapLoadStatus::BadVersion);

    // And so is one newer than this writer knows how to produce.
    old[4] = static_cast<u8>(MAP_VERSION + 1);
    ByteReader r15(old, len);
    CHECK(load_map(r15, dst) == MapLoadStatus::BadVersion);
}

static void test_roundtrip() {
    std::printf("mapfile: save/load round-trips exactly\n");

    static MapStorage src;

    src.clear();
    build_reference(src);

    u8 buf[8192];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));
    CHECK(w.size() > 0);

    static MapStorage dst;

    dst.clear();
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
        CHECK(x.anchor == y.anchor);
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
    static MapStorage plain;
    plain.clear();
    build_reference(plain);
    // Strip every rendering attribute back to its default.
    for (u32 i = 0; i < plain.view().sector_count; ++i) plain.sector_render_mut(i) = SectorRender{};
    for (u32 i = 0; i < plain.view().side_count; ++i) plain.side_render_mut(i) = SideRender{};

    static MapStorage painted;

    painted.clear();
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

    static MapStorage built;

    built.clear();
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

    static MapStorage src;

    src.clear();
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

    static MapStorage dst;

    dst.clear();
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

    static MapStorage src;

    src.clear();
    build_reference(src);
    u8 buf[8192];
    ByteWriter w(buf, sizeof(buf));
    save_map(src, w);
    const size_t n = w.size();

    static MapStorage dst;

    dst.clear();

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
    static MapStorage dst;
    dst.clear();
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

    static MapStorage src;

    src.clear();
    build_reference(src);
    const u64 default_hash = src.hash();

    // Doom's player at 1/48: 16 and 56 units.
    src.set_body(fx::from_frac(1, 3), fx::from_frac(7, 6));
    CHECK(src.hash() != default_hash);

    u8 buf[8192];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));

    static MapStorage dst;

    dst.clear();
    ByteReader r(buf, w.size());
    CHECK(load_map(r, dst) == MapLoadStatus::Ok);
    CHECK(dst.body_radius() == fx::from_frac(1, 3));
    CHECK(dst.body_height() == fx::from_frac(7, 6));
    CHECK(dst.hash() == src.hash());

    // CONTROL: a map that says nothing keeps the engine's defaults, so every
    // map authored before this field existed means what it always meant.
    static MapStorage plain;
    plain.clear();
    build_reference(plain);
    u8 buf2[8192];
    ByteWriter w2(buf2, sizeof(buf2));
    CHECK(save_map(plain, w2));
    static MapStorage dst2;
    dst2.clear();
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

    // ROOM FOR A WHOLE STUDIO LEVEL: three runtimes at two scripts apiece, the
    // graph, the module's file and the map's own -- and more -- up to sixteen,
    // and they survive the file. The seventeenth is refused.
    {
        static MapStorage many;
        build_reference(many);
        char name[16];
        for (u32 i = 0; i < MAX_MAP_SCRIPTS; ++i) {
            const int n = std::snprintf(name, sizeof(name), "s%02u.lua", i);
            CHECK(many.add_script(name, static_cast<u32>(n)));
        }
        CHECK(many.script_count() == 16);
        CHECK(!many.add_script("one_more.lua", 12));
        u8 b3[16384];
        ByteWriter w3(b3, sizeof(b3));
        CHECK(save_map(many, w3));
        static MapStorage back;
        ByteReader r3(b3, w3.size());
        CHECK(load_map(r3, back) == MapLoadStatus::Ok);
        CHECK(back.script_count() == 16 && back.hash() == many.hash());
        CHECK(back.script(15).name[1] == '1' && back.script(15).name[2] == '5');
    }

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

// SECTOR EFFECTS (version 14, docs/SECTOR_FX.md): they round-trip, they are
// hashed only when present, and everything a reader cannot understand is
// refused -- with a CONTROL each time, a variant that must still load.
static void test_sector_effects() {
    std::printf("mapfile: sector effects round-trip, hash only when present, refuse nonsense\n");
    static MapStorage src;
    static MapStorage dst;
    static u8 buf[16384];
    src.clear();
    build_reference(src);
    const u64 bare = src.hash();

    const auto key = [](const char* name) {
        for (u8 k = 1; k <= FX_KEY_COUNT; ++k) if (std::strcmp(FX_KEYS[k - 1].name, name) == 0) return k;
        return static_cast<u8>(0);
    };
    SectorFx rain;
    rain.sector = 0; rain.kind = static_cast<u8>(FxKind::Rain);
    rain.params[rain.count++] = { key("density"), fx::from_int(10).raw };
    rain.params[rain.count++] = { key("windx"), fx::from_frac(6, 5).raw };
    rain.params[rain.count++] = { key("color"), 0x9aa8c0 };
    SectorFx fog;
    fog.sector = 2; fog.kind = static_cast<u8>(FxKind::Fog);
    fog.params[fog.count++] = { key("density"), fx::from_frac(7, 20).raw };
    fog.params[fog.count++] = { key("falloff"), fx::from_frac(6, 5).raw };
    CHECK(src.add_effect(rain) && src.add_effect(fog));
    CHECK(src.hash() != bare);      // CONTROL for the next claim: effects ARE hashed

    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));
    const size_t n = w.size();
    CHECK(buf[4] == 14);   // effects need 14; a map without them is written as 13
    {
        ByteReader r(buf, n);
        CHECK(load_map(r, dst) == MapLoadStatus::Ok);
        CHECK(dst.effect_count() == 2 && dst.hash() == src.hash());
        const SectorFx& a = dst.effect(0);
        CHECK(a.sector == 0 && a.kind == rain.kind && a.count == 3 &&
              a.params[1].key == key("windx") && a.params[1].raw == fx::from_frac(6, 5).raw &&
              a.params[2].raw == 0x9aa8c0);
        CHECK(dst.effect(1).sector == 2 && dst.effect(1).params[1].key == key("falloff"));
    }

    // NO EFFECTS, NO CHANGE: a map without any hashes as it always did, and
    // is WRITTEN as it always was -- version 13, four bytes shorter.
    {
        static MapStorage plain;
        static u8 pb[16384];
        plain.clear();
        build_reference(plain);
        CHECK(plain.effect_count() == 0 && plain.hash() == bare);
        ByteWriter pw(pb, sizeof(pb));
        CHECK(save_map(plain, pw));
        CHECK(pb[4] == 13);
        ByteReader pr(pb, pw.size());
        CHECK(load_map(pr, dst) == MapLoadStatus::Ok && dst.effect_count() == 0 && dst.hash() == bare);
    }

    // Refusals. Each builds a map with one bad effect and must not load it.
    const auto refused = [&](SectorFx bad) {
        static MapStorage m;
        m.clear();
        build_reference(m);
        m.add_effect(bad);
        static u8 b[16384];
        ByteWriter bw(b, sizeof(b));
        if (!save_map(m, bw)) return false;
        ByteReader br(b, bw.size());
        return load_map(br, dst) != MapLoadStatus::Ok;
    };
    SectorFx e = fog;
    CHECK(!refused(e));                                     // CONTROL: the good one loads
    e = fog; e.sector = 99;                     CHECK(refused(e));   // no such sector
    e = fog; e.kind = 9;                        CHECK(refused(e));   // no such kind
    e = fog; e.params[0].key = key("speed");    CHECK(refused(e));   // fog takes no speed
    e = fog; e.params[1].key = key("density");  CHECK(refused(e));   // a key twice
    e = rain; e.params[2].raw = 0x1000000;      CHECK(refused(e));   // not a colour
    e = rain; e.params[0].key = 0;              CHECK(refused(e));   // key 0
    e = rain; e.params[0].key = FX_KEY_COUNT + 1; CHECK(refused(e)); // past the table
    // A fill: a colour and an alpha load; a density, which it has none of, does not.
    SectorFx fill;
    fill.sector = 1; fill.kind = static_cast<u8>(FxKind::Fill);
    fill.params[fill.count++] = { key("color"), 0x3a6a8a };
    fill.params[fill.count++] = { key("alpha"), fx::from_frac(2, 5).raw };
    CHECK(!refused(fill) && std::strcmp(fx_kind_name(fill.kind), "fill") == 0);
    e = fill; e.params[1].key = key("density"); CHECK(refused(e));   // CONTROL: fill takes no density

    // And no single flipped bit in the file loads silently -- the effects
    // section included, which is the last thing before the hash.
    {
        Rng rng(14);
        int loaded_silently = 0;
        static u8 bad[16384];
        for (int trial = 0; trial < 4000; ++trial) {
            for (size_t i = 0; i < n; ++i) bad[i] = buf[i];
            // Half the trials aim at the effects section, the last 46 + 26 bytes.
            const u32 span = trial % 2 ? static_cast<u32>(n) : 80u;
            const u32 at = trial % 2 ? rng.next_below(span) : static_cast<u32>(n - 8 - 1 - rng.next_below(span - 8));
            bad[at] = static_cast<u8>(bad[at] ^ (1u << rng.next_below(8)));
            ByteReader r(bad, n);
            if (load_map(r, dst) == MapLoadStatus::Ok && dst.hash() == src.hash()) ++loaded_silently;
        }
        CHECK(loaded_silently == 0);
    }
}

// EFFECT SECTORS (version 17): a map with one is written as 17, round-trips the
// flag and hashes it; one without is written as it always was (the tests above
// still read 13, 14, 15 and 16). Canonical like the styles: every bit of the
// block changes the hash or is refused -- and an effect sector with a slab is
// refused, as mapc refuses it.
static void test_effect_sectors() {
    std::printf("mapfile: effect sectors -- version 17 only when used, hashed, no slab\n");
    static MapStorage src;
    static MapStorage dst;
    static u8 buf[16384];
    src.clear();
    build_reference(src);
    const u64 bare = src.hash();
    src.sector_mut(2).effect = true;
    CHECK(src.hash() != bare);      // CONTROL for what follows: the flag IS hashed

    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));
    const size_t n = w.size();
    CHECK(buf[4] == 17);
    {
        ByteReader r(buf, n);
        const MapLoadStatus st = load_map(r, dst);
        std::printf("  version %u, %zu bytes: %s\n", buf[4], n, map_load_status_text(st));
        CHECK(st == MapLoadStatus::Ok);
        CHECK(dst.view().sector(2).effect && !dst.view().sector(0).effect &&
              !dst.view().sector(1).effect && dst.hash() == src.hash());
    }
    {
        static MapStorage slab;
        static u8 sb[16384];
        slab.clear();
        build_reference(slab);
        slab.sector_mut(1).effect = true;           // the reference's floating slab
        ByteWriter bw(sb, sizeof(sb));
        CHECK(save_map(slab, bw));
        ByteReader br(sb, bw.size());
        CHECK(load_map(br, dst) != MapLoadStatus::Ok);
    }
    {
        Rng rng(17);
        int loaded_silently = 0;
        static u8 bad[16384];
        for (int trial = 0; trial < 4000; ++trial) {
            for (size_t i = 0; i < n; ++i) bad[i] = buf[i];
            // Half the trials aim at the block: the last index, just before the hash.
            const u32 at = trial % 2 ? rng.next_below(static_cast<u32>(n))
                                     : static_cast<u32>(n - 8 - 1 - rng.next_below(4));
            bad[at] = static_cast<u8>(bad[at] ^ (1u << rng.next_below(8)));
            ByteReader r(bad, n);
            if (load_map(r, dst) == MapLoadStatus::Ok && dst.hash() == src.hash()) ++loaded_silently;
        }
        CHECK(loaded_silently == 0);
    }
    // A LIQUID (version 18): the effect sector's medium, one byte more, written
    // only when some sector has one -- the map above stayed 17.
    {
        const u64 effect_only = src.hash();
        src.sector_mut(2).medium = sector_medium::LIQUID;
        src.rebuild_grid();
        CHECK(src.hash() != effect_only);     // CONTROL: the medium IS hashed
        CHECK(src.view().liquid_count == 1 && src.view().liquids[0] == 2);
        static u8 lb[16384];
        ByteWriter lw(lb, sizeof(lb));
        CHECK(save_map(src, lw));
        CHECK(lb[4] == 18);
        ByteReader lr(lb, lw.size());
        CHECK(load_map(lr, dst) == MapLoadStatus::Ok && dst.view().sector(2).medium == sector_medium::LIQUID &&
              dst.view().liquid_count == 1 && dst.hash() == src.hash());
        // A medium nobody knows is refused, not read as water.
        lb[lw.size() - 8 - 1] = 2;
        ByteReader br(lb, lw.size());
        CHECK(load_map(br, dst) != MapLoadStatus::Ok);
        src.sector_mut(2).medium = sector_medium::NONE;
        src.rebuild_grid();
    }
}

// A WALL'S TEXTURE ANGLE AND SCALE (version 15): a map that uses them is
// written as 15, round-trips them, and hashes them; one that does not is
// written as it always was. The block is canonical -- in side order, none
// twice, never a "no change" entry, never a zero scale -- so every bit of it
// either changes the hash or is refused.
static void test_wall_styles() {
    std::printf("mapfile: wall texture angle and scale -- version 15 only when used, canonical\n");
    static MapStorage src;
    static MapStorage dst;
    static u8 buf[16384];
    src.clear();
    build_reference(src);
    const u64 bare = src.hash();
    src.side_render_mut(1).angle = 0x40000000u;            // a quarter turn
    src.side_render_mut(3).scale = fx::from_int(2);         // twice the size,
    src.side_render_mut(3).scale_y = fx::from_int(2);       // both ways: still a 15
    CHECK(src.hash() != bare);      // CONTROL for what follows: the styles ARE hashed

    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(src, w));
    const size_t n = w.size();
    CHECK(buf[4] == 15);
    {
        ByteReader r(buf, n);
        CHECK(load_map(r, dst) == MapLoadStatus::Ok);
        CHECK(dst.hash() == src.hash());
        const RenderData rd = dst.render_view();
        CHECK(rd.side(1).angle == 0x40000000u && rd.side(1).scale == fx::one());
        CHECK(rd.side(3).angle == 0 && rd.side(3).scale == fx::from_int(2));
        u32 others = 0;
        for (u32 i = 0; i < dst.view().side_count; ++i) if (i != 1 && i != 3 && !rd.side(i).styled()) ++others;
        CHECK(others == dst.view().side_count - 2);
    }

    // STRETCHED: a different scale up the wall is 16, and survives; the
    // uniform map above stayed 15 (CONTROL: the byte checked above).
    {
        static MapStorage st, back;
        static u8 sb[16384];
        st.clear();
        build_reference(st);
        st.side_render_mut(3).scale = fx::from_int(2);
        const u64 uniform = st.hash();
        st.side_render_mut(3).scale_y = fx::from_frac(1, 2);
        CHECK(st.hash() != uniform);      // the y scale is hashed
        ByteWriter sw(sb, sizeof(sb));
        CHECK(save_map(st, sw));
        CHECK(sb[4] == 16);
        ByteReader sr(sb, sw.size());
        CHECK(load_map(sr, back) == MapLoadStatus::Ok && back.hash() == st.hash());
        CHECK(back.render_view().side(3).scale == fx::from_int(2) &&
              back.render_view().side(3).scale_y == fx::from_frac(1, 2));
    }

    // Unstyled, unchanged: written as 13 (no effects either) and hashed as before.
    {
        static MapStorage plain;
        static u8 pb[16384];
        plain.clear();
        build_reference(plain);
        ByteWriter pw(pb, sizeof(pb));
        CHECK(save_map(plain, pw));
        CHECK(pb[4] == 13 && plain.hash() == bare);
    }

    // The block is the last thing before the eight-byte hash: two entries of
    // {u32 side, u32 angle, i64 scale}.
    const size_t at = n - 8 - 2 * 16;
    const auto status = [&](void (*mutate)(u8*), size_t off) {
        static u8 m[16384];
        std::memcpy(m, buf, n);
        mutate(m + off);
        ByteReader r(m, n);
        return load_map(r, dst);
    };
    const auto put32 = [](u8* p, u32 v) { for (int k = 0; k < 4; ++k) p[k] = static_cast<u8>(v >> (8 * k)); };
    (void)put32;
    {   // CONTROL: the buffer as written loads.
        ByteReader r(buf, n);
        CHECK(load_map(r, dst) == MapLoadStatus::Ok);
    }
    // The second entry naming the first's side again (out of order, twice).
    CHECK(status([](u8* p) { const u8 first = p[-16]; p[0] = first; p[1] = p[2] = p[3] = 0; }, at + 16) == MapLoadStatus::BadIndex);
    // A side the map does not have.
    CHECK(status([](u8* p) { p[0] = 0xFF; p[1] = 0x7F; p[2] = 0; p[3] = 0; }, at + 16) == MapLoadStatus::BadIndex);
    // An entry that says "no change": the first's quarter turn set back to none.
    CHECK(status([](u8* p) { p[0] = p[1] = p[2] = p[3] = 0; }, at + 4) == MapLoadStatus::BadIndex);
    // A scale of zero, written by the writer itself.
    {
        static MapStorage z;
        static u8 zb[16384];
        z.clear();
        build_reference(z);
        z.side_render_mut(2).scale = fx::zero();
        ByteWriter zw(zb, sizeof(zb));
        CHECK(save_map(z, zw));
        ByteReader zr(zb, zw.size());
        CHECK(load_map(zr, dst) == MapLoadStatus::BadIndex);
    }
    // NO DON'T-CARE BYTES: every bit of the block, flipped, is refused or moves
    // the hash -- a byte the reader ignored would load to the same hash.
    u32 silent = 0;
    for (size_t b = at; b < at + 32; ++b) {
        for (int bit = 0; bit < 8; ++bit) {
            static u8 m[16384];
            std::memcpy(m, buf, n);
            m[b] ^= static_cast<u8>(1u << bit);
            ByteReader r(m, n);
            const MapLoadStatus st = load_map(r, dst);
            if (st == MapLoadStatus::Ok && dst.hash() == src.hash()) ++silent;
        }
    }
    CHECK(silent == 0);
}

int main() {
    std::printf("== core/geom map file suite ==\n");
    test_sector_effects();
    test_effect_sectors();
    test_wall_styles();
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
