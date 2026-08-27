//
// The simulation world. See world.hpp for why it lives in core/.
//
#include "dnp/sim/world.hpp"

namespace dnp::sim {

using namespace dnp::physics;

void World::reset() noexcept {
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
    overlaps_.clear();
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
    c.buttons = b;
    return c;
}

}  // namespace

void World::step_agent(u32 i) noexcept {
    AgentState& a = agents_[i];
    const MoveCommand cmd = to_command(inputs_[i]);

    a.facing = cmd.aim;

    // Interaction, on the PRESS edge and before the move.
    //
    // Before, so that what an entity reaches for is what was in front of it
    // when the button went down, not wherever the tick's movement carried it.
    // Edge-triggered, or a held button works a door sixty times a second.
    {
        const bool want = (cmd.buttons & act::INTERACT) != 0;
        if (want && !a.mv.interact_held && map_) {
            const i32 side = physics::find_interact_side(
                *map_, a.mv.sector, fx2{a.pos.x, a.pos.y}, a.facing,
                INTERACT_REACH, a.pos.z, params_[i].height);
            if (side >= 0) {
                interacts_.push(InteractEvent{i, static_cast<u32>(side), a.mv.sector});
            }
        }
        a.mv.interact_held = want;
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

    // Sectors move BEFORE agents do. The order matters: an agent riding a lift
    // must be carried by this tick's motion and then run its own physics from
    // the new position, not the other way round -- resolving the agent first
    // would leave it a tick behind the platform, which reads as the platform
    // sliding out from under it every frame.
    if (store_) motions_.apply(*store_, tick_);

    carry_anchored();
    sweep_moving_sectors();

    // Ascending index order, always. Any order would be deterministic on its
    // own, but it must be the SAME order on client and server, and index order
    // is the one that cannot drift as the agent set changes.
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        if (agents_[i].active) step_agent(i);
    }

    // Last, on settled positions. Running it before the agents moved would
    // report a crush on an agent that was about to walk out from under the
    // ceiling in the same tick, and miss one that walked in.
    detect_vertical_crush();
    detect_overlaps();

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
    h.byte(m.jumps_used);
    h.byte(m.dashes_used);
    h.boolv(m.jump_held);
    h.boolv(m.dash_held);
    h.boolv(m.grapple_held);
    h.boolv(m.interact_held);
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

void OverlapLog::hash_into(StateHasher& h) const noexcept {
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
    }
}

u32 World::spawn_prop(PropKind kind, fx3 pos) noexcept {
    for (u32 i = 0; i < MAX_PROPS; ++i) {
        if (props_[i].active) continue;
        props_[i] = PropState{};
        props_[i].kind = kind;
        props_[i].pos = pos;
        props_[i].active = true;
        return MAX_AGENTS + i;
    }
    return MAX_ENTITIES;
}

void World::despawn_prop(u32 entity) noexcept {
    if (!is_prop(entity)) return;
    if (!props_[entity - MAX_AGENTS].active) return;
    props_[entity - MAX_AGENTS] = PropState{};
    // Bump the generation so every handle to what used to be here goes stale.
    // Without it a script holding the key it just consumed would keep finding a
    // live entity in the slot the next pickup lands in.
    generation_[entity] = static_cast<u8>(generation_[entity] + 1);
}

// Which agents are standing inside which props.
//
// A cylinder test, not the collision system: nothing is pushed out of a prop and
// a prop does not block. Reported EVERY tick the overlap holds rather than only
// on entry, because "picked up once" is a decision a script makes by removing
// the prop -- and a key that vanishes is self-limiting in a way an engine-side
// edge trigger would only duplicate.
void World::detect_overlaps() noexcept {
    overlaps_.clear();
    for (u32 pi = 0; pi < MAX_PROPS; ++pi) {
        const PropState& p = props_[pi];
        if (!p.active) continue;
        for (u32 a = 0; a < MAX_AGENTS; ++a) {
            if (!agents_[a].active) continue;
            const fx3& ap = agents_[a].pos;

            // Vertical first: it rejects most pairs with two compares, and a
            // key on a balcony is not picked up from the floor below it.
            const fx a_top = ap.z + params_[a].height;
            if (a_top < p.pos.z) continue;
            if (ap.z > p.pos.z + p.height) continue;

            const fx dx = ap.x - p.pos.x;
            const fx dy = ap.y - p.pos.y;
            const fx r = params_[a].radius + p.radius;
            // Squared, in 128-bit: comparing distances would need a square root
            // and the sum of two radii is small enough that the product never
            // approaches the limit.
            const i128 d_sq = static_cast<i128>(dx.raw) * dx.raw + static_cast<i128>(dy.raw) * dy.raw;
            const i128 r_sq = static_cast<i128>(r.raw) * r.raw;
            if (d_sq > r_sq) continue;

            overlaps_.push(OverlapEvent{a, MAX_AGENTS + pi});
        }
    }
}

u64 World::hash() const noexcept {
    StateHasher h;
    h.u32v(tick_);
    motions_.hash_into(h);
    crushes_.hash_into(h);
    interacts_.hash_into(h);
    overlaps_.hash_into(h);
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
        h.u32v(i);
        h.byte(generation_[i]);
        h.u64v(agent_hash(i));
    }
    return h.value();
}

}  // namespace dnp::sim
