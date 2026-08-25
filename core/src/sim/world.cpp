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

    if (map_) {
        step_controller(*map_, params_[i], a.mv, cmd, a.pos, a.vel, DT);
    } else {
        // No map: integrate under gravity only. Not a hidden flat floor -- a
        // silently substituted floor turns a map-loading failure into a physics
        // mystery, and falling forever is at least obviously wrong.
        a.vel.z += GRAVITY * params_[i].gravity_scale * DT;
        a.pos += a.vel * DT;
        a.mv.grounded = false;
    }
}

void World::step() noexcept {
    // Ascending index order, always. Any order would be deterministic on its
    // own, but it must be the SAME order on client and server, and index order
    // is the one that cannot drift as the agent set changes.
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        if (agents_[i].active) step_agent(i);
    }
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
