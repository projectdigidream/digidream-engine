#pragma once
//
// The standard platformer controller (design §7.1, §7.1.1).
//
// Native and fixed. Lua decides WHETHER an ability is available and how it is
// tuned, through MoveParams; it never implements the movement, because the
// client must be able to run the identical maths without server Lua.
//
// Every ability here is a sub-state of standard physics, so they are all gated
// by one check: `mode == Standard`. An entity on a rail or in a vehicle has
// them suppressed automatically, with no per-ability exclusion logic to keep in
// sync (design §7.1.1).
//
#include "dnp/physics/collide.hpp"
#include "dnp/physics/params.hpp"

namespace dnp::physics {

struct MoveDefs;   // movedefs.hpp

// Advance one agent by exactly one fixed timestep.
//
// `pos` and `vel` are updated in place, as is `st`. Pure: given identical
// arguments it produces identical results on every target, which is what
// client-side prediction depends on.
//
// `defs` are the module's movement states (movedefs.hpp, docs/MOVEMENT.md);
// none, or none declared, is `default` alone -- the controller as it always was.
void step_controller(const geom::MapData& map, const MoveParams& p, MoveState& st,
                     const MoveCommand& cmd, fx3& pos, fx3& vel, fx dt,
                     const MoveDefs* defs = nullptr) noexcept;

// Hand an entity's current motion back to standard physics.
//
// Design §7.1.1 specifies this same instantaneous-momentum-snapshot pattern for
// every source of prior motion: anchor detach (§3.2.5), climb release, grapple
// release, and controller detach (§7.4). Writing it once means those four
// cannot drift apart -- and they would, because each is added at a different
// time by a different piece of work.
void handoff_to_standard(MoveState& st, fx3& vel, fx3 carried_velocity) noexcept;

}  // namespace dnp::physics
