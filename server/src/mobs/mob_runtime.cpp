//
// The per-tick mob loop, in C++.
//
// A TRANSCRIPTION of maps/scripts/mobs.lua's on_tick, think, drive, enter and
// steer_target, and it has to be exact rather than equivalent: two runtimes
// that agree about what a monster does most of the time are two monsters. The
// reasons behind the subtle parts are repeated here rather than referred to,
// because whoever changes one of these will be reading this file.
//
// THE THREE RULES THAT MAKE IT AFFORDABLE are the Lua's, unchanged, and none
// of them is an implementation detail:
//
//   think buckets   a mob decides every THINK-th tick, and the bucket is a
//                   LIST rather than a filter over all of them. Walking every
//                   mob to ask whether it is due throws away seven eighths of
//                   the answers.
//   deadlines       every timer is an absolute tick, compared when the mob is
//                   already in hand. A countdown has to be visited.
//   sleep           with no player in the level nothing here is defined -- a
//                   mob chases a target, aims at one, decides whether it can
//                   see one.
//
// WHAT STAYS IN LUA: every action that touches the world -- hitscan, missile,
// spawn, revive, the blast radius, the escape hatch. They are called back at
// the tick they would have fired, through the same `acts` table, so there is
// one implementation of each. What runs here is the handful that write state
// this file owns (set_var, add_var, set_cooldown, set_anim, goto, face) plus
// the decision itself, which is the part that runs every think.
//
#include "mobs/mob_runtime.hpp"

#include "dnp/math/trig.hpp"
#include "lua/world_binding.hpp"

namespace dnp::server::mobs {

// How often a mob may ask for a new route, and mobs.lua's reason for it: A*
// over a Doom level is cheap but not free, and the binding refuses past 64
// calls a tick -- so a level's worth of monsters all repathing on the same
// tick would have most of them silently refused. Spread by index.
constexpr u32 REPATH_TICKS = 30;

// A route is followed sector by sector and re-planned every REPATH_TICKS, so a
// cap this far out is never reached by anything that is still following it.
// Counted when it is, because a silent truncation is a monster that walks
// straight at a wall for reasons nothing reports.
constexpr u32 MAX_ROUTE = 48;


namespace {

// The nearest player by PLAN distance squared -- no square root, and no
// vertical term. A cacodemon above you is still near you, and taking a root
// per mob to compare two distances is work for an answer nobody reads.
struct Nearest {
    i32  agent = -1;
    u128 d_sq = 0;
    fx3  pos{};
};

Nearest nearest_player(const sim::World& w, fx3 from) {
    Nearest best;
    bool any = false;
    for (u32 i = 0; i < w.agent_high_water(); ++i) {
        const sim::AgentState& a = w.agent(i);
        // A player is an agent with no catalogue kind: nobody placed it, so it
        // walked in. The same test mobs.lua's dnp.players() makes.
        if (!a.active || a.kind != 0) continue;
        const fx dx = a.pos.x - from.x;
        const fx dy = a.pos.y - from.y;
        const u128 d2 = len_sq_raw(fx2{dx, dy});
        if (!any || d2 < best.d_sq) {
            best.agent = static_cast<i32>(i);
            best.d_sq = d2;
            best.pos = a.pos;
            any = true;
        }
    }
    return best;
}

}  // namespace

// ---------------------------------------------------------------------------

// set_anim and set_move DO NOTHING when nothing would change.
//
// For a monster nobody has walked near yet that is every decision it will ever
// make -- and each one it skips is a command that does not enter the queue,
// does not replicate and does not appear in a snapshot.
void set_anim(sim::World& w, Mob& m, i32 anim) {
    if (anim < 0 || m.anim == anim) return;
    m.anim = anim;
    w.agent_mut(m.entity).animation_state = static_cast<u16>(anim);
}

void set_move(sim::World& w, Mob& m, fx mx, fx my, u32 aim, u16 buttons) {
    if (m.input_sent && m.mx.raw == mx.raw && m.my.raw == my.raw &&
        m.sent_aim == aim && m.buttons == buttons) {
        return;
    }
    m.input_sent = true;
    m.mx = mx; m.my = my; m.sent_aim = aim; m.buttons = buttons;

    // Q1.15 on the wire, and a script writes them the same way, so a scripted
    // agent's input is byte-identical to one that arrived over the network.
    // Clamped rather than wrapped: asking to move at 3 gets full speed, not a
    // reversal. Transcribed from l_entity_set_input.
    const auto axis = [](fx v) {
        const fx one = fx::one();
        const fx c = v > one ? one : (v < -one ? -one : v);
        return static_cast<i16>((c.raw * 32767) >> 32);
    };
    proto::InputFrame f;
    f.move_x = axis(mx);
    f.move_y = axis(my);
    f.aim = brad(aim);
    f.buttons = buttons;
    w.set_input(m.entity, f);
}


// Entering a state, exactly as mobs.lua's enter() does it.
//
// A missing state is a NO-OP rather than an error, which mobs.lua relies on:
// `corpse = true` with no `death` state dies in whatever pose it was in, and
// the loader complains about that at load rather than the runtime at tick.
void enter(MobRuntime& rt, sim::World& w, Mob& m, i32 state, u32 tick) {
    const Kind& k = rt.graph.kinds[static_cast<u32>(m.kind)];
    if (state < 0 || static_cast<u32>(state) >= k.state_count) return;
    const State& st = rt.graph.states[k.first_state + state];
    m.state = static_cast<i16>(state);
    m.entered = tick;
    m.seq_i = 0;
    set_anim(w, m, st.anim);
    if (st.solid >= 0) w.agent_mut(m.entity).solid = st.solid != 0;
    if (st.move == Move::Stop) set_move(w, m, fx::zero(), fx::zero(), m.aim, 0);
}

// Where the next waypoint is, or the target itself.
//
// STRAIGHT AT IT is the fallback, not the plan: it is what this did before
// pathfinding existed, and it is why monsters pressed into corners -- "walk
// towards the player" is only the right move when nothing is in the way.
bool steer_target(MobRuntime& rt, const sim::World& w, const nav::NavGraph* graph,
                  nav::NavQuery& q, Mob& m, u32 tick, fx2& out) {
    if (!m.has_tp) return false;
    out = fx2{m.tp.x, m.tp.y};
    const i32 here = w.agent(m.entity).mv.sector;
    if (here < 0 || graph == nullptr || w.map() == nullptr) return true;

    // The target's sector is only needed when a repath might happen. Asking
    // for it unconditionally is work per mob per think for an answer that is
    // thrown away most of the time.
    const bool due = tick >= m.repath_at;
    const i32 there = (due || m.route_len == 0)
                          ? (m.target >= 0 ? w.agent(static_cast<u32>(m.target)).mv.sector : -1)
                          : m.route_to;
    if (there < 0) return true;

    // Same sector: no route needed, and asking for one is the storm the budget
    // exists to stop.
    if (here == there) { m.route_len = 0; return true; }

    const bool stale = m.route_len == 0 || m.route_to != there || due;
    if (stale) {
        // Spread by index, so a level's worth of monsters do not all repath on
        // the same tick and have most of them refused.
        m.repath_at = tick + REPATH_TICKS + (m.entity % REPATH_TICKS);
        m.route_to = there;
        const Kind& k = rt.graph.kinds[static_cast<u32>(m.kind)];
        nav::NavBody body;
        body.radius = w.params(m.entity).radius;
        body.height = w.params(m.entity).height;
        body.jump_height = k.jump_height;

        static u32 path[nav::MAX_NAV_NODES];
        static bool jumps[nav::MAX_NAV_NODES];
        const nav::PathResult r =
            nav::find_path(*w.map(), *graph, q, static_cast<u32>(here),
                           static_cast<u32>(there), body, path, nav::MAX_NAV_NODES, jumps);
        m.route_len = 0;
        if (r.status == nav::PathStatus::Ok || r.status == nav::PathStatus::SameSector) {
            u32 n = r.length;
            if (n > MAX_ROUTE) { n = MAX_ROUTE; ++rt.routes_truncated; }
            const u32 base = static_cast<u32>(rt.route.size());
            for (u32 i = 0; i < n; ++i) {
                rt.route.push_back(static_cast<u16>(path[i]));
                rt.route_jump.push_back(jumps[i] ? 1 : 0);
            }
            m.route_at = base;
            m.route_len = n;
            m.route_i = 1;          // [0] is the sector it is already in
        }
    }
    if (m.route_len == 0) return true;

    // Advance past waypoints already reached. Compared by SECTOR rather than
    // by distance: a mob is at a waypoint when it is IN that sector, and a
    // radius test would have it circle a centroid it cannot stand on.
    while (m.route_i < m.route_len &&
           static_cast<i32>(rt.route[m.route_at + m.route_i]) == here) {
        ++m.route_i;
    }
    if (m.route_i >= m.route_len) return true;
    const u32 node = rt.route[m.route_at + m.route_i];
    // A ROUTE THROUGH A JUMP IS A PROMISE. The graph planned this step on the
    // understanding that the body would hop; walking it instead means pressing
    // into the ledge forever, which is worse than never having routed here.
    m.want_jump = rt.route_jump[m.route_at + m.route_i] != 0;
    out = graph->node(node).centroid;
    return true;
}

// How the body is driven. On the THINK tick, not every tick: set_input is
// STICKY -- the engine keeps applying the last frame -- so a mob walks
// smoothly on an input set eight ticks ago and only the DIRECTION updates at
// 7.5 Hz, which is more often than Doom turned a monster.
void drive(MobRuntime& rt, sim::World& w, const nav::NavGraph* graph,
           nav::NavQuery& q, Mob& m, u32 tick) {
    const Kind& k = rt.graph.kinds[static_cast<u32>(m.kind)];
    if (m.state < 0) return;
    const State& st = rt.graph.states[k.first_state + m.state];
    if (st.move == Move::Free) return;

    u32 aim = m.aim;
    if (st.face == Face::Target && m.has_tp) {
        aim = fx_atan2(m.tp.y - m.p.y, m.tp.x - m.p.x).raw;
    }

    // WHERE IT WALKS and WHERE IT LOOKS are different questions. It faces the
    // target -- so a shot goes at you, and a monster rounding a corner is
    // still looking at you -- while its feet follow the route. Steering by the
    // aim is what made monsters walk into the wall between you and them.
    u32 walk_aim = aim;
    m.want_jump = false;
    if (st.move == Move::Chase) {
        fx2 wp{};
        if (steer_target(rt, w, graph, q, m, tick, wp)) {
            walk_aim = fx_atan2(wp.y - m.p.y, wp.x - m.p.x).raw;
        }
    }

    if (st.move == Move::Stop) {
        set_move(w, m, fx::zero(), fx::zero(), aim, 0);
        m.aim = aim;
        return;
    }

    u32 ang = walk_aim;
    if (st.move == Move::Flee) ang = aim + 2147483648u;
    else if (st.move == Move::Strafe) ang = aim + 1073741824u;
    else if (st.move == Move::Wander) ang = m.wander;

    // HELD, not tapped. The engine's jump is edge-triggered off the button, so
    // holding it while approaching the ledge means the hop happens on the tick
    // the body is grounded and in reach rather than on whichever tick the
    // script guessed.
    const u16 buttons = (m.want_jump && st.move == Move::Chase)
                            ? static_cast<u16>(proto::button::JUMP) : 0;

    // A WORLD direction, not "forward". move_x/move_y are world axes, because
    // the client computes a screen-relative direction and sends the world
    // vector -- so the engine never has to know where a camera is.
    const fx2 d = fx_dir(brad(ang));
    set_move(w, m, d.x, d.y, aim, buttons);
    m.aim = aim;

    // A flyer holds a height above its target rather than falling. No engine
    // support needed: gravity is off for it and this is a velocity.
    if (k.flying && m.has_tp && rt.commands != nullptr) {
        const fx want = m.tp.z + k.hover_z;
        const fx dz = want - m.p.z;
        const fx3 v = w.agent(m.entity).vel;
        // Through the QUEUE, like every other mutation a script makes: reads
        // are immediate and writes are deferred, so physics never observes an
        // entity moving underneath it mid-resolution.
        rt.commands->push(lua::Command{lua::CmdKind::SetVelocity, m.entity, fx3{v.x, v.y, dz}});
    }
}


// A state's due sequence entries, in order. Returns true if the mob LEFT the
// state, which stops the rest -- `goto`'s whole purpose.
//
// A DEADLINE, not a countdown: `at` is compared against how long the mob has
// been in the state, when the mob is already in hand. Nothing here is visited
// between a mob's own decisions.
bool run_due(MobRuntime& rt, sim::World& w, Mob& m, u32 tick, ActionSink* lua) {
    const Kind& k = rt.graph.kinds[static_cast<u32>(m.kind)];
    if (m.state < 0) return false;
    const State& st = rt.graph.states[k.first_state + m.state];
    const i64 age = static_cast<i64>(tick) - static_cast<i64>(m.entered);
    i32* vars = rt.vars.empty() ? nullptr : &rt.vars[m.first_var];

    while (m.seq_i < st.seq_count) {
        const SeqEntry& e = rt.graph.sequence[st.first_seq + m.seq_i];
        if (e.at > age) break;
        ++m.seq_i;
        switch (e.id) {
        case Act::SetVar:
            if (e.var >= 0 && vars) vars[e.var] = e.i;
            break;
        case Act::AddVar:
            if (e.var >= 0 && vars) vars[e.var] += e.i;
            break;
        case Act::SetCooldown:
            // An absolute tick, so a mob nobody is near is not paying to tick
            // one down.
            if (e.var >= 0 && vars) vars[e.var] = static_cast<i32>(tick) + e.i;
            break;
        case Act::SetAnim:
            set_anim(w, m, e.i);
            break;
        case Act::Face:
            if (e.at_target && m.has_tp) {
                m.aim = fx_atan2(m.tp.y - m.p.y, m.tp.x - m.p.x).raw;
                set_move(w, m, m.mx, m.my, m.aim, m.buttons);
            }
            break;
        case Act::Goto:
            enter(rt, w, m, e.state, tick);
            return true;
        case Act::Lua:
            // Out to maps/scripts/mobs.lua, by the index of this entry in the
            // state's own `sequence`. One implementation of each action, and
            // it is that one.
            ++rt.lua_actions;
            if (lua && lua->run(m.entity, rt.graph.str(st.name), e.lua_index)) {
                // It left the state -- a Lua action that called goto, or a
                // death that entered `death`. The rest of the sequence belongs
                // to a state the mob is no longer in.
                return true;
            }
            break;
        }
    }
    return false;
}

// One decision.
//
// `ticks` running out is a transition too, and the LAST one considered: an
// explicit transition that matches beats the state simply expiring, which is
// what lets `pain` be cut short by death.
void think(MobRuntime& rt, sim::World& w, Mob& m, u32 tick, ActionSink* lua) {
    if (run_due(rt, w, m, tick, lua)) return;
    if (m.state < 0) return;

    const Kind& k = rt.graph.kinds[static_cast<u32>(m.kind)];
    const State& st = rt.graph.states[k.first_state + m.state];
    EvalWorld ev{&w, tick, rt.rng};
    const i32* vars = rt.vars.empty() ? nullptr : &rt.vars[m.first_var];

    for (u16 i = 0; i < st.trans_count; ++i) {
        const Transition& t = rt.graph.transitions[st.first_trans + i];
        if (holds(rt.graph, k, m, t, ev, vars)) {
            // FIRST MATCH WINS, in declaration order. No scoring and no
            // backtracking: a graph whose behaviour depends on a weighting
            // nobody can see is not one an editor can show you.
            enter(rt, w, m, t.to, tick);
            return;
        }
    }

    if (st.ticks >= 0 && static_cast<i64>(tick) - static_cast<i64>(m.entered) >= st.ticks) {
        enter(rt, w, m, st.next, tick);
    }
}

// The whole tick: one bucket of mobs, each refreshed, aimed, decided and
// driven.
void tick_mobs(MobRuntime& rt, u32 tick, ActionSink* lua) {
    sim::World& w = *rt.world;
    rt.thought = 0;
    rt.lua_actions = 0;

    // NOBODY HERE: stop. A level keeps ticking after its last player leaves,
    // and every level runs its own mobs; on a server where clients have walked
    // through several maps most of the work each tick would be on behalf of
    // empty rooms. Nothing a mob does is even defined without a target.
    bool any_player = false;
    for (u32 i = 0; i < w.agent_high_water() && !any_player; ++i) {
        const sim::AgentState& a = w.agent(i);
        if (a.active && a.kind == 0) any_player = true;
    }
    if (!any_player) {
        if (!rt.asleep) {
            rt.asleep = true;
            for (Mob& m : rt.mobs) {
                if (m.kind < 0 || m.dead) continue;
                if (!w.agent(m.entity).active) continue;
                set_move(w, m, fx::zero(), fx::zero(), m.aim, 0);
            }
        }
        return;
    }
    rt.asleep = false;

    static nav::NavQuery query;   // tens of kilobytes; never a local
    std::vector<u32>& list = rt.bucket[tick % THINK];
    for (size_t bi = list.size(); bi-- > 0;) {
        Mob& m = rt.mobs[list[bi]];
        // The entity went away. Forgotten here rather than left in the bucket,
        // exactly as mobs.lua drops it from `live` -- a stale record is not
        // merely dead weight, it is one that will be found for somebody
        // else's entity once the slot is reused.
        if (m.kind < 0 || !w.agent(m.entity).active ||
            w.generation(m.entity) != m.generation) {
            rt.by_entity[m.entity] = -1;
            m.kind = -1;
            list.erase(list.begin() + static_cast<long>(bi));
            continue;
        }
        if (m.dormant || m.dead) continue;

        ++rt.thought;

        // POSITIONS ARE FETCHED ONCE PER THINK, not once per condition. Every
        // condition wanted the mob's position and its target's, and each asked
        // again; a zombieman's `chase` alone did it four times before it had
        // decided anything.
        m.p = w.agent(m.entity).pos;
        m.has_tp = false;
        if (m.target >= 0) {
            const sim::AgentState& t = w.agent(static_cast<u32>(m.target));
            const bool gone = !t.active || w.generation(static_cast<u32>(m.target)) != m.target_gen;
            // A mob that died keeps its entity -- that is what a corpse IS --
            // so `active` goes on saying yes long after it stopped being an
            // opponent. Anything that retaliated during infighting kept it as
            // a target and went on shooting the body.
            const i32 other = rt.by_entity[static_cast<u32>(m.target)];
            const bool corpse = other >= 0 && rt.mobs[static_cast<u32>(other)].dead;
            if (gone || corpse) m.target = -1;
            else { m.tp = t.pos; m.has_tp = true; }
        }

        // TARGET ACQUISITION, on the think tick only. A raycast per mob per
        // tick is the cost this is arranged around; a distance gate comes
        // first and `sight` is what makes it cheap.
        if (m.target < 0) {
            const Kind& k = rt.graph.kinds[static_cast<u32>(m.kind)];
            const Nearest n = nearest_player(w, m.p);
            if (n.agent >= 0 &&
                (!k.has_sight ||
                 n.d_sq < static_cast<u128>(static_cast<i128>(k.sight.raw) * k.sight.raw))) {
                m.target = n.agent;
                m.target_gen = w.generation(static_cast<u32>(n.agent));
                m.tp = n.pos;
                m.has_tp = true;
            }
        }

        think(rt, w, m, tick, lua);
        // Steered here, in the bucket, rather than in a second pass over every
        // mob -- see drive().
        if (m.kind >= 0 && !m.dead) drive(rt, w, rt.nav, query, m, tick);
    }
}


Mob* mob_of(MobRuntime& rt, u32 entity) {
    if (entity >= rt.by_entity.size()) return nullptr;
    const i32 i = rt.by_entity[entity];
    return i >= 0 ? &rt.mobs[static_cast<u32>(i)] : nullptr;
}

i32* mob_var(MobRuntime& rt, Mob& m, const char* name, size_t len) {
    if (m.kind < 0) return nullptr;
    const Kind& k = rt.graph.kinds[static_cast<u32>(m.kind)];
    const i32 slot = rt.graph.var_slot(k, name, len);
    if (slot < 0 || rt.vars.empty()) return nullptr;
    return &rt.vars[m.first_var + static_cast<u32>(slot)];
}

bool mob_enter(MobRuntime& rt, u32 entity, const char* state, size_t len, u32 tick) {
    Mob* m = mob_of(rt, entity);
    if (!m || m->kind < 0) return false;
    const Kind& k = rt.graph.kinds[static_cast<u32>(m->kind)];
    const i32 s = rt.graph.state_by_name(k, state, len);
    if (s < 0) return false;
    enter(rt, *rt.world, *m, s, tick);
    return true;
}

bool track(MobRuntime& rt, u32 entity, const char* kind, size_t kind_len,
           bool dormant, u32 tick) {
    if (rt.world == nullptr || entity >= sim::MAX_AGENTS) return false;
    const i32 ki = rt.graph.kind_by_name(kind, kind_len);
    if (ki < 0) return false;
    const Kind& k = rt.graph.kinds[static_cast<u32>(ki)];
    // A kind this loop cannot run stays entirely in Lua. Saying no here is the
    // whole of the fallback: nothing is half-tracked.
    if (!k.supported()) return false;

    if (rt.by_entity.empty()) rt.by_entity.assign(sim::MAX_AGENTS, -1);
    untrack(rt, entity);

    Mob m;
    m.entity = entity;
    m.generation = rt.world->generation(entity);
    m.kind = ki;
    m.hp = m.max_hp = k.hp;
    m.dormant = dormant;

    // Each mob gets its OWN copy of the declared vars. Sharing the
    // definition's table would have every zombieman on the level share one
    // cooldown -- which is what mobs.lua's per-mob copy exists to prevent.
    m.first_var = static_cast<u32>(rt.vars.size());
    rt.vars.resize(rt.vars.size() + k.var_count, 0);

    const u32 idx = static_cast<u32>(rt.mobs.size());
    rt.mobs.push_back(m);
    rt.by_entity[entity] = static_cast<i32>(idx);

    // Round-robin by SPAWN ORDER, so a level's mobs are spread evenly across
    // the think buckets whatever their entity indices turn out to be.
    rt.bucket[rt.spawned % THINK].push_back(idx);
    ++rt.spawned;

    enter(rt, *rt.world, rt.mobs[idx], k.initial, tick);
    return true;
}

void untrack(MobRuntime& rt, u32 entity) {
    if (entity >= rt.by_entity.size()) return;
    const i32 i = rt.by_entity[entity];
    if (i < 0) return;
    // The record is retired rather than removed: the buckets hold indices into
    // `mobs`, and compacting the vector would renumber every one of them.
    // tick_mobs drops it from its bucket when it next comes round.
    rt.mobs[static_cast<u32>(i)].kind = -1;
    rt.by_entity[entity] = -1;
}

}  // namespace dnp::server::mobs
