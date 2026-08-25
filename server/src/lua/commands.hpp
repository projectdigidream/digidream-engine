#pragma once
//
// The command queue: everything Lua does to the world passes through here.
//
// Scripts never mutate `sim::World` directly. They append commands, and the
// host applies them at two defined points in the tick -- before `world.step()`
// and after the events it produced. Three things fall out of that which direct
// mutation would each have to remember separately, at every call site:
//
//   1. `touched(agent)` is recorded in one place, which is what lets a snapshot
//      set LUA_APPLIED for the observers it actually concerns (protocol §5).
//   2. Mutation cannot happen part-way through `world.step()`, so the physics
//      never observes an entity moving underneath it mid-resolution.
//   3. The applied log is a replay artifact. Replaying the COMMANDS reproduces
//      the world without replaying the scripts -- so Lua's own reproducibility
//      (`pairs` order, GC timing, JIT warm-up) never has to be guaranteed for
//      the simulation to be reproducible.
//
// The cost, stated because it is real: a script reading back a value it just
// wrote sees the OLD one, within the same callback. That is why the mutating
// verbs are named for what they do rather than reading like assignments.
//
// Absolute vs relative is a deliberate part of the API, not an accident of
// which primitive was easiest. `add_impulse` accumulates, so two effects that
// both want to push an entity this tick both push it. `teleport` and
// `set_velocity` are last-write-wins, because "put it there" has no sensible
// additive reading.
//
#include "dnp/math/vec.hpp"
#include "dnp/sim/world.hpp"

namespace dnp::server::lua {

using namespace dnp;

enum class CmdKind : u8 {
    Teleport = 0,     // absolute
    AddImpulse = 1,   // relative, accumulates
    SetVelocity = 2,  // absolute

    // A scripted-discrete sector offset (§3.1.2). Routed through the queue like
    // everything else rather than written straight to the motion set: the claim
    // that "replaying the command log reproduces the world" is only true if the
    // log is COMPLETE, and a per-tick mutation that skipped it would quietly
    // make that false.
    //
    // `subject` is the sector index, not an agent.
    SetSectorOffset = 3,
};

struct Command {
    CmdKind kind = CmdKind::Teleport;
    u32     subject = 0;   // agent index, or sector index for SetSectorOffset
    fx3     v{};
};

// Bounded like everything else in the tick path. The bound is not a gameplay
// limit; it is what stops a scripting mistake from turning into unbounded work
// inside one frame.
inline constexpr u32 MAX_COMMANDS = 512;

class CommandQueue {
public:
    void clear() noexcept { count_ = 0; dropped_ = 0; }

    bool push(const Command& c) noexcept {
        if (count_ >= MAX_COMMANDS) { ++dropped_; return false; }
        cmds_[count_++] = c;
        return true;
    }

    // Apply every queued command in append order and clear the queue.
    //
    // `touched` receives the index of every agent a command actually changed,
    // so the caller can flag exactly those observers rather than all of them.
    // Flagging everyone would excuse every client's mismatch on every Lua tick
    // and destroy the signal LUA_APPLIED exists to carry.
    template <typename OnTouched>
    u32 apply(sim::World& world, OnTouched&& touched) noexcept {
        u32 applied = 0;
        for (u32 i = 0; i < count_; ++i) {
            const Command& c = cmds_[i];

            if (c.kind == CmdKind::SetSectorOffset) {
                // Sector, not agent -- and no `touched` call: a moving sector is
                // already replicated to every client as a SectorOffset in each
                // snapshot, so it is not an unpredictable change to one
                // observer's own entity and LUA_APPLIED would be the wrong
                // signal for it.
                if (world.motions().set_discrete(c.subject, c.v)) ++applied;
                continue;
            }

            if (c.subject >= sim::MAX_AGENTS || !world.agent(c.subject).active) continue;
            switch (c.kind) {
                case CmdKind::Teleport:    world.teleport(c.subject, c.v); break;
                case CmdKind::AddImpulse:  world.add_impulse(c.subject, c.v); break;
                case CmdKind::SetVelocity: world.set_velocity(c.subject, c.v); break;
                case CmdKind::SetSectorOffset: break;   // handled above
            }
            touched(c.subject);
            ++applied;
        }
        count_ = 0;
        return applied;
    }

    u32 count() const noexcept { return count_; }

    // Never silently cap -- the same rule the snapshot encoder follows (R10).
    u32 dropped() const noexcept { return dropped_; }

private:
    Command cmds_[MAX_COMMANDS]{};
    u32     count_ = 0;
    u32     dropped_ = 0;
};

}  // namespace dnp::server::lua
