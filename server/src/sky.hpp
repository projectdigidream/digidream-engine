#pragma once
//
// THE SKY a level is drawn under (docs/SKY.md): read from a module's
// `sky` lines and from dnp.sky, both in one words-form so the two cannot
// disagree about what a sky is.
//
//   strip <IMAGE> [repeat N] [yaw T] [height H]   DOOM's: wrapped round the horizon
//   panorama <IMAGE> [yaw T]                      one equirectangular picture
//   cube <E> <W> <N> <S> <UP> <DOWN> [yaw T]       six pictures (docs/SKY.md 2.3)
//   none                                          no sky (the clear colour)
//
// Pictures are TEXTURE names, served from /textures/ like any wall's. Header
// only and allocation-light: the server reads it at start-up and when a script
// asks, never per tick.
//
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "dnp/proto/messages.hpp"

namespace dnp::server {

inline constexpr u8 SKY_FIELD_REPEAT = 1;
inline constexpr u8 SKY_FIELD_YAW    = 2;
inline constexpr u8 SKY_FIELD_HEIGHT = 3;

// A picture name: what a texture may be called (letters, digits, _ - . and /
// for a module's own folders), never `..`.
inline bool sky_image_ok(const std::string& n) {
    if (n.empty() || n.size() > 64 || n.find("..") != std::string::npos) return false;
    for (char c : n) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.' || c == '/';
        if (!ok) return false;
    }
    return true;
}

// Words to a sky. False with `err` set on anything it does not understand:
// a sky half-read would be drawn wrong, and wrong is worse than said.
inline bool parse_sky(const std::vector<std::string>& w, proto::SkySet& out, std::string& err) {
    proto::SkySet s;
    if (w.empty()) { err = "a sky needs a kind: strip, panorama, cube or none"; return false; }
    const std::string& kind = w[0];
    size_t pics = 0;
    if (kind == "none") { s.kind = proto::SkyKind::None; pics = 0; }
    else if (kind == "strip") { s.kind = proto::SkyKind::Strip; pics = 1; }
    else if (kind == "panorama") { s.kind = proto::SkyKind::Panorama; pics = 1; }
    else if (kind == "cube") { s.kind = proto::SkyKind::Cube; pics = 6; }
    else { err = "unknown sky kind '" + kind + "' (strip, panorama, cube or none)"; return false; }
    if (w.size() < 1 + pics) {
        err = kind + " needs " + std::to_string(pics) + " picture(s)";
        return false;
    }
    for (size_t i = 0; i < pics; ++i) {
        const std::string& n = w[1 + i];
        if (!sky_image_ok(n)) { err = "'" + n + "' is not a texture name"; return false; }
        proto::UiText& t = s.image[i];
        t.len = static_cast<u16>(n.size());
        std::memcpy(t.text, n.data(), n.size());
    }
    s.images = static_cast<u8>(pics);
    for (size_t i = 1 + pics; i < w.size(); i += 2) {
        const std::string& key = w[i];
        u8 id = 0;
        if (key == "repeat" && s.kind == proto::SkyKind::Strip) id = SKY_FIELD_REPEAT;
        else if (key == "height" && s.kind == proto::SkyKind::Strip) id = SKY_FIELD_HEIGHT;
        else if (key == "yaw" && s.kind != proto::SkyKind::None) id = SKY_FIELD_YAW;
        else { err = "'" + key + "' is not a setting of a " + kind + " sky"; return false; }
        if (i + 1 >= w.size()) { err = key + " needs a number"; return false; }
        char* end = nullptr;
        const double v = std::strtod(w[i + 1].c_str(), &end);
        if (end == w[i + 1].c_str() || *end != '\0' || !std::isfinite(v) || std::fabs(v) > 1e6) {
            err = key + " needs a number, not '" + w[i + 1] + "'";
            return false;
        }
        if (id == SKY_FIELD_REPEAT && !(v > 0)) { err = "repeat must be above 0"; return false; }
        if (id == SKY_FIELD_HEIGHT && !(v > 0)) { err = "height must be above 0"; return false; }
        if (s.count >= proto::SKY_MAX_FIELDS) { err = "too many settings"; return false; }
        s.fields[s.count].id = id;
        s.fields[s.count].milli = static_cast<i32>(std::lround(v * 1000.0));
        ++s.count;
    }
    out = s;
    return true;
}

// A sky back to its words: what a level's saved state keeps, read again by
// parse_sky. Settings are written in thousandths' exact decimal.
inline std::string sky_text(const proto::SkySet& s) {
    static const char* KINDS[] = {"none", "strip", "panorama", "cube"};
    const u8 k = static_cast<u8>(s.kind);
    std::string out = KINDS[k <= 3 ? k : 0];
    for (u8 i = 0; i < s.images && i < proto::SKY_MAX_IMAGES; ++i)
        out += " " + std::string(s.image[i].text, s.image[i].len);
    for (u16 i = 0; i < s.count && i < proto::SKY_MAX_FIELDS; ++i) {
        const char* key = s.fields[i].id == SKY_FIELD_REPEAT ? "repeat"
                        : s.fields[i].id == SKY_FIELD_YAW    ? "yaw"
                        : s.fields[i].id == SKY_FIELD_HEIGHT ? "height" : nullptr;
        if (key == nullptr) continue;
        char num[32];
        const i32 m = s.fields[i].milli;
        std::snprintf(num, sizeof(num), "%s%d.%03d", m < 0 ? "-" : "", std::abs(m) / 1000, std::abs(m) % 1000);
        out += std::string(" ") + key + " " + num;
    }
    return out;
}

inline std::vector<std::string> sky_words(const std::string& text) {
    std::vector<std::string> w;
    std::istringstream in(text);
    std::string t;
    while (in >> t) {
        if (t[0] == '#') break;
        w.push_back(t);
    }
    return w;
}

// A module's skies: `sky <level|default> <words...>`. The FIRST module to name
// a key wins, as for every asset (the same order the devserver serves files in).
struct SkyEntry {
    std::string    level;     // a level name, or "default"
    proto::SkySet  sky;
};

// The sky for a level: its own name, then `<prefix>_<name>`'s <name> (what
// dw_map05 shares with map05, the music index's rule), then the default.
inline const proto::SkySet* sky_for(const std::vector<SkyEntry>& skies, const std::string& level) {
    const auto find = [&](const std::string& k) -> const proto::SkySet* {
        for (const auto& e : skies) if (e.level == k) return &e.sky;
        return nullptr;
    };
    if (const auto* s = find(level)) return s;
    const size_t cut = level.rfind('_');
    if (cut != std::string::npos) if (const auto* s = find(level.substr(cut + 1))) return s;
    return find("default");
}

}  // namespace dnp::server
