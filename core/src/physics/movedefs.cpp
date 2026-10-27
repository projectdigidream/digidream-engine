#include "dnp/physics/movedefs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dnp/physics/params.hpp"
#include "dnp/physics/param_text.hpp"
#include "dnp/sim/constants.hpp"

namespace dnp::physics {

namespace {

constexpr u32 LINE_MAX = 512;
constexpr u32 TOKENS_MAX = 48;

struct Line {
    char buf[LINE_MAX];
    const char* tok[TOKENS_MAX];
    u32 n = 0;
};

// One line into whitespace-separated tokens, a `#` ending it. False when it is
// too long to be a line anybody meant.
bool tokenize(const char* s, u32 len, Line& out) noexcept {
    if (len >= LINE_MAX) return false;
    std::memcpy(out.buf, s, len);
    out.buf[len] = '\0';
    out.n = 0;
    char* p = out.buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
        if (*p == '\0' || *p == '#') break;
        if (out.n == TOKENS_MAX) return false;
        out.tok[out.n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '#') ++p;
        if (*p == '#') { *p = '\0'; break; }
        if (*p) *p++ = '\0';
    }
    return true;
}

bool is(const char* a, const char* b) noexcept { return std::strcmp(a, b) == 0; }

bool number(const char* s, fx& out) noexcept {
    i64 raw = 0;
    if (!parse_fx_decimal(s, raw)) return false;
    out = fx::from_raw(raw);
    return true;
}

u16 button(const char* s) noexcept {
    if (is(s, "jump")) return act::JUMP;
    if (is(s, "dash")) return act::DASH;
    if (is(s, "climb")) return act::CLIMB;
    if (is(s, "hover")) return act::HOVER;
    if (is(s, "flight")) return act::FLIGHT;
    if (is(s, "grapple")) return act::GRAPPLE;
    if (is(s, "use")) return act::INTERACT;
    if (is(s, "attack")) return act::ATTACK;
    if (is(s, "crouch")) return move_button::CROUCH;
    if (is(s, "up")) return move_button::UP;
    return 0;
}

u16 flag(const char* s) noexcept {
    if (is(s, "no_jump")) return move_flag::NO_JUMP;
    if (is(s, "no_dash")) return move_flag::NO_DASH;
    if (is(s, "no_attack")) return move_flag::NO_ATTACK;
    if (is(s, "no_use")) return move_flag::NO_USE;
    if (is(s, "no_crouch")) return move_flag::NO_CROUCH;
    if (is(s, "weapon_hidden")) return move_flag::WEAPON_HIDDEN;
    if (is(s, "counts_as_ground")) return move_flag::COUNTS_AS_GROUND;
    if (is(s, "invulnerable")) return move_flag::INVULNERABLE;
    return 0;
}

bool copy_name(char (&dst)[MOVE_NAME_LEN], const char* s) noexcept {
    const size_t n = std::strlen(s);
    if (n == 0 || n >= MOVE_NAME_LEN) return false;
    std::memcpy(dst, s, n + 1);
    return true;
}

// `default` is 0; `any` only where the caller allows it.
i32 state_of(const MoveDefs& d, const char* s) noexcept {
    if (is(s, "default")) return 0;
    return move_state_index(d, s);
}

const char* read_state(MoveDefs& d, const Line& L) noexcept {
    if (L.n < 2) return "movestate needs a name";
    if (d.state_count == MAX_MOVE_STATES) return "a module has at most 8 movement states";
    if (is(L.tok[1], "default") || is(L.tok[1], "any")) return "'default' and 'any' are not names a state can have";
    if (move_state_index(d, L.tok[1]) >= 0) return "a state of that name is already declared";
    MoveStateDef s;
    if (!copy_name(s.name, L.tok[1])) return "a state's name is 1 to 15 characters";
    bool grouped = false;
    for (u32 i = 2; i < L.n; ++i) {
        const char* k = L.tok[i];
        if (is(k, "flags")) continue;   // a word for the reader; the flags follow it
        if (const u16 f = flag(k)) { s.flags |= f; continue; }
        if (i + 1 >= L.n) return "a setting with no value";
        const char* v = L.tok[++i];
        if (is(k, "group")) {
            grouped = true;
            if (is(v, "floor")) s.group = MoveGroup::Floor;
            else if (is(v, "air")) s.group = MoveGroup::Air;
            else if (is(v, "liquid")) s.group = MoveGroup::Liquid;
            else if (is(v, "none")) s.group = MoveGroup::None;
            else return "group is floor, air, liquid or none";
            continue;
        }
        if (is(k, "anim")) {
            if (!copy_name(s.anim, v)) return "an animation's name is 1 to 15 characters";
            continue;
        }
        fx* slot = nullptr;
        u8 bit = 0;
        if (is(k, "speed")) { slot = &s.speed; bit = move_has::SPEED; }
        else if (is(k, "accel")) { slot = &s.accel; bit = move_has::ACCEL; }
        else if (is(k, "friction")) { slot = &s.friction; bit = move_has::FRICTION; }
        else if (is(k, "gravity")) { slot = &s.gravity; bit = move_has::GRAVITY; }
        else if (is(k, "buoyancy")) { slot = &s.buoyancy; bit = move_has::BUOYANCY; }
        else if (is(k, "jump")) { slot = &s.jump; bit = move_has::JUMP; }
        else return "not a movestate setting (group speed accel friction gravity buoyancy jump anim, or a flag)";
        if (!number(v, *slot)) return "a number is a decimal, like 9 or 0.25";
        s.has |= bit;
    }
    if (!grouped) return "a movestate needs a group: floor, air, liquid or none";
    if (s.group == MoveGroup::Liquid) d.uses_depth = true;
    d.states[d.state_count++] = s;
    return nullptr;
}

const char* read_transition(MoveDefs& d, const Line& L) noexcept {
    if (d.transition_count == MAX_MOVE_TRANSITIONS) return "a module has at most 32 transitions";
    if (L.n < 5 || !is(L.tok[2], "->") || !is(L.tok[4], "when"))
        return "a transition is: transition <from|any> -> <to> when <conditions> [do <action>]";
    MoveTransition t;
    if (is(L.tok[1], "any")) t.from = MOVE_ANY;
    else {
        const i32 f = state_of(d, L.tok[1]);
        if (f < 0) return "it names a state no movestate line declares";
        t.from = static_cast<u8>(f);
    }
    const i32 to = state_of(d, L.tok[3]);
    if (to < 0) return "it names a state no movestate line declares";
    t.to = static_cast<u8>(to);
    if (t.from == t.to) return "a transition goes from a state to another one";

    u32 i = 5;
    while (i < L.n && !is(L.tok[i], "do")) {
        if (t.cond_count == MAX_MOVE_CONDITIONS) return "a transition has at most 4 conditions";
        MoveCondition& c = t.cond[t.cond_count];
        const char* k = L.tok[i++];
        const char* a = i < L.n ? L.tok[i] : "";
        if (is(k, "grounded")) c.kind = MoveCond::Grounded;
        else if (is(k, "airborne")) c.kind = MoveCond::Airborne;
        else if (is(k, "rising")) c.kind = MoveCond::Rising;
        else if (is(k, "falling")) c.kind = MoveCond::Falling;
        else if (is(k, "pressed") || is(k, "held") || is(k, "released")) {
            c.kind = is(k, "pressed") ? MoveCond::Pressed : is(k, "held") ? MoveCond::Held : MoveCond::Released;
            c.arg = button(a);
            if (c.arg == 0) return "a button is jump, crouch, up, dash, climb, hover, flight, grapple, use or attack";
            ++i;
        } else if (is(k, "speed")) {
            if (i + 1 >= L.n || !(is(a, "above") || is(a, "below")) || !number(L.tok[i + 1], c.value))
                return "speed above <n> or speed below <n>";
            c.kind = is(a, "above") ? MoveCond::SpeedAbove : MoveCond::SpeedBelow;
            i += 2;
        } else if (is(k, "after")) {
            fx s{};
            if (!number(a, s) || s < fx::zero() || s > fx::from_int(1000)) return "after <seconds>, 0 to 1000";
            c.kind = MoveCond::After;
            c.arg = static_cast<u32>((s.raw * static_cast<i64>(sim::TICK_RATE_HZ) + (i64{1} << 31)) >> 32);
            ++i;
        } else if (is(k, "floor")) {
            if (i + 1 >= L.n || !is(a, "tag")) return "floor tag <n>";
            c.kind = MoveCond::FloorTag;
            c.arg = static_cast<u32>(std::atoi(L.tok[i + 1]));
            i += 2;
        } else if (is(k, "depth")) {
            // depth none|feet|waist|eyes (at least that deep), depth below <level>
            const bool below = is(a, "below");
            const char* lv = below ? (i + 1 < L.n ? L.tok[i + 1] : "") : a;
            const u32 level = is(lv, "feet") ? 1 : is(lv, "waist") ? 2 : is(lv, "eyes") ? 3 : is(lv, "none") ? 0 : 99;
            if (level == 99 || (below && level == 0)) return "depth none|feet|waist|eyes, or depth below feet|waist|eyes";
            c.kind = below ? MoveCond::DepthBelow : level == 0 ? MoveCond::DepthNone : MoveCond::DepthAtLeast;
            c.arg = level;
            d.uses_depth = true;
            i += below ? 2 : 1;
        } else if (is(k, "meter")) {
            return "meters are not built yet (docs/MOVEMENT.md, step 3)";
        } else {
            return "not a condition (grounded airborne rising falling pressed held released speed after floor depth)";
        }
        ++t.cond_count;
    }
    if (t.cond_count == 0) return "a transition needs at least one condition";
    if (i < L.n) {   // `do`
        if (i + 1 >= L.n) return "do what? (jump, stop, impulse up|facing|back <n>)";
        const char* act = L.tok[i + 1];
        if (is(act, "jump")) { t.action = MoveAction::Jump; i += 2; }
        else if (is(act, "stop")) { t.action = MoveAction::Stop; i += 2; }
        else if (is(act, "impulse")) {
            const char* way = i + 2 < L.n ? L.tok[i + 2] : "";
            if (is(way, "up")) t.action = MoveAction::ImpulseUp;
            else if (is(way, "facing")) t.action = MoveAction::ImpulseFacing;
            else if (is(way, "back")) t.action = MoveAction::ImpulseBack;
            else return "impulse up, facing or back, then a speed";
            if (i + 3 >= L.n || !number(L.tok[i + 3], t.value)) return "impulse needs a speed";
            i += 4;
        } else if (is(act, "move") || is(act, "meter")) {
            return "layered moves and meters are not built yet (docs/MOVEMENT.md)";
        } else return "do jump, stop, or impulse up|facing|back <n>";
        if (i < L.n) return "one action after `do`";
    }
    d.transitions[d.transition_count++] = t;
    return nullptr;
}

}  // namespace

i32 move_state_index(const MoveDefs& d, const char* name) noexcept {
    for (u8 i = 1; i < d.state_count; ++i) {
        if (std::strcmp(d.states[i].name, name) == 0) return i;
    }
    return -1;
}

bool parse_move_defs(MoveDefs& out, const char* text, u32 len, char* err, u32 err_cap) noexcept {
    out = MoveDefs{};
    std::memcpy(out.states[0].name, "default", 8);
    static Line L;   // a line's worth of tokens; the core has no heap, and this is too big for a wasm frame
    for (int pass = 0; pass < 2; ++pass) {
        u32 at = 0;
        u32 lineno = 0;
        while (at < len) {
            u32 end = at;
            while (end < len && text[end] != '\n') ++end;
            ++lineno;
            const char* why = nullptr;
            if (!tokenize(text + at, end - at, L)) why = "a line too long, or with too many words";
            else if (L.n > 0 && pass == 0 && is(L.tok[0], "movestate")) why = read_state(out, L);
            else if (L.n > 0 && pass == 1 && is(L.tok[0], "transition")) why = read_transition(out, L);
            if (why) {
                if (err && err_cap) std::snprintf(err, err_cap, "line %u: %s", lineno, why);
                out = MoveDefs{};
                std::memcpy(out.states[0].name, "default", 8);
                return false;
            }
            at = end + 1;
        }
    }
    return true;
}

}  // namespace dnp::physics
