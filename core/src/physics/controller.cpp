#include "dnp/physics/controller.hpp"

#include "dnp/sim/constants.hpp"

namespace dnp::physics {

using namespace dnp::geom;

namespace {

// Move `v` toward `target` by at most `rate * dt`, without overshooting.
// Used for ground/air acceleration and friction so those share one rule.
fx approach(fx v, fx target, fx rate, fx dt) noexcept {
    const fx step = rate * dt;
    const fx diff = target - v;
    if (fx_abs(diff) <= step) return target;
    return v + (diff > fx::zero() ? step : -step);
}

fx2 approach2(fx2 v, fx2 target, fx rate, fx dt) noexcept {
    return fx2{approach(v.x, target.x, rate, dt), approach(v.y, target.y, rate, dt)};
}

// Tick a timer down without letting it go negative.
fx decay(fx t, fx dt) noexcept {
    const fx n = t - dt;
    return n < fx::zero() ? fx::zero() : n;
}

}  // namespace

void handoff_to_standard(MoveState& st, fx3& vel, fx3 carried_velocity) noexcept {
    st.mode = ControllerMode::Standard;
    st.climbing = false;
    st.grappling = false;
    st.grapple_pulling = false;
    st.hovering = false;
    st.flying = false;
    vel = carried_velocity;
}

void step_controller(const MapData& map, const MoveParams& p, MoveState& st,
                     const MoveCommand& cmd, fx3& pos, fx3& vel, fx dt) noexcept {
    // One gate for every ability below (design §7.1.1). A rail, wire or vehicle
    // controller owns motion entirely while it is active.
    if (st.mode != ControllerMode::Standard) return;

    const bool jump_down    = (cmd.buttons & act::JUMP) != 0;
    const bool dash_down    = (cmd.buttons & act::DASH) != 0;
    const bool climb_down   = (cmd.buttons & act::CLIMB) != 0;
    const bool hover_down   = (cmd.buttons & act::HOVER) != 0;
    const bool flight_down  = (cmd.buttons & act::FLIGHT) != 0;
    const bool grapple_down = (cmd.buttons & act::GRAPPLE) != 0;

    // Rising edges. Held input must never re-trigger, and the previous state is
    // simulation state so a replay reproduces the same edges.
    const bool jump_pressed    = jump_down && !st.jump_held;
    const bool dash_pressed    = dash_down && !st.dash_held;
    const bool grapple_pressed = grapple_down && !st.grapple_held;
    st.jump_held = jump_down;
    st.dash_held = dash_down;
    st.grapple_held = grapple_down;

    st.dash_cooldown_timer = decay(st.dash_cooldown_timer, dt);

    // Aim direction, shared by dash and the ranged grapple.
    const fx2 facing = fx_dir(cmd.aim);
    const fx2 wish = cmd.move;
    const bool has_wish = wish.x.raw != 0 || wish.y.raw != 0;
    const fx2 wish_dir = has_wish ? normalize(wish) : facing;

    // ---------------------------------------------------------------- grapple

    if (st.grappling) {
        if (!grapple_down || !p.grapple_enabled) {
            // Release: hand the current velocity straight into standard physics.
            // Same pattern as every other detach in this file.
            handoff_to_standard(st, vel, vel);
        }
    } else if (grapple_pressed && p.grapple_enabled) {
        // Direct grip first: an anchor already within reach attaches with no
        // travel time (design §7.1.1's handle/ring case).
        fx2 n{};
        const i32 near_side = find_side_with_flag(map, st.sector, fx2{pos.x, pos.y},
                                                  p.radius * 2, side_flag::GRAPPLE, n);
        if (near_side >= 0) {
            const Side& sd = map.side(static_cast<u32>(near_side));
            const fx2 anchor = closest_point_on_segment(map.point(sd.a), map.point(sd.b),
                                                        fx2{pos.x, pos.y});
            st.grappling = true;
            st.grapple_anchor = fx3{anchor.x, anchor.y, pos.z + p.height};
            st.grapple_dist = length(st.grapple_anchor - pos);
        } else {
            // Ranged: the same swept test CCD uses, so a wall that blocks
            // movement also blocks the hook.
            const RayHit h = raycast(map, st.sector, fx2{pos.x, pos.y}, facing,
                                     p.grapple_range, side_flag::GRAPPLE);
            if (h.hit) {
                st.grappling = true;
                st.grapple_anchor = fx3{h.point.x, h.point.y, pos.z + p.height};
                st.grapple_dist = length(st.grapple_anchor - pos);
            }
        }
        st.grapple_pulling = (cmd.buttons & act::PULL) != 0;
    }

    // ---------------------------------------------------------------- climbing

    if (st.climbing) {
        const bool still_touching =
            find_side_with_flag(map, st.sector, fx2{pos.x, pos.y},
                                p.radius + fx::from_frac(1, 8), side_flag::CLIMBABLE,
                                st.climb_normal) >= 0;
        if (!climb_down || !still_touching || jump_pressed) {
            // Letting go, moving off the surface, or jumping off all end the
            // state the same way: carry whatever velocity there is into
            // standard physics.
            fx3 carried = vel;
            if (jump_pressed) {
                // Wall jump: push off the surface and upward.
                carried.x = st.climb_normal.x * p.move_speed;
                carried.y = st.climb_normal.y * p.move_speed;
                carried.z = p.jump_impulse;
                st.jumps_used = 1;
            }
            st.climbing = false;
            handoff_to_standard(st, vel, carried);
        }
    } else if (climb_down && p.climb_enabled && !st.grappling) {
        fx2 n{};
        if (find_side_with_flag(map, st.sector, fx2{pos.x, pos.y},
                                p.radius + fx::from_frac(1, 8), side_flag::CLIMBABLE, n) >= 0) {
            st.climbing = true;
            st.climb_normal = n;
            vel = fx3::zero();   // grabbing the surface kills momentum
        }
    }

    // -------------------------------------------------------- hover / flight

    // Fuel is shared or separate at the server's discretion (design §7.1.1),
    // so both paths read and write through the same helper.
    const bool want_flight = flight_down && p.flight_enabled && !st.climbing && !st.grappling;
    const bool want_hover  = hover_down && p.hover_enabled && !st.climbing &&
                             !st.grappling && !want_flight && !st.grounded;

    st.flying = want_flight && st.flight_fuel > fx::zero();
    st.hovering = want_hover && st.hover_fuel > fx::zero();

    if (st.flying) {
        st.flight_fuel = decay(st.flight_fuel, p.flight_drain * dt);
        if (p.shared_fuel) st.hover_fuel = st.flight_fuel;
    } else if (st.hovering) {
        st.hover_fuel = decay(st.hover_fuel, p.hover_drain * dt);
        if (p.shared_fuel) st.flight_fuel = st.hover_fuel;
    }

    // -------------------------------------------------------------------- dash

    if (st.dash_timer > fx::zero()) {
        st.dash_timer = decay(st.dash_timer, dt);
    } else if (dash_pressed && st.dashes_used < p.max_dashes &&
               st.dash_cooldown_timer <= fx::zero() && !st.climbing) {
        st.dash_timer = p.dash_duration;
        st.dash_cooldown_timer = p.dash_cooldown;
        st.dash_dir = wish_dir;
        ++st.dashes_used;
        // Added ON TOP of existing velocity, not replacing it (design §7.1.1).
        vel.x += st.dash_dir.x * p.dash_speed;
        vel.y += st.dash_dir.y * p.dash_speed;
    }

    // --------------------------------------------------------------- movement

    fx effective_gravity_scale = p.gravity_scale;

    if (st.climbing) {
        // Gravity suspended, movement locked to the surface plane, climb speed
        // rather than ground speed.
        effective_gravity_scale = fx::zero();
        const fx2 along = fx2{-st.climb_normal.y, st.climb_normal.x};
        const fx2 lateral = along * dot(wish, along);
        vel.x = lateral.x * p.climb_speed;
        vel.y = lateral.y * p.climb_speed;
        vel.z = cmd.up * p.climb_speed;
    } else if (st.flying) {
        // Gravity off; input maps to full 3D velocity rather than lateral only.
        effective_gravity_scale = fx::zero();
        vel.x = wish.x * p.flight_speed;
        vel.y = wish.y * p.flight_speed;
        vel.z = cmd.up * p.flight_speed;
    } else {
        if (st.hovering) {
            effective_gravity_scale = p.hover_gravity_scale;
            vel.z = fx_clamp(vel.z, -p.hover_vspeed_clamp, p.hover_vspeed_clamp);
        }

        if (st.dash_timer <= fx::zero()) {
            // Ordinary acceleration toward the requested velocity. During a dash
            // this is skipped so the impulse is not immediately eaten by
            // friction, which would make a dash feel like a nudge.
            const fx2 target = wish * p.move_speed;
            const fx rate = st.grounded ? (has_wish ? p.ground_accel : p.friction)
                                        : p.air_accel;
            const fx2 v2 = approach2(fx2{vel.x, vel.y}, target, rate, dt);
            vel.x = v2.x;
            vel.y = v2.y;
        }

        // Multi-jump. `max_jumps` generalises to any count, so "double jump" is
        // a parameter value and not a code branch.
        if (jump_pressed && st.jumps_used < p.max_jumps && !st.grappling) {
            vel.z = p.jump_impulse;
            ++st.jumps_used;
            st.grounded = false;
        }

        vel.z += sim::GRAVITY * effective_gravity_scale * dt;
    }

    // ------------------------------------------------- grapple constraint

    if (st.grappling) {
        const fx3 to_anchor = st.grapple_anchor - pos;
        const fx  dist = length(to_anchor);

        if (st.grapple_pulling) {
            // Reel in: accelerate toward the anchor with gravity's influence
            // reduced, ending when it is reached.
            if (dist <= p.radius) {
                handoff_to_standard(st, vel, vel);
            } else {
                const fx3 dir = normalize(to_anchor);
                vel = dir * p.pull_speed;
            }
        } else if (dist > st.grapple_dist && dist > fx::zero()) {
            // Distance constraint: hold the rope length, which turns gravity
            // into pendulum motion. Only the outward component is removed, so
            // swinging inward stays free.
            const fx3 dir = normalize(to_anchor);
            pos = st.grapple_anchor - dir * st.grapple_dist;
            const fx radial = dot(vel, dir);
            if (radial < fx::zero()) vel -= dir * radial;

            // Damping, applied natively, so an undamped swing cannot oscillate
            // forever (design §7.1.1).
            vel -= vel * p.swing_damping;
        }
    }

    // ------------------------------------------------------------- integrate

    MoveInput mi;
    mi.pos = pos;
    mi.vel = vel;
    mi.radius = p.radius;
    mi.height = p.height;
    mi.sector = st.sector;
    mi.grounded = st.grounded;

    const MoveResult mr = move(map, mi, dt);

    pos = mr.pos;
    vel = mr.vel;
    st.sector = mr.sector;

    // Climbing and flight hold themselves up; being at floor level while doing
    // either must not count as landing.
    const bool now_grounded = mr.grounded && !st.climbing && !st.flying;

    if (now_grounded) {
        // Charges reset on every grounded tick, not on the landing transition.
        // A standing agent must be able to jump again immediately rather than
        // waiting to re-enter the grounded state, so `max_jumps` and
        // `max_dashes` bound AIRBORNE use -- which is exactly what they are for.
        // Ground dashing is paced by dash_cooldown instead.
        st.jumps_used = 0;
        st.dashes_used = 0;

        // Fuel refills on the landing TRANSITION only. Design §7.1.1 leaves
        // refill policy to Lua (over time, on an item, on landing); this is the
        // one native case, and doing it every tick would make a grounded agent's
        // fuel un-drainable.
        if (!st.grounded) {
            st.hover_fuel = p.hover_fuel_max;
            st.flight_fuel = p.flight_fuel_max;
        }
    }
    st.grounded = now_grounded;
}

}  // namespace dnp::physics
