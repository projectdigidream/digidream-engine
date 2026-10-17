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

static void test_negate() {
    std::printf("weapons: `negate` inverts a condition, and survives the wire\n");
    static WeaponTable t;
    t.clear();
    WeaponDef& w = t.weapons[t.weapon_count++];
    name(w.name, "vent"); w.phase_count = 3; w.initial = 0;
    w.var_count = 1; w.vars[0] = 0;
    name(w.phases[0].name, "ready");
    name(w.phases[1].name, "fire"); w.phases[1].ticks = 2;
    name(w.phases[2].name, "vent"); w.phases[2].ticks = 2;
    // ready: [pressed & heat is NOT 0 -> vent] [pressed -> fire]
    WCondDef hot = cond(WCond::VarIs); hot.var = 0; hot.n = 0; hot.negate = true;
    trans(w.phases[0], 2, { cond(WCond::Pressed), hot });
    trans(w.phases[0], 1, { cond(WCond::Pressed) });
    WStep& f = step(w.phases[1], WAct::AddVar); f.var = 0; f.value = 1;
    WStep& v = step(w.phases[2], WAct::SetVar); v.var = 0; v.value = 0;
    t.initial_have = 1;
    Run r(t);
    r.step(0);
    r.step(1); r.step(0); r.step(0); r.step(0);
    CHECK(r.s.vars[0] == 1);                               // cold: fired
    r.step(1);
    CHECK(r.s.phase == 2 && r.s.vars[0] == 0);             // hot: vented, not fired
    // CONTROL: the same table without `negate` vents when cold instead.
    static WeaponTable plain;
    plain = t;
    plain.weapons[0].phases[0].transitions[0].conds[1].negate = false;
    Run q(plain);
    q.step(0);
    q.step(1);
    CHECK(q.s.phase == 2 && q.s.vars[0] == 0);
    CHECK(plain.hash() != t.hash());                       // the flag is in the hash
    // The wire keeps it.
    static u8 buf[1 << 16];
    ByteWriter bw(buf, sizeof(buf));
    CHECK(encode_weapon_table(bw, t));
    static WeaponTable back;
    ByteReader br(buf, bw.size());
    CHECK(decode_weapon_table(br, back));
    CHECK(back.weapons[0].phases[0].transitions[0].conds[1].negate);
    CHECK(!back.weapons[0].phases[0].transitions[0].conds[0].negate);
}

// A BFG that may fire once every 20 ticks, and a pistol to switch to.
static WeaponTable& cooled() {
    static WeaponTable t;
    t.clear();
    WeaponDef& b = t.weapons[t.weapon_count++];
    name(b.name, "bfg"); b.order = 7; b.phase_count = 2; b.initial = 0;
    b.var_count = 2; b.vars[0] = 0; b.vars[1] = 0;      // 0: the cooldown, 1: a plain counter
    name(b.phases[0].name, "ready"); name(b.phases[1].name, "fire"); b.phases[1].ticks = 2;
    WCondDef ready = cond(WCond::CooldownReady); ready.var = 0;
    trans(b.phases[0], 1, { cond(WCond::Pressed), ready });
    WStep& cd = step(b.phases[1], WAct::SetCooldown); cd.var = 0; cd.value = 20;
    WStep& n = step(b.phases[1], WAct::AddVar); n.var = 1; n.value = 1;
    WStep& hs = step(b.phases[1], WAct::Hitscan); hs.count = 1; hs.range = fx::from_int(40);
    WeaponDef& p = t.weapons[t.weapon_count++];
    name(p.name, "pistol"); p.order = 2; p.phase_count = 1; p.initial = 0;
    name(p.phases[0].name, "ready");
    t.initial_have = 0b11;
    CHECK(weapon_assign_cooldowns(t));
    return t;
}

static void test_cooldown() {
    std::printf("weapons: a cooldown gates the next shot, and outlives a switch\n");
    WeaponTable& t = cooled();
    CHECK(t.weapons[0].cool_slot[0] == 0 && t.weapons[0].cool_slot[1] == NO_COOL);
    CHECK(t.weapons[1].cool_slot[0] == NO_COOL);
    Run r(t);
    r.step(0, 0);                                          // the bfg
    CHECK(r.s.current == 0);
    const u32 fired_at = r.tick;
    CHECK(r.step(1) == 1);                                 // ready: it fires
    CHECK(r.s.vars[0] == static_cast<i32>(fired_at + 20) && r.s.cool[0] == r.s.vars[0]);
    r.step(0); r.step(0);
    CHECK(r.step(1) == 0);                                 // cooling: a press does nothing
    r.step(0);
    r.step(0, 1); r.step(0, 1);                            // put it away...
    CHECK(r.s.armed == 1);
    r.step(0, 0);                                          // ...and take it out again
    CHECK(r.s.armed == 0 && r.s.vars[0] == static_cast<i32>(fired_at + 20));
    CHECK(r.s.vars[1] == 0);                               // CONTROL: a plain var starts again
    CHECK(r.step(1) == 0);                                 // still cooling
    while (r.tick < fired_at + 20) r.step(0);
    CHECK(r.step(1) == 1);                                 // the deadline passed: it fires
    // A fresh player and a reset start every cooldown again.
    WeaponState f{};
    weapon_fresh(t, f);
    CHECK(f.cool[0] == 0);
    // The wire does not carry the slots; the decoder derives the same ones.
    static u8 buf[1 << 16];
    ByteWriter w(buf, sizeof(buf));
    CHECK(encode_weapon_table(w, t));
    static WeaponTable back;
    ByteReader rd(buf, w.size());
    CHECK(decode_weapon_table(rd, back));
    CHECK(back.weapons[0].cool_slot[0] == 0 && back.weapons[0].cool_slot[1] == NO_COOL);
    // More than the core holds is refused.
    static WeaponTable many;
    many.clear();
    for (u32 k = 0; k < 3; ++k) {
        WeaponDef& d = many.weapons[many.weapon_count++];
        d.name[0] = static_cast<char>('a' + k); d.phase_count = 1; d.var_count = 3;
        for (u8 v = 0; v < 3; ++v) {
            WStep& c = step(d.phases[0], WAct::SetCooldown); c.var = v; c.value = 5;
        }
    }
    CHECK(!weapon_assign_cooldowns(many));                 // 9 > MAX_WEAPON_COOLDOWNS
    many.weapon_count = 2;
    CHECK(weapon_assign_cooldowns(many));                  // CONTROL: 6 fit
}

static void test_missile_spec_on_the_wire() {
    std::printf("weapons: a launch's gravity, homing and trail survive the wire\n");
    static WeaponTable t;
    t.clear();
    WeaponDef& w = t.weapons[t.weapon_count++];
    name(w.name, "lobber"); w.phase_count = 1; w.initial = 0;
    name(w.phases[0].name, "ready");
    WStep& l = step(w.phases[0], WAct::Launch);
    l.count = 1;
    l.missile.kind = 21; l.missile.speed = fx::from_int(6); l.missile.life = 400;
    l.missile.gravity = fx::from_frac(98, 10);
    l.missile.homes = true;
    l.missile.bounces = true;
    l.missile.throws = true;
    l.missile.trail = 13;
    static u8 buf[1 << 16];
    ByteWriter bw(buf, sizeof(buf));
    CHECK(encode_weapon_table(bw, t));
    static WeaponTable back;
    ByteReader br(buf, bw.size());
    CHECK(decode_weapon_table(br, back));
    const WMissile& m = back.weapons[0].phases[0].steps[0].missile;
    CHECK(m.gravity == fx::from_frac(98, 10) && m.homes && m.bounces && m.throws);
    CHECK(m.trail == 13);   // what the shooter's client keeps on its ghost
    // CONTROL: the hash moves with each, so a client running a table that
    // flies its missiles differently is a detected mismatch.
    static WeaponTable other;
    other = t;
    other.weapons[0].phases[0].steps[0].missile.gravity = fx::zero();
    CHECK(other.hash() != t.hash());
    other = t;
    other.weapons[0].phases[0].steps[0].missile.homes = false;
    CHECK(other.hash() != t.hash());
    other = t;
    other.weapons[0].phases[0].steps[0].missile.bounces = false;
    CHECK(other.hash() != t.hash());
    other = t;
    other.weapons[0].phases[0].steps[0].missile.throws = false;
    CHECK(other.hash() != t.hash());
    other = t;
    other.weapons[0].phases[0].steps[0].missile.trail = 0;
    CHECK(other.hash() != t.hash());
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

// A `swing` step: the machine names it to the body, the last of a tick winning;
// its own shape survives the wire through the table's pool; and the state that
// names it is in the hash.
static void test_swing_steps() {
    std::printf("weapons: a swing step asks for a swing, and its shape travels with the table\n");
    WeaponTable& t = doomish();
    // fists' punch swings at 0 with no shape, then at 0 again with a reach of
    // its own: two in one tick, and the later is the one.
    WPhase& punch = t.weapons[0].phases[1];
    step(punch, WAct::Swing, 0);
    WStep& own = step(punch, WAct::Swing, 0);
    WSwing& o = t.step_swings[t.step_swing_count];
    o.present = true; o.set = swing_field::REACH | swing_field::HALF_ARC;
    o.reach = fx::from_int(5); o.half_arc = fx::from_frac(1, 16);
    own.swing = t.step_swing_count++;

    Run r{t};
    r.body = WeaponBody{};
    r.step(0, 0);                                  // to the fists
    CHECK(r.s.armed == 0 && !r.body.swing);        // CONTROL: resting, nothing asked
    r.body = WeaponBody{};
    r.step(1);                                     // pressed: into punch, both steps due
    CHECK(r.body.swing && r.body.swing_weapon == 0 && r.body.swing_phase == 1 && r.body.swing_step == 1);
    CHECK(t.step_swing(0, 1, 1) == &t.step_swings[own.swing]);
    CHECK(t.step_swing(0, 1, 0) == nullptr);       // the step with no shape of its own
    CHECK(t.step_swing(0, 1, 2) == nullptr && t.step_swing(9, 1, 1) == nullptr);   // out of range

    // The wire: the pool is the decoder's, the shape comes back on its step.
    static u8 buf[1 << 18];
    ByteWriter w(buf, sizeof(buf));
    CHECK(encode_weapon_table(w, t));
    static WeaponTable back;
    ByteReader rd(buf, w.size());
    CHECK(decode_weapon_table(rd, back));
    const WSwing* b = back.step_swing(0, 1, 1);
    CHECK(b != nullptr && b->set == o.set && b->reach == o.reach && b->half_arc == o.half_arc);
    CHECK(back.step_swing_count == 1 && back.step_swing(0, 1, 0) == nullptr);
    CHECK(back.hash() == t.hash());

    // More step shapes than the pool holds are refused, not wrapped: one more
    // than it holds, over every phase of the three weapons, each with a shape.
    // (doomish() is one static table: `o` is about to be cleared with it.)
    const WSwing shape = o;
    WeaponTable& many = doomish();
    many.step_swing_count = 1;
    many.step_swings[0] = shape;
    u32 swings = 0;
    for (u8 k = 0; k < many.weapon_count && swings <= MAX_STEP_SWINGS; ++k) {
        for (u8 p = 0; p < many.weapons[k].phase_count && swings <= MAX_STEP_SWINGS; ++p) {
            WPhase& ph = many.weapons[k].phases[p];
            while (ph.step_count < MAX_PHASE_STEPS && swings <= MAX_STEP_SWINGS) {
                step(ph, WAct::Swing, 0).swing = 0;    // the encoder writes each inline
                ++swings;
            }
        }
    }
    CHECK(swings == MAX_STEP_SWINGS + 1);
    ByteWriter wm(buf, sizeof(buf));
    CHECK(encode_weapon_table(wm, many));
    ByteReader rm(buf, wm.size());
    static WeaponTable m2;
    CHECK(!decode_weapon_table(rm, m2));
    // CONTROL: one fewer is a table.
    WPhase& last = [&]() -> WPhase& {
        for (int k = many.weapon_count - 1; k >= 0; --k)
            for (int p = many.weapons[k].phase_count - 1; p >= 0; --p)
                if (many.weapons[k].phases[p].step_count > 0 &&
                    many.weapons[k].phases[p].steps[many.weapons[k].phases[p].step_count - 1].action == WAct::Swing)
                    return many.weapons[k].phases[p];
        return many.weapons[0].phases[0];
    }();
    --last.step_count;
    ByteWriter wm2(buf, sizeof(buf));
    CHECK(encode_weapon_table(wm2, many));
    ByteReader rm2(buf, wm2.size());
    CHECK(decode_weapon_table(rm2, m2) && m2.step_swing_count == MAX_STEP_SWINGS);

    // The state that names the step is hashed: a client naming another step
    // is a misprediction, not a silent difference.
    WeaponState s1{}, s2{};
    s2.swung_step = 1;
    StateHasher h1, h2;
    hash_weapon_state(h1, s1);
    hash_weapon_state(h2, s2);
    CHECK(h1.value() != h2.value());
}

int main() {
    test_swing_steps();
    test_fresh_and_edges();
    test_held_refire_and_timing();
    test_dry_switches_to_usable();
    test_spread_is_deterministic();
    test_goto_hops_and_vars();
    test_swing_persists_until_another();
    test_impulse_away();
    test_negate();
    test_cooldown();
    test_missile_spec_on_the_wire();
    test_table_round_trip();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
