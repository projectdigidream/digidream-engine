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
//     vertex 0  40  neighbor 3 trigger        -- crossing it is reported to Lua
//     vertex 0  40  wall usable                -- reaching for it is reported to Lua
//
//     body radius 0.333 height 1.167          -- the body this map is built for
//     script ../scripts/doom.lua              -- a script this map needs to work
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
//   sector floor 8 ceil 40 tag 2
//     vertex 0  0   wall
//     vertex 40 0   wall
//     vertex 40 40  wall
//     vertex 0  40  wall
//     hole                      # a pit: the floor does NOT exist here
//       vertex 14 14  neighbor 3
//       vertex 26 14  neighbor 3
//       vertex 26 26  neighbor 3
//       vertex 14 26  neighbor 3
//     end
//   end
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
#include <algorithm>
#include <cmath>
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

// --- camera path arc length (design §4.1) -----------------------------------
//
// A PORT of client/src/spline.js, and it has to stay one.
//
// The camera rides a centripetal Catmull-Rom through a path's points, and that
// curve is the client's: core does not implement it, deliberately, because a
// camera is never hashed and a curve in fixed point would buy nothing. But the
// SERVER needs one number out of it -- how long the path is -- because the
// engine has no completion event and a cutscene therefore times its own
// flythrough in advance, exactly as COND.sector_at_rest times a door.
//
// So the length is computed here, once, and baked into the map. If this drifts
// from spline.js every cinematic ends at the wrong moment and nothing says why,
// which is why ALPHA and SAMPLES below are written as the same numbers that
// file exports and why a test compares the two answers on a real map.
//
// Doubles, because tools/ is outside the no-floats guard and this is authoring
// arithmetic: the result is quantised to fx on the way into the file, and the
// thing it is used for is measured in whole ticks.
constexpr double PATH_ALPHA = 0.5;
constexpr int    PATH_SAMPLES_PER_SEGMENT = 64;
constexpr double PATH_KNOT_FLOOR = 1e-9;

struct Dv3 { double x, y, z; };

// fx <-> double, spelled out here rather than added to fx itself. The no-floats
// guard keeps core/include integer-only on purpose, and a to_double() sitting
// on fx is an invitation to use it somewhere that matters. Q32.32, so the scale
// is 2^32 exactly and both directions are a multiply.
constexpr double FX_SCALE = 4294967296.0;   // fx::ONE

double fx_to_d(fx v) { return static_cast<double>(v.raw) / FX_SCALE; }

fx d_to_fx(double v) {
    // Round rather than truncate, and clamp: a length is a magnitude, and a
    // path long enough to overflow i64 at 2^-32 resolution is four billion
    // units of camera track.
    const double scaled = v * FX_SCALE;
    if (scaled <= -9.2233720368547758e18) return fx::min();
    if (scaled >= 9.2233720368547758e18) return fx::max();
    return fx::from_raw(static_cast<i64>(std::llround(scaled)));
}

Dv3 dv(fx3 p) {
    return Dv3{fx_to_d(p.x), fx_to_d(p.y), fx_to_d(p.z)};
}

double dv_dist(const Dv3& a, const Dv3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Dv3 dv_lerp_knot(const Dv3& a, const Dv3& b, double ta, double tb, double t) {
    const double d = tb - ta;
    const double w = d == 0.0 ? 0.0 : (t - ta) / d;
    return Dv3{a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w, a.z + (b.z - a.z) * w};
}

// Barry-Goldman, matching spline.js. Written as three rounds of interpolation
// rather than a basis matrix because the matrix form is only valid for UNIFORM
// knots -- using it with a centripetal knot sequence is the usual way a
// "centripetal" implementation turns out not to be one.
Dv3 path_eval(const Dv3& p0, const Dv3& p1, const Dv3& p2, const Dv3& p3,
              double t0, double t1, double t2, double t3, double t) {
    const Dv3 a1 = dv_lerp_knot(p0, p1, t0, t1, t);
    const Dv3 a2 = dv_lerp_knot(p1, p2, t1, t2, t);
    const Dv3 a3 = dv_lerp_knot(p2, p3, t2, t3, t);
    const Dv3 b1 = dv_lerp_knot(a1, a2, t0, t2, t);
    const Dv3 b2 = dv_lerp_knot(a2, a3, t1, t3, t);
    return dv_lerp_knot(b1, b2, t1, t2, t);
}

// Arc length of the curve through `pts`, by chords over a uniform sampling in
// the curve parameter -- the same approximation, at the same resolution, that
// the client's arc-length table is built from.
double path_curve_length(const std::vector<fx3>& pts, bool loop) {
    const size_t n = pts.size();
    if (n < 2) return 0.0;
    const bool looped = loop && n >= 3;
    const size_t segs = looped ? n : n - 1;

    std::vector<Dv3> p;
    p.reserve(n);
    for (const fx3& q : pts) p.push_back(dv(q));

    double total = 0.0;
    for (size_t s = 0; s < segs; ++s) {
        Dv3 c0, c1, c2, c3;
        if (looped) {
            c0 = p[(s + n - 1) % n]; c1 = p[s % n];
            c2 = p[(s + 1) % n];     c3 = p[(s + 2) % n];
        } else {
            // Reflected phantom control points at the ends, as spline.js does:
            // p(-1) = p0 + (p0 - p1). Duplicating the endpoint instead gives a
            // zero knot delta and a curve that begins by standing still.
            c0 = (s == 0) ? Dv3{2 * p[0].x - p[1].x, 2 * p[0].y - p[1].y, 2 * p[0].z - p[1].z}
                          : p[s - 1];
            c1 = p[s];
            c2 = p[s + 1];
            c3 = (s + 2 < n) ? p[s + 2]
                             : Dv3{2 * p[n - 1].x - p[n - 2].x,
                                   2 * p[n - 1].y - p[n - 2].y,
                                   2 * p[n - 1].z - p[n - 2].z};
        }
        const double k01 = std::max(std::pow(dv_dist(c0, c1), PATH_ALPHA), PATH_KNOT_FLOOR);
        const double k12 = std::max(std::pow(dv_dist(c1, c2), PATH_ALPHA), PATH_KNOT_FLOOR);
        const double k23 = std::max(std::pow(dv_dist(c2, c3), PATH_ALPHA), PATH_KNOT_FLOOR);
        const double t0 = 0.0, t1 = k01, t2 = t1 + k12, t3 = t2 + k23;

        Dv3 prev = path_eval(c0, c1, c2, c3, t0, t1, t2, t3, t1);
        for (int i = 1; i <= PATH_SAMPLES_PER_SEGMENT; ++i) {
            const double u = static_cast<double>(i) / PATH_SAMPLES_PER_SEGMENT;
            const Dv3 q = path_eval(c0, c1, c2, c3, t0, t1, t2, t3, t1 + (t2 - t1) * u);
            total += dv_dist(prev, q);
            prev = q;
        }
    }
    return total;
}

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

    // Scripts the map declared, resolved and copied after the map itself is
    // known to be good -- there is no point writing a script beside an output
    // that is never going to exist.
    struct PendingScript { std::string path; int line; };
    std::vector<PendingScript> declared_scripts;

    std::vector<PendingVertex> verts;
    // Holes are collected while the sector is open and applied at `end`,
    // because a hole's points must go in after the outer loop's and the outer
    // loop is not committed until then.
    std::vector<std::vector<PendingVertex>> pending_holes;
    bool in_hole = false;
    bool in_sector = false;
    bool in_rail = false;
    int  rail_start_line = 0;
    u32  cur_rail = 0;
    u32  rail_waypoints = 0;

    // A camera path is buffered rather than streamed into the store, because
    // its curve_length is a property of ALL its points and the store wants that
    // number when the path is created. So the block is collected and committed
    // at `end`.
    bool in_path = false;
    int  path_start_line = 0;
    u16  path_tag = 0;
    u8   path_flags = 0;
    std::vector<fx3> path_pts;
    std::vector<i32> path_anchors;

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
            pending_holes.clear();
            in_hole = false;
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
        } else if (tok[0] == "hole") {
            if (!in_sector) { err(lineno, "hole outside a sector"); continue; }
            if (in_hole) { err(lineno, "hole inside a hole; missing 'end'?"); continue; }
            in_hole = true;
            pending_holes.push_back({});
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
                else if (tok[i] == "trigger") v.flags |= side_flag::TRIGGER;
                else if (tok[i] == "usable") v.flags |= side_flag::USABLE;
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
            if (in_hole) pending_holes.back().push_back(v);
            else verts.push_back(v);
        } else if (tok[0] == "end" && in_hole) {
            in_hole = false;
            if (pending_holes.back().size() < 3) {
                err(lineno, "hole has %zu vertices; need at least 3",
                    pending_holes.back().size());
                pending_holes.pop_back();
            }
        } else if (tok[0] == "end" && in_path) {
            in_path = false;
            if (path_pts.size() < 2) {
                err(path_start_line, "path has %zu point(s); need at least 2", path_pts.size());
                continue;
            }
            // The curve's length, not the polyline's, computed HERE because the
            // curve belongs to the client and the number belongs to the server.
            // See path_curve_length above for why that split exists.
            const double len = path_curve_length(path_pts, (path_flags & path_flag::LOOP) != 0);
            const u32 pi = store.add_path(path_tag, path_flags, d_to_fx(len));
            if (pi >= MAX_MAP_PATHS) {
                err(path_start_line, "too many paths (max %u)", MAX_MAP_PATHS);
                continue;
            }
            for (size_t i = 0; i < path_pts.size(); ++i) {
                if (!store.add_path_point(pi, path_pts[i], path_anchors[i])) {
                    err(path_start_line, "could not add path point (too many, or path out of order)");
                    break;
                }
            }
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

            // Holes before sides: a hole's points must be contiguous with the
            // sector's outer loop, and add_side adds none.
            for (const auto& hv : pending_holes) {
                std::vector<fx2> hp;
                hp.reserve(hv.size());
                for (const auto& v : hv) hp.push_back(v.pos);
                if (!store.add_hole(si, hp.data(), static_cast<u32>(hp.size()))) {
                    err(sector_start_line,
                        "hole rejected: not a simple polygon inside its sector, "
                        "or a capacity limit was hit");
                }
            }

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

            // A hole's edges are sides too -- they are what a body walking into
            // a pit collides with, and what the renderer draws facing into it.
            // Their local indices continue past the outer loop, which is how
            // add_hole laid the points out.
            u16 local = static_cast<u16>(verts.size());
            for (const auto& hv : pending_holes) {
                const u16 base = local;
                for (size_t i = 0; i < hv.size(); ++i) {
                    const u16 a2 = static_cast<u16>(base + i);
                    const u16 b2 = static_cast<u16>(base + (i + 1) % hv.size());
                    if (!store.add_side(si, a2, b2, hv[i].neighbor, hv[i].flags)) {
                        err(sector_start_line, "could not add hole side %u", a2);
                        continue;
                    }
                    store.side_render_mut(next_side++) = hv[i].render;
                }
                local = static_cast<u16>(local + hv.size());
            }
        } else if (tok[0] == "script") {
            // script <path>
            //
            // The path is relative to THIS FILE, so a map can name a shared
            // library that lives beside it or a directory up; what gets baked
            // is the bare file name, and mapc copies the file next to its
            // output. That split is deliberate: an author writes the path that
            // makes sense from where they are standing, and the server only
            // ever resolves a name inside the map's own directory.
            if (in_sector || in_rail) { err(lineno, "script inside a sector or rail"); continue; }
            if (tok.size() != 2) { err(lineno, "script needs exactly one path"); continue; }
            declared_scripts.push_back({tok[1], lineno});
        } else if (tok[0] == "body") {
            // body radius <r> height <h>
            //
            // The proportions this map is built for, as a fact about the
            // GEOMETRY. A Doom map converted at 1/48 has 56-unit doorways,
            // which is 1.167 here against the engine's default 2.0 body -- so
            // without this the player simply does not fit through the map's
            // own doors, and the only cure was two flags nobody remembers.
            if (in_sector || in_rail) { err(lineno, "body inside a sector or rail"); continue; }
            // Seeded with what the map already has, so `body height 1.167`
            // alone changes the height and leaves the radius alone -- the
            // store's setter assigns both and has no opinion about defaults.
            fx radius = store.body_radius();
            fx height = store.body_height();
            bool said = false;
            for (size_t i = 1; i < tok.size(); ++i) {
                if (tok[i] == "radius" && i + 1 < tok.size()) {
                    if (!parse_fx(tok[++i], radius)) err(lineno, "bad body radius");
                    said = true;
                } else if (tok[i] == "height" && i + 1 < tok.size()) {
                    if (!parse_fx(tok[++i], height)) err(lineno, "bad body height");
                    said = true;
                } else {
                    err(lineno, "unknown body attribute '%s'", tok[i].c_str());
                    break;
                }
            }
            if (!said) { err(lineno, "body needs radius and/or height"); continue; }
            if (radius <= fx::zero() || height <= fx::zero()) {
                err(lineno, "body radius and height must both be positive");
                continue;
            }
            store.set_body(radius, height);
        } else if (tok[0] == "texscale") {
            // How many texture pixels cover one world unit. A fact about the
            // geometry, like `body`: a map converted at 1/48 from a game whose
            // textures are one pixel per map unit wants 48, and saying nothing
            // keeps the renderer's own 32.
            fx px{};
            if (tok.size() < 2 || !parse_fx(tok[1], px) || px <= fx::zero()) {
                err(lineno, "texscale needs a positive number of pixels per unit");
            } else {
                store.set_texture_scale(px);
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
        } else if (tok[0] == "path") {
            // A CAMERA path (design §4.1), and deliberately not a rail: a rail
            // is what §7.4 carries an entity along, and nothing about junctions
            // or wire-hanging means anything to a camera. See map.hpp.
            if (in_sector) { err(lineno, "path inside a sector; missing 'end'?"); continue; }
            if (in_rail || in_path) { err(lineno, "path inside a path or rail; missing 'end'?"); continue; }
            in_path = true;
            path_start_line = lineno;
            path_tag = 0;
            path_flags = 0;
            path_pts.clear();
            path_anchors.clear();
            for (size_t i = 1; i < tok.size(); ++i) {
                if (tok[i] == "loop") { path_flags |= path_flag::LOOP; }
                else if (tok[i] == "tag" && i + 1 < tok.size()) {
                    path_tag = static_cast<u16>(std::atoi(tok[++i].c_str()));
                } else {
                    err(lineno, "unknown path attribute '%s'", tok[i].c_str());
                }
            }
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
            // One keyword for both blocks. A path's points ARE waypoints -- the
            // same anchored, sector-relative point, and the same word an author
            // already knows -- so which container is open decides where it goes
            // rather than making them learn a second spelling.
            if (!in_rail && !in_path) { err(lineno, "waypoint outside a rail or path"); continue; }
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
            if (in_path) {
                if (path_pts.size() >= MAX_MAP_PATH_POINTS) {
                    err(lineno, "too many path points (max %u)", MAX_MAP_PATH_POINTS);
                    continue;
                }
                path_pts.push_back(fx3{x, y, z});
                path_anchors.push_back(anchor);
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
    if (in_path) err(lineno, "unterminated path (missing 'end')");

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
                // Every loop of b, its holes included. A hole's rim is as much
                // the sector's boundary as its outer edge is -- and a pit's
                // corners lie exactly on it, so checking only the outer loop
                // reported a sector sitting in another's hole as overlapping
                // it.
                bool on_boundary = false;
                u32 loop_first = sb.first_point;
                u32 loop_len = sb.point_count;
                for (u16 loop = 0; loop <= sb.hole_count && !on_boundary; ++loop) {
                    if (loop > 0) {
                        const PolyLoop& L = m.hole(sb.first_hole + loop - 1);
                        loop_first = sb.first_point + L.first;
                        loop_len = L.count;
                    }
                    for (u32 j = 0; j < loop_len && !on_boundary; ++j) {
                        const fx2 p0 = m.point(loop_first + j);
                        const fx2 p1 = m.point(loop_first + (j + 1) % loop_len);
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

    // The mesh must cover the footprint, exactly.
    //
    // For a SIMPLE polygon, "triangulate() returned Ok" already means this: the
    // ear clipper's own tests are exact and core/tests/test_triangulate.cpp
    // pins the area. The Doom converter is what makes it worth re-asking here.
    // It emits WEAKLY simple loops -- outlines that touch themselves at a
    // vertex, which is how Doom expresses a pond meeting the edge of a
    // courtyard -- and for those, "found an ear every time" is a weaker
    // statement than "meshed the region I meant". Nothing else downstream would
    // ever notice the difference: a sector with floor across a gap looks
    // entirely normal until somebody walks out over it.
    //
    // Both sides are 128-bit integer sums over the same points, so this is ==
    // and not a tolerance. A sector with holes is measured against outer minus
    // holes, which is the area its floor actually has.
    //
    // It has never fired -- not on DOOM2, DOOM1, or 600k fuzzed weakly-simple
    // polygons, where the clipper either refused outright or got the area
    // exactly right. That is the result it is here to keep true, cheaply, on
    // every map baked from here on.

    for (u32 i = 0; i < m.sector_count && g_errors == 0; ++i) {
        const Sector& sec = m.sector(i);
        if (sec.tri_count == 0) continue;

        i128 mesh = 0;
        for (u16 t = 0; t < sec.tri_count; ++t) {
            // first_tri indexes the flat index array, not the triangle list.
            const u32 base = sec.first_tri + static_cast<u32>(t) * 3;
            const fx2 a = m.point(m.tris[base + 0]);
            const fx2 b = m.point(m.tris[base + 1]);
            const fx2 c = m.point(m.tris[base + 2]);
            mesh += (static_cast<i128>(b.x.raw) - a.x.raw) * (static_cast<i128>(c.y.raw) - a.y.raw) -
                    (static_cast<i128>(b.y.raw) - a.y.raw) * (static_cast<i128>(c.x.raw) - a.x.raw);
        }

        i128 poly = geom::polygon_area_2x(&m.points[sec.first_point], sec.point_count);
        if (poly < 0) poly = -poly;
        for (u16 h = 0; h < sec.hole_count; ++h) {
            const PolyLoop& L = m.hole(sec.first_hole + h);
            i128 ha = geom::polygon_area_2x(&m.points[sec.first_point + L.first], L.count);
            if (ha < 0) ha = -ha;
            poly -= ha;
        }

        if (mesh != poly) {
            const double scale = 4294967296.0 * 4294967296.0;
            std::fprintf(stderr,
                "sector %u: its mesh covers %.3f square units but its footprint is %.3f.\n"
                "The triangulation succeeded and is of the wrong shape, which happens when a\n"
                "loop touches itself: the clipper bridges the pinch and fills what is not\n"
                "floor. Split the loop into separate sectors at the point where it touches.\n",
                i, static_cast<double>(mesh) / (2.0 * scale),
                static_cast<double>(poly) / (2.0 * scale));
            ++g_errors;
        }
    }

    if (g_errors > 0) {
        std::fprintf(stderr, "%d error(s); not writing output\n", g_errors);
        return 1;
    }

    // Bake the declared script NAMES before serialising. Only the bare file
    // name goes in: the path in the source says where to find it now, and the
    // baked name says what the server should look for beside the map.
    for (const PendingScript& ps : declared_scripts) {
        const size_t ns = ps.path.find_last_of("/\\");
        const std::string base = ns == std::string::npos ? ps.path : ps.path.substr(ns + 1);
        if (!store.add_script(base.c_str(), static_cast<u32>(base.size()))) {
            err(ps.line, "cannot declare script '%s' (bad name, or more than %u)",
                base.c_str(), MAX_MAP_SCRIPTS);
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

    // The map's declared scripts travel with it.
    //
    // A baked map and the Lua that gives it doors are one deliverable, and the
    // server resolves each declared name inside the map's own directory -- so
    // compiling a map somewhere else has to bring them along, or the map loads
    // perfectly and its switches do nothing.
    //
    // This replaces a blanket "copy any .lua sitting next to the source", which
    // guessed. The map says what it needs now.
    {
        const std::string dst_path = argv[2];
        const size_t slash = dst_path.find_last_of("/\\");
        const std::string dst_dir = slash == std::string::npos ? std::string(".")
                                                              : dst_path.substr(0, slash);
        const std::string src_path = argv[1];
        const size_t sslash = src_path.find_last_of("/\\");
        const std::string src_dir = sslash == std::string::npos ? std::string(".")
                                                                : src_path.substr(0, sslash);

        for (const PendingScript& ps : declared_scripts) {
            const size_t ns = ps.path.find_last_of("/\\");
            const std::string base = ns == std::string::npos ? ps.path : ps.path.substr(ns + 1);
            if (!geom::script_name_is_safe(base.c_str(), static_cast<u32>(base.size()))) {
                err(ps.line, "'%s' is not a usable script file name", base.c_str());
                continue;
            }

            const std::string from = ps.path.empty() || ps.path[0] == '/'
                                         ? ps.path : src_dir + "/" + ps.path;
            std::FILE* sf = std::fopen(from.c_str(), "rb");
            if (!sf) {
                // An error, not a warning. A map that declares a script it does
                // not ship is a map that will load and not work, and finding
                // that out at bake time is the whole point of declaring it.
                err(ps.line, "cannot open script %s", from.c_str());
                continue;
            }

            const std::string to = dst_dir + "/" + base;
            if (to != from) {
                std::vector<char> buf(1 << 16);
                if (std::FILE* df = std::fopen(to.c_str(), "wb")) {
                    size_t n;
                    while ((n = std::fread(buf.data(), 1, buf.size(), sf)) > 0) {
                        std::fwrite(buf.data(), 1, n, df);
                    }
                    std::fclose(df);
                } else {
                    err(ps.line, "cannot write %s", to.c_str());
                }
            }
            std::fclose(sf);
            std::printf("%s: script %s\n", argv[2], base.c_str());
        }
    }
    // The copies are part of the deliverable, so a missing one fails the bake
    // the same way a bad sector does -- and takes the output with it, because
    // a .dnpm whose scripts are absent loads perfectly and does nothing.
    if (g_errors > 0) {
        std::remove(argv[2]);
        std::fprintf(stderr, "%d error(s); output removed\n", g_errors);
        return 1;
    }

    std::printf("%s: %u sectors, %u points, %u sides, %u triangles, %u spawns, "
                "%u rails (%u waypoints, %u junctions), %u camera paths (%u points) "
                "-> %zu bytes (hash %016llX)\n",
                argv[2], m.sector_count, m.point_count, m.side_count,
                m.tri_index_count / 3, store.spawn_count(),
                store.rail_count(), store.waypoint_count(), store.rail_link_count(),
                store.path_count(), store.path_point_count(),
                w.size(), static_cast<unsigned long long>(store.hash()));

    // The baked arc length, per path, because it is the one number in the file
    // an author cannot check by looking at what they typed -- it is the length
    // of a CURVE through their points, always longer than the polyline, and it
    // is what `cam_path_speed` is divided into to time a cutscene.
    for (u32 i = 0; i < store.path_count(); ++i) {
        const Path& p = m.path(i);
        std::printf("  path tag %u: %u points%s, curve length %.4f\n",
                    p.tag, p.point_count,
                    (p.flags & path_flag::LOOP) ? ", loop" : "",
                    fx_to_d(p.curve_length));
    }
    return 0;
}
