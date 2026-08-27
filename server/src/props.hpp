#pragma once
//
// The prop catalogue, as the SERVER reads it.
//
// Half of a file the client reads the other half of (see assets/props.txt). The
// simulation needs to know how big a thing is; how it is drawn is the
// renderer's business and is deliberately not parsed here -- a `sprite` line
// changing does not restart the server, and a `radius` changing does not
// rebuild the assets.
//
// Line-based rather than JSON so that both readers stay small and obviously
// correct. A parser is a place bugs live, and this one has two implementations
// in two languages; the format is chosen so neither needs to be clever.
//
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "dnp/math/fixed.hpp"
#include "dnp/sim/world.hpp"
#include "lua/world_binding.hpp"

namespace dnp::server {

struct PropDef {
    sim::PropKind kind = 0;
    std::string   name;
    fx            radius = fx::from_frac(1, 2);
    fx            height = fx::from_int(1);
};

// The binding layer's view of it. An interface rather than a direct dependency
// so that server/src/lua/ does not have to know how a catalogue is stored -- and
// so a test can hand the bindings a two-entry one without a file.
class PropCatalogue : public lua::PropCatalogueView {
public:
    u16 kind_by_name(const char* name, size_t len) const override {
        const PropDef* d = find(name, len);
        return d ? d->kind : 0;
    }
    fx radius_of(u16 kind) const override {
        const PropDef* d = find(static_cast<sim::PropKind>(kind));
        return d ? d->radius : fx::from_frac(1, 2);
    }
    bool empty() const override { return defs_.empty(); }
    fx height_of(u16 kind) const override {
        const PropDef* d = find(static_cast<sim::PropKind>(kind));
        return d ? d->height : fx::from_int(1);
    }

    // Returns false only if the file cannot be opened. A malformed LINE is
    // reported and skipped: a catalogue is edited by hand and by artists, and
    // refusing to start a server because one entry has a typo would make the
    // whole file feel dangerous to touch.
    bool load(const char* path) {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        defs_.clear();

        char line[512];
        int lineno = 0;
        int bad = 0;
        while (std::fgets(line, sizeof(line), f)) {
            ++lineno;
            std::vector<std::string> tok;
            for (char* p = std::strtok(line, " \t\r\n"); p; p = std::strtok(nullptr, " \t\r\n")) {
                if (p[0] == '#') break;   // comment to end of line
                tok.emplace_back(p);
            }
            if (tok.empty()) continue;
            // Anything that is not ours belongs to the renderer. Skipped in
            // silence, on purpose: the client's half of this file is none of
            // the server's business and warning about it would train operators
            // to ignore warnings.
            if (tok[0] != "prop") continue;

            if (tok.size() < 3) {
                std::fprintf(stderr, "[props] %s:%d: prop needs an id and a name\n", path, lineno);
                ++bad;
                continue;
            }
            PropDef d;
            d.kind = static_cast<sim::PropKind>(std::atoi(tok[1].c_str()));
            d.name = tok[2];
            if (d.kind == 0) {
                // Kind 0 is "no prop", which is what an unparsable id becomes.
                std::fprintf(stderr, "[props] %s:%d: id must be a positive number\n", path, lineno);
                ++bad;
                continue;
            }
            bool ok = true;
            for (size_t i = 3; i + 1 < tok.size(); i += 2) {
                if (tok[i] == "radius")      d.radius = parse_fx(tok[i + 1], ok);
                else if (tok[i] == "height") d.height = parse_fx(tok[i + 1], ok);
                else {
                    std::fprintf(stderr, "[props] %s:%d: unknown attribute '%s'\n",
                                 path, lineno, tok[i].c_str());
                    ok = false;
                }
            }
            if (!ok || d.radius <= fx::zero() || d.height <= fx::zero()) {
                std::fprintf(stderr, "[props] %s:%d: '%s' has no usable size\n",
                             path, lineno, d.name.c_str());
                ++bad;
                continue;
            }
            if (find(d.kind) != nullptr) {
                std::fprintf(stderr, "[props] %s:%d: id %u is already '%s'\n",
                             path, lineno, d.kind, find(d.kind)->name.c_str());
                ++bad;
                continue;
            }
            defs_.push_back(d);
        }
        std::fclose(f);
        std::fprintf(stderr, "[props] %s: %zu prop kinds%s\n", path, defs_.size(),
                     bad ? " (with errors above)" : "");
        return true;
    }

    const PropDef* find(sim::PropKind kind) const {
        for (const PropDef& d : defs_) if (d.kind == kind) return &d;
        return nullptr;
    }

    // By NAME, which is what a script uses. A number in a script is a number
    // somebody has to look up, and looking it up in a file the script does not
    // mention is how a map ends up spawning the wrong thing.
    const PropDef* find(const char* name, size_t len) const {
        for (const PropDef& d : defs_) {
            if (d.name.size() == len && std::memcmp(d.name.data(), name, len) == 0) return &d;
        }
        return nullptr;
    }

    size_t size() const { return defs_.size(); }
    const PropDef& at(size_t i) const { return defs_[i]; }

private:
    // Decimal, parsed in integer arithmetic. The same reason mapc parses its own
    // numbers: strtod would put an IEEE double between the file and Q32.32, and
    // "0.35" is not exactly representable in either -- but only one of them is
    // allowed to decide how it rounds.
    static fx parse_fx(const std::string& s, bool& ok) {
        const char* p = s.c_str();
        bool neg = false;
        if (*p == '-') { neg = true; ++p; }
        i64 whole = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p++ - '0'); any = true; }
        i64 num = 0, den = 1;
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9' && den < 1000000000LL) {
                num = num * 10 + (*p++ - '0');
                den *= 10;
                any = true;
            }
        }
        if (!any || *p != '\0') { ok = false; return fx::zero(); }
        fx v = fx::from_int(whole) + fx::from_frac(num, den);
        return neg ? -v : v;
    }

    std::vector<PropDef> defs_;
};

}  // namespace dnp::server
