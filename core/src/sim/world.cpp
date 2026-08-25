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
    c.buttons = b;
    return c;
}

}  // namespace

void World::step_agent(u32 i) noexcept {
    AgentState& a = agents_[i];
    const MoveCommand cmd = to_command(inputs_[i]);

    a.facing = cmd.aim;

    const bool was_anchored = a.mv.anchored;
    const i32  prev_anchor = a.mv.anchor;

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
    } else {
        // No map: integrate under gravity only. Not a hidden flat floor -- a
        // silently substituted floor turns a map-loading failure into a physics
        // mystery, and falling forever is at least obviously wrong.
        a.vel.z += GRAVITY * params_[i].gravity_scale * DT;
        a.pos += a.vel * DT;
        a.mv.grounded = false;
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

void World::step() noexcept {
    // Events describe the tick about to run, so the previous tick's are dropped
    // here rather than at the end -- a caller reading them after step() sees
    // exactly what this tick raised.
    crushes_.clear();

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
    return f;
}

u64 World::agent_hash(u32 i) const noexcept {
    StateHasher h;
    const AgentState& a = agents_[i];
    const MoveState& m = a.mv;

    h.vec3(a.pos);
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
    h.fxv(m.dash_timer);
    h.fxv(m.dash_cooldown_timer);
    h.vec2(m.dash_dir);
    h.boolv(m.climbing);
    h.vec2(m.climb_normal);
    h.boolv(m.grappling);
    h.boolv(m.grapple_pulling);
    h.vec3(m.grapple_anchor);
    h.fxv(m.grapple_dist);
    h.fxv(m.hover_fuel);
    h.fxv(m.flight_fuel);
    h.boolv(m.hovering);
    h.boolv(m.flying);
    return h.value();
}

u64 World::hash() const noexcept {
    StateHasher h;
    h.u32v(tick_);
    motions_.hash_into(h);
    crushes_.hash_into(h);
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
