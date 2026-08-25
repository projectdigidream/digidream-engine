//
// mapc -- the map compiler.
//
//   mapc <input.map> <output.dnpm>
//
// Reads a small line-based authoring format and writes the baked binary. It
// links dnp_core and calls the SAME triangulator the runtime uses, which is the
// whole point: baking with a second implementation would produce meshes that
// agree with the runtime's until the first update_shape(), at which point a
// sector would silently change shape.
//
// The authoring format is a Phase 3 bridge. The level editor (Phase 8) emits
// the binary directly; this exists so maps can be written and reviewed as text
// in the meantime, and it stays useful afterwards for fixtures and diffs.
//
//   # comment
//   sector floor 0 ceil 40 tag 1
//     vertex 0  0   wall
//     vertex 40 0   neighbor 1
//     vertex 40 40  wall
//     vertex 0  40  wall climbable
//   end
//
//   spawn start 10 10 2
//
// Each `vertex` line declares the point AND the edge leading from it to the
// next vertex, which is how sectors are actually drawn. Numbers are decimal and
// are converted to fixed point exactly, via a rational, never through a double.
//
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dnp/geom/mapfile.hpp"
#include "dnp/geom/mapstore.hpp"

using namespace dnp;
using namespace dnp::geom;

namespace {

int g_errors = 0;

void err(int line, const char* fmt, ...) {
    std::fprintf(stderr, "%d: ", line);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    ++g_errors;
}

// Decimal -> fx, exactly.
//
// Parsed as a rational and built with fx::from_frac rather than going through a
// double: a double would round "0.1" before it ever reached fixed point, and
// the whole map would inherit an error introduced by the compiler rather than
// by the format.
bool parse_fx(const std::string& tok, fx& out) {
    size_t i = 0;
    bool neg = false;
    if (i < tok.size() && (tok[i] == '-' || tok[i] == '+')) { neg = tok[i] == '-'; ++i; }
    if (i >= tok.size()) return false;

    i64 whole = 0;
    bool any = false;
    for (; i < tok.size() && tok[i] >= '0' && tok[i] <= '9'; ++i) {
        whole = whole * 10 + (tok[i] - '0');
        if (whole > (i64(1) << 30)) return false;   // keep from_frac well inside range
        any = true;
    }

    i64 frac = 0, scale = 1;
    if (i < tok.size() && tok[i] == '.') {
        ++i;
        // Nine digits is where scale would start risking the 64-bit numerator in
        // from_frac; well beyond what any level needs.
        for (int d = 0; i < tok.size() && tok[i] >= '0' && tok[i] <= '9'; ++i, ++d) {
            if (d >= 9) return false;
            frac = frac * 10 + (tok[i] - '0');
            scale *= 10;
            any = true;
        }
    }
    if (!any || i != tok.size()) return false;

    const i64 num = whole * scale + frac;
    out = fx::from_frac(neg ? -num : num, scale);
    return true;
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '#') break;                       // comment to end of line
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct PendingVertex {
    fx2 pos{};
    i32 neighbor = NO_SECTOR;
    u8  flags = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: mapc <input.map> <output.dnpm>\n");
        return 2;
    }

    std::FILE* in = std::fopen(argv[1], "rb");
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    MapStorage store;
    store.clear();

    std::vector<PendingVertex> verts;
    bool in_sector = false;
    fx floor_z{}, ceil_z{};
    u16 tag = 0;
    int sector_start_line = 0;

    char buf[1024];
    int lineno = 0;

    while (std::fgets(buf, sizeof(buf), in)) {
        ++lineno;
        const auto tok = tokenize(std::string(buf));
        if (tok.empty()) continue;

        if (tok[0] == "sector") {
            if (in_sector) { err(lineno, "sector inside sector; missing 'end'?"); continue; }
            in_sector = true;
            verts.clear();
            floor_z = fx::zero();
            ceil_z = fx::from_int(32);
            tag = 0;
            sector_start_line = lineno;
            for (size_t i = 1; i + 1 < tok.size(); i += 2) {
                if (tok[i] == "floor") { if (!parse_fx(tok[i + 1], floor_z)) err(lineno, "bad floor '%s'", tok[i + 1].c_str()); }
                else if (tok[i] == "ceil") { if (!parse_fx(tok[i + 1], ceil_z)) err(lineno, "bad ceil '%s'", tok[i + 1].c_str()); }
                else if (tok[i] == "tag") { tag = static_cast<u16>(std::atoi(tok[i + 1].c_str())); }
                else err(lineno, "unknown sector attribute '%s'", tok[i].c_str());
            }
        } else if (tok[0] == "vertex") {
            if (!in_sector) { err(lineno, "vertex outside a sector"); continue; }
            if (tok.size() < 3) { err(lineno, "vertex needs x and y"); continue; }
            PendingVertex v;
            if (!parse_fx(tok[1], v.pos.x) || !parse_fx(tok[2], v.pos.y)) {
                err(lineno, "bad vertex coordinates");
                continue;
            }
            for (size_t i = 3; i < tok.size(); ++i) {
                if (tok[i] == "wall") v.neighbor = NO_SECTOR;
                else if (tok[i] == "neighbor") {
                    if (i + 1 >= tok.size()) { err(lineno, "neighbor needs a sector index"); break; }
                    v.neighbor = std::atoi(tok[++i].c_str());
                } else if (tok[i] == "climbable") v.flags |= side_flag::CLIMBABLE;
                else if (tok[i] == "grapple") v.flags |= side_flag::GRAPPLE;
                else if (tok[i] == "blocking") v.flags |= side_flag::BLOCKING;
                else err(lineno, "unknown edge attribute '%s'", tok[i].c_str());
            }
            verts.push_back(v);
        } else if (tok[0] == "end") {
            if (!in_sector) { err(lineno, "'end' outside a sector"); continue; }
            in_sector = false;

            if (verts.size() < 3) {
                err(sector_start_line, "sector has %zu vertices; need at least 3", verts.size());
                continue;
            }

            std::vector<fx2> pts;
            pts.reserve(verts.size());
            for (const auto& v : verts) pts.push_back(v.pos);

            const u32 si = store.add_sector(pts.data(), static_cast<u32>(pts.size()),
                                            floor_z, ceil_z, tag);
            if (si >= MAX_MAP_SECTORS) {
                // add_sector triangulates, so this is where a self-intersecting
                // or degenerate polygon is caught -- at bake time, which is the
                // whole reason the design puts triangulation at edit time.
                err(sector_start_line, "sector rejected: not a simple polygon, or a capacity limit was hit");
                continue;
            }
            if (store.view().sector(si).tri_count == 0) {
                err(sector_start_line, "sector could not be triangulated (self-intersecting?)");
                continue;
            }

            for (size_t i = 0; i < verts.size(); ++i) {
                const u16 a = static_cast<u16>(i);
                const u16 b = static_cast<u16>((i + 1) % verts.size());
                if (!store.add_side(si, a, b, verts[i].neighbor, verts[i].flags)) {
                    err(sector_start_line, "could not add side %u", a);
                }
            }
        } else if (tok[0] == "spawn") {
            if (tok.size() < 5) { err(lineno, "spawn needs a tag and x y z"); continue; }
            fx x, y, z;
            if (!parse_fx(tok[2], x) || !parse_fx(tok[3], y) || !parse_fx(tok[4], z)) {
                err(lineno, "bad spawn coordinates");
                continue;
            }
            if (!store.add_spawn(tok[1].c_str(), static_cast<u32>(tok[1].size()), fx3{x, y, z})) {
                err(lineno, "too many spawns, or tag too long");
            }
        } else {
            err(lineno, "unknown directive '%s'", tok[0].c_str());
        }
    }
    std::fclose(in);

    if (in_sector) err(lineno, "unterminated sector (missing 'end')");

    // Neighbour indices are resolved only now, because a sector may legally
    // name one that appears later in the file.
    const MapData m = store.view();
    for (u32 i = 0; i < m.side_count; ++i) {
        const i32 n = m.side(i).neighbor;
        if (n != NO_SECTOR && (n < 0 || static_cast<u32>(n) >= m.sector_count)) {
            std::fprintf(stderr, "side %u names sector %d, which does not exist\n", i, n);
            ++g_errors;
        }
    }

    if (g_errors > 0) {
        std::fprintf(stderr, "%d error(s); not writing output\n", g_errors);
        return 1;
    }

    std::vector<u8> out(1 << 20);
    ByteWriter w(out.data(), out.size());
    if (!save_map(store, w)) {
        std::fprintf(stderr, "map too large to serialise\n");
        return 1;
    }

    std::FILE* of = std::fopen(argv[2], "wb");
    if (!of) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    std::fwrite(out.data(), 1, w.size(), of);
    std::fclose(of);

    std::printf("%s: %u sectors, %u points, %u sides, %u triangles, %u spawns -> %zu bytes (hash %016llX)\n",
                argv[2], m.sector_count, m.point_count, m.side_count,
                m.tri_index_count / 3, store.spawn_count(), w.size(),
                static_cast<unsigned long long>(store.hash()));
    return 0;
}
