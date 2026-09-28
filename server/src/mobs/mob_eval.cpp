//
// The conditions, in C++.
//
// EACH ONE IS A TRANSCRIPTION of the corresponding `conds.*` in
// maps/scripts/mobs.lua, and the transcription has to be exact rather than
// equivalent: two runtimes that agree about what a monster does most of the
// time are two monsters. Where the Lua is subtle the reason is repeated here
// rather than referred to, because the next person to change one of these will
// be reading this file and not that one.
//
// WHAT "EXACT" RULES OUT, concretely:
//
//   No floats. `target_within` compares squared distances in the same 128-bit
//   integer arithmetic the fx metamethods use, so the answer is the same at
//   the boundary and not merely near it.
//
//   No libm. `target_in_front` needs an angle, and it takes it from the
//   engine's CORDIC atan2 -- the same one dnp.atan2 hands Lua. A
//   double-precision atan2 would produce an angle the simulation could not
//   have produced.
//
//   The RNG is the world's. `chance` is the one condition that is not a pure
//   read, and it must draw from the same stream in the same order, or a replay
//   stops reproducing.
//
#include "mobs/mob_runtime.hpp"

#include "dnp/math/trig.hpp"
#include "dnp/physics/collide.hpp"

#include <cstring>

#include "dnp/nav/navgraph.hpp"

namespace dnp::server::mobs {

namespace {

// A ray reaches it.
//
// NOT transcribed any more: this and l_dnp_can_see call the same function in
// core, because the two runtimes answering the same question in two places is
// the thing this whole arrangement is arranged against -- and the sight rule
// has now collected three decisions that each cost a bug to find. The cast is
// at EYE height because a two-sided line with a balcony behind it is a wall to
// look through; it SLOPES because a flat cast at the looker's own height is a
// different line from the one to its target; and the target is a COLUMN
// because a body whose head is over a ledge your eyeline runs under is visible.
bool can_see(const sim::World& w, u32 from_a, u32 to_a) {
    const geom::MapData* m = w.map();
    if (m == nullptr) return false;
    const sim::AgentState& a = w.agent(from_a);
    const sim::AgentState& b = w.agent(to_a);
    const fx3 eye{a.pos.x, a.pos.y, a.pos.z + w.params(from_a).height / 2};
    return physics::can_see(*m, a.mv.sector, eye, fx2{b.pos.x, b.pos.y},
                            b.pos.z, b.pos.z + w.params(to_a).height);
}

}  // namespace

bool evaluate(const MobGraph& g, const Kind& k, const Mob& m,
              const Condition& c, const EvalWorld& w, const i32* vars) {
    (void)g;
    const sim::World& world = *w.world;

    switch (c.id) {
    case Cond::HasTarget:
        return m.has_tp;

    case Cond::TargetWithin: {
        // PLAN distance, not 3D: a cacodemon above you is still near you.
        //
        // Squared on both sides and never rooted, which is what the Lua does --
        // and in 128 bits, because two coordinates a map apart square past 64.
        if (!m.has_tp) return false;
        const fx dx = m.tp.x - m.p.x;
        const fx dy = m.tp.y - m.p.y;
        const u128 d_sq = len_sq_raw(fx2{dx, dy});
        const u128 r_sq = static_cast<u128>(static_cast<i128>(c.f.raw) * c.f.raw);
        return d_sq < r_sq;
    }

    case Cond::TargetVisible:
        if (!m.has_tp || m.target < 0) return false;
        return can_see(world, m.entity, static_cast<u32>(m.target));

    case Cond::TargetInFront: {
        if (!m.has_tp) return false;
        const i32 arc = c.i != 0 ? c.i : 90;
        const u32 want = fx_atan2(m.tp.y - m.p.y, m.tp.x - m.p.x).raw;
        // THE BODY'S FACING, not the mob's aim. `aim` is where it has DECIDED
        // to look; facing is where it is actually pointing, and turning is a
        // rate -- so the two differ for as long as a monster is coming round.
        // Comparing against the aim asks "have I decided to look at you",
        // which is true the instant a target is acquired: monsters fired while
        // still turned away, and hit each other instead of the player.
        const u32 have = world.agent(m.entity).facing.raw;
        // A turn is 2^32 and the shortest way round is the SIGNED difference.
        // Subtracting as plain numbers is what made a monster refuse to shoot
        // at anything just past due north.
        i64 d = static_cast<i64>(want) - static_cast<i64>(have);
        while (d > 2147483647LL) d -= 4294967296LL;
        while (d < -2147483648LL) d += 4294967296LL;
        if (d < 0) d = -d;
        // arc * 2^32 / 360, in integers and in that order, so the rounding is
        // the same one Lua's `arc * 4294967296 / 360` produces.
        return d <= static_cast<i64>(arc) * 4294967296LL / 360;
    }

    case Cond::HpBelow:
        // Of its own STARTING hp, and multiplied out rather than divided, so
        // there is no integer division to round differently.
        return m.hp * 100 < m.max_hp * c.i;

    case Cond::StateElapsed:
        return static_cast<i64>(w.tick) - static_cast<i64>(m.entered) >= c.i;

    case Cond::Chance:
        // dnp.random(1, 100) <= percent, and the binding's dnp.random(lo, hi)
        // is next_range(lo, hi + 1) -- which for 1..100 is next_below(100) + 1.
        // Written that way rather than as next_range so the equivalence is
        // visible: the draw has to come off the same stream in the same order
        // or a replay stops reproducing.
        //
        // Exactly ONE draw per evaluation, which is what the vocabulary
        // promises. Two would desynchronise every later `chance` in the tick.
        return w.rng != nullptr &&
               static_cast<i32>(w.rng->next_below(100)) + 1 <= c.i;

    case Cond::VarAtLeast:
        return c.var >= 0 && vars[c.var] >= c.i;

    case Cond::VarIs:
        return c.var >= 0 && vars[c.var] == c.i;

    case Cond::CooldownReady:
        // A DEADLINE, not a counter: the var holds an absolute tick, so a mob
        // nobody is near is not paying to tick one down.
        return c.var >= 0 && static_cast<i64>(w.tick) >= vars[c.var];

    case Cond::Grounded:
        return world.agent(m.entity).mv.grounded;

    case Cond::WasHurt: {
        // Default 1 -- this tick. `hurt_ever` is what stands in for Lua's nil:
        // without it a mob that has never been hurt reads as hurt on tick 0.
        const i32 within = c.i != 0 ? c.i : 1;
        return m.hurt_ever &&
               static_cast<i64>(w.tick) - static_cast<i64>(m.hurt_at) < within;
    }

    case Cond::HurtByMob:
        return m.hurt_by_mob;

    case Cond::MobWithin:
    case Cond::CountWithin: {
        // PLAN distance squared, like target_within and for the same reason.
        // Itself never counts.
        if (w.mobs == nullptr || w.graph == nullptr) return false;
        const u128 r_sq = static_cast<u128>(static_cast<i128>(c.f.raw) * c.f.raw);
        const i32 want = (c.id == Cond::CountWithin) ? (c.i > 0 ? c.i : 1) : 1;
        i32 found = 0;
        for (const Mob& o : *w.mobs) {
            if (o.kind < 0 || o.entity == m.entity) continue;
            if (!world.agent(o.entity).active) continue;
            // `alive` defaults true; count_within never counts corpses.
            if ((c.id == Cond::CountWithin || c.flag) && o.dead) continue;
            if (c.name != NO_NAME) {
                const Kind& ok = w.graph->kinds[static_cast<u32>(o.kind)];
                if (ok.name != c.name) continue;
            }
            const fx3 q = world.agent(o.entity).pos;
            const u128 d_sq = len_sq_raw(fx2{q.x - m.p.x, q.y - m.p.y});
            if (d_sq > r_sq) continue;
            // Stop as soon as the answer cannot change.
            if (++found >= want) return true;
        }
        return false;
    }

    case Cond::TargetAbove:
        // The FEET, both of them, which is what `by` is measured between.
        return m.has_tp && (m.tp.z - m.p.z) >= c.f;

    case Cond::TargetBelow:
        return m.has_tp && (m.p.z - m.tp.z) >= c.f;

    case Cond::AllyInFront: {
        // The line this mob would SHOOT along, which is its body's facing --
        // not its aim. Turning is a rate, so the two differ for as long as it
        // is coming round, and a shot leaves along the body.
        if (w.mobs == nullptr || w.graph == nullptr) return false;
        const Kind& mine = *&k;
        fx reach = c.f;
        if (reach.raw == 0) {
            if (!mine.has_sight) return false;
            reach = mine.sight;
        }
        const u128 r_sq = static_cast<u128>(static_cast<i128>(reach.raw) * reach.raw);
        const i32 arc = c.i != 0 ? c.i : 12;
        const i64 half = static_cast<i64>(arc) * 4294967296ll / 360;
        const u32 have = world.agent(m.entity).facing.raw;
        for (const Mob& o : *w.mobs) {
            if (o.kind < 0 || o.entity == m.entity || o.dead) continue;
            if (!world.agent(o.entity).active) continue;
            if (c.name != NO_NAME) {
                const Kind& ok = w.graph->kinds[static_cast<u32>(o.kind)];
                if (ok.name != c.name) continue;
            }
            const fx3 q = world.agent(o.entity).pos;
            const fx dx = q.x - m.p.x;
            const fx dy = q.y - m.p.y;
            if (len_sq_raw(fx2{dx, dy}) > r_sq) continue;
            i64 d = static_cast<i64>(fx_atan2(dy, dx).raw) - static_cast<i64>(have);
            while (d > 2147483647LL) d -= 4294967296LL;
            while (d < -2147483648LL) d += 4294967296LL;
            if (d < 0) d = -d;
            if (d <= half) return true;
        }
        return false;
    }

    case Cond::CanReach: {
        // A ROUTE EXISTS for a body this size. The expensive one: an A* per
        // ask, against the same sixty-four-a-tick budget dnp.find_path spends,
        // because one budget with two spenders and only one of them counting
        // is a difference that appears only at scale.
        if (!m.has_tp || m.target < 0 || w.nav == nullptr || w.ctx == nullptr) return false;
        const geom::MapData* map = world.map();
        if (map == nullptr) return false;
        const i32 here = world.agent(m.entity).mv.sector;
        const i32 there = world.agent(static_cast<u32>(m.target)).mv.sector;
        if (here < 0 || there < 0) return false;
        if (here == there) return true;
        if (!w.ctx->take_path_budget(w.tick)) return false;

        nav::NavBody body;
        body.radius = world.params(m.entity).radius;
        body.height = world.params(m.entity).height;
        body.jump_height = k.jump_height;
        static nav::NavQuery query;          // tens of kilobytes; never a local
        static u32 path[nav::MAX_NAV_NODES];
        const nav::PathResult r =
            nav::find_path(*map, *w.nav, query, static_cast<u32>(here),
                           static_cast<u32>(there), body, path, nav::MAX_NAV_NODES);
        return r.status == nav::PathStatus::Ok ||
               r.status == nav::PathStatus::SameSector;
    }

    case Cond::Stuck: {
        // How long the anchor has stood. `ticks` defaults to 16 -- two of the
        // mob's own decisions -- and a zero means absent, as everywhere else
        // here: the graph carries the argument, and nobody asks for zero.
        if (!m.has_anchor) return false;
        const i32 within = c.i != 0 ? c.i : 16;
        return static_cast<i64>(w.tick) - static_cast<i64>(m.still_since) >= within;
    }

    case Cond::PropWithin: {
        // Props are the other half of the entity index space and have no
        // graph; this is how a brain notices the furniture.
        const u128 r_sq = static_cast<u128>(static_cast<i128>(c.f.raw) * c.f.raw);
        const char* want = (c.name != NO_NAME && w.graph) ? w.graph->str(c.name) : nullptr;
        for (u32 e = sim::MAX_AGENTS; e < world.prop_high_water(); ++e) {
            const sim::PropState& p = world.prop(e);
            if (!p.active) continue;
            if (want != nullptr) {
                // BY NAME, because a number in a script is a number somebody
                // has to look up in a file the script does not mention.
                if (w.props == nullptr) continue;
                const char* have = w.props->name_of(p.kind);
                if (have == nullptr || std::strcmp(have, want) != 0) continue;
            }
            const u128 d_sq = len_sq_raw(fx2{p.pos.x - m.p.x, p.pos.y - m.p.y});
            if (d_sq <= r_sq) return true;
        }
        return false;
    }

    case Cond::Unsupported:
        // Unreachable: a kind with one of these never runs here. Answering
        // false rather than asserting, because a wrong answer in a tick loop
        // is a monster standing still and an assert is the server gone.
        return false;
    }
    return false;
}

bool holds(const MobGraph& g, const Kind& k, const Mob& m,
           const Transition& t, const EvalWorld& w, const i32* vars) {
    for (u16 i = 0; i < t.cond_count; ++i) {
        const Condition& c = g.conditions[t.first_cond + i];
        bool v = evaluate(g, k, m, c, w, vars);
        if (c.negate) v = !v;
        if (!v) return false;
    }
    return true;
}

}  // namespace dnp::server::mobs
