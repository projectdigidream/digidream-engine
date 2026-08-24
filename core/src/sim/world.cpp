//
// Placeholder point-mass simulation. See world.hpp for why it lives in core/.
//
#include "dnp/sim/world.hpp"

namespace dnp::sim {

void World::reset() noexcept {
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        agents_[i] = AgentState{};
        params_[i] = AgentParams{};
        inputs_[i] = proto::InputFrame{};
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
            params_[i] = AgentParams{};
            inputs_[i] = proto::InputFrame{};
            return i;
        }
    }
    return MAX_AGENTS;
}

void World::despawn(u32 index) noexcept {
    if (index >= MAX_AGENTS) return;   // defined no-op, never an error
    agents_[index] = AgentState{};
    inputs_[index] = proto::InputFrame{};
}

void World::set_input(u32 index, const proto::InputFrame& f) noexcept {
    if (index >= MAX_AGENTS) return;
    inputs_[index] = f;
}

void World::step_agent(u32 i) noexcept {
    AgentState& a = agents_[i];
    const AgentParams& p = params_[i];
    const proto::InputFrame& in = inputs_[i];

    const bool grounded = (a.state_flags & proto::state_flag::GROUNDED) != 0;

    // Horizontal velocity is set directly from input rather than accelerated
    // toward it. That is the placeholder part: it makes the harness's expected
    // positions trivially checkable from the test client, which is the point at
    // this stage. Phase 2 replaces it.
    const fx mx = proto::axis_to_fx(in.move_x);
    const fx my = proto::axis_to_fx(in.move_y);
    a.vel.x = mx * p.move_speed;
    a.vel.y = my * p.move_speed;

    // Jump on the rising edge only. Holding the button must not re-trigger.
    const bool jump_now = (in.buttons & proto::button::JUMP) != 0;
    if (jump_now && !a.jump_held && a.jumps_used < p.max_jumps) {
        a.vel.z = p.jump_impulse;
        ++a.jumps_used;
    }
    a.jump_held = jump_now;

    a.vel.z += GRAVITY * p.gravity_scale * DT;

    a.pos += a.vel * DT;

    // Flat floor at z = 0 stands in for sector collision.
    if (a.pos.z <= fx::zero()) {
        a.pos.z = fx::zero();
        if (a.vel.z < fx::zero()) a.vel.z = fx::zero();
        a.state_flags |= proto::state_flag::GROUNDED;
        // Landing replenishes jumps -- the same rule that will apply to
        // re-anchoring and vehicle entry in Phase 2 (design 7.1.1).
        a.jumps_used = 0;
    } else {
        a.state_flags = static_cast<u16>(a.state_flags & ~proto::state_flag::GROUNDED);
    }

    (void)grounded;
    a.facing = in.aim;
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

u64 World::agent_hash(u32 i) const noexcept {
    StateHasher h;
    const AgentState& a = agents_[i];
    h.vec3(a.pos);
    h.vec3(a.vel);
    h.bradv(a.facing);
    h.u32v(a.state_flags);
    h.byte(a.jumps_used);
    h.boolv(a.jump_held);
    return h.value();
}

u64 World::hash() const noexcept {
    StateHasher h;
    h.u32v(tick_);
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
        h.boolv(agents_[i].active);
        if (!agents_[i].active) continue;
        h.u32v(i);
        h.u64v(agent_hash(i));
    }
    return h.value();
}

}  // namespace dnp::sim
