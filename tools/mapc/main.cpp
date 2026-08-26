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
//   sector floor 0 ceil 40 tag 1 light 200 floortex FLOOR4 ceiltex CEIL3 skirt 2
//     vertex 0  0   wall           tex BRICK1
//     vertex 40 0   neighbor 1     upper STEP2 lower STEP1 unpeg-lower
//     vertex 40 40  wall           tex BRICK1 xoff 8
//     vertex 0  40  wall climbable tex VINE1
//   end
//
// `open` in place of `wall` or `neighbor` makes an edge the sector ENDS at: an
// entity that walks off it leaves the map and falls. `skirt N` then draws the
// platform's edge hanging N units below its floor, so a floating sector reads
// as a slab rather than a sheet of paper.
//
// Rendering attributes (light, the three texture fields, offsets and pegging)
// are OPTIONAL everywhere. A map that names none of them compiles and plays
// identically -- they are read by the renderer and by nothing else.
//
//   spawn start 10 10 2
//
//   light 20 40 10 color 255 200 120 radius 18 intensity 1.0
//
//   rail tag 1 loop
//     waypoint 10 10 4
//     waypoint 30 10 4
//   end
//   junction 0 1 to 1 0
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

// Intern a texture name, reporting the two ways it can fail rather than
// returning a silent NO_TEXTURE -- an untextured wall and a misspelt texture
// look identical in the output, and only one of them is what the author meant.
u16 intern(int line, MapStorage& store, const std::string& name) {
    if (name.size() > MAX_TEXTURE_NAME) {
        err(line, "texture name '%s' is longer than %u characters",
            name.c_str(), MAX_TEXTURE_NAME);
        return NO_TEXTURE;
    }
    const u16 id = store.intern_texture(name.data(), static_cast<u32>(name.size()));
    if (id == NO_TEXTURE) err(line, "too many distinct textures (max %u)", MAX_MAP_TEXTURES);
    return id;
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

    // Rendering, carried alongside but kept apart: these end up in the store's
    // parallel render arrays, never in the Side the collision code reads.
    SideRender render{};
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
    bool in_rail = false;
    int  rail_start_line = 0;
    u32  cur_rail = 0;
    u32  rail_waypoints = 0;

    // Junctions are collected and resolved at the end, because a rail may name
    // one declared later in the file -- the same reason sector neighbours are.
    struct PendingLink { u32 rail, wp, to_rail, to_wp; int line; };
    std::vector<PendingLink> links;
    fx floor_z{}, ceil_z{};
    u16 tag = 0;
    SectorRender sector_render{};
    fx thickness{};
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
            sector_render = SectorRender{};
            thickness = fx::zero();
            sector_start_line = lineno;
            for (size_t i = 1; i + 1 < tok.size(); i += 2) {
                if (tok[i] == "floor") { if (!parse_fx(tok[i + 1], floor_z)) err(lineno, "bad floor '%s'", tok[i + 1].c_str()); }
                else if (tok[i] == "ceil") { if (!parse_fx(tok[i + 1], ceil_z)) err(lineno, "bad ceil '%s'", tok[i + 1].c_str()); }
                else if (tok[i] == "tag") { tag = static_cast<u16>(std::atoi(tok[i + 1].c_str())); }
                else if (tok[i] == "light") {
                    const int v = std::atoi(tok[i + 1].c_str());
                    if (v < 0 || v > 255) err(lineno, "light must be 0..255, got '%s'", tok[i + 1].c_str());
                    else sector_render.light_level = static_cast<u8>(v);
                }
                else if (tok[i] == "thickness" || tok[i] == "skirt") {
                    // `skirt` is kept as a spelling because it says what you
                    // SEE; the field says what you hit. They are one number.
                    if (!parse_fx(tok[i + 1], thickness)) {
                        err(lineno, "bad thickness '%s'", tok[i + 1].c_str());
                    }
                }
                else if (tok[i] == "floortex") sector_render.floor_tex = intern(lineno, store, tok[i + 1]);
                else if (tok[i] == "ceiltex") sector_render.ceil_tex = intern(lineno, store, tok[i + 1]);
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
                } else if (tok[i] == "open") {
                    // An edge the sector simply ends at. `wall` and `open` are
                    // both explicit because the difference is invisible in the
                    // geometry and total in play: one is a barrier, the other
                    // is a fall.
                    v.neighbor = NO_SECTOR;
                    v.flags |= side_flag::LEDGE;
                } else if (tok[i] == "climbable") v.flags |= side_flag::CLIMBABLE;
                else if (tok[i] == "grapple") v.flags |= side_flag::GRAPPLE;
                else if (tok[i] == "blocking") v.flags |= side_flag::BLOCKING;
                // Doom's three texture fields. `tex` is the middle one, which is
                // the only one a solid wall has, so the common case stays one
                // word rather than making every author write `middle`.
                else if (tok[i] == "tex" || tok[i] == "middle") {
                    if (i + 1 >= tok.size()) { err(lineno, "%s needs a texture name", tok[i].c_str()); break; }
                    v.render.middle = intern(lineno, store, tok[++i]);
                } else if (tok[i] == "upper") {
                    if (i + 1 >= tok.size()) { err(lineno, "upper needs a texture name"); break; }
                    v.render.upper = intern(lineno, store, tok[++i]);
                } else if (tok[i] == "lower") {
                    if (i + 1 >= tok.size()) { err(lineno, "lower needs a texture name"); break; }
                    v.render.lower = intern(lineno, store, tok[++i]);
                } else if (tok[i] == "xoff") {
                    if (i + 1 >= tok.size() || !parse_fx(tok[i + 1], v.render.x_offset)) {
                        err(lineno, "xoff needs a number"); break;
                    }
                    ++i;
                } else if (tok[i] == "yoff") {
                    if (i + 1 >= tok.size() || !parse_fx(tok[i + 1], v.render.y_offset)) {
                        err(lineno, "yoff needs a number"); break;
                    }
                    ++i;
                } else if (tok[i] == "unpeg-upper") v.render.peg_flags |= peg_flag::UPPER_UNPEGGED;
                else if (tok[i] == "unpeg-lower") v.render.peg_flags |= peg_flag::LOWER_UNPEGGED;
                else err(lineno, "unknown edge attribute '%s'", tok[i].c_str());
            }
            verts.push_back(v);
        } else if (tok[0] == "end" && in_rail) {
            in_rail = false;
            // Two waypoints is the minimum that describes a line. One is a
            // point, and every query below would silently treat it as a rail of
            // zero segments -- which is a level-authoring mistake worth naming
            // rather than a shape worth supporting.
            if (rail_waypoints < 2) {
                err(rail_start_line, "rail has %u waypoint(s); need at least 2", rail_waypoints);
            }
        } else if (tok[0] == "end") {
            if (!in_sector) { err(lineno, "'end' outside a sector or rail"); continue; }
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

            store.sector_render_mut(si) = sector_render;
            store.sector_mut(si).thickness = thickness;

            // The index the NEXT side will land at. Deliberately not
            // sector.first_side: that field is assigned lazily by the first
            // add_side, so before the loop below it still reads zero -- and
            // every sector would write its wall textures over sector 0's. That
            // is exactly the trap add_sector's own first_side handling was
            // fixed for once already, which is why this reads a count that is
            // true at the moment it is read instead of a field that becomes
            // true later.
            u32 next_side = store.view().side_count;
            for (size_t i = 0; i < verts.size(); ++i) {
                const u16 a = static_cast<u16>(i);
                const u16 b = static_cast<u16>((i + 1) % verts.size());
                if (!store.add_side(si, a, b, verts[i].neighbor, verts[i].flags)) {
                    err(sector_start_line, "could not add side %u", a);
                    continue;
                }
                store.side_render_mut(next_side++) = verts[i].render;
            }
        } else if (tok[0] == "light") {
            // light <x> <y> <z> [color R G B] [radius N] [intensity N]
            if (in_sector || in_rail) { err(lineno, "light inside a sector or rail"); continue; }
            if (tok.size() < 4) { err(lineno, "light needs x y z"); continue; }
            MapLight l;
            if (!parse_fx(tok[1], l.pos.x) || !parse_fx(tok[2], l.pos.y) ||
                !parse_fx(tok[3], l.pos.z)) {
                err(lineno, "bad light position");
                continue;
            }
            l.radius = fx::from_int(12);
            l.intensity = fx::one();
            for (size_t i = 4; i < tok.size(); ++i) {
                if (tok[i] == "color" && i + 3 < tok.size()) {
                    l.r = static_cast<u8>(std::atoi(tok[++i].c_str()));
                    l.g = static_cast<u8>(std::atoi(tok[++i].c_str()));
                    l.b = static_cast<u8>(std::atoi(tok[++i].c_str()));
                } else if (tok[i] == "radius" && i + 1 < tok.size()) {
                    if (!parse_fx(tok[++i], l.radius)) err(lineno, "bad light radius");
                } else if (tok[i] == "intensity" && i + 1 < tok.size()) {
                    if (!parse_fx(tok[++i], l.intensity)) err(lineno, "bad light intensity");
                } else {
                    err(lineno, "unknown light attribute '%s'", tok[i].c_str());
                    break;
                }
            }
            if (!store.add_light(l)) err(lineno, "too many lights (max %u)", MAX_MAP_LIGHTS);
        } else if (tok[0] == "rail" || tok[0] == "wire") {
            if (in_sector) { err(lineno, "rail inside a sector; missing 'end'?"); continue; }
            if (in_rail) { err(lineno, "rail inside a rail; missing 'end'?"); continue; }
            in_rail = true;
            rail_start_line = lineno;
            rail_waypoints = 0;
            u16 rtag = 0;
            u8 flags = tok[0] == "wire" ? rail_flag::WIRE : 0;
            for (size_t i = 1; i < tok.size(); ++i) {
                if (tok[i] == "loop") { flags |= rail_flag::LOOP; }
                else if (tok[i] == "tag" && i + 1 < tok.size()) {
                    rtag = static_cast<u16>(std::atoi(tok[++i].c_str()));
                } else {
                    err(lineno, "unknown rail attribute '%s'", tok[i].c_str());
                }
            }
            cur_rail = store.add_rail(rtag, flags);
            if (cur_rail >= MAX_MAP_RAILS) err(lineno, "too many rails");
        } else if (tok[0] == "waypoint") {
            if (!in_rail) { err(lineno, "waypoint outside a rail"); continue; }
            if (tok.size() < 4) { err(lineno, "waypoint needs x y z"); continue; }
            fx x, y, z;
            if (!parse_fx(tok[1], x) || !parse_fx(tok[2], y) || !parse_fx(tok[3], z)) {
                err(lineno, "bad waypoint coordinates");
                continue;
            }

            // The anchor sector, INFERRED from the point it sits over unless
            // the author says otherwise -- and then written into the file
            // explicitly. Design §3.4 wants waypoints to be sector-relative;
            // making the author state that for every point would be a chore
            // they would get wrong, and making the RUNTIME infer it would mean
            // a rail's anchor changed the moment a sector moved under it.
            // Deciding once, at bake time, is the only version of this that is
            // both convenient and stable.
            i32 anchor = find_sector(store.view(), fx2{x, y});
            for (size_t i = 4; i < tok.size(); ++i) {
                if (tok[i] == "anchor" && i + 1 < tok.size()) {
                    const std::string& a = tok[++i];
                    anchor = a == "none" ? NO_SECTOR : std::atoi(a.c_str());
                } else {
                    err(lineno, "unknown waypoint attribute '%s'", tok[i].c_str());
                }
            }
            if (anchor != NO_SECTOR &&
                (anchor < 0 || static_cast<u32>(anchor) >= store.view().sector_count)) {
                err(lineno, "waypoint anchor %d is not a sector", anchor);
                continue;
            }
            if (!store.add_waypoint(cur_rail, fx3{x, y, z}, anchor)) {
                err(lineno, "could not add waypoint (too many, or rail out of order)");
                continue;
            }
            ++rail_waypoints;
        } else if (tok[0] == "junction") {
            // junction <rail> <waypoint> to <rail> <waypoint>
            if (tok.size() < 6) {
                err(lineno, "junction needs: <rail> <waypoint> to <rail> <waypoint>");
                continue;
            }
            PendingLink l;
            l.rail = static_cast<u32>(std::atoi(tok[1].c_str()));
            l.wp = static_cast<u32>(std::atoi(tok[2].c_str()));
            l.to_rail = static_cast<u32>(std::atoi(tok[4].c_str()));
            l.to_wp = static_cast<u32>(std::atoi(tok[5].c_str()));
            l.line = lineno;
            links.push_back(l);
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
    if (in_rail) err(lineno, "unterminated rail (missing 'end')");

    for (const PendingLink& l : links) {
        if (!store.add_rail_link(l.rail, static_cast<u16>(l.wp), l.to_rail,
                                 static_cast<u16>(l.to_wp))) {
            err(l.line, "junction names a rail or waypoint that does not exist");
        }
    }

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

    // Overlapping footprints.
    //
    // A sector is a 2D footprint, and find_sector returns the FIRST one
    // containing a point -- so where two footprints overlap, which sector you
    // are in depends on the order they were declared in. Everything downstream
    // inherits that: which floor you stand on, which ceiling crushes you, which
    // light level shades the ground.
    //
    // Stacked sectors are a design feature (§3.1) that is NOT implemented, and
    // this is what an unimplemented feature looks like when a map uses it
    // anyway. Rejecting is far better than the alternative, which is a tower
    // you can stand on until you walk two units north and drop through it --
    // a bug I authored into maps/void.map and then had reported back to me.
    //
    // The test is one vertex of A strictly inside B. That is exact enough and
    // has no false positives on legal maps: adjacent sectors SHARE edges, so
    // their vertices lie ON each other's boundaries rather than inside them.
    for (u32 a = 0; a < m.sector_count && g_errors == 0; ++a) {
        const Sector& sa = m.sector(a);
        for (u32 b = 0; b < m.sector_count; ++b) {
            if (a == b) continue;
            const Sector& sb = m.sector(b);

            u32 inside = 0;
            u32 first_inside = 0;
            fx2 witness{};
            for (u16 k = 0; k < sa.point_count; ++k) {
                const fx2 v = m.point(sa.first_point + k);
                if (!point_in_sector(m, b, v)) continue;

                // On a shared boundary is not inside. An EXACT test in 128-bit
                // integers, not distance_sq_to_segment: that projects through a
                // division, so a point lying exactly on a segment comes back a
                // few ulp off zero and every abutting sector reads as
                // overlapping.
                bool on_boundary = false;
                for (u16 j = 0; j < sb.point_count && !on_boundary; ++j) {
                    const fx2 p0 = m.point(sb.first_point + j);
                    const fx2 p1 = m.point(sb.first_point + (j + 1) % sb.point_count);
                    const i128 cross =
                        static_cast<i128>(p1.x.raw - p0.x.raw) * (v.y.raw - p0.y.raw) -
                        static_cast<i128>(p1.y.raw - p0.y.raw) * (v.x.raw - p0.x.raw);
                    if (cross != 0) continue;
                    const i64 lox = p0.x.raw < p1.x.raw ? p0.x.raw : p1.x.raw;
                    const i64 hix = p0.x.raw < p1.x.raw ? p1.x.raw : p0.x.raw;
                    const i64 loy = p0.y.raw < p1.y.raw ? p0.y.raw : p1.y.raw;
                    const i64 hiy = p0.y.raw < p1.y.raw ? p1.y.raw : p0.y.raw;
                    if (v.x.raw >= lox && v.x.raw <= hix &&
                        v.y.raw >= loy && v.y.raw <= hiy) on_boundary = true;
                }
                if (on_boundary) continue;

                if (inside == 0) { first_inside = k; witness = v; }
                ++inside;
            }

            if (inside == 0) continue;

            // NESTED is fine: a pillar standing in a hall, a pit sunk into a
            // floor. geom::find_sector answers with the smallest sector
            // containing a point, so the inner one wins and the answer does not
            // depend on which was declared first.
            if (inside == sa.point_count) continue;

            // PARTIAL overlap is fine when the two are separated in HEIGHT.
            //
            // That is a bridge over a path, and geom::find_sector_at resolves it
            // without ambiguity: which floor applies is decided by where a
            // body's feet are. The condition is that the lower sector's ceiling
            // is at or below the upper one's slab -- a real gap, with the upper
            // sector's underside closing the lower one off rather than hanging
            // inside its headroom.
            const Sector& lo = sa.floor_z <= sb.floor_z ? sa : sb;
            const Sector& hi = sa.floor_z <= sb.floor_z ? sb : sa;
            if (lo.ceil_z <= hi.floor_z - hi.thickness) continue;

            // Otherwise the two share space a body could occupy, and which
            // sector it is in has no answer that geometry can give. Every
            // consequence follows from that one: which floor it stands on,
            // which ceiling crushes it, which light level shades the ground.
            std::fprintf(stderr,
                "sector %u partially overlaps sector %u: %u of its %u corners are inside,\n"
                "starting at corner %u (%.3f, %.3f), and the two share space vertically.\n"
                "\n"
                "Footprints may overlap in two cases: NESTED (a pillar inside a hall), or\n"
                "SEPARATED IN HEIGHT (a bridge over a path, where the lower sector's ceiling\n"
                "is at or below the upper one's underside). This is neither: the lower\n"
                "ceiling is %.3f and the upper underside is %.3f.\n",
                a, b, inside, sa.point_count, first_inside,
                static_cast<double>(witness.x.raw) / 4294967296.0,
                static_cast<double>(witness.y.raw) / 4294967296.0,
                static_cast<double>(lo.ceil_z.raw) / 4294967296.0,
                static_cast<double>((hi.floor_z - hi.thickness).raw) / 4294967296.0);
            ++g_errors;
            break;
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

    std::printf("%s: %u sectors, %u points, %u sides, %u triangles, %u spawns, "
                "%u rails (%u waypoints, %u junctions) -> %zu bytes (hash %016llX)\n",
                argv[2], m.sector_count, m.point_count, m.side_count,
                m.tri_index_count / 3, store.spawn_count(),
                store.rail_count(), store.waypoint_count(), store.rail_link_count(),
                w.size(), static_cast<unsigned long long>(store.hash()));
    return 0;
}
