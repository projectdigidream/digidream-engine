#include "dnp/sim/weapon.hpp"

#include "dnp/math/trig.hpp"

namespace dnp::sim {

namespace {

// splitmix64: a mixer, not a stream -- each draw is a pure function of its
// inputs, so a draw on the server and the same draw on a client agree without
// either having consumed anything else first.
constexpr u64 mix(u64 x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// floor(a / b) for b > 0, both signs of a.
constexpr i64 floor_div(i64 a, i64 b) noexcept {
    const i64 q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

// weapons.lua's deg(n) = floor(n * 2^32 / 360), for n = num / den exactly.
constexpr i64 deg_brad(i64 num, i64 den = 1) noexcept {
    return floor_div(num * 4294967296LL, 360 * den);
}

constexpr u8 bit_of(u8 trigger) noexcept { return trigger == TRIGGER_SECONDARY ? 2u : 1u; }

bool has_ammo(const WeaponTable& t, const WeaponState& s, u8 w, u16 n) noexcept {
    if (w >= t.weapon_count) return true;
    const WeaponDef& d = t.weapons[w];
    if (d.pool < 0) return true;
    return s.ammo[d.pool] >= (n ? n : d.per);
}

// weapons.lua's switch_to_usable: the HIGHEST-order weapon held that has
// ammunition. Ties go to the lower index, which pairs() order never promised
// and a deterministic machine has to.
void switch_to_usable(const WeaponTable& t, WeaponState& s) noexcept {
    u8 best = NO_WEAPON;
    for (u8 w = 0; w < t.weapon_count; ++w) {
        if (!((s.have >> w) & 1u) || !has_ammo(t, s, w, 0)) continue;
        if (best == NO_WEAPON || t.weapons[w].order > t.weapons[best].order) best = w;
    }
    if (best != NO_WEAPON) s.current = best;
}

void enter(WeaponState& s, u8 phase, u32 tick) noexcept {
    s.phase = phase;
    s.entered = tick;
    s.done = 0;
}

void arm(const WeaponTable& t, WeaponState& s, u32 tick) noexcept {
    const WeaponDef& d = t.weapons[s.current];
    for (u32 i = 0; i < MAX_WEAPON_VARS; ++i) s.vars[i] = i < d.var_count ? d.vars[i] : 0;
    s.armed = s.current;
    s.swing_phase = NO_PHASE;
    enter(s, d.initial, tick);
}

struct Ctx {
    const WeaponTable& t;
    WeaponState& s;
    WeaponBody& body;
    const WeaponDef& def;
    u8 weapon;
    u8 held, before;
    u32 tick, agent;
    u64 seed;
    WeaponEventLog& log;
    u32 elapsed = 0;
    u8 jump = NO_PHASE;
    u32 draws = 0;      // salt for the random stream, in draw order

    i32 random(i32 lo, i32 hi) noexcept { return weapon_random(seed, agent, tick, draws++, lo, hi); }
};

bool cond_holds(Ctx& c, const WCondDef& k) noexcept {
    const u8 m = bit_of(k.trigger);
    switch (k.kind) {
        case WCond::Always:   return true;
        case WCond::Pressed:  return (c.held & m) && !(c.before & m);
        case WCond::Released: return !(c.held & m) && (c.before & m);
        case WCond::Held:     return (c.held & m) != 0;
        case WCond::HeldFor: {
            if (!(c.held & m)) return false;
            const u32 since = c.s.held_since[k.trigger];
            return since != 0 && static_cast<i64>(c.tick) - static_cast<i64>(since - 1) >= k.n;
        }
        case WCond::HasAmmo:      return has_ammo(c.t, c.s, c.weapon, static_cast<u16>(k.has ? k.n : 0));
        case WCond::NoAmmo:       return !has_ammo(c.t, c.s, c.weapon, 0);
        case WCond::PhaseElapsed: return static_cast<i64>(c.elapsed) >= k.n;
        case WCond::Chance:       return c.random(0, 99) < k.n;
        case WCond::VarAtLeast:   return c.s.vars[k.var] >= k.n;
        case WCond::VarIs:        return c.s.vars[k.var] == k.n;
        case WCond::Grounded:     return k.has ? (c.body.grounded == k.flag) : c.body.grounded;
        case WCond::Moving: {
            const fx lim = k.has ? k.speed : fx::from_frac(1, 2);
            const fx sq = c.body.vel.x * c.body.vel.x + c.body.vel.y * c.body.vel.y;
            return sq > lim * lim;
        }
        case WCond::Falling:
            return c.body.vel.z < (k.has ? -k.speed : fx::zero());
    }
    return false;
}

void report(Ctx& c, u8 phase, u8 step, const WStep& a) noexcept {
    WeaponEvent e;
    e.agent = c.agent;
    e.tick = c.tick;
    e.weapon = c.weapon;
    e.phase = phase;
    e.step = step;
    e.action = a.action;
    e.aim = c.body.facing;
    if (a.action == WAct::Hitscan) {
        // One offset per pellet: weapons.lua via mobs.lua's hitscan, in exact
        // integers -- floor(r / 1000 * spread * 2^32 / 360), r in -1000..1000.
        const u32 n = a.count == 0 ? 1u : (a.count > MAX_PELLETS ? MAX_PELLETS : a.count);
        e.count = static_cast<u8>(n);
        for (u32 i = 0; i < n; ++i) {
            i64 off = 0;
            if (a.spread != 0) {
                const i64 r = c.random(-1000, 1000);
                off = floor_div(r * a.spread * 4294967296LL, 360LL * 1000);
            }
            e.offsets[i] = static_cast<i32>(static_cast<u32>(off));
        }
    } else if (a.action == WAct::Launch) {
        // Centred on the aim: `angle`, then `arc` apart, as weapons.lua's launch.
        const u32 n = a.count == 0 ? 1u : (a.count > MAX_PELLETS ? MAX_PELLETS : a.count);
        e.count = static_cast<u8>(n);
        for (u32 i = 0; i < n; ++i) {
            i64 off = a.angle ? deg_brad(a.angle) : 0;
            if (n > 1 && a.arc) off += deg_brad((2 * static_cast<i64>(i) - (n - 1)) * a.arc, 2);
            e.offsets[i] = static_cast<i32>(static_cast<u32>(off));
        }
    }
    c.log.push(e);
}

void perform(Ctx& c, u8 phase, u8 step, const WStep& a) noexcept {
    switch (a.action) {
        case WAct::Consume: {
            const i8 pool = a.pool >= 0 ? a.pool : c.def.pool;
            if (pool < 0) return;
            const i32 n = a.value ? a.value : c.def.per;
            const i32 left = static_cast<i32>(c.s.ammo[pool]) - n;
            c.s.ammo[pool] = static_cast<u16>(left < 0 ? 0 : left);
            return;
        }
        case WAct::SetVar: c.s.vars[a.var] = a.value; return;
        case WAct::AddVar: c.s.vars[a.var] += a.value; return;
        case WAct::Goto:   c.jump = a.phase; return;
        case WAct::SetAnim: c.body.anim_set = true; c.body.anim = a.state; return;
        case WAct::SwitchIfEmpty:
            if (!has_ammo(c.t, c.s, c.weapon, 0)) switch_to_usable(c.t, c.s);
            return;
        case WAct::Impulse: {
            // mobs.lua's acts.impulse for a player's synthetic owner: aim is
            // the body's facing.
            i64 ang = static_cast<i64>(c.body.facing.raw);
            if (a.towards == WTowards::Away) ang += 2147483648LL;
            else if (a.towards == WTowards::Random) ang = deg_brad(c.random(0, 359));
            if (a.jitter) ang += deg_brad(c.random(-a.jitter, a.jitter));
            const fx2 d = fx_dir(brad(static_cast<u32>(ang)));
            c.body.impulse = c.body.impulse + fx3{d.x * a.speed, d.y * a.speed, a.vertical};
            return;
        }
        default:
            if (reported(a.action)) report(c, phase, step, a);
            return;
    }
}

}  // namespace

i32 weapon_random(u64 seed, u32 agent, u32 tick, u32 salt, i32 lo, i32 hi) noexcept {
    if (hi <= lo) return lo;
    const u64 h = mix(seed ^ mix((static_cast<u64>(agent) << 32) ^ tick) ^ mix(0xC0FFEEULL + salt));
    const u64 span = static_cast<u64>(static_cast<i64>(hi) - lo + 1);
    return static_cast<i32>(lo + static_cast<i64>(h % span));
}

bool weapon_has_ammo(const WeaponTable& t, const WeaponState& s, u8 weapon, u16 n) noexcept {
    return has_ammo(t, s, weapon, n);
}

void weapon_fresh(const WeaponTable& t, WeaponState& s) noexcept {
    s = WeaponState{};
    u32 have = t.initial_have;
    if (have == 0) {
        u8 low = NO_WEAPON;
        for (u8 w = 0; w < t.weapon_count; ++w) {
            if (low == NO_WEAPON || t.weapons[w].order < t.weapons[low].order) low = w;
        }
        if (low != NO_WEAPON) have = 1u << low;
    }
    s.have = have;
    for (u8 w = 0; w < t.weapon_count; ++w) {
        if (!((have >> w) & 1u)) continue;
        if (s.current == NO_WEAPON || t.weapons[w].order > t.weapons[s.current].order) s.current = w;
    }
    for (u32 p = 0; p < t.pool_count; ++p) s.ammo[p] = t.pools[p].initial;
}

const WSwing* weapon_swing(const WeaponTable& t, const WeaponState& s) noexcept {
    if (s.armed >= t.weapon_count) return nullptr;
    const WeaponDef& d = t.weapons[s.armed];
    if (s.swing_phase < d.phase_count && d.phases[s.swing_phase].swing.present) {
        return &d.phases[s.swing_phase].swing;
    }
    return d.swing.present ? &d.swing : nullptr;
}

u32 step_weapon(const WeaponTable& t, WeaponState& s, WeaponBody& body,
                u8 buttons, u8 want, u32 tick, u32 agent, u64 seed,
                WeaponEventLog& log) noexcept {
    // The weapon wanted, if it is held: weapons.lua's W.select, carried in the
    // input now so a switch is predicted.
    if (want < t.weapon_count && ((s.have >> want) & 1u) && want != s.current) s.current = want;
    if (s.current >= t.weapon_count) { s.prev_buttons = buttons & 3u; return 0; }
    if (s.armed != s.current) arm(t, s, tick);

    const u8 held = buttons & 3u;
    const u8 before = s.prev_buttons;
    for (u8 trig = 0; trig < 2; ++trig) {
        if (held & bit_of(trig)) { if (s.held_since[trig] == 0) s.held_since[trig] = tick + 1; }
        else s.held_since[trig] = 0;
    }

    const u8 weapon = s.armed;
    const WeaponDef& def = t.weapons[weapon];
    Ctx c{t, s, body, def, weapon, held, before, tick, agent, seed, log};

    u32 hops = 0;
    for (;;) {
        if (s.phase >= def.phase_count) enter(s, def.initial, tick);
        if (s.phase >= def.phase_count) break;
        const WPhase& ph = def.phases[s.phase];
        c.elapsed = tick - s.entered;
        c.jump = NO_PHASE;

        // The sequence: each due step once, `goto` abandoning the rest.
        for (u8 i = 0; i < ph.step_count; ++i) {
            if ((s.done >> i) & 1u) continue;
            if (c.elapsed < ph.steps[i].at) continue;
            s.done |= 1u << i;
            perform(c, s.phase, i, ph.steps[i]);
            if (c.jump != NO_PHASE) break;
        }

        u8 to = c.jump;
        if (to == NO_PHASE) {
            for (u8 k = 0; k < ph.transition_count && to == NO_PHASE; ++k) {
                const WTransition& tr = ph.transitions[k];
                bool all = tr.count > 0;
                for (u8 j = 0; j < tr.count && all; ++j) all = cond_holds(c, tr.conds[j]);
                if (all) to = tr.to;
            }
        }
        if (to == NO_PHASE && ph.ticks != PHASE_WAITS && c.elapsed >= ph.ticks) {
            to = ph.next != NO_PHASE ? ph.next : def.initial;
        }
        if (to == NO_PHASE) break;
        if (++hops > WEAPON_HOPS) break;   // a transition loop: stop, let the tick finish
        enter(s, to, tick);
        if (to < def.phase_count && def.phases[to].swing.present) s.swing_phase = to;
    }
    s.prev_buttons = held;
    return hops;
}

void hash_weapon_state(StateHasher& h, const WeaponState& s) noexcept {
    h.u32v(s.have);
    h.byte(s.current); h.byte(s.phase); h.byte(s.armed); h.byte(s.swing_phase);
    h.byte(s.prev_buttons);
    h.u32v(s.entered); h.u32v(s.done);
    h.u32v(s.held_since[0]); h.u32v(s.held_since[1]);
    for (i32 v : s.vars) h.i32v(v);
    for (u16 a : s.ammo) h.u32v(a);
}

// --- the table on the wire --------------------------------------------------

namespace {

void put_name(ByteWriter& w, const char* n) noexcept {
    u8 len = 0;
    while (len < WEAPON_NAME_LEN && n[len] != 0) ++len;
    w.u8v(len);
    w.bytes(reinterpret_cast<const u8*>(n), len);
}

bool get_name(ByteReader& r, char* out) noexcept {
    const u8 len = r.u8v();
    if (!r.ok() || len > WEAPON_NAME_LEN) return false;
    const u8* p = r.bytes(len);
    if (p == nullptr) return false;
    for (u8 i = 0; i < len; ++i) out[i] = static_cast<char>(p[i]);
    out[len] = 0;
    return true;
}

void put_swing(ByteWriter& w, const WSwing& s) noexcept {
    w.u8v(s.present ? 1 : 0);
    if (!s.present) return;
    w.u16v(s.set);
    w.fxraw(s.reach); w.fxraw(s.half_arc); w.fxraw(s.sweep_from); w.fxraw(s.sweep_to);
    w.fxraw(s.min_reach); w.fxraw(s.pitch_from); w.fxraw(s.pitch_to); w.fxraw(s.half_pitch);
    w.u8v(s.ticks); w.u8v(s.cooldown);
}

bool get_swing(ByteReader& r, WSwing& s) noexcept {
    const u8 present = r.u8v();
    if (!r.ok() || present > 1) return false;
    s.present = present == 1;
    if (!s.present) return true;
    s.set = r.u16v();
    s.reach = r.fxraw(); s.half_arc = r.fxraw(); s.sweep_from = r.fxraw(); s.sweep_to = r.fxraw();
    s.min_reach = r.fxraw(); s.pitch_from = r.fxraw(); s.pitch_to = r.fxraw(); s.half_pitch = r.fxraw();
    s.ticks = r.u8v(); s.cooldown = r.u8v();
    return r.ok() && s.set < (1u << 10);
}

}  // namespace

bool encode_weapon_table(ByteWriter& w, const WeaponTable& t) noexcept {
    w.u8v(t.pool_count);
    for (u32 p = 0; p < t.pool_count; ++p) {
        put_name(w, t.pools[p].name);
        w.u16v(t.pools[p].max);
        w.u16v(t.pools[p].initial);
    }
    w.u32v(t.initial_have);
    w.u8v(t.weapon_count);
    for (u32 i = 0; i < t.weapon_count; ++i) {
        const WeaponDef& d = t.weapons[i];
        put_name(w, d.name);
        w.u16v(static_cast<u16>(d.order));
        w.u8v(static_cast<u8>(d.pool));
        w.u16v(d.per);
        w.u8v(d.initial);
        w.u8v(d.aim_snap);
        put_swing(w, d.swing);
        w.u8v(d.var_count);
        for (u32 v = 0; v < d.var_count; ++v) w.i32v(d.vars[v]);
        w.u8v(d.phase_count);
        for (u32 p = 0; p < d.phase_count; ++p) {
            const WPhase& ph = d.phases[p];
            put_name(w, ph.name);
            w.u16v(ph.anim); w.u16v(ph.ticks); w.u8v(ph.next);
            put_swing(w, ph.swing);
            w.u8v(ph.step_count);
            for (u32 k = 0; k < ph.step_count; ++k) {
                const WStep& s = ph.steps[k];
                w.u16v(s.at); w.u8v(static_cast<u8>(s.action)); w.u8v(s.var); w.u8v(s.phase);
                w.u8v(static_cast<u8>(s.pool)); w.i32v(s.value); w.u16v(s.state);
                w.fxraw(s.speed); w.fxraw(s.vertical); w.u8v(static_cast<u8>(s.towards));
                w.i32v(s.jitter); w.u8v(s.count); w.i32v(s.spread); w.i32v(s.arc); w.i32v(s.angle);
                w.fxraw(s.range); w.u16v(s.effect); w.u32v(s.sound);
                w.u8v(static_cast<u8>((s.predicted ? 1 : 0) | (s.attach ? 2 : 0)));
            }
            w.u8v(ph.transition_count);
            for (u32 k = 0; k < ph.transition_count; ++k) {
                const WTransition& tr = ph.transitions[k];
                w.u8v(tr.count); w.u8v(tr.to);
                for (u32 j = 0; j < tr.count; ++j) {
                    const WCondDef& c = tr.conds[j];
                    w.u8v(static_cast<u8>(c.kind)); w.u8v(c.trigger); w.u8v(c.var);
                    w.u8v(static_cast<u8>((c.flag ? 1 : 0) | (c.has ? 2 : 0)));
                    w.i32v(c.n); w.fxraw(c.speed);
                }
            }
        }
    }
    return w.ok();
}

bool decode_weapon_table(ByteReader& r, WeaponTable& out) noexcept {
    static WeaponTable t;      // ~MBs of fixed arrays: never on a stack
    t.clear();
    t.pool_count = r.u8v();
    if (!r.ok() || t.pool_count > MAX_AMMO_POOLS) return false;
    for (u32 p = 0; p < t.pool_count; ++p) {
        if (!get_name(r, t.pools[p].name)) return false;
        t.pools[p].max = r.u16v();
        t.pools[p].initial = r.u16v();
    }
    t.initial_have = r.u32v();
    t.weapon_count = r.u8v();
    if (!r.ok() || t.weapon_count > MAX_WEAPONS) return false;
    if (t.weapon_count < 32 && (t.initial_have >> t.weapon_count) != 0) return false;
    for (u32 i = 0; i < t.weapon_count; ++i) {
        WeaponDef& d = t.weapons[i];
        if (!get_name(r, d.name)) return false;
        d.order = static_cast<i16>(r.u16v());
        d.pool = static_cast<i8>(r.u8v());
        d.per = r.u16v();
        d.initial = r.u8v();
        d.aim_snap = r.u8v();
        if (!get_swing(r, d.swing)) return false;
        d.var_count = r.u8v();
        if (!r.ok() || d.var_count > MAX_WEAPON_VARS) return false;
        for (u32 v = 0; v < d.var_count; ++v) d.vars[v] = r.i32v();
        d.phase_count = r.u8v();
        if (!r.ok() || d.phase_count == 0 || d.phase_count > MAX_WEAPON_PHASES) return false;
        if (d.initial >= d.phase_count) return false;
        if (d.pool != NO_POOL && (d.pool < 0 || d.pool >= static_cast<i8>(t.pool_count))) return false;
        for (u32 p = 0; p < d.phase_count; ++p) {
            WPhase& ph = d.phases[p];
            if (!get_name(r, ph.name)) return false;
            ph.anim = r.u16v(); ph.ticks = r.u16v(); ph.next = r.u8v();
            if (!r.ok() || (ph.next != NO_PHASE && ph.next >= d.phase_count)) return false;
            if (!get_swing(r, ph.swing)) return false;
            ph.step_count = r.u8v();
            if (!r.ok() || ph.step_count > MAX_PHASE_STEPS) return false;
            for (u32 k = 0; k < ph.step_count; ++k) {
                WStep& s = ph.steps[k];
                s.at = r.u16v();
                const u8 act = r.u8v();
                s.var = r.u8v(); s.phase = r.u8v();
                s.pool = static_cast<i8>(r.u8v()); s.value = r.i32v(); s.state = r.u16v();
                s.speed = r.fxraw(); s.vertical = r.fxraw();
                const u8 tw = r.u8v();
                s.jitter = r.i32v(); s.count = r.u8v(); s.spread = r.i32v(); s.arc = r.i32v(); s.angle = r.i32v();
                s.range = r.fxraw(); s.effect = r.u16v(); s.sound = r.u32v();
                const u8 flags = r.u8v();
                if (!r.ok()) return false;
                const bool own = act <= static_cast<u8>(WAct::Impulse);
                const bool rep = act >= static_cast<u8>(WAct::Hitscan) && act <= static_cast<u8>(WAct::Sound);
                if (!own && !rep) return false;
                if (tw > 2 || flags > 3 || s.var >= MAX_WEAPON_VARS || s.count > MAX_PELLETS) return false;
                if (s.pool != NO_POOL && (s.pool < 0 || s.pool >= static_cast<i8>(t.pool_count))) return false;
                s.action = static_cast<WAct>(act);
                if (s.action == WAct::Goto && s.phase >= d.phase_count) return false;
                s.towards = static_cast<WTowards>(tw);
                s.predicted = flags & 1; s.attach = (flags & 2) != 0;
            }
            ph.transition_count = r.u8v();
            if (!r.ok() || ph.transition_count > MAX_PHASE_TRANSITIONS) return false;
            for (u32 k = 0; k < ph.transition_count; ++k) {
                WTransition& tr = ph.transitions[k];
                tr.count = r.u8v(); tr.to = r.u8v();
                if (!r.ok() || tr.count == 0 || tr.count > MAX_TRANSITION_CONDS || tr.to >= d.phase_count) return false;
                for (u32 j = 0; j < tr.count; ++j) {
                    WCondDef& c = tr.conds[j];
                    const u8 kind = r.u8v();
                    c.trigger = r.u8v(); c.var = r.u8v();
                    const u8 flags = r.u8v();
                    c.n = r.i32v(); c.speed = r.fxraw();
                    if (!r.ok() || kind > static_cast<u8>(WCond::Falling) || c.trigger > 1 ||
                        c.var >= MAX_WEAPON_VARS || flags > 3) return false;
                    c.kind = static_cast<WCond>(kind);
                    c.flag = flags & 1; c.has = (flags & 2) != 0;
                }
            }
        }
    }
    if (!r.ok()) return false;
    out = t;
    return true;
}

u64 WeaponTable::hash() const noexcept {
    static u8 buf[1 << 18];
    ByteWriter w(buf, sizeof(buf));
    encode_weapon_table(w, *this);
    StateHasher h;
    for (size_t i = 0; i < w.size(); ++i) h.byte(buf[i]);
    return h.value();
}

u8 WeaponTable::find(const char* name) const noexcept {
    for (u8 i = 0; i < weapon_count; ++i) {
        u32 k = 0;
        while (k <= WEAPON_NAME_LEN && weapons[i].name[k] == name[k] && name[k] != 0) ++k;
        if (k <= WEAPON_NAME_LEN && weapons[i].name[k] == 0 && name[k] == 0) return i;
    }
    return NO_WEAPON;
}

i8 WeaponTable::find_pool(const char* name) const noexcept {
    for (u8 i = 0; i < pool_count; ++i) {
        u32 k = 0;
        while (k <= WEAPON_NAME_LEN && pools[i].name[k] == name[k] && name[k] != 0) ++k;
        if (k <= WEAPON_NAME_LEN && pools[i].name[k] == 0 && name[k] == 0) return static_cast<i8>(i);
    }
    return NO_POOL;
}

}  // namespace dnp::sim
