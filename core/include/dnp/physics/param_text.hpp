#pragma once
//
// A movement parameter written as TEXT: `jump_impulse 9.5`, `hover_enabled on`.
//
// What a module's `player` lines say (docs/PLAYER.md), read into the same
// replicated table every other path writes. In the core, beside the table,
// because a tool that writes these lines (the studio) must agree with the
// server reading them on exactly the same names and the same bits.
//
// Decimals are converted EXACTLY, with no float in the path: `0.1` is
// from_frac(1, 10), the same raw value on every platform. A float-parsed
// parameter would be the one input to a deterministic simulation that two
// machines could disagree on.
//
#include <cstring>

#include "dnp/physics/params_table.hpp"

namespace dnp::physics {

struct ParamText {
    u16  id = 0;
    i64  raw = 0;
};

// Parse "<decimal>" into fx raw: an optional sign, digits, an optional point
// and up to 9 more digits. False on anything else (exponents, junk, empty).
inline bool parse_fx_decimal(const char* s, i64& raw) noexcept {
    if (s == nullptr || *s == '\0') return false;
    bool neg = false;
    if (*s == '-' || *s == '+') { neg = *s == '-'; ++s; }
    i64 whole = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        if (whole > 1000000000LL) return false;          // past any sane parameter
        whole = whole * 10 + (*s - '0');
        ++s; ++digits;
    }
    i64 frac = 0, scale = 1;
    if (*s == '.') {
        ++s;
        while (*s >= '0' && *s <= '9') {
            if (scale >= 1000000000LL) return false;     // 9 places is finer than 2^-32 matters
            frac = frac * 10 + (*s - '0');
            scale *= 10;
            ++s; ++digits;
        }
    }
    if (digits == 0 || *s != '\0') return false;
    const fx v = fx::from_frac(whole * scale + frac, scale);
    raw = neg ? -v.raw : v.raw;
    return true;
}

// `name value` to an id and a raw value, by the parameter's own type:
//   Fx   a decimal (units are the table's: units, seconds, turns per second)
//   Bool on/off, true/false, yes/no, 1/0
//   U8   a whole number 0..255
// `err` says what was wrong; false leaves `out` alone.
inline bool parse_param_text(const char* name, const char* value, ParamText& out, const char*& err) noexcept {
    const i32 id = param_id_by_name(name);
    if (id < 0) { err = "no movement parameter by that name"; return false; }
    const ParamType t = PARAM_TABLE[id].type;
    i64 raw = 0;
    if (t == ParamType::Bool) {
        const char* yes[] = {"on", "true", "yes", "1"};
        const char* no[]  = {"off", "false", "no", "0"};
        bool found = false;
        for (const char* y : yes) if (std::strcmp(value, y) == 0) { raw = 1; found = true; }
        for (const char* n : no)  if (std::strcmp(value, n) == 0) { raw = 0; found = true; }
        if (!found) { err = "a switch is on or off"; return false; }
    } else if (t == ParamType::U8) {
        i64 v = 0;
        const char* p = value;
        if (*p == '\0') { err = "a count is a whole number 0..255"; return false; }
        while (*p >= '0' && *p <= '9' && v <= 255) { v = v * 10 + (*p - '0'); ++p; }
        if (*p != '\0' || v > 255) { err = "a count is a whole number 0..255"; return false; }
        raw = v;
    } else {
        if (!parse_fx_decimal(value, raw)) { err = "a number is a decimal, like 9 or 0.25"; return false; }
    }
    out.id = static_cast<u16>(id);
    out.raw = raw;
    return true;
}

}  // namespace dnp::physics
