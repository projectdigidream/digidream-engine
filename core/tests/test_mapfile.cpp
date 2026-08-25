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
    }
    CHECK(dst.spawn_count() == 2);
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

int main() {
    std::printf("== core/geom map file suite ==\n");
    test_roundtrip();
    test_matches_hardcoded_map();
    test_rejects_bad_input();
    test_fuzz();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
