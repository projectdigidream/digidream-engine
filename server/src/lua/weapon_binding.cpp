//
// The `dnp.weapon*` bindings: weapons.lua's validated tables compiled into the
// core's WeaponTable (dnp/sim/weapon.hpp), and a player's weapon stepped by the
// core instead of by Lua (docs/WEAPON_PREDICTION.md, phase B).
//
// THE SPLIT. weapons.lua stays the authority on the language: it loads and
// validates the tables, keeps the inventory a script sees, and carries out
// everything that touches the world. This file is the bridge: it compiles the
// table once, and each tick weapons.lua hands the inventory down, steps the
// machine here, and takes the inventory and the events back. The machine
// itself is the one a predicting client runs.
//
// ONE TABLE PER lua_State, as for the mob graph: one VM, one weapons.load. A
// game VM's table serves every level its players walk into.
//
#include "lua/world_binding.hpp"
#include "lua/fx_binding.hpp"
#include "dnp/sim/weapon.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dnp::server::lua {

namespace {

using sim::WAct;
using sim::WCond;

struct Compiled {
    sim::WeaponTable table;
    // The random stream's seed. The table's own hash, so a client holding the
    // same table draws the same spread without being told anything else.
    u64 seed = 0;
};

std::map<lua_State*, std::unique_ptr<Compiled>>& compiled() {
    static std::map<lua_State*, std::unique_ptr<Compiled>> c;
    return c;
}

Compiled* compiled_of(lua_State* L) {
    auto it = compiled().find(L);
    return it == compiled().end() ? nullptr : it->second.get();
}

// The vocabulary's names, in the enums' order. weapons.lua names actions by
// string, and an event goes back to it the same way.
const char* const ACT_NAME[] = {
    "consume", "set_var", "add_var", "goto", "set_anim", "switch_if_empty", "impulse",
};
const char* const REPORTED_NAME[] = {
    "hitscan", "launch", "damage_area", "telegraph", "vfx", "sound",
};
static_assert(sizeof(ACT_NAME) / sizeof(ACT_NAME[0]) == static_cast<size_t>(WAct::Impulse) + 1,
              "ACT_NAME must name every action the core performs, in order");
static_assert(sizeof(REPORTED_NAME) / sizeof(REPORTED_NAME[0]) ==
                  static_cast<size_t>(WAct::Sound) - static_cast<size_t>(WAct::Hitscan) + 1,
              "REPORTED_NAME must name every reported action, in order");

const char* const COND_NAME[] = {
    "always", "button_pressed", "button_released", "button_held", "held_for",
    "has_ammo", "no_ammo", "phase_elapsed", "chance", "var_at_least", "var_is",
    "grounded", "moving", "falling",
};
static_assert(sizeof(COND_NAME) / sizeof(COND_NAME[0]) == static_cast<size_t>(WCond::Falling) + 1,
              "COND_NAME must name every condition, in order");

const char* act_name(WAct a) {
    const u8 v = static_cast<u8>(a);
    if (v <= static_cast<u8>(WAct::Impulse)) return ACT_NAME[v];
    return REPORTED_NAME[v - static_cast<u8>(WAct::Hitscan)];
}

bool act_by_name(const char* s, WAct& out) {
    for (u8 i = 0; i < sizeof(ACT_NAME) / sizeof(ACT_NAME[0]); ++i) {
        if (std::strcmp(s, ACT_NAME[i]) == 0) { out = static_cast<WAct>(i); return true; }
    }
    for (u8 i = 0; i < sizeof(REPORTED_NAME) / sizeof(REPORTED_NAME[0]); ++i) {
        if (std::strcmp(s, REPORTED_NAME[i]) == 0) {
            out = static_cast<WAct>(static_cast<u8>(WAct::Hitscan) + i);
            return true;
        }
    }
    return false;
}

bool cond_by_name(const char* s, WCond& out) {
    for (u8 i = 0; i < sizeof(COND_NAME) / sizeof(COND_NAME[0]); ++i) {
        if (std::strcmp(s, COND_NAME[i]) == 0) { out = static_cast<WCond>(i); return true; }
    }
    return false;
}

// The string keys of the table at `t`, SORTED: an index is a position in this
// order, and pairs() has none.
std::vector<std::string> sorted_keys(lua_State* L, int t) {
    std::vector<std::string> out;
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    std::sort(out.begin(), out.end());
    return out;
}

i32 index_in(const std::vector<std::string>& v, const char* s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return static_cast<i32>(i);
    return -1;
}

// Reads fields off one Lua table, and remembers the first thing it could not
// take, with where it was. Every reader leaves the stack as it found it.
struct Reader {
    lua_State* L;
    std::string err;
    // For a launch: the missile definitions by name (a stack index, 0 for
    // none) and the catalogue that names their bodies.
    int missiles = 0;
    const PropCatalogueView* props = nullptr;

    bool fail(const std::string& where, const std::string& what) {
        if (err.empty()) err = where + ": " + what;
        return false;
    }

    // Pushes t[name]; true if it is not nil. The caller pops.
    bool push(int t, const char* name) {
        lua_getfield(L, t, name);
        if (lua_isnil(L, -1)) return false;
        return true;
    }

    bool int_at(int t, const char* name, const std::string& where, i64 lo, i64 hi,
                i64& out, bool* present = nullptr) {
        const bool has = push(t, name);
        if (present) *present = has;
        if (!has) { lua_pop(L, 1); return true; }
        if (!lua_isnumber(L, -1)) { lua_pop(L, 1); return fail(where, std::string("`") + name + "` is not a number"); }
        const lua_Number v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (v != static_cast<lua_Number>(static_cast<i64>(v)) || v < lo || v > hi) {
            return fail(where, std::string("`") + name + "` is out of the range the core holds");
        }
        out = static_cast<i64>(v);
        return true;
    }

    bool fx_at(int t, const char* name, const std::string& where, fx& out, bool* present = nullptr) {
        const bool has = push(t, name);
        if (present) *present = has;
        if (!has) { lua_pop(L, 1); return true; }
        bool ok = true;
        if (is_fx(L, -1)) out = check_fx(L, -1);
        else if (lua_isnumber(L, -1) && lua_tonumber(L, -1) == static_cast<lua_Number>(static_cast<i64>(lua_tonumber(L, -1)))) {
            out = fx::from_int(static_cast<i64>(lua_tonumber(L, -1)));
        } else ok = fail(where, std::string("`") + name + "` is not a fixed-point value");
        lua_pop(L, 1);
        return ok;
    }

    // A string field, or "" when absent.
    std::string str_at(int t, const char* name) {
        std::string s;
        if (push(t, name) && lua_type(L, -1) == LUA_TSTRING) s = lua_tostring(L, -1);
        lua_pop(L, 1);
        return s;
    }

    bool bool_at(int t, const char* name, bool& out, bool* present = nullptr) {
        const bool has = push(t, name);
        if (present) *present = has;
        if (has) out = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return true;
    }
};

struct WeaponNames {
    std::vector<std::string> phases;
    std::vector<std::string> vars;
};

void set_name(char* dst, const std::string& s) {
    const size_t n = std::min<size_t>(s.size(), sim::WEAPON_NAME_LEN);
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

bool compile_swing(Reader& r, int t, const std::string& where, sim::WSwing& s) {
    using namespace sim::swing_field;
    s = sim::WSwing{};
    s.present = true;
    struct F { const char* name; fx* field; u16 bit; };
    const F fx_fields[] = {
        {"reach", &s.reach, REACH}, {"half_arc", &s.half_arc, HALF_ARC},
        {"sweep_from", &s.sweep_from, SWEEP_FROM}, {"sweep_to", &s.sweep_to, SWEEP_TO},
        {"min_reach", &s.min_reach, MIN_REACH}, {"pitch_from", &s.pitch_from, PITCH_FROM},
        {"pitch_to", &s.pitch_to, PITCH_TO}, {"half_pitch", &s.half_pitch, HALF_PITCH},
    };
    for (const F& f : fx_fields) {
        bool has = false;
        if (!r.fx_at(t, f.name, where, *f.field, &has)) return false;
        if (has) s.set |= f.bit;
    }
    i64 v = 0; bool has = false;
    if (!r.int_at(t, "ticks", where, 0, 255, v, &has)) return false;
    if (has) { s.ticks = static_cast<u8>(v); s.set |= TICKS; }
    if (!r.int_at(t, "cooldown", where, 0, 255, v, &has)) return false;
    if (has) { s.cooldown = static_cast<u8>(v); s.set |= COOLDOWN; }
    return true;
}

// One condition: `c` is a table holding the condition's arguments, `name` its
// vocabulary name.
bool compile_cond(Reader& r, int c, const char* name, const std::string& where,
                  const WeaponNames& wn, sim::WCondDef& out) {
    out = sim::WCondDef{};
    if (std::strcmp(name, "lua") == 0) return r.fail(where, "uses `lua`, which only Lua can run");
    if (!cond_by_name(name, out.kind)) return r.fail(where, std::string("unknown condition '") + name + "'");
    const std::string button = r.str_at(c, "button");
    out.trigger = button == "secondary" ? sim::TRIGGER_SECONDARY : sim::TRIGGER_PRIMARY;
    i64 v = 0; bool has = false;
    switch (out.kind) {
        case WCond::HeldFor: case WCond::PhaseElapsed:
            if (!r.int_at(c, "ticks", where, 0, 1 << 30, v)) return false;
            out.n = static_cast<i32>(v);
            break;
        case WCond::HasAmmo:
            if (!r.int_at(c, "n", where, 0, 65535, v, &has)) return false;
            out.has = has; out.n = static_cast<i32>(v);
            break;
        case WCond::Chance:
            if (!r.int_at(c, "percent", where, -1000, 1000, v)) return false;
            out.n = static_cast<i32>(v);
            break;
        case WCond::VarAtLeast: case WCond::VarIs: {
            const i32 slot = index_in(wn.vars, r.str_at(c, "var").c_str());
            if (slot < 0) return r.fail(where, "names a var the weapon does not declare");
            out.var = static_cast<u8>(slot);
            if (!r.int_at(c, "value", where, INT32_MIN, INT32_MAX, v)) return false;
            out.n = static_cast<i32>(v);
            break;
        }
        case WCond::Grounded:
            r.bool_at(c, "value", out.flag, &out.has);
            break;
        case WCond::Moving: case WCond::Falling:
            if (!r.fx_at(c, "speed", where, out.speed, &out.has)) return false;
            break;
        default: break;
    }
    return true;
}

bool compile_step(Reader& r, int e, const std::string& where, const WeaponNames& wn,
                  const std::vector<std::string>& pools, sim::WStep& s) {
    s = sim::WStep{};
    const std::string action = r.str_at(e, "action");
    if (action == "lua") return r.fail(where, "uses `lua`, which only Lua can run");
    if (!act_by_name(action.c_str(), s.action)) return r.fail(where, "unknown action '" + action + "'");
    i64 v = 0; bool has = false;
    if (!r.int_at(e, "at", where, 0, 65534, v)) return false;
    s.at = static_cast<u16>(v);
    switch (s.action) {
        case WAct::Consume: {
            if (!r.int_at(e, "n", where, 0, 65535, v)) return false;
            s.value = static_cast<i32>(v);
            const std::string pool = r.str_at(e, "ammo");
            if (!pool.empty()) {
                const i32 p = index_in(pools, pool.c_str());
                if (p < 0) return r.fail(where, "names ammunition '" + pool + "', which is not declared");
                s.pool = static_cast<i8>(p);
            }
            break;
        }
        case WAct::SetVar: case WAct::AddVar: {
            const i32 slot = index_in(wn.vars, r.str_at(e, "var").c_str());
            if (slot < 0) return r.fail(where, "names a var the weapon does not declare");
            s.var = static_cast<u8>(slot);
            if (!r.int_at(e, "value", where, INT32_MIN, INT32_MAX, v)) return false;
            s.value = static_cast<i32>(v);
            break;
        }
        case WAct::Goto: {
            const i32 p = index_in(wn.phases, r.str_at(e, "phase").c_str());
            if (p < 0) return r.fail(where, "`goto` names a phase this weapon does not have");
            s.phase = static_cast<u8>(p);
            break;
        }
        case WAct::SetAnim:
            if (!r.int_at(e, "state", where, 0, 65535, v)) return false;
            s.state = static_cast<u16>(v);
            break;
        case WAct::Impulse: {
            if (!r.fx_at(e, "speed", where, s.speed)) return false;
            if (!r.fx_at(e, "vertical", where, s.vertical)) return false;
            const std::string towards = r.str_at(e, "towards");
            s.towards = towards == "away" ? sim::WTowards::Away
                      : towards == "random" ? sim::WTowards::Random : sim::WTowards::Facing;
            if (!r.int_at(e, "jitter", where, 0, 360, v)) return false;
            s.jitter = static_cast<i32>(v);
            break;
        }
        case WAct::Hitscan:
            if (!r.int_at(e, "count", where, 1, sim::MAX_PELLETS, v, &has)) return false;
            s.count = has ? static_cast<u8>(v) : 1;
            if (!r.int_at(e, "spread", where, 0, 360, v)) return false;
            s.spread = static_cast<i32>(v);
            if (!r.fx_at(e, "range", where, s.range)) return false;
            // The tracer a predicting client draws for each pellet.
            v = 0;
            if (!r.int_at(e, "tracer", where, 0, 65535, v)) return false;
            s.effect = static_cast<u16>(v);
            break;
        case WAct::Launch: {
            if (!r.int_at(e, "count", where, 1, sim::MAX_PELLETS, v, &has)) return false;
            s.count = has ? static_cast<u8>(v) : 1;
            if (!r.int_at(e, "arc", where, -360, 360, v)) return false;
            s.arc = static_cast<i32>(v);
            v = 0;
            if (!r.int_at(e, "angle", where, -360, 360, v)) return false;
            s.angle = static_cast<i32>(v);
            // THE MISSILE, as a predicting client needs it to fly it itself. A
            // missile nobody described (kind 0) is simply not predicted.
            sim::WMissile& m = s.missile;
            if (!r.fx_at(e, "offset", where, m.offset)) return false;
            const std::string name = r.str_at(e, "missile");
            lua_State* L = r.L;
            if (r.missiles != 0 && !name.empty()) {
                lua_getfield(L, r.missiles, name.c_str());
                if (lua_istable(L, -1)) {
                    const int d = lua_gettop(L);
                    const std::string mw = where + " missile '" + name + "'";
                    std::string prop = r.str_at(d, "prop");
                    if (prop.empty()) prop = name;
                    m.kind = r.props ? r.props->kind_by_name(prop.c_str(), prop.size()) : 0;
                    bool ok = r.fx_at(d, "speed", mw, m.speed) && r.fx_at(d, "z_offset", mw, m.z_offset);
                    bool has_radius = false;
                    ok = ok && r.fx_at(d, "radius", mw, m.radius, &has_radius);
                    if (!has_radius) m.radius = fx::from_frac(11, 48);   // mobs.lua's default
                    i64 n = 300;
                    ok = ok && r.int_at(d, "life", mw, 1, 65535, n);
                    m.life = static_cast<u16>(n);
                    bool has_burst = false;
                    n = 0;
                    ok = ok && r.int_at(d, "anim_burst", mw, 0, 65534, n, &has_burst);
                    m.burst_anim = has_burst ? static_cast<u16>(n) : sim::NO_ANIM;
                    n = 20;
                    ok = ok && r.int_at(d, "burst_ticks", mw, 0, 65535, n);
                    m.burst_ticks = static_cast<u16>(n);
                    // Its impact effects: on_hit's `vfx` entries drawn at the impact.
                    lua_getfield(L, d, "on_hit");
                    if (ok && lua_istable(L, -1)) {
                        const int oh = lua_gettop(L);
                        const size_t cnt = lua_objlen(L, oh);
                        // Every entry, not only until the effects are full:
                        // the blast comes after them in a rocket's on_hit.
                        for (size_t i = 1; i <= cnt; ++i) {
                            lua_rawgeti(L, oh, static_cast<int>(i));
                            const int a = lua_gettop(L);
                            if (lua_istable(L, a) && m.vfx_count < sim::MAX_MISSILE_VFX &&
                                r.str_at(a, "action") == "vfx" && r.str_at(a, "at") == "impact") {
                                i64 k = 0;
                                fx rad{};
                                if (r.int_at(a, "kind", mw, 0, 65535, k) && r.fx_at(a, "radius", mw, rad)) {
                                    m.vfx_kind[m.vfx_count] = static_cast<u16>(k);
                                    m.vfx_radius[m.vfx_count] = rad;
                                    ++m.vfx_count;
                                }
                            }
                            // Its blast, when it shoves the one who fired it
                            // and does not hurt them: that player's client
                            // predicts the shove (a rocket jump).
                            if (lua_istable(L, a) && !m.self_shove && r.str_at(a, "action") == "damage_area") {
                                bool hurts = true, shoves = false, falloff = false, has_hurts = false, has_kb = false;
                                r.bool_at(a, "hurts_owner", hurts, &has_hurts);
                                r.bool_at(a, "shoves_owner", shoves);
                                r.bool_at(a, "falloff", falloff);
                                if (has_hurts && !hurts && shoves &&
                                    r.fx_at(a, "radius", mw, m.blast_radius) &&
                                    r.fx_at(a, "knockback", mw, m.blast_knockback, &has_kb) && has_kb &&
                                    r.fx_at(a, "lift", mw, m.blast_lift)) {
                                    m.self_shove = true;
                                    m.blast_falloff = falloff;
                                }
                            }
                            lua_pop(L, 1);
                        }
                    }
                    lua_pop(L, 1);   // on_hit
                    if (!ok) { lua_pop(L, 1); return false; }
                    if (m.speed <= fx::zero()) m.kind = 0;   // nothing to fly
                }
                lua_pop(L, 1);   // the def
            }
            break;
        }
        case WAct::Sound: {
            r.bool_at(e, "predicted", s.predicted);
            // By the wire's own hash, which is what a client plays by.
            const std::string name = r.str_at(e, "name");
            s.sound = proto::sound_hash(name.data(), name.size());
            break;
        }
        case WAct::Vfx:
            r.bool_at(e, "attach", s.attach);
            v = 0;
            if (!r.int_at(e, "effect", where, 0, 65535, v)) return false;
            s.effect = static_cast<u16>(v);
            break;
        default: break;
    }
    return true;
}

bool compile_weapon(Reader& r, int d, const std::string& name, const std::vector<std::string>& pools,
                    sim::WeaponDef& w, WeaponNames& wn) {
    lua_State* L = r.L;
    const std::string where = "weapon '" + name + "'";
    w = sim::WeaponDef{};
    set_name(w.name, name);
    i64 v = 0;
    if (!r.int_at(d, "order", where, INT16_MIN, INT16_MAX, v)) return false;
    w.order = static_cast<i16>(v);
    const std::string pool = r.str_at(d, "ammo");
    if (!pool.empty()) {
        const i32 p = index_in(pools, pool.c_str());
        if (p < 0) return r.fail(where, "names ammunition '" + pool + "', which is not declared");
        w.pool = static_cast<i8>(p);
    }
    v = 1;
    if (!r.int_at(d, "per", where, 1, 65535, v)) return false;
    w.per = static_cast<u16>(v);
    v = 0;
    if (!r.int_at(d, "aim_snap", where, 0, 255, v)) return false;
    w.aim_snap = static_cast<u8>(v);

    if (r.push(d, "swing") && lua_istable(L, -1)) {
        if (!compile_swing(r, lua_gettop(L), where + " swing", w.swing)) { lua_pop(L, 1); return false; }
    }
    lua_pop(L, 1);

    // Vars, by name, sorted: a slot is a position in that order.
    wn.vars.clear();
    if (r.push(d, "vars") && lua_istable(L, -1)) {
        const int vt = lua_gettop(L);
        wn.vars = sorted_keys(L, vt);
        if (wn.vars.size() > sim::MAX_WEAPON_VARS) { lua_pop(L, 1); return r.fail(where, "has more vars than the core holds"); }
        w.var_count = static_cast<u8>(wn.vars.size());
        for (size_t i = 0; i < wn.vars.size(); ++i) {
            if (!r.int_at(vt, wn.vars[i].c_str(), where + " vars", INT32_MIN, INT32_MAX, v)) { lua_pop(L, 1); return false; }
            w.vars[i] = static_cast<i32>(v);
        }
    }
    lua_pop(L, 1);

    if (!r.push(d, "phases") || !lua_istable(L, -1)) { lua_pop(L, 1); return r.fail(where, "has no phases"); }
    const int pt = lua_gettop(L);
    wn.phases = sorted_keys(L, pt);
    if (wn.phases.empty() || wn.phases.size() > sim::MAX_WEAPON_PHASES) {
        lua_pop(L, 1);
        return r.fail(where, "has more phases than the core holds");
    }
    w.phase_count = static_cast<u8>(wn.phases.size());
    const i32 initial = index_in(wn.phases, r.str_at(d, "initial").c_str());
    if (initial < 0) { lua_pop(L, 1); return r.fail(where, "has no resting phase"); }
    w.initial = static_cast<u8>(initial);

    bool ok = true;
    for (size_t pi = 0; ok && pi < wn.phases.size(); ++pi) {
        const std::string pwhere = where + " phase '" + wn.phases[pi] + "'";
        sim::WPhase& ph = w.phases[pi];
        set_name(ph.name, wn.phases[pi]);
        lua_getfield(L, pt, wn.phases[pi].c_str());
        const int p = lua_gettop(L);
        bool has = false;
        v = 0;
        ok = r.int_at(p, "anim", pwhere, 0, 65535, v);
        ph.anim = static_cast<u16>(v);
        if (ok) { v = 0; ok = r.int_at(p, "ticks", pwhere, 0, 65534, v, &has); ph.ticks = has ? static_cast<u16>(v) : sim::PHASE_WAITS; }
        if (ok) {
            const std::string next = r.str_at(p, "next");
            if (!next.empty()) {
                const i32 n = index_in(wn.phases, next.c_str());
                if (n < 0) ok = r.fail(pwhere, "`next` names a phase this weapon does not have");
                else ph.next = static_cast<u8>(n);
            }
        }
        if (ok && r.push(p, "swing") && lua_istable(L, -1)) ok = compile_swing(r, lua_gettop(L), pwhere + " swing", ph.swing);
        lua_pop(L, 1);

        if (ok && r.push(p, "sequence") && lua_istable(L, -1)) {
            const int sq = lua_gettop(L);
            const size_t n = lua_objlen(L, sq);
            if (n > sim::MAX_PHASE_STEPS) ok = r.fail(pwhere, "has more sequence steps than the core holds");
            for (size_t i = 1; ok && i <= n; ++i) {
                lua_rawgeti(L, sq, static_cast<int>(i));
                ok = lua_istable(L, -1) &&
                     compile_step(r, lua_gettop(L), pwhere + " step " + std::to_string(i), wn, pools,
                                  ph.steps[i - 1]);
                lua_pop(L, 1);
            }
            if (ok) ph.step_count = static_cast<u8>(n);
        }
        lua_pop(L, 1);

        if (ok && r.push(p, "transitions") && lua_istable(L, -1)) {
            const int tt = lua_gettop(L);
            const size_t n = lua_objlen(L, tt);
            if (n > sim::MAX_PHASE_TRANSITIONS) ok = r.fail(pwhere, "has more transitions than the core holds");
            for (size_t i = 1; ok && i <= n; ++i) {
                const std::string twhere = pwhere + " transition " + std::to_string(i);
                sim::WTransition& tr = ph.transitions[i - 1];
                tr = sim::WTransition{};
                lua_rawgeti(L, tt, static_cast<int>(i));
                const int t = lua_gettop(L);
                const i32 to = index_in(wn.phases, r.str_at(t, "to").c_str());
                if (to < 0) ok = r.fail(twhere, "`to` names a phase this weapon does not have");
                else tr.to = static_cast<u8>(to);
                lua_getfield(L, t, "when");
                if (ok && lua_type(L, -1) == LUA_TSTRING) {
                    // One condition, its arguments on the transition itself.
                    const std::string cname = lua_tostring(L, -1);
                    tr.count = 1;
                    ok = compile_cond(r, t, cname.c_str(), twhere, wn, tr.conds[0]);
                } else if (ok && lua_istable(L, -1)) {
                    const int wt = lua_gettop(L);
                    const size_t m = lua_objlen(L, wt);
                    if (m == 0 || m > sim::MAX_TRANSITION_CONDS) ok = r.fail(twhere, "has more conditions than the core holds");
                    for (size_t j = 1; ok && j <= m; ++j) {
                        lua_rawgeti(L, wt, static_cast<int>(j));
                        const int c = lua_gettop(L);
                        const std::string cname = r.str_at(c, "cond");
                        ok = compile_cond(r, c, cname.c_str(), twhere + " condition " + std::to_string(j),
                                          wn, tr.conds[j - 1]);
                        lua_pop(L, 1);
                    }
                    if (ok) tr.count = static_cast<u8>(m);
                } else if (ok) {
                    ok = r.fail(twhere, "has no `when`");
                }
                lua_pop(L, 2);   // when, the transition
            }
            if (ok) ph.transition_count = static_cast<u8>(n);
        }
        lua_pop(L, 1);   // transitions
        lua_pop(L, 1);   // the phase
    }
    lua_pop(L, 1);       // phases
    return ok;
}

// Name -> 0-based index, and the 1-based array of names, for one list.
void push_names(lua_State* L, const std::vector<std::string>& names, const char* by_name,
                const char* list, int into) {
    lua_newtable(L);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushnumber(L, static_cast<lua_Number>(i));
        lua_setfield(L, -2, names[i].c_str());
    }
    lua_setfield(L, into, by_name);
    lua_newtable(L);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, into, list);
}

// dnp.weapons_compile(defs, ammo [, initial_weapons [, autoaim [, missiles]]]) -> index | nil, why
//
// `defs` is weapons.defs and `ammo` weapons.ammo, AFTER weapons.load has
// validated them. ALL OR NOTHING: a weapon the core cannot run (one using
// `lua`, or bigger than the core's fixed sizes) keeps the whole table on the
// Lua path, because the inventory has one owner and a player holding one
// weapon of each kind would have two.
//
// On success the index says where everything went -- weapon, pool, phase and
// var names to the 0-based indices the state and the events use, and back --
// so weapons.lua never has to repeat the ordering rule.
int l_weapons_compile(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    compiled().erase(L);

    auto c = std::make_unique<Compiled>();
    sim::WeaponTable& t = c->table;
    Reader r{L, {}};
    if (lua_istable(L, 5)) r.missiles = 5;
    r.props = context_of(L).props;

    const std::vector<std::string> pools = sorted_keys(L, 2);
    const std::vector<std::string> weapons = sorted_keys(L, 1);
    if (pools.size() > sim::MAX_AMMO_POOLS) { lua_pushnil(L); lua_pushstring(L, "more ammunition kinds than the core holds"); return 2; }
    if (weapons.empty() || weapons.size() > sim::MAX_WEAPONS) { lua_pushnil(L); lua_pushstring(L, "no weapons, or more than the core holds"); return 2; }

    t.pool_count = static_cast<u8>(pools.size());
    for (size_t i = 0; i < pools.size(); ++i) {
        lua_getfield(L, 2, pools[i].c_str());
        const int a = lua_gettop(L);
        i64 v = 0;
        set_name(t.pools[i].name, pools[i]);
        const std::string where = "ammo '" + pools[i] + "'";
        if (lua_istable(L, a)) {
            if (r.int_at(a, "max", where, 0, 65535, v)) t.pools[i].max = static_cast<u16>(v);
            v = 0;
            if (r.int_at(a, "initial", where, 0, 65535, v)) t.pools[i].initial = static_cast<u16>(v);
        }
        lua_pop(L, 1);
    }

    std::vector<WeaponNames> names(weapons.size());
    t.weapon_count = static_cast<u8>(weapons.size());
    for (size_t i = 0; i < weapons.size() && r.err.empty(); ++i) {
        lua_getfield(L, 1, weapons[i].c_str());
        if (lua_istable(L, -1)) compile_weapon(r, lua_gettop(L), weapons[i], pools, t.weapons[i], names[i]);
        lua_pop(L, 1);
    }

    if (r.err.empty() && lua_istable(L, 3)) {
        const size_t n = lua_objlen(L, 3);
        for (size_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, 3, static_cast<int>(i));
            const char* s = lua_tostring(L, -1);
            const i32 w = s ? index_in(weapons, s) : -1;
            if (w < 0) r.fail("initial_weapons", std::string("names '") + (s ? s : "?") + "', which is not a weapon");
            else t.initial_have |= 1u << w;
            lua_pop(L, 1);
        }
    }

    // AUTO-AIM: {range = fx, cone = degrees}, mobs.lua's two numbers, so a
    // predicting client corrects a shot exactly as the server does.
    if (r.err.empty() && lua_istable(L, 4)) {
        r.fx_at(4, "range", "autoaim", t.autoaim_range);
        lua_getfield(L, 4, "cone");
        if (lua_isnumber(L, -1)) {
            const lua_Number deg = lua_tonumber(L, -1);
            // floor(deg * 2^32 / 360), as mobs.lua's own conversion.
            const lua_Number b = deg * 4294967296.0 / 360.0;
            t.autoaim_cone = b <= 0 ? 0u : (b >= 2147483648.0 ? 2147483647u : static_cast<u32>(b));
        }
        lua_pop(L, 1);
    }

    if (!r.err.empty()) { lua_pushnil(L); lua_pushstring(L, r.err.c_str()); return 2; }

    c->seed = t.hash();
    lua_newtable(L);
    const int idx = lua_gettop(L);
    push_names(L, weapons, "weapons", "weapon_names", idx);
    push_names(L, pools, "pools", "pool_names", idx);
    lua_newtable(L);   // phases by weapon
    lua_newtable(L);   // phase names by weapon
    lua_newtable(L);   // vars by weapon
    lua_newtable(L);   // var names by weapon
    for (size_t i = 0; i < weapons.size(); ++i) {
        lua_newtable(L);
        const int tmp = lua_gettop(L);
        push_names(L, names[i].phases, "by", "list", tmp);
        lua_getfield(L, tmp, "by");   lua_setfield(L, idx + 1, weapons[i].c_str());
        lua_getfield(L, tmp, "list"); lua_setfield(L, idx + 2, weapons[i].c_str());
        push_names(L, names[i].vars, "by", "list", tmp);
        lua_getfield(L, tmp, "by");   lua_setfield(L, idx + 3, weapons[i].c_str());
        lua_getfield(L, tmp, "list"); lua_setfield(L, idx + 4, weapons[i].c_str());
        lua_pop(L, 1);
    }
    lua_setfield(L, idx, "var_names");
    lua_setfield(L, idx, "vars");
    lua_setfield(L, idx, "phase_names");
    lua_setfield(L, idx, "phases");
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(c->seed));
    lua_pushstring(L, hash);
    lua_setfield(L, idx, "hash");
    compiled()[L] = std::move(c);
    return 1;
}

// dnp.weapon_sync(e, have, current, ammo)
//
// THE INVENTORY, handed down: the held-weapon mask, the one in hand (0-based,
// -1 for none) and each pool's count (a 1-based array in pool order). A change
// of weapon here re-arms the machine on its next step, exactly as changing
// weapons.lua's `inv.current` re-armed its own.
int l_weapon_sync(lua_State* L) {
    const i32 a = entity_agent(L, 1);
    if (a < 0) return 0;
    WorldContext& ctx = context_of(L);
    sim::WeaponState& s = ctx.world->weapon_mut(static_cast<u32>(a));
    sim::WeaponState want = s;
    want.have = static_cast<u32>(luaL_checknumber(L, 2));
    const i32 cur = static_cast<i32>(luaL_checknumber(L, 3));
    want.current = (cur < 0 || cur >= static_cast<i32>(sim::MAX_WEAPONS)) ? sim::NO_WEAPON : static_cast<u8>(cur);
    if (lua_istable(L, 4)) {
        for (u32 p = 0; p < sim::MAX_AMMO_POOLS; ++p) {
            lua_rawgeti(L, 4, static_cast<int>(p + 1));
            const lua_Number v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0;
            want.ammo[p] = static_cast<u16>(v < 0 ? 0 : (v > 65535 ? 65535 : v));
            lua_pop(L, 1);
        }
    }
    // A REAL change -- a pickup, the panel, a load -- is one the player's
    // client could not have predicted, so its snapshot is marked as a
    // script's doing. Only a real one: this runs every tick for every player,
    // and marking them all would excuse every misprediction there is.
    bool changed = want.have != s.have || want.current != s.current;
    for (u32 p = 0; p < sim::MAX_AMMO_POOLS; ++p) changed = changed || want.ammo[p] != s.ammo[p];
    if (changed) {
        s = want;
        if (ctx.commands && ctx.commands->push(Command{CmdKind::Touch, static_cast<u32>(a), fx3{}})) {
            ++ctx.stats.commands_queued;
        }
    }
    return 0;
}

// dnp.weapon_step(e, tick) -> hops, events
//
// One tick of this player's weapon, by the core. `events` is what weapons.lua
// must now carry out, in order: {action, weapon, phase, step, aim, count,
// offsets}, all indices 0-based, `offsets` the per-pellet brad the core drew.
int l_weapon_step(lua_State* L) {
    const i32 a = entity_agent(L, 1);
    const u32 tick = static_cast<u32>(luaL_checknumber(L, 2));
    Compiled* c = compiled_of(L);
    if (a < 0 || c == nullptr) { lua_pushnumber(L, 0); lua_pushnil(L); return 2; }
    static sim::WeaponEventLog log;
    log.clear();
    // The weapon this tick's input asks for: a switch the client predicted.
    // Its shots' auto-aim judged where this player saw the others.
    const RewindForShot rewound(context_of(L), static_cast<u32>(a));
    sim::World& world = *context_of(L).world;
    const u32 hops = world.step_weapon(static_cast<u32>(a), c->table,
                                       world.input(static_cast<u32>(a)).weapon, tick, c->seed, log);
    lua_pushnumber(L, hops);
    lua_createtable(L, static_cast<int>(log.count()), 0);
    for (u32 i = 0; i < log.count(); ++i) {
        const sim::WeaponEvent& e = log.at(i);
        lua_createtable(L, 0, 7);
        lua_pushstring(L, act_name(e.action)); lua_setfield(L, -2, "action");
        lua_pushnumber(L, e.weapon);           lua_setfield(L, -2, "weapon");
        lua_pushnumber(L, e.phase);            lua_setfield(L, -2, "phase");
        lua_pushnumber(L, e.step);             lua_setfield(L, -2, "step");
        lua_pushnumber(L, e.aim.raw);          lua_setfield(L, -2, "aim");
        // The slope the core aimed with, absent when it is level by default.
        if (e.slope_set) { push_fx(L, e.slope); lua_setfield(L, -2, "slope"); }
        lua_pushnumber(L, e.count);            lua_setfield(L, -2, "count");
        lua_createtable(L, e.count, 0);
        for (u32 k = 0; k < e.count; ++k) {
            lua_pushnumber(L, e.offsets[k]);
            lua_rawseti(L, -2, static_cast<int>(k + 1));
        }
        lua_setfield(L, -2, "offsets");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 2;
}

// dnp.weapon_state(e) -> the machine's state, for weapons.lua to read back:
// have, current, armed, phase, swing_phase (0-based, -1 for none), entered,
// vars and ammo (1-based arrays in slot and pool order).
int l_weapon_state(lua_State* L) {
    const i32 a = entity_agent(L, 1);
    if (a < 0) { lua_pushnil(L); return 1; }
    const sim::WeaponState& s = context_of(L).world->weapon(static_cast<u32>(a));
    const auto idx = [&](u8 v, u8 none) { lua_pushnumber(L, v == none ? -1 : v); };
    lua_createtable(L, 0, 8);
    lua_pushnumber(L, s.have);                 lua_setfield(L, -2, "have");
    idx(s.current, sim::NO_WEAPON);            lua_setfield(L, -2, "current");
    idx(s.armed, sim::NO_WEAPON);              lua_setfield(L, -2, "armed");
    idx(s.phase, sim::NO_PHASE);               lua_setfield(L, -2, "phase");
    idx(s.swing_phase, sim::NO_PHASE);         lua_setfield(L, -2, "swing_phase");
    lua_pushnumber(L, s.entered);              lua_setfield(L, -2, "entered");
    lua_createtable(L, sim::MAX_WEAPON_VARS, 0);
    for (u32 i = 0; i < sim::MAX_WEAPON_VARS; ++i) { lua_pushnumber(L, s.vars[i]); lua_rawseti(L, -2, static_cast<int>(i + 1)); }
    lua_setfield(L, -2, "vars");
    lua_createtable(L, sim::MAX_AMMO_POOLS, 0);
    for (u32 i = 0; i < sim::MAX_AMMO_POOLS; ++i) { lua_pushnumber(L, s.ammo[i]); lua_rawseti(L, -2, static_cast<int>(i + 1)); }
    lua_setfield(L, -2, "ammo");
    return 1;
}

// dnp.weapon_reset(e): drop the machine, keep the inventory -- weapons.lua's
// rearm_player. The next step arms from the resting phase with fresh vars, and
// with the buttons as if just put down.
int l_weapon_reset(lua_State* L) {
    const i32 a = entity_agent(L, 1);
    if (a < 0) return 0;
    sim::WeaponState& s = context_of(L).world->weapon_mut(static_cast<u32>(a));
    sim::WeaponState fresh{};
    fresh.have = s.have;
    fresh.current = s.current;
    for (u32 i = 0; i < sim::MAX_AMMO_POOLS; ++i) fresh.ammo[i] = s.ammo[i];
    s = fresh;
    return 0;
}

// dnp.weapon_autoaim(e, range, cone_degrees) -> heading, slope | nil
//
// The core's auto-aim (World::autoaim), the one a player's predicted shot is
// corrected with. FOR THE TEST beside mobs.autoaim: the claim is that the two
// agree, and a claim with no way to ask both is not one anybody can check.
int l_weapon_autoaim(lua_State* L) {
    const i32 a = entity_agent(L, 1);
    if (a < 0) { lua_pushnil(L); return 1; }
    const fx range = check_fx(L, 2);
    const lua_Number b = luaL_checknumber(L, 3) * 4294967296.0 / 360.0;
    brad heading{};
    fx slope{};
    bool slope_set = false;
    const RewindForShot rewound(context_of(L), static_cast<u32>(a));
    if (!context_of(L).world->autoaim(static_cast<u32>(a), range, b <= 0 ? 0u : static_cast<u32>(b),
                                      heading, slope, slope_set)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, heading.raw);
    push_fx(L, slope);
    return 2;
}

const luaL_Reg WEAPON_FUNCS[] = {
    {"weapon_autoaim", l_weapon_autoaim},
    {"weapons_compile", l_weapons_compile},
    {"weapon_sync", l_weapon_sync},
    {"weapon_step", l_weapon_step},
    {"weapon_state", l_weapon_state},
    {"weapon_reset", l_weapon_reset},
    {nullptr, nullptr},
};

}  // namespace

const sim::WeaponTable* compiled_weapons(lua_State* L, u64* seed) {
    const Compiled* c = compiled_of(L);
    if (c == nullptr) return nullptr;
    if (seed) *seed = c->seed;
    return &c->table;
}

void open_weapons(lua_State* L) {
    for (const luaL_Reg* r = WEAPON_FUNCS; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}

}  // namespace dnp::server::lua
