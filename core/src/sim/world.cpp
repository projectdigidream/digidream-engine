//
// The simulation world. See world.hpp for why it lives in core/.
//
#include <cstdio>
#include "dnp/sim/world.hpp"

namespace dnp::sim {

// The wire has to be able to carry everything a world can hold. These are two
// separate constants on purpose -- one is the interoperability contract, the
// other is this build's capacity -- and this is what stops them drifting apart
// silently, which would present as entities that exist and are never replicated.
static_assert(MAX_ENTITIES <= proto::MAX_ENTITIES,
              "the protocol cannot carry as many entities as the world holds");

using namespace dnp::physics;

void World::reset() noexcept {
    agent_high_water_ = 0;
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        agents_[i] = AgentState{};
        params_[i] = MoveParams{};
        inputs_[i] = proto::InputFrame{};
        // Generations start at 1, not 0: handle raw 0 means "no entity", and a
        // slot-0 agent at generation 0 would encode to exactly that.
        generation_[i] = 1;
    }
    for (u32 i = 0; i < MAX_PROPS; ++i) {
        props_[i] = PropState{};
        generation_[MAX_AGENTS + i] = 1;
    }
    prop_events_.clear();
    attacks_.clear();
    // Transients last exactly one tick. A tracer held over would be drawn
    // again on a snapshot that had nothing to do with it.
    vfx_count_ = 0;
    tick_ = 0;
}

u32 World::spawn(fx3 pos) noexcept {
    // Lowest free index wins. Deliberately not "next free" or a free-list pop:
    // allocation order has to be a pure function of world state, or two peers
    // that spawned the same agents in the same order could still disagree about
    // which index each landed on.
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        if (!agents_[i].active) {
            agents_[i] = AgentState{};
            agents_[i].pos = pos;
            agents_[i].active = true;
            params_[i] = MoveParams{};
            inputs_[i] = proto::InputFrame{};

            if (i + 1 > agent_high_water_) agent_high_water_ = i + 1;

            agents_[i].mv.sector =
                map_ ? geom::find_sector(*map_, fx2{pos.x, pos.y}) : geom::NO_SECTOR;
            agents_[i].mv.hover_fuel = params_[i].hover_fuel_max;
            agents_[i].mv.flight_fuel = params_[i].flight_fuel_max;
            return i;
        }
    }
    return MAX_AGENTS;
}

void World::despawn(u32 index) noexcept {
    if (index >= MAX_AGENTS) return;   // defined no-op, never an error
    if (agents_[index].active) {
        // Wraps deliberately, skipping 0. Eight bits is enough because the only
        // question a client ever asks is "is this the same entity I saw last
        // snapshot", and 255 intervening reuses of one slot cannot happen
        // within a snapshot round trip.
        generation_[index] = static_cast<u8>(generation_[index] + 1);
        if (generation_[index] == 0) generation_[index] = 1;
    }
    agents_[index] = AgentState{};
    inputs_[index] = proto::InputFrame{};
}

void World::set_input(u32 index, const proto::InputFrame& f) noexcept {
    if (index >= MAX_AGENTS) return;
    inputs_[index] = f;
}

namespace {

// Translate a wire input frame into a controller command.
//
// The vertical axis is derived from buttons rather than carried as its own
// wire field: JUMP means up and CROUCH means down, which is what those inputs
// already mean everywhere else, and it keeps InputFrame at 10 bytes. Climbing
// and flight are the only states that read it.
MoveCommand to_command(const proto::InputFrame& f) noexcept {
    MoveCommand c;
    c.move = fx2{proto::axis_to_fx(f.move_x), proto::axis_to_fx(f.move_y)};
    c.aim = f.aim;

    fx up = fx::zero();
    if (f.buttons & proto::button::JUMP) up += fx::one();
    if (f.buttons & proto::button::CROUCH) up -= fx::one();
    c.up = up;

    // The physics core does not depend on the wire format, so the bits are
    // translated rather than aliased.
    u16 b = 0;
    if (f.buttons & proto::button::JUMP)    b |= act::JUMP;
    if (f.buttons & proto::button::DASH)    b |= act::DASH;
    if (f.buttons & proto::button::CLIMB)   b |= act::CLIMB;
    if (f.buttons & proto::button::HOVER)   b |= act::HOVER;
    if (f.buttons & proto::button::FLIGHT)  b |= act::FLIGHT;
    if (f.buttons & proto::button::GRAPPLE) b |= act::GRAPPLE;
    if (f.buttons & proto::button::CROUCH)  b |= act::PULL;   // grapple modifier
    if (f.buttons & proto::button::INTERACT) b |= act::INTERACT;
    if (f.buttons & proto::button::ATTACK_PRIMARY) b |= act::ATTACK;
    c.buttons = b;
    return c;
}

}  // namespace

void World::step_agent(u32 i) noexcept {
    AgentState& a = agents_[i];
    const MoveCommand cmd = to_command(inputs_[i]);

    // Turned toward the aim at a bounded rate, not snapped to it.
    //
    // brad arithmetic does this exactly: the difference between two angles as a
    // SIGNED 32-bit value is the shortest way round by construction, because
    // the type wraps. No branch on "which way is shorter", no comparison
    // against pi, and no case at the 359-to-1 boundary -- the arithmetic that
    // makes a turn wrap is the same arithmetic that makes it short.
    {
        const fx step_turns = params_[i].turn_speed * DT;
        // A turn fraction and a brad are the same number: brad is a turn in
        // 2^32 and fx is Q32.32, so the raw value carries straight across. A
        // rate fast enough to cover a whole turn in one tick is a snap, which
        // is what a script asks for by setting it high.
        const u64 step = static_cast<u64>(step_turns.raw < 0 ? 0 : step_turns.raw);
        if (step >= 0x100000000ull) {
            a.facing = cmd.aim;
        } else {
            const i32 delta = static_cast<i32>(cmd.aim.raw - a.facing.raw);
            const i32 limit = static_cast<i32>(static_cast<u32>(step));
            const i32 move = delta > limit ? limit : (delta < -limit ? -limit : delta);
            a.facing = brad(a.facing.raw + static_cast<u32>(move));
        }
    }

    // Interaction, on the PRESS edge and before the move.
    //
    // Before, so that what an entity reaches for is what was in front of it
    // when the button went down, not wherever the tick's movement carried it.
    // Edge-triggered, or a held button works a door sixty times a second.
    {
        const bool want = (cmd.buttons & act::INTERACT) != 0;
        if (want && !a.mv.interact_held && map_) {
            // A prop first. If you are standing in front of a lever, that is
            // what you meant -- the wall behind it is always also there, and
            // reporting the wall would make a lever unusable wherever it hangs
            // on one.
            const i32 usable = find_usable_prop(i);
            if (usable >= 0) {
                prop_events_.push(PropEvent{i, static_cast<u32>(usable), PropEventKind::Use});
            } else {
                const i32 side = physics::find_interact_side(
                    *map_, a.mv.sector, fx2{a.pos.x, a.pos.y}, a.facing,
                    INTERACT_REACH, a.pos.z, params_[i].height);
                if (side >= 0) {
                    interacts_.push(InteractEvent{i, static_cast<u32>(side), a.mv.sector});
                }
            }
        }
        a.mv.interact_held = want;
    }

    // A swing, on the press edge and before the move, for the same reason use
    // is: what you hit is what was in front of you when you pressed, not
    // whatever this tick's movement carried you into.
    {
        const bool swing = (cmd.buttons & act::ATTACK) != 0;
        // A press STARTS a swing; it does not resolve one. What the collider
        // touches is decided over the ticks that follow, and a press while one
        // is already running is ignored -- the weapon is busy.
        // The cooldown runs down whether or not the button is held, so holding
        // it does not extend the wait and releasing it does not shorten one.
        if (a.mv.attack_cooldown_left > 0) --a.mv.attack_cooldown_left;

        if (swing && !a.mv.attack_held && a.mv.attack_left == 0 &&
            a.mv.attack_cooldown_left == 0) {
            a.mv.attack_left = params_[i].attack_ticks == 0 ? 1 : params_[i].attack_ticks;
            for (u64& w : a.mv.attack_hit_agents) w = 0;
            a.mv.attack_hit_props[0] = 0;
            a.mv.attack_hit_props[1] = 0;
            a.mv.attack_hit_props[2] = 0;
        }
        a.mv.attack_held = swing;

        // Swept BEFORE the move, so the collider is placed where the swing
        // started rather than where this tick's walking carried it.
        if (a.mv.attack_left > 0) {
            resolve_attack(i);
            --a.mv.attack_left;
            // Started when the swing ENDS, not when it begins, so the two are
            // additive: a weapon's rate is how long it swings plus how long it
            // takes to recover, which is how a player experiences it.
            if (a.mv.attack_left == 0) a.mv.attack_cooldown_left = params_[i].attack_cooldown;
        }
    }

    const bool was_anchored = a.mv.anchored;
    const i32  prev_anchor = a.mv.anchor;
    const fx2  from2 = fx2{a.pos.x, a.pos.y};
    const i32  from_sector = a.mv.sector;

    if (map_) {
        step_controller(*map_, params_[i], a.mv, cmd, a.pos, a.vel, DT);

        // Anchoring follows footprint contact, not proximity (design §3.2.5):
        // standing on a sector anchors to it, and the anchor survives until the
        // entity stands on a different one.
        if (a.mv.grounded && a.mv.sector >= 0) {
            a.mv.anchor = a.mv.sector;
            a.mv.anchored = true;
        } else if (was_anchored) {
            // Detached this tick -- walked off, jumped, or the sector pulled out
            // from under it. Take an instantaneous snapshot of the sector's
            // velocity and add it, then the entity is a fully independent free
            // body. That is what makes jumping off a rising lift preserve the
            // fling, and it is the same handoff pattern used for climb release,
            // grapple release and controller detach.
            const i32 mi = prev_anchor >= 0 ? motions_.find(static_cast<u32>(prev_anchor)) : -1;
            if (mi >= 0) a.vel += motions_.at(static_cast<u32>(mi)).velocity();
            a.mv.anchored = false;
        }

        // After the map move, and before the crossing report, so a trip-wire
        // is judged against where the body actually ended up.
        resolve_prop_collisions(i);

        report_crossings(i, from2, fx2{a.pos.x, a.pos.y}, from_sector);
    } else {
        // No map: integrate under gravity only. Not a hidden flat floor -- a
        // silently substituted floor turns a map-loading failure into a physics
        // mystery, and falling forever is at least obviously wrong.
        a.vel.z += GRAVITY * params_[i].gravity_scale * DT;
        a.pos += a.vel * DT;
        a.mv.grounded = false;
    }
}

// Trip-wires crossed by this tick's movement.
//
// Tested against the sides of the sector the entity STARTED in and the one it
// ended in, not against the whole map. A two-sided edge belongs to both sectors
// that meet at it, so any crossing that actually changed sectors is a side of
// one of those two -- and an entity that crossed three sectors in one tick was
// moving fast enough that Doom's own blockmap walk would have been the only
// thing to catch it either.
//
// Only sides the map marked TRIGGER are tested at all, so the ordinary case is
// a flag read per side and nothing else.
void World::report_crossings(u32 i, fx2 from, fx2 to, i32 from_sector) noexcept {
    if (!map_) return;
    if (from.x == to.x && from.y == to.y) return;

    const i32 to_sector = agents_[i].mv.sector;
    const i32 sectors[2] = {from_sector, to_sector};
    for (u32 k = 0; k < 2; ++k) {
        const i32 sec = sectors[k];
        if (sec < 0 || static_cast<u32>(sec) >= map_->sector_count) continue;
        if (k == 1 && to_sector == from_sector) continue;

        const geom::Sector& s = map_->sector(static_cast<u32>(sec));
        for (u16 e = 0; e < s.side_count; ++e) {
            const u32 si = s.first_side + e;
            const geom::Side& sd = map_->side(si);
            if ((sd.flags & geom::side_flag::TRIGGER) == 0) continue;
            // The shared boundary is a side of BOTH sectors, so the second pass
            // would find the same physical edge again and report the crossing
            // twice -- once as the sector left, once as the one entered. A
            // one-shot Lua trigger would then consume itself before the script
            // ever ran, and a repeatable one would fire double. The edge that
            // leads back where we came from is exactly that duplicate.
            if (k == 1 && sd.neighbor == from_sector) continue;
            fx t{};
            if (!geom::segment_intersect(from, to, map_->point(sd.a), map_->point(sd.b), t)) {
                continue;
            }
            interacts_.push(InteractEvent{i, si, sec, TouchKind::Cross});
        }
    }
}

void World::carry_anchored() noexcept {
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        AgentState& a = agents_[i];
        if (!a.active || !a.mv.anchored || a.mv.anchor < 0) continue;
        const i32 mi = motions_.find(static_cast<u32>(a.mv.anchor));
        if (mi < 0) continue;
        // The floor's displacement, not the sector's raw offset: a rider is
        // carried by the surface under its feet, and a descending ceiling moves
        // no surface anybody is standing on.
        a.pos += motions_.at(static_cast<u32>(mi)).floor_delta();
    }
}

void World::sweep_agent(u32 i, const SectorMotion& m) noexcept {
    AgentState& a = agents_[i];
    const MoveParams& p = params_[i];

    fx2 pos2{a.pos.x, a.pos.y};
    const physics::SweepContact hit =
        physics::sweep_out_of_sector(*map_, m.sector, pos2, p.radius, a.pos.z, p.height);
    if (!hit.contacted) return;

    // The wall wins the argument about where it is; the only question left is
    // whether the entity has anywhere to go. Deliberately positional and not an
    // impulse: a shove is a constraint being resolved, and adding the wall's
    // velocity would launch an entity that a slow platform merely nudged, then
    // leave it sliding after the platform stopped.
    const i32 host = geom::find_sector(*map_, pos2);
    if (host == geom::NO_SECTOR) {
        // Shoved clean out of the world. Nothing to resolve against and nowhere
        // to put the entity, so it stays where it was and is reported pinned.
        crushes_.push(CrushEvent{i, m.sector, CrushKind::Horizontal, length(hit.escape)});
        return;
    }

    // Let the static world push back. If it yields, the entity is simply
    // displaced -- the ordinary case, and not a crush. If the moving sector is
    // STILL overlapping afterwards, the two surfaces have closed on the entity
    // from both directions and there is no position that satisfies either.
    physics::push_out_of_sector(*map_, static_cast<u32>(host), pos2, nullptr,
                                p.radius, a.pos.z, p.height);
    const physics::SweepContact again =
        physics::sweep_out_of_sector(*map_, m.sector, pos2, p.radius, a.pos.z, p.height);

    a.pos.x = pos2.x;
    a.pos.y = pos2.y;
    a.mv.sector = host;

    if (again.contacted) {
        crushes_.push(CrushEvent{i, m.sector, CrushKind::Horizontal, length(again.escape)});
    }
}

void World::sweep_moving_sectors() noexcept {
    if (!map_) return;
    for (u32 k = 0; k < motions_.count(); ++k) {
        const SectorMotion& m = motions_.at(k);
        const fx3 d = m.floor_delta();
        // Only a footprint that MOVED sweeps anything. A lift travelling
        // straight up displaces no horizontal space, and running the test for it
        // would push every agent standing beside the shaft away from a wall that
        // never came at them.
        if (d.x.raw == 0 && d.y.raw == 0) continue;

        for (u32 i = 0; i < MAX_AGENTS; ++i) {
            if (!agents_[i].active) continue;
            // A rider was already carried with this sector and is where the
            // sector expects it to be; sweeping it too would push it off its own
            // platform every tick.
            if (agents_[i].mv.anchored && agents_[i].mv.anchor == static_cast<i32>(m.sector)) continue;
            sweep_agent(i, m);
        }
    }
}

void World::detect_vertical_crush() noexcept {
    if (!map_) return;
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        const AgentState& a = agents_[i];
        if (!a.active) continue;
        const i32 s = a.mv.sector;
        if (s < 0 || static_cast<u32>(s) >= map_->sector_count) continue;

        const geom::Sector& sec = map_->sector(static_cast<u32>(s));
        const fx gap = sec.ceil_z - sec.floor_z;
        const fx height = params_[i].height;
        if (gap >= height) continue;

        // No check for whether the sector is a mover, and none is needed: an
        // agent can only BE somewhere its body does not fit if the geometry
        // moved around it, because sector_admits() refuses to let it walk in.
        // The one other way is a map that spawns a player inside a crawlspace,
        // and reporting that is right too.
        crushes_.push(CrushEvent{i, static_cast<u32>(s), CrushKind::Vertical, height - gap});
    }
}

void World::teleport(u32 index, fx3 pos) noexcept {
    if (index >= MAX_AGENTS || !agents_[index].active) return;   // defined no-op
    AgentState& a = agents_[index];
    a.pos = pos;

    // Re-derive the sector. Without this the next tick tests the new position
    // against the old sector's walls, which is how a teleported entity ends up
    // inside geometry or falling through a floor that is not there.
    a.mv.sector = map_ ? geom::find_sector(*map_, fx2{pos.x, pos.y}) : geom::NO_SECTOR;

    // And drop the anchor. An entity moved off a platform is no longer riding
    // it, and leaving the anchor set would have the next tick carry it by that
    // platform's motion from wherever it now is (design §3.2.5).
    a.mv.anchored = false;
    a.mv.anchor = -1;

    // Grounded is re-derived by the next step from the new surroundings rather
    // than asserted here: whether the destination has a floor under it is the
    // mover's question, not the caller's.
    a.mv.grounded = false;
}

void World::add_impulse(u32 index, fx3 dv) noexcept {
    if (index >= MAX_AGENTS || !agents_[index].active) return;
    agents_[index].vel += dv;
    // A pushed entity is no longer resting on anything.
    if (dv.z.raw != 0) agents_[index].mv.grounded = false;
}

void World::set_velocity(u32 index, fx3 v) noexcept {
    if (index >= MAX_AGENTS || !agents_[index].active) return;
    agents_[index].vel = v;
    if (v.z.raw != 0) agents_[index].mv.grounded = false;
}

void World::step() noexcept {
    // Events describe the tick about to run, so the previous tick's are dropped
    // here rather than at the end -- a caller reading them after step() sees
    // exactly what this tick raised.
    crushes_.clear();
    interacts_.clear();
    prop_events_.clear();
    attacks_.clear();

    // Sectors move BEFORE agents do. The order matters: an agent riding a lift
    // must be carried by this tick's motion and then run its own physics from
    // the new position, not the other way round -- resolving the agent first
    // would leave it a tick behind the platform, which reads as the platform
    // sliding out from under it every frame.
    if (store_) motions_.apply(*store_, tick_);

    carry_anchored();
    sweep_moving_sectors();

    // Where every body was before anything moved. Both resolutions read it --
    // see the declaration.
    for (u32 i = 0; i < agent_high_water_; ++i) tick_start_pos_[i] = agents_[i].pos;

    // Ascending index order, always. Any order would be deterministic on its
    // own, but it must be the SAME order on client and server, and index order
    // is the one that cannot drift as the agent set changes.
    for (u32 i = 0; i < agent_high_water_; ++i) {
        if (agents_[i].active) step_agent(i);
    }

    resolve_agent_collisions();

    // Last, on settled positions. Running it before the agents moved would
    // report a crush on an agent that was about to walk out from under the
    // ceiling in the same tick, and miss one that walked in.

    detect_vertical_crush();
    detect_prop_events();

    ++tick_;
}

u16 World::state_flags(u32 i) const noexcept {
    const MoveState& m = agents_[i].mv;
    u16 f = 0;
    if (m.grounded)  f |= proto::state_flag::GROUNDED;
    if (m.climbing)  f |= proto::state_flag::CLIMBING;
    if (m.hovering)  f |= proto::state_flag::HOVERING;
    if (m.flying)    f |= proto::state_flag::FLYING;
    if (m.grappling) f |= proto::state_flag::GRAPPLING;
    // Declared in the protocol since Phase 1 and never populated until now.
    // Its absence cost 18 ticks of divergence after every teleport: the client
    // kept riding an anchor the server had dropped, and the two only reconverged
    // once the entity landed and both re-derived it.
    if (m.anchored) f |= proto::state_flag::ANCHORED;
    return f;
}

fx3 World::anchor_frame(const MoveState& m) const noexcept {
    // Anchor-relative prediction (design §3.2.5), expressed as a coordinate
    // frame rather than as a special case in the comparison.
    //
    // A client cannot compute where a scripted-discrete sector will be: the
    // position is whatever a Lua callback decided, and no parameter set
    // describes it. What the client CAN predict exactly is where the entity is
    // relative to the sector it is riding -- it is standing on it, and its own
    // movement is ordinary physics. So for an entity anchored to an
    // unpredictable sector, positions are hashed in that sector's frame.
    //
    // Both sides subtract their own copy of the offset. The client's is stale
    // by up to a snapshot interval and the server's is current, and that is
    // exactly the difference the subtraction removes: the client's rider was
    // carried by the stale offset from a position adopted with the stale
    // offset, so the two relative positions agree bit for bit.
    //
    // Parametric anchors are deliberately NOT included. Those are predictable,
    // absolute agreement is achievable, and asking for less would stop the gate
    // noticing if it ever stopped holding.
    if (!m.anchored || m.anchor < 0) return fx3::zero();
    const i32 mi = motions_.find(static_cast<u32>(m.anchor));
    if (mi < 0) return fx3::zero();
    const SectorMotion& sm = motions_.at(static_cast<u32>(mi));
    if (sm.type != MotionType::ScriptedDiscrete) return fx3::zero();
    return sm.offset;
}

u64 World::agent_hash(u32 i) const noexcept {
    StateHasher h;
    const AgentState& a = agents_[i];
    const MoveState& m = a.mv;

    h.vec3(a.pos - anchor_frame(m));
    h.vec3(a.vel);
    h.bradv(a.facing);

    // Every field the controller carries between ticks. A replay that started
    // from the wrong dash timer or the wrong jump-edge flag would diverge from
    // the server for reasons no amount of position correction repairs, so the
    // hash has to be able to see all of it.
    h.byte(static_cast<unsigned char>(m.mode));
    h.boolv(m.grounded);
    h.i32v(m.sector);
    h.i32v(m.anchor);
    h.boolv(m.anchored);
    h.u32v(a.kind);
    h.u32v(a.animation_state);
    h.byte(m.jumps_used);
    h.byte(m.dashes_used);
    h.boolv(m.jump_held);
    h.boolv(m.dash_held);
    h.boolv(m.grapple_held);
    h.boolv(m.interact_held);
    h.boolv(m.attack_held);
    h.u32v(m.attack_left);
    // The cooldown IS predictable -- it counts down from a replicated parameter
    // on a schedule the client can run itself -- so unlike the hit masks below
    // it belongs here. A client that thought its weapon was ready when the
    // server did not would predict a swing that never happened.
    h.u32v(m.attack_cooldown_left);
    // The hit masks are deliberately ABSENT.
    //
    // agent_hash is the prediction comparison, so what belongs in it is what
    // the client can reproduce. Which targets a swing has already connected
    // with is engine bookkeeping for deduplicating events the client never
    // receives -- it cannot know it, so including it would make every swing a
    // guaranteed mismatch. They stay in World::hash, where the determinism gate
    // still covers them.
    h.fxv(m.dash_timer);
    h.fxv(m.dash_cooldown_timer);
    h.vec2(m.dash_dir);
    h.boolv(m.climbing);
    h.vec2(m.climb_normal);
    h.boolv(m.grappling);
    h.boolv(m.grapple_pulling);
    // NOT rebased, deliberately. A grapple anchor is a point on the geometry
    // the rope is attached to, which is usually not the platform the entity is
    // standing on -- rebasing it by the platform's offset would move an anchor
    // bolted to a static wall. It is also zero when nothing is grappling, and
    // subtracting a different offset from zero on each side turns "no grapple"
    // into a divergence, which is exactly what it did when this was written the
    // other way.
    h.vec3(m.grapple_anchor);
    h.fxv(m.grapple_dist);
    h.fxv(m.hover_fuel);
    h.fxv(m.flight_fuel);
    h.boolv(m.hovering);
    h.boolv(m.flying);
    return h.value();
}

void AttackLog::hash_into(StateHasher& h) const noexcept {
    // Hashed like every other engine-detected event, and for the same reason:
    // this adds no information about the world, it adds coverage of the
    // DETECTION. A swing that connected on one target under wasm and another
    // natively would otherwise be found by a player whose barrel did not
    // explode.
    h.u32v(count_);
    h.u32v(dropped_);
    for (u32 i = 0; i < count_; ++i) {
        h.u32v(events_[i].attacker);
        h.u32v(events_[i].target);
    }
}

void PropEventLog::hash_into(StateHasher& h) const noexcept {
    // Hashed like the crush and interact logs, and for the same reason: the
    // events are a pure function of state the gate already covers, so this adds
    // no information about the world -- what it adds is coverage of the
    // DETECTION. A pickup that fired on one target and not another would
    // otherwise be found by a player walking over a key that does nothing.
    h.u32v(count_);
    h.u32v(dropped_);
    for (u32 i = 0; i < count_; ++i) {
        h.u32v(events_[i].agent);
        h.u32v(events_[i].prop);
        h.byte(static_cast<unsigned char>(events_[i].kind));
    }
}

u32 World::spawn_prop(PropKind kind, fx3 pos) noexcept {
    // SPAWNABLE_PROPS, not MAX_PROPS: the top of the range is reserved for the
    // body mirrors a client places, and a prop allocated there would land on
    // one of them.
    for (u32 i = 0; i < SPAWNABLE_PROPS; ++i) {
        if (props_[i].active) continue;
        props_[i] = PropState{};
        props_[i].kind = kind;
        props_[i].pos = pos;
        props_[i].active = true;
        ++prop_epoch_;
        return MAX_AGENTS + i;
    }
    return MAX_ENTITIES;
}

u32 World::spawn_scripted(PropKind kind, fx3 pos) noexcept {
    const u32 i = spawn(pos);
    if (i >= MAX_AGENTS) return MAX_AGENTS;
    agents_[i].kind = kind;
    agents_[i].scripted = true;
    return i;
}

bool World::place_prop(u32 entity, PropKind kind, fx3 pos,
                       fx radius, fx height, u8 flags) noexcept {
    if (!is_prop(entity)) return false;
    PropState& p = props_[entity - MAX_AGENTS];
    p.kind = kind;
    p.pos = pos;
    p.radius = radius;
    p.height = height;
    p.flags = flags;
    p.active = true;
    ++prop_epoch_;
    return true;
}

void World::despawn_prop(u32 entity) noexcept {
    if (!is_prop(entity)) return;
    if (!props_[entity - MAX_AGENTS].active) return;
    props_[entity - MAX_AGENTS] = PropState{};
    ++prop_epoch_;
    // Bump the generation so every handle to what used to be here goes stale.
    // Without it a script holding the key it just consumed would keep finding a
    // live entity in the slot the next pickup lands in.
    generation_[entity] = static_cast<u8>(generation_[entity] + 1);
}

// Push an agent out of any solid prop it ended the tick inside.
//
// A POST-PASS on the settled position rather than part of the sweep, and the
// trade is worth naming: the mover guarantees a body never crosses a blocking
// SIDE, because a side is map geometry it can sweep against. A prop is not
// geometry -- it appears, moves and vanishes -- so it is resolved by pushing
// out afterwards, which is correct for anything a body cannot cross in a single
// tick and is not a tunnelling guarantee. At 60 Hz a barrel is about forty
// ticks wide at running speed.
//
// Iterated in index order so two overlapping props resolve the same way on
// every target, which is the whole reason this is in core and not in a script.
// Put a body back inside the world after something displaced it.
//
// Every push in this file -- out of a solid prop, out of another body -- moves a
// cylinder along the line between two centres and asks nothing about the map.
// That is fine in open floor and wrong against a wall: a monster walking into a
// player standing in a doorway pushed the player straight through it and out of
// the level. Reported from a browser as "enemies can push the player out of
// bounds if the player tries to move in the direction of enemies", which is
// exactly what it is -- the push is a teleport, and a teleport that skips
// collision skips the walls too.
//
// THREE steps, because there are three ways to end up somewhere impossible, and
// the first one is the one that actually bites.
//
// The push is a jump, not a walk, so it can cross a wall entirely: start on one
// side, land on the other, and never be inside it at any moment a resting
// contact test could notice. push_out_of_sector cannot help -- by the time it
// looks, the body is legitimately standing in a sector it is allowed to be in,
// just not the one it should have reached. A single flat room hides this
// completely, which is why the first fixture written for it passed with the
// whole fix commented out. So the push is SWEPT first, against the same ray the
// mover's own CCD uses (§7.1.1: aiming and collision must not disagree about
// what is in the way).
//
// Then ordinary resting contact, which resolves the shallow case by sliding
// along the wall. Then, last, the case neither can resolve -- pushed clean out
// of the map, where there is no sector to be pushed out of -- and there the
// honest answer is to refuse the push. Two bodies briefly overlapping is a
// cosmetic problem; a body outside the world is not recoverable.
void World::keep_in_map(u32 i, fx2 before) noexcept {
    if (!map_) return;
    AgentState& a = agents_[i];
    fx2 p{a.pos.x, a.pos.y};

    // 1. Did the push cross something solid on its way?
    const fx2 step{p.x - before.x, p.y - before.y};
    const fx dist = length(step);
    if (dist > fx::zero()) {
        const fx2 dir{step.x / dist, step.y / dist};
        const physics::RayHit hit =
            physics::raycast(*map_, a.mv.sector, before, dir, dist + params_[i].radius, 0);
        if (hit.blocked) {
            // Stop a radius short of the surface, which is where a body resting
            // against it would be.
            const fx back = hit.distance - params_[i].radius;
            const fx go = back > fx::zero() ? back : fx::zero();
            p.x = before.x + dir.x * go;
            p.y = before.y + dir.y * go;
            a.pos.x = p.x;
            a.pos.y = p.y;
        }
    }

    const physics::SurfaceQuery q =
        physics::query_surface(*map_, p, a.mv.sector, a.pos.z, params_[i].height);
    if (!q.in_map) { a.pos.x = before.x; a.pos.y = before.y; return; }

    a.mv.sector = q.sector;
    physics::push_out_of_sector(*map_, static_cast<u32>(q.sector), p, &a.vel,
                                params_[i].radius, a.pos.z, params_[i].height);

    const physics::SurfaceQuery after =
        physics::query_surface(*map_, p, q.sector, a.pos.z, params_[i].height);
    if (!after.in_map) { a.pos.x = before.x; a.pos.y = before.y; return; }
    a.pos.x = p.x;
    a.pos.y = p.y;
    a.mv.sector = after.sector;
}

// Push one body out of one cylinder, and report nothing -- the caller decides
// what the cylinder is.
//
// ONE rule, used for solid props and for other bodies alike, because a client
// and a server that resolved the same contact by two different routes disagreed
// about where it ended. A client mirrors the other players and monsters into its
// own world as solid props, so its own resolution ran through the prop path
// while the server's ran through the agent path -- the prop path teleported the
// body clear, the agent path capped the correction at how far it had travelled,
// and the two produced different positions from identical inputs. Every tick of
// contact was a misprediction, and the cause was two implementations of one
// idea rather than anything about the network.
//
// `budget` is how far this body may be moved: its own displacement this tick,
// plus a small allowance. That is what makes a body block rather than shove --
// you can be put back where you came from, never pushed past it -- and for a
// static obstacle it is exactly equivalent to stopping at the surface, because
// a body that just walked into a crate travelled at least as far as it needs to
// come back out.
static void push_out_of_cylinder(fx3& pos, fx3& vel, fx radius, fx height,
                                 fx2 other_xy, fx other_r, fx other_z0, fx other_z1,
                                 fx budget, bool tie_positive) noexcept {
    // Vertically clear: over the top or under the bottom. First, because it
    // rejects most pairs in two compares -- and because a crate you can stand
    // on stops being a wall the moment you are above it.
    if (pos.z >= other_z1) return;
    if (pos.z + height <= other_z0) return;

    const fx dx = pos.x - other_xy.x;
    const fx dy = pos.y - other_xy.y;
    const fx want = radius + other_r;
    const u128 d_sq = len_sq_raw(fx2{dx, dy});
    const u128 want_sq = static_cast<u128>(static_cast<i128>(want.raw) * want.raw);
    if (d_sq >= want_sq) return;

    // Exactly concentric: no separation direction exists, so one is picked
    // rather than normalizing a zero vector. `tie_positive` lets a caller give
    // the two halves of a pair OPPOSITE fallbacks -- a single fixed direction
    // sends both bodies the same way by the same distance and leaves them as
    // concentric as they started.
    const fx2 n = (d_sq == 0)
                      ? fx2{tie_positive ? fx::one() : -fx::one(), fx::zero()}
                      : normalize(fx2{dx, dy});

    const fx2 target{other_xy.x + n.x * want, other_xy.y + n.y * want};
    const fx2 correction{target.x - pos.x, target.y - pos.y};
    const fx need = length(correction);
    if (need > budget) {
        pos.x += correction.x * budget / need;
        pos.y += correction.y * budget / need;
    } else {
        pos.x = target.x;
        pos.y = target.y;
    }

    // And drop the velocity pointing into it, or the next tick walks straight
    // back in and the body buzzes against the surface.
    const fx into = dot(n, fx2{vel.x, vel.y});
    if (into < fx::zero()) {
        vel.x -= n.x * into;
        vel.y -= n.y * into;
    }
}

void World::resolve_prop_collisions(u32 agent) noexcept {
    AgentState& a = agents_[agent];
    const fx2 before{a.pos.x, a.pos.y};
    const fx2 travelled{a.pos.x - tick_start_pos_[agent].x,
                        a.pos.y - tick_start_pos_[agent].y};
    const fx budget = length(travelled) + DEPENETRATE_PER_TICK;

    for (u32 pi = 0; pi < MAX_PROPS; ++pi) {
        const PropState& p = props_[pi];
        if (!p.active) continue;
        if ((p.flags & prop_flag::SOLID) == 0) continue;
        push_out_of_cylinder(a.pos, a.vel, params_[agent].radius, params_[agent].height,
                             fx2{p.pos.x, p.pos.y}, p.radius, p.pos.z, p.pos.z + p.height,
                             budget, true);
    }
    keep_in_map(agent, before);
}

// Bodies do not stand inside one another.
//
// Modelled on resolve_prop_collisions above, and deliberately: a body and a
// solid crate are the same problem, and a second answer to it would be a second
// thing to keep in step with the client.
//
// TWO DECISIONS, both of which the obvious implementation gets wrong.
//
// It BLOCKS, it does not shove. Each agent is pushed out of the others; nobody
// is pushed by them. A monster that walks into a standing player moves itself
// out and leaves the player exactly where they were. The first version of this
// displaced the lower-priority body instead, and monsters -- which chase --
// walked players off the doors they were opening. A body you cannot walk
// through is what "solid" means; a body that pushes you around is a mechanic,
// and not one anybody asked for.
//
// And each agent is resolved against where the others were at the START of the
// tick, not where they ended it. That is what makes the answer independent of
// which agent the loop reached first: "first" is an iteration detail, and a
// simulation whose outcome depends on it has a hidden input. It is also the
// only version a client can reproduce -- the snapshot it holds is exactly a
// set of start-of-tick positions.
//
// The cost is one tick of latency: two bodies that walk into each other head-on
// each stop at where the other WAS, which can leave them fractionally
// overlapped for a tick before the next pass separates them. That is a frame of
// softness, which is what §7.1 asked for anyway.
void World::resolve_agent_collisions() noexcept {
    const fx3* was = tick_start_pos_;
    for (u32 j = 0; j < agent_high_water_; ++j) {
        if (!agents_[j].active || !agents_[j].solid) continue;
        AgentState& b = agents_[j];
        const fx2 before{b.pos.x, b.pos.y};
        const fx2 travelled{b.pos.x - was[j].x, b.pos.y - was[j].y};
        const fx budget = length(travelled) + DEPENETRATE_PER_TICK;

        for (u32 i = 0; i < agent_high_water_; ++i) {
            if (i == j || !agents_[i].active || !agents_[i].solid) continue;
            const fx3 a = was[i];
            push_out_of_cylinder(b.pos, b.vel, params_[j].radius, params_[j].height,
                                 fx2{a.x, a.y}, params_[i].radius, a.z,
                                 a.z + params_[i].height, budget, j > i);
        }
        keep_in_map(j, before);
    }
}

// The prop an agent is reaching for, or -1.
//
// Nearest USABLE prop in front and within reach -- a proximity test, not the ray
// the map's sides use. Deliberately different: a ray is right for a wall, which
// is large and continuous, and wrong for a lever, which is a small target the
// player is aiming at with a camera that is not on their shoulder.
i32 World::find_usable_prop(u32 agent) const noexcept {
    const AgentState& a = agents_[agent];
    const fx2 dir = fx_dir(a.facing);
    const fx2 from{a.pos.x, a.pos.y};

    i32  best = -1;
    u128 best_d = 0;
    for (u32 pi = 0; pi < MAX_PROPS; ++pi) {
        const PropState& p = props_[pi];
        if (!p.active) continue;
        if ((p.flags & prop_flag::USABLE) == 0) continue;

        // Roughly level with it: you cannot work a lever on the floor above.
        if (a.pos.z >= p.pos.z + p.height + INTERACT_REACH) continue;
        if (a.pos.z + params_[agent].height + INTERACT_REACH <= p.pos.z) continue;

        const fx2 toward = fx2{p.pos.x, p.pos.y} - from;
        if (dot(dir, toward) <= fx::zero()) continue;   // behind

        // Reach measured to the SURFACE, not the centre: a big object is
        // reachable from further away, which is what a player expects of one.
        const fx limit = INTERACT_REACH + p.radius;
        const u128 d_sq = len_sq_raw(toward);
        if (d_sq > static_cast<u128>(static_cast<i128>(limit.raw) * limit.raw)) continue;

        // Strictly nearer, so a tie resolves to the lower index rather than to
        // whichever the loop reached last.
        if (best >= 0 && d_sq >= best_d) continue;
        best_d = d_sq;
        best = static_cast<i32>(MAX_AGENTS + pi);
    }
    return best;
}

// Everything in the arc of a swing.
//
// The engine's whole contribution to combat: who was in front of whom, close
// enough, and within the arc. Damage, knockback, whether a weapon was even
// equipped -- all script, for the same reason a door's behaviour is: a client
// predicts the swing because the button is in its own input frame, and it must
// not have to predict the consequence.
//
// Targets are ENTITY indices, so agents and props come back through one list.
// A script that damages both writes it once, and a prop that becomes an enemy
// later changes nothing here.
//
// Order is agents first then props, each by ascending index, so two targets in
// the same swing resolve the same way on every target and on a replay.
void World::resolve_attack(u32 agent) noexcept {
    AgentState& a = agents_[agent];
    const MoveParams& mp = params_[agent];
    if (a.mv.attack_left == 0) return;

    // Where the collider is THIS tick.
    //
    // A swing sweeps: it starts at one angle relative to the aim and ends at
    // another, and a target that steps out of the way part-way through is
    // missed. That is the whole difference between this and the one-tick arc
    // test it replaces, and it is why the parameters describe a path rather
    // than a shape.
    //
    // Phase runs 0..1 across the swing. attack_left counts DOWN, so the first
    // live tick is the last value and phase has to be derived from the total.
    const u8 total = mp.attack_ticks == 0 ? 1 : mp.attack_ticks;
    const u8 elapsed = static_cast<u8>(total - a.mv.attack_left);
    const fx phase = fx::from_frac(elapsed, total);
    const fx offset = fx_lerp(mp.attack_sweep_from, mp.attack_sweep_to, phase);

    // Turn fractions to brad. A turn is 2^32, so a fraction of a turn is the
    // fraction scaled by 2^32 -- and fx is already Q32.32, so the raw value IS
    // the answer once the integer part is discarded by the wrap.
    const brad centre = brad(a.facing.raw + static_cast<u32>(offset.raw));
    const u32  half   = static_cast<u32>(mp.attack_half_arc.raw);

    const fx2 from{a.pos.x, a.pos.y};
    const fx  a_top = a.pos.z + mp.height;

    const auto in_arc = [&](fx2 toward) {
        if (toward.x.raw == 0 && toward.y.raw == 0) return true;   // inside us
        const brad to = fx_atan2(toward.y, toward.x);
        const u32 diff = to.raw - centre.raw;   // wraps, deliberately
        return diff <= half || diff >= (0u - half);
    };
    const auto reaches = [&](fx2 toward, fx surface_radius) {
        const u128 d_sq = len_sq_raw(toward);
        const fx limit = mp.attack_reach + surface_radius;
        if (d_sq > static_cast<u128>(static_cast<i128>(limit.raw) * limit.raw)) return false;
        // And the INNER edge, which is what makes a spear a spear: too close
        // and the weapon misses. Measured to the target's surface like the
        // outer edge, so a wide target is reachable from further in than a
        // narrow one -- the alternative is a halberd that cannot hit a wall.
        if (mp.attack_min_reach > fx::zero()) {
            const fx inner = mp.attack_min_reach + surface_radius;
            if (d_sq < static_cast<u128>(static_cast<i128>(inner.raw) * inner.raw)) return false;
        }
        return true;
    };

    for (u32 t = 0; t < MAX_AGENTS; ++t) {
        if (t == agent || !agents_[t].active) continue;
        if ((a.mv.attack_hit_agents[t / 64] >> (t % 64)) & 1u) continue;   // already hit
        if (a_top + mp.attack_reach < agents_[t].pos.z) continue;
        if (a.pos.z > agents_[t].pos.z + params_[t].height + mp.attack_reach) continue;
        const fx2 toward = fx2{agents_[t].pos.x, agents_[t].pos.y} - from;
        if (!reaches(toward, params_[t].radius)) continue;
        if (!in_arc(toward)) continue;
        a.mv.attack_hit_agents[t / 64] |= (static_cast<u64>(1) << (t % 64));
        attacks_.push(AttackEvent{agent, t});
    }

    for (u32 pi = 0; pi < MAX_PROPS; ++pi) {
        const PropState& p = props_[pi];
        if (!p.active) continue;
        if ((a.mv.attack_hit_props[pi / 64] >> (pi % 64)) & 1u) continue;
        if (a_top + mp.attack_reach < p.pos.z) continue;
        if (a.pos.z > p.pos.z + p.height + mp.attack_reach) continue;
        const fx2 toward = fx2{p.pos.x, p.pos.y} - from;
        if (!reaches(toward, p.radius)) continue;
        if (!in_arc(toward)) continue;
        a.mv.attack_hit_props[pi / 64] |= (static_cast<u64>(1) << (pi % 64));
        attacks_.push(AttackEvent{agent, MAX_AGENTS + pi});
    }
}

// Which agents are standing inside which props.
//
// A cylinder test, not the collision system: touching is about being in the same
// place, and a SOLID prop is one you can touch without ever being inside.
// Reported EVERY tick the overlap holds rather than only on entry, because
// "picked up once" is a decision a script makes by removing the prop -- and a
// key that vanishes is self-limiting in a way an engine-side edge trigger would
// only duplicate.
void World::detect_prop_events() noexcept {
    for (u32 pi = 0; pi < MAX_PROPS; ++pi) {
        const PropState& p = props_[pi];
        if (!p.active) continue;
        for (u32 a = 0; a < MAX_AGENTS; ++a) {
            if (!agents_[a].active) continue;
            const fx3& ap = agents_[a].pos;

            const fx a_top = ap.z + params_[a].height;
            if (a_top < p.pos.z) continue;
            if (ap.z > p.pos.z + p.height) continue;

            const fx dx = ap.x - p.pos.x;
            const fx dy = ap.y - p.pos.y;
            const fx r = params_[a].radius + p.radius;
            const i128 d_sq = static_cast<i128>(dx.raw) * dx.raw + static_cast<i128>(dy.raw) * dy.raw;
            const i128 r_sq = static_cast<i128>(r.raw) * r.raw;
            if (d_sq > r_sq) continue;

            prop_events_.push(PropEvent{a, MAX_AGENTS + pi, PropEventKind::Touch});
        }
    }
}

u64 World::hash() const noexcept {
    StateHasher h;
    h.u32v(tick_);
    motions_.hash_into(h);
    crushes_.hash_into(h);
    interacts_.hash_into(h);
    prop_events_.hash_into(h);
    attacks_.hash_into(h);
    for (u32 i = 0; i < MAX_PROPS; ++i) {
        h.boolv(props_[i].active);
        if (!props_[i].active) continue;
        h.u32v(i);
        h.byte(generation_[MAX_AGENTS + i]);
        h.vec3(props_[i].pos);
        h.u32v(props_[i].facing.raw);
        h.u32v(props_[i].kind);
        h.u32v(props_[i].animation_state);
        h.fxv(props_[i].radius);
        h.fxv(props_[i].height);
    }
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        h.boolv(agents_[i].active);
        if (!agents_[i].active) continue;
        // Whether a body collides is something a client predicts against, so a
        // disagreement about it has to show up on the gate rather than as an
        // agent being stopped by something that is not there.
        h.boolv(agents_[i].solid);
        h.u32v(i);
        h.byte(generation_[i]);
        h.u64v(agent_hash(i));
    }
    return h.value();
}

}  // namespace dnp::sim
