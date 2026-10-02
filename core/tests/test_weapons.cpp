// THE WEAPON MACHINE IN THE CORE (core/include/dnp/sim/weapon.hpp), held to
// weapons.lua's tick_player rule by rule, each with a control.
//
// Small hand-built tables rather than the Doom data: each case here exists to
// make one rule observable, and a table that exercised five at once would let
// one bug hide behind another.
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "dnp/sim/weapon.hpp"

using namespace dnp;
using namespace dnp::sim;

static int g_checks = 0;
static int g_fails = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fails; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void name(char* d, const char* s) { std::strncpy(d, s, WEAPON_NAME_LEN); d[WEAPON_NAME_LEN] = 0; }

static WCondDef cond(WCond k, u8 trig = TRIGGER_PRIMARY, i32 n = 0) {
    WCondDef c; c.kind = k; c.trigger = trig; c.n = n; return c;
}
static void trans(WPhase& ph, u8 to, std::initializer_list<WCondDef> cs) {
    WTransition& t = ph.transitions[ph.transition_count++];
    t.to = to;
    for (const auto& c : cs) t.conds[t.count++] = c;
}
static WStep& step(WPhase& ph, WAct a, u16 at = 0) {
    WStep& s = ph.steps[ph.step_count++];
    s.action = a; s.at = at; return s;
}

// fists (order 0, no ammo) and a pistol (order 2, bullets, spread 3) and a
// chaingun (order 4, held refire), weapons.lua's Doom shape.
static WeaponTable& doomish() {
    static WeaponTable t;
    t.clear();
    t.pool_count = 1;
    name(t.pools[0].name, "bullets"); t.pools[0].max = 200; t.pools[0].initial = 3;

    // 0 fists: ready --pressed--> punch(6 ticks, a swing) --> ready
    WeaponDef& f = t.weapons[t.weapon_count++];
    name(f.name, "fists"); f.order = 0; f.pool = NO_POOL; f.phase_count = 2; f.initial = 0;
    name(f.phases[0].name, "ready");
    name(f.phases[1].name, "punch"); f.phases[1].ticks = 6;
    f.phases[1].swing.present = true; f.phases[1].swing.set = swing_field::REACH;
    f.phases[1].swing.reach = fx::from_int(3);
    trans(f.phases[0], 1, { cond(WCond::Pressed) });

    // 1 pistol: ready [held & no_ammo -> dry] [pressed -> fire]; fire 4 ticks:
    // consume, hitscan; dry: switch_if_empty, 1 tick.
    WeaponDef& p = t.weapons[t.weapon_count++];
    name(p.name, "pistol"); p.order = 2; p.pool = 0; p.per = 1; p.phase_count = 3; p.initial = 0;
    name(p.phases[0].name, "ready");
    name(p.phases[1].name, "fire"); p.phases[1].ticks = 4;
    name(p.phases[2].name, "dry"); p.phases[2].ticks = 1;
    trans(p.phases[0], 2, { cond(WCond::Held), cond(WCond::NoAmmo) });
    trans(p.phases[0], 1, { cond(WCond::Pressed) });
    step(p.phases[1], WAct::Consume);
    WStep& hs = step(p.phases[1], WAct::Hitscan); hs.spread = 3; hs.count = 1; hs.range = fx::from_int(40);
    step(p.phases[2], WAct::SwitchIfEmpty);

    // 2 chaingun: ready [held -> fire]; fire 2 ticks, consume + hitscan
    WeaponDef& g = t.weapons[t.weapon_count++];
    name(g.name, "chaingun"); g.order = 4; g.pool = 0; g.per = 1; g.phase_count = 2; g.initial = 0;
    name(g.phases[0].name, "ready");
    name(g.phases[1].name, "fire"); g.phases[1].ticks = 2;
    trans(g.phases[0], 1, { cond(WCond::Held), cond(WCond::HasAmmo) });
    step(g.phases[1], WAct::Consume);
    WStep& gs = step(g.phases[1], WAct::Hitscan); gs.count = 7; gs.spread = 14; gs.range = fx::from_int(40);

    t.initial_have = 0b011;      // fists and pistol
    return t;
}

struct Run {
    const WeaponTable& t;
    WeaponState s{};
    WeaponBody body{};
    WeaponEventLog log;
    u32 tick = 100;
    u32 hits = 0;
    explicit Run(const WeaponTable& tt) : t(tt) { weapon_fresh(t, s); }
    // One tick; returns this tick's hitscan event count.
    u32 step(u8 buttons, u8 want = NO_WEAPON) {
        log.clear();
        body.impulse = fx3{};
        step_weapon(t, s, body, buttons, want, tick++, 7, 0x5eed, log);
        u32 n = 0;
        for (u32 i = 0; i < log.count(); ++i) if (log.at(i).action == WAct::Hitscan) ++n;
        hits += n;
        return n;
    }
};

static void test_fresh_and_edges() {
    std::printf("weapons: a fresh player, and a press is an edge\n");
    WeaponTable& t = doomish();
    Run r(t);
    CHECK(r.s.have == 0b011 && r.s.current == 1);         // highest-order held
    CHECK(r.s.ammo[0] == 3);
    CHECK(r.step(0) == 0);                                 // arms, nothing pressed
    CHECK(r.s.armed == 1 && r.s.phase == 0);
    CHECK(r.step(1) == 1 && r.s.ammo[0] == 2);             // press: fire, consume, a shot
    for (int i = 0; i < 8; ++i) r.step(1);                 // HELD: a pistol does not refire
    CHECK(r.hits == 1 && r.s.ammo[0] == 2);
    CHECK(r.s.phase == 0);                                 // back to ready after 4 ticks
    r.step(0); CHECK(r.step(1) == 1 && r.s.ammo[0] == 1);  // release, press: again
}

static void test_held_refire_and_timing() {
    std::printf("weapons: held refire, at the phase's own rate\n");
    WeaponTable& t = doomish();
    Run r(t);
    r.s.have |= 0b100; r.s.ammo[0] = 50;
    r.step(0, 2);                                          // switch: a want
    CHECK(r.s.current == 2 && r.s.armed == 2);
    u32 shots = 0;
    for (int i = 0; i < 20; ++i) shots += r.step(1) ? 1 : 0;
    // ready->fire on the first held tick, fire lasts 2, back to ready then fire
    // again the same tick (the hop): one volley every 2 ticks.
    CHECK(shots == 10);
    CHECK(r.s.ammo[0] == 40);
    // CONTROL: released, it stops.
    u32 after = 0;
    for (int i = 0; i < 6; ++i) after += r.step(0);
    CHECK(after == 0);
}

static void test_dry_switches_to_usable() {
    std::printf("weapons: held on empty, it drops to the best weapon with ammunition\n");
    WeaponTable& t = doomish();
    Run r(t);
    r.s.ammo[0] = 0;
    r.step(0);
    r.step(1);                                             // held & no_ammo -> dry
    CHECK(r.s.phase == 2);
    r.step(1);                                             // dry runs switch_if_empty
    CHECK(r.s.current == 0);                               // the fists
    r.step(1);                                             // re-armed next tick
    CHECK(r.s.armed == 0);
    // CONTROL: with ammunition, the same press fires instead.
    Run q(t);
    q.step(0);
    CHECK(q.step(1) == 1 && q.s.current == 1);
}

static void test_spread_is_deterministic() {
    std::printf("weapons: the spread is the same draw on both sides, and really spreads\n");
    WeaponTable& t = doomish();
    Run a(t), b(t);
    a.s.have = b.s.have = 0b111; a.s.ammo[0] = b.s.ammo[0] = 50;
    a.step(0, 2); b.step(0, 2);
    a.step(1); b.step(1);
    const WeaponEvent* ea = nullptr; const WeaponEvent* eb = nullptr;
    for (u32 i = 0; i < a.log.count(); ++i) if (a.log.at(i).action == WAct::Hitscan) ea = &a.log.at(i);
    for (u32 i = 0; i < b.log.count(); ++i) if (b.log.at(i).action == WAct::Hitscan) eb = &b.log.at(i);
    CHECK(ea && eb && ea->count == 7);
    bool same = true, distinct = true, bounded = true;
    for (u32 i = 0; ea && eb && i < 7; ++i) {
        same = same && ea->offsets[i] == eb->offsets[i];
        for (u32 j = 0; j < i; ++j) distinct = distinct && ea->offsets[i] != ea->offsets[j];
        // within +-14 degrees
        const i64 lim = 14LL * 4294967296LL / 360 + 1;
        bounded = bounded && ea->offsets[i] >= -lim && ea->offsets[i] <= lim;
    }
    CHECK(same);        // two machines, one input: one volley
    CHECK(distinct);    // seven pellets, seven rays
    CHECK(bounded);
    // CONTROL: a different tick draws differently. (A copy: the log is reused.)
    const WeaponEvent first = *ea;
    a.step(0); a.step(0); a.step(1);
    const WeaponEvent* later = nullptr;
    for (u32 i = 0; i < a.log.count(); ++i) if (a.log.at(i).action == WAct::Hitscan) later = &a.log.at(i);
    CHECK(later && later->tick != first.tick && later->offsets[0] != first.offsets[0]);
}

static void test_goto_hops_and_vars() {
    std::printf("weapons: goto abandons the rest, a loop is stopped, vars re-arm\n");
    static WeaponTable t;
    t.clear();
    WeaponDef& w = t.weapons[t.weapon_count++];
    name(w.name, "odd"); w.phase_count = 3; w.initial = 0;
    w.var_count = 1; w.vars[0] = 5;
    name(w.phases[0].name, "a"); name(w.phases[1].name, "b"); name(w.phases[2].name, "c");
    // a: add_var +1, goto b, add_var +100 (never runs)
    WStep& s1 = step(w.phases[0], WAct::AddVar); s1.var = 0; s1.value = 1;
    WStep& s2 = step(w.phases[0], WAct::Goto); s2.phase = 1;
    WStep& s3 = step(w.phases[0], WAct::AddVar); s3.var = 0; s3.value = 100;
    // b <-> c on `always`: a loop the hop limit has to stop
    trans(w.phases[1], 2, { cond(WCond::Always) });
    trans(w.phases[2], 1, { cond(WCond::Always) });
    t.initial_have = 1;
    Run r(t);
    r.log.clear();
    const u32 hops = step_weapon(t, r.s, r.body, 0, NO_WEAPON, 1, 1, 0, r.log);
    CHECK(r.s.vars[0] == 6);                               // +1, and NOT +100
    CHECK(hops == WEAPON_HOPS + 1);                        // stopped, the tick finished
    // CONTROL: vars come back to the weapon's own on a re-arm.
    r.s.armed = NO_WEAPON;
    step_weapon(t, r.s, r.body, 0, NO_WEAPON, 2, 1, 0, r.log);
    CHECK(r.s.vars[0] == 6);                               // re-armed to 5, then a's +1
}

static void test_swing_persists_until_another() {
    std::printf("weapons: a phase's swing stays in force after the phase, until another\n");
    WeaponTable& t = doomish();
    Run r(t);
    r.step(0, 0);                                          // the fists
    CHECK(weapon_swing(t, r.s) == nullptr);                // the weapon has none of its own
    r.step(1);                                             // punch: its swing
    const WSwing* sw = weapon_swing(t, r.s);
    CHECK(sw && sw->reach == fx::from_int(3));
    for (int i = 0; i < 8; ++i) r.step(0);                 // back to ready
    CHECK(r.s.phase == 0);
    CHECK(weapon_swing(t, r.s) == sw);                     // still punch's: weapons.lua writes on entry only
    r.step(0, 1);                                          // CONTROL: a re-arm clears it
    CHECK(weapon_swing(t, r.s) == nullptr);
}

static void test_impulse_away() {
    std::printf("weapons: recoil is a velocity, predicted\n");
    static WeaponTable t;
    t.clear();
    WeaponDef& w = t.weapons[t.weapon_count++];
    name(w.name, "kick"); w.phase_count = 2; w.initial = 0;
    name(w.phases[0].name, "ready"); name(w.phases[1].name, "fire"); w.phases[1].ticks = 3;
    trans(w.phases[0], 1, { cond(WCond::Pressed) });
    WStep& s = step(w.phases[1], WAct::Impulse); s.speed = fx::from_int(4); s.towards = WTowards::Away;
    s.vertical = fx::from_int(1);
    t.initial_have = 1;
    Run r(t);
    r.body.facing = brad(0);                               // facing +x
    r.step(0);
    r.step(1);
    CHECK(r.body.impulse.x < fx::from_int(-3) && r.body.impulse.z == fx::from_int(1));
    r.step(1);
    CHECK(r.body.impulse.x == fx::zero());                 // CONTROL: once, not every tick held
}

static void test_table_round_trip() {
    std::printf("weapons: the table survives the wire, and a corrupt one is refused\n");
    WeaponTable& t = doomish();
    static u8 buf[1 << 18];
    ByteWriter w(buf, sizeof(buf));
    CHECK(encode_weapon_table(w, t));
    const size_t n = w.size();
    static WeaponTable back;
    ByteReader r(buf, n);
    CHECK(decode_weapon_table(r, back));
    CHECK(back.hash() == t.hash() && back.weapon_count == 3 && std::strcmp(back.weapons[1].name, "pistol") == 0);
    // Every cut is refused.
    int accepted = 0;
    for (size_t k = 0; k < n; ++k) {
        ByteReader rk(buf, k);
        static WeaponTable x;
        if (decode_weapon_table(rk, x)) ++accepted;
    }
    CHECK(accepted == 0);
    // A goto to a phase that does not exist is refused.
    WeaponTable& bad = doomish();
    WStep& g = step(bad.weapons[1].phases[1], WAct::Goto); g.phase = 9;
    ByteWriter wb(buf, sizeof(buf));
    encode_weapon_table(wb, bad);
    ByteReader rb(buf, wb.size());
    static WeaponTable y;
    CHECK(!decode_weapon_table(rb, y));
    // CONTROL: the table's hash moves when the table does.
    CHECK(bad.hash() != doomish().hash());
}

int main() {
    test_fresh_and_edges();
    test_held_refire_and_timing();
    test_dry_switches_to_usable();
    test_spread_is_deterministic();
    test_goto_hops_and_vars();
    test_swing_persists_until_another();
    test_impulse_away();
    test_table_round_trip();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
