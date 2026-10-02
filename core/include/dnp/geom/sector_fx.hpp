#pragma once
//
// SECTOR EFFECTS: rain, snow, dust, embers and fog (docs/SECTOR_FX.md).
//
// Rendering attributes of a sector, authored as `fx` lines in a map's source
// and carried in the baked file from version 14. Nothing in the simulation
// reads one. They live in core only because the map file does, and because
// this table is THE definition: tools/mapc parses against it, the loader
// validates against it, and the client is handed names and defaults through
// the wasm bindings rather than keeping a copy of its own to drift.
//
#include <cstdint>
#include "dnp/math/fixed.hpp"

namespace dnp::geom {

enum class FxKind : u8 { Rain = 1, Snow = 2, Dust = 3, Embers = 4, Fog = 5 };
inline constexpr u8 FX_KIND_COUNT = 5;

inline const char* fx_kind_name(u8 k) noexcept {
    switch (k) {
        case 1: return "rain";
        case 2: return "snow";
        case 3: return "dust";
        case 4: return "embers";
        case 5: return "fog";
        default: return nullptr;
    }
}

// Which kinds take a key: one bit per kind, bit (kind - 1).
inline constexpr u8 FX_PARTICLES = 0x0F;   // rain, snow, dust, embers
inline constexpr u8 FX_FOG = 0x10;
inline constexpr u8 FX_ANY = FX_PARTICLES | FX_FOG;

// A key's value is an fx, except `color`, which is 0xRRGGBB in the raw bits.
// The defaults are per kind -- rain, snow, dust, embers, fog -- in THOUSANDTHS
// (integers: core holds no floating point, docs/physics-contract.md), except a
// colour's, which is the colour. FX_NONE marks a key the kind does not take;
// `top` -1000 is "the ceiling".
struct FxKeyDef {
    const char* name;
    u8          kinds;
    bool        is_color;
    i64         defaults[FX_KIND_COUNT];
};

inline constexpr i64 FX_NONE = INT64_MIN;
#define DNP_FX_N FX_NONE

// ORDER IS THE FORMAT: a key's id is its index here plus one. Append only.
inline constexpr FxKeyDef FX_KEYS[] = {
    //  name       kinds          color   rain      snow      dust      embers    fog
    { "density",  FX_ANY,        false, { 8000,     4000,     1500,     600,      150 } },
    { "speed",    FX_PARTICLES,  false, { 9000,     800,      150,      600,      DNP_FX_N } },
    { "windx",    FX_PARTICLES,  false, { 0,        0,        0,        0,        DNP_FX_N } },
    { "windy",    FX_PARTICLES,  false, { 0,        0,        0,        0,        DNP_FX_N } },
    { "sway",     FX_PARTICLES,  false, { 0,        250,      100,      150,      DNP_FX_N } },
    { "size",     FX_PARTICLES,  false, { 12,       40,       15,       30,       DNP_FX_N } },
    { "length",   FX_PARTICLES,  false, { 350,      0,        0,        0,        DNP_FX_N } },
    { "color",    FX_ANY,        true,  { 0xaab4c8, 0xffffff, 0xc8b48c, 0xff8a2a, 0x8090a0 } },
    { "alpha",    FX_ANY,        false, { 350,      900,      400,      900,      1000 } },
    { "bottom",   FX_ANY,        false, { 0,        0,        0,        0,        0 } },
    { "top",      FX_ANY,        false, { -1000,    -1000,    -1000,    -1000,    -1000 } },
    { "glow",     FX_ANY,        false, { 0,        0,        0,        1000,     0 } },
    { "falloff",  FX_FOG,        false, { DNP_FX_N, DNP_FX_N, DNP_FX_N, DNP_FX_N, 0 } },
    { "noise",    FX_FOG,        false, { DNP_FX_N, DNP_FX_N, DNP_FX_N, DNP_FX_N, 0 } },
    { "drift",    FX_FOG,        false, { DNP_FX_N, DNP_FX_N, DNP_FX_N, DNP_FX_N, 300 } },
};
#undef DNP_FX_N
inline constexpr u8 FX_KEY_COUNT = static_cast<u8>(sizeof(FX_KEYS) / sizeof(FX_KEYS[0]));

inline constexpr u32 MAX_MAP_FX = 256;
inline constexpr u8  MAX_FX_PARAMS = FX_KEY_COUNT;

/** Does `kind` take key id `key` (1-based)? */
inline bool fx_takes(u8 kind, u8 key) noexcept {
    if (kind < 1 || kind > FX_KIND_COUNT || key < 1 || key > FX_KEY_COUNT) return false;
    return (FX_KEYS[key - 1].kinds >> (kind - 1)) & 1;
}

struct FxParam {
    u8  key = 0;      // 1-based index into FX_KEYS
    i64 raw = 0;      // fx raw, or 0xRRGGBB for a colour
};

struct SectorFx {
    u32     sector = 0;
    u8      kind = 0;
    u8      count = 0;
    FxParam params[MAX_FX_PARAMS]{};
};

}  // namespace dnp::geom
