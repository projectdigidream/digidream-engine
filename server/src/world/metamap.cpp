#include "world/metamap.hpp"

#include <cstdlib>

namespace dnp::server::world {

namespace {

// One line's worth of whitespace-separated words, with `#` starting a comment.
//
// The same shape mapc's reader has, deliberately: an author editing a metamap
// has just been editing a .map, and two text formats in one project that
// disagree about what a comment is are two formats to remember.
u32 split(const std::string& line, std::string out[4]) {
    u32 n = 0;
    size_t i = 0;
    while (i < line.size() && n < 4) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' ||
                                   line[i] == '\r')) ++i;
        if (i >= line.size() || line[i] == '#') break;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' &&
               line[i] != '\r' && line[i] != '#') ++i;
        out[n++] = line.substr(start, i - start);
    }
    return n;
}

// An integer, refusing anything that is not entirely one.
//
// `std::atoi` answers 0 for "north", which would silently place a map at the
// origin and take a neighbour's place there. The whole token has to be the
// number or it is not a number.
bool parse_int(const std::string& t, i32& out) {
    if (t.empty()) return false;
    size_t i = (t[0] == '-' || t[0] == '+') ? 1 : 0;
    if (i >= t.size()) return false;
    for (size_t k = i; k < t.size(); ++k) {
        if (t[k] < '0' || t[k] > '9') return false;
    }
    out = static_cast<i32>(std::strtol(t.c_str(), nullptr, 10));
    return true;
}

// A world distance: an integer, or a decimal with up to four places. Exact,
// through fx::from_frac, because a grid pitch that is a hair out puts a seam a
// hair out at every cell and the error accumulates across the world.
bool parse_fx(const std::string& t, fx& out) {
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[i] == '-' || t[i] == '+') { neg = t[i] == '-'; ++i; }
    i64 whole = 0, frac = 0, scale = 1;
    bool any = false;
    for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i) {
        whole = whole * 10 + (t[i] - '0');
        if (whole > (i64(1) << 30)) return false;
        any = true;
    }
    if (i < t.size() && t[i] == '.') {
        ++i;
        for (int d = 0; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i, ++d) {
            if (d >= 9) return false;
            frac = frac * 10 + (t[i] - '0');
            scale *= 10;
            any = true;
        }
    }
    if (!any || i != t.size()) return false;
    const i64 num = whole * scale + frac;
    out = fx::from_frac(neg ? -num : num, scale);
    return true;
}

std::string at_line(u32 line, const std::string& what) {
    return "line " + std::to_string(line) + ": " + what;
}

}  // namespace

const MetaCell* MetaMap::find(const std::string& name) const noexcept {
    for (const MetaCell& c : cells) if (c.map == name) return &c;
    return nullptr;
}

MetaMap parse_metamap(const char* text, size_t len) {
    MetaMap m;
    std::string line;
    u32 lineno = 0;

    const auto finish = [&]() {
        ++lineno;
        std::string tok[4];
        const u32 n = split(line, tok);
        line.clear();
        if (n == 0) return;

        if (tok[0] == "cell") {
            if (n < 4) { m.problems.push_back(at_line(lineno, "cell needs x, y and a map name")); return; }
            MetaCell c;
            if (!parse_int(tok[1], c.x) || !parse_int(tok[2], c.y)) {
                m.problems.push_back(at_line(lineno, "cell coordinates must be whole numbers"));
                return;
            }
            c.map = tok[3];
            // ONE MAP IN ONE PLACE, and one place holding one map. Both are
            // authoring mistakes with no sensible reading: a map in two cells
            // has no answer to "which neighbours", and two maps in one cell
            // means whichever loaded last silently wins.
            for (const MetaCell& e : m.cells) {
                if (e.map == c.map) {
                    m.problems.push_back(at_line(lineno, "'" + c.map + "' is already placed"));
                    return;
                }
                if (e.x == c.x && e.y == c.y) {
                    m.problems.push_back(at_line(lineno, "cell " + tok[1] + " " + tok[2] +
                                                 " already holds '" + e.map + "'"));
                    return;
                }
            }
            m.cells.push_back(c);
            return;
        }

        if (tok[0] == "radius") {
            i32 r = 0;
            if (n < 2 || !parse_int(tok[1], r) || r < 0 ||
                static_cast<u32>(r) > MAX_METAMAP_RADIUS) {
                m.problems.push_back(at_line(lineno, "radius must be 0.." +
                                             std::to_string(MAX_METAMAP_RADIUS)));
                return;
            }
            m.radius = static_cast<u32>(r);
            return;
        }

        if (tok[0] == "pitch") {
            if (n < 3 || !parse_fx(tok[1], m.pitch_x) || !parse_fx(tok[2], m.pitch_y)) {
                m.problems.push_back(at_line(lineno, "pitch needs two world distances"));
                return;
            }
            if (m.pitch_x <= fx::zero() || m.pitch_y <= fx::zero()) {
                m.problems.push_back(at_line(lineno, "pitch must be greater than zero"));
                m.pitch_x = fx::zero();
                m.pitch_y = fx::zero();
            }
            return;
        }

        m.problems.push_back(at_line(lineno, "unknown directive '" + tok[0] + "'"));
    };

    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\n') finish();
        else line.push_back(text[i]);
    }
    if (!line.empty()) finish();
    return m;
}

fx2 cell_origin(const LevelCell& c, fx pitch_x, fx pitch_y) noexcept {
    if (!c.on_grid) return fx2{};
    return fx2{pitch_x * fx::from_int(c.x), pitch_y * fx::from_int(c.y)};
}

i32 cell_index(fx v, fx pitch) noexcept {
    if (pitch.raw <= 0) return 0;
    const i64 q = v.raw / pitch.raw;
    const i64 r = v.raw % pitch.raw;
    return static_cast<i32>((r != 0 && v.raw < 0) ? q - 1 : q);
}

void residency(const std::vector<LevelCell>& cells,
               const std::vector<bool>& occupied,
               u32 radius,
               std::vector<bool>& want) {
    want.assign(cells.size(), false);
    const u32 n = static_cast<u32>(cells.size());
    for (u32 i = 0; i < n; ++i) {
        if (i >= occupied.size() || !occupied[i]) continue;
        // Somebody is in it, so it stays whatever else is true -- including a
        // level the metamap does not mention, which is the case that keeps an
        // unplaced map playable.
        want[i] = true;
        if (!cells[i].on_grid) continue;
        for (u32 j = 0; j < n; ++j) {
            if (!cells[j].on_grid) continue;
            const i64 dx = static_cast<i64>(cells[j].x) - cells[i].x;
            const i64 dy = static_cast<i64>(cells[j].y) - cells[i].y;
            const i64 r = static_cast<i64>(radius);
            // A SQUARE window, not a circle: the diagonal neighbour of the cell
            // you are standing in is one step away through a corner, and a
            // player walking diagonally reaches it as fast as they reach the
            // one beside you.
            if (dx <= r && dx >= -r && dy <= r && dy >= -r) want[j] = true;
        }
    }
}

}  // namespace dnp::server::world
