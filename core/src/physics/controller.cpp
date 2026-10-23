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

// How high a climbable side reaches, from the climber's sector: a wall to the
// ceiling, a step up to the floor beyond it. A side with nothing above the
// floor here (a step DOWN) has no climbable face and reaches nowhere.
fx climb_top(const MapData& map, u32 side, i32 sector) noexcept {
    const Sector& s = map.sector(static_cast<u32>(sector));
    const Side& sd = map.side(side);
    if (sd.neighbor == NO_SECTOR) return s.ceil_z;
    const Sector& back = map.sector(static_cast<u32>(sd.neighbor));
    return back.floor_z > s.floor_z ? back.floor_z : s.floor_z;
}

// THE LEDGE NEAREST `p` within `radius` (MoveParams::ledge_grab): a side of
// `sector` whose far side is a floor higher than this one, with headroom for
// the body on it, and whose top lies in [lo, hi]. Any side -- a ledge needs no
// CLIMBABLE flag; what makes it one is that there is somewhere to stand.
i32 find_ledge(const MapData& map, i32 sector, fx2 p, fx radius, fx lo, fx hi,
               fx body_height, fx2& out_normal) noexcept {
    if (sector < 0 || static_cast<u32>(sector) >= map.sector_count) return -1;
    const Sector& sec = map.sector(static_cast<u32>(sector));
    const u128 r_sq = static_cast<u128>(static_cast<i128>(radius.raw) * radius.raw);
    i32  best = -1;
    u128 best_d = r_sq;
    for (u32 k = 0; k < sec.side_count; ++k) {
        const u32 si = sec.first_side + k;
        const Side& sd = map.side(si);
        if (sd.neighbor == NO_SECTOR || (sd.flags & side_flag::BLOCKING) != 0) continue;
        const Sector& back = map.sector(static_cast<u32>(sd.neighbor));
        if (back.floor_z <= sec.floor_z || back.floor_z < lo || back.floor_z > hi) continue;
        if (back.ceil_z - back.floor_z < body_height) continue;
        const u128 d_sq = distance_sq_to_segment(map.point(sd.a), map.point(sd.b), p);
        if (d_sq < best_d) { best_d = d_sq; best = static_cast<i32>(si); }
    }
    if (best < 0) return -1;
    out_normal = -side_normal(map, static_cast<u32>(best), static_cast<u32>(sector));
    return best;
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
    st.hanging = false;
    st.hopping = false;
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
    // Scaled HERE, once, before anything reads it -- so a frozen player is
    // frozen for walking, flying, dashing and the ground/air accel alike rather
    // than in whichever of them somebody remembered. At zero, has_wish goes
    // false and wish_dir falls back to `facing`, which is the right answer: a
    // player who cannot move still faces where they were aiming.
    const fx2 wish = cmd.move * p.input_scale;
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
    //
    // A climbable side is grabbed by holding CLIMB, or -- with climb_auto -- by
    // pushing into it. On it: toward the wall climbs up, away climbs down,
    // along it slides, JUMP leaps off, CROUCH lets go (and so, held, does
    // letting CLIMB go). A wall counts only where it IS: below its top, so a
    // short one cannot be climbed into thin air, and reaching the top of a
    // step is a MANTLE onto the ledge beyond it.

    // How hard the stick pushes into a wall whose inward normal is `n`.
    const auto into = [&](fx2 n) { return -dot(wish, n); };
    const fx push_grab = fx::from_frac(3, 10);   // a deliberate push, not a graze
    const fx reach = p.radius + fx::from_frac(1, 8);

    // A LEDGE, when the module allows grabbing one (ledge_grab): held while the
    // hands are still on it -- its top no more than ledge_reach above the head.
    const fx hands = pos.z + p.height + p.ledge_reach;
    // PULLING UP onto a ledge (JUMP while hanging): still climbing, no longer
    // hanging. The movement below rises until the feet clear `ledge_top`, then
    // moves in over it; once the body's centre is over the ledge's floor the
    // ledge is behind it, and letting go stands it there.
    bool ledge_pull = false;
    fx   ledge_top{};
    if (st.climbing) {
        fx2 n{};
        i32 side = find_side_with_flag(map, st.sector, fx2{pos.x, pos.y}, reach,
                                       side_flag::CLIMBABLE, n);
        bool on_ledge = false;
        if (side < 0 && p.ledge_grab) {
            // Held while the hands can still reach it; below, anything down to
            // the feet -- a hop, or pulling up, takes them above it.
            side = find_ledge(map, st.sector, fx2{pos.x, pos.y}, reach, pos.z - p.height, hands, p.height, n);
            on_ledge = side >= 0;
        }
        const fx top = side >= 0 ? climb_top(map, static_cast<u32>(side), st.sector) : fx::zero();
        if (side >= 0) st.climb_normal = n;
        if (!on_ledge) st.hanging = false;
        // A ledge is held without CLIMB: it was caught, not chosen.
        const bool holding = p.climb_auto || climb_down || on_ledge;
        const bool let_go = cmd.up < fx::zero();
        const fx v = into(st.climb_normal);
        // Climbed down onto the floor: back on your feet. (The mover never
        // calls a climber grounded, so the feet are asked directly.)
        const bool landed = v <= fx::zero() &&
            pos.z <= map.sector(static_cast<u32>(st.sector)).floor_z + fx::from_frac(1, 64);
        if (on_ledge) {
            // HANGING ON A LEDGE. JUMP with the stick at it pulls up onto it;
            // away from it, leaps off as from a wall; still, a small hop that
            // falls back and catches it again. A push away, or CROUCH, lets go.
            if (st.hanging && jump_pressed) {
                if (v > push_grab) {
                    st.hanging = false;
                    st.hopping = true;
                    st.jumps_used = 1;
                } else if (v < -push_grab) {
                    st.climbing = false;
                    st.jumps_used = 1;
                    handoff_to_standard(st, vel, fx3{st.climb_normal.x * p.move_speed,
                                                     st.climb_normal.y * p.move_speed, p.jump_impulse});
                } else {
                    vel.z = p.jump_impulse / fx::from_int(2);
                    st.hopping = true;
                }
            } else if (st.hanging && (let_go || v < -push_grab)) {
                st.climbing = false;
                handoff_to_standard(st, vel, fx3::zero());
            }
            ledge_pull = st.climbing && !st.hanging;
            ledge_top = top;
        } else if (side >= 0 && holding && !let_go && !jump_pressed && !landed && v > fx::zero() &&
            pos.z + v * p.climb_speed * dt >= top && top < map.sector(static_cast<u32>(st.sector)).ceil_z) {
            // THE MANTLE: over the top of a step, onto its ledge. Still rising at
            // climb speed (16 units/s^2 of gravity takes that 4/s to a further
            // 0.5 -- a step's height) and moving onto the ledge, which the body
            // clears and lands on.
            st.climbing = false;
            handoff_to_standard(st, vel, fx3{-st.climb_normal.x * p.climb_speed,
                                             -st.climb_normal.y * p.climb_speed, p.climb_speed});
        } else if (side < 0 || pos.z >= top || !holding || let_go || jump_pressed || landed) {
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
    } else if ((p.climb_enabled || p.ledge_grab) && !st.grappling && cmd.up >= fx::zero() && !jump_pressed) {
        fx2 n{};
        const i32 side = p.climb_enabled
            ? find_side_with_flag(map, st.sector, fx2{pos.x, pos.y}, reach, side_flag::CLIMBABLE, n)
            : -1;
        // Something to climb: more of it above the feet than a step, which
        // the body walks up anyway.
        const bool face = side >= 0 &&
            climb_top(map, static_cast<u32>(side), st.sector) - pos.z > sim::STEP_HEIGHT;
        // THE LEDGE GRAB: in the air and FALLING, pushing into the side of a
        // higher floor whose top is above the waist and within reach above the
        // head. Going up, a body passes a ledge by: a jump is not caught on its
        // way past one, only on its way back down.
        fx2 ln{};
        const i32 ls = (!face && p.ledge_grab && !st.grounded && vel.z <= fx::zero())
            ? find_ledge(map, st.sector, fx2{pos.x, pos.y}, reach,
                         pos.z + p.height / fx::from_int(2), hands, p.height, ln)
            : -1;
        const bool ledge = ls >= 0 && into(ln) > push_grab;
        if (ledge) { n = ln; ledge_top = climb_top(map, static_cast<u32>(ls), st.sector); }
        if (ledge || (face && (climb_down || (p.climb_auto && into(n) > push_grab)))) {
            st.climbing = true;
            st.hanging = ledge;
            st.hopping = false;
            st.climb_normal = n;
            vel = fx3::zero();   // grabbing the surface kills momentum
            st.jumps_used = 0;   // a wall you hold is ground enough to jump from
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
        const fx was_vz = vel.z;   // a hop from a ledge, rising or falling
        // Along the wall slides; toward it climbs, away from it descends. The
        // stick's own direction, so it is the same on any camera and in first
        // person: push at the wall to go up it.
        const fx2 along = fx2{-st.climb_normal.y, st.climb_normal.x};
        const fx2 lateral = along * dot(wish, along);
        vel.x = lateral.x * p.climb_speed;
        vel.y = lateral.y * p.climb_speed;
        vel.z = fx_clamp(-dot(wish, st.climb_normal), -fx::one(), fx::one()) * p.climb_speed;
        // Hanging from a ledge is hanging: no climbing it, only the pull-up --
        // up at twice climb speed, a hop's quickness, until the feet clear the
        // top; then in over it.
        if (st.hanging) {
            // THE HANG: head level with the ledge's top. Above it -- caught
            // high, or in a hop -- gravity brings the body down to it and the
            // hands catch; below it, it pulls itself up to it.
            const fx want = ledge_top - p.height;
            vel = fx3::zero();
            if (was_vz > fx::zero() || pos.z > want) {
                vel.z = was_vz + sim::GRAVITY * dt;
                // Caught: the hop is over -- here, not when the body is exactly
                // at the hang, which a fixed-point hair above it never is.
                if (vel.z <= fx::zero() && pos.z + vel.z * dt <= want) { vel.z = (want - pos.z) / dt; st.hopping = false; }
            } else {
                vel.z = fx_clamp((want - pos.z) / dt, fx::zero(), p.climb_speed);
                st.hopping = false;   // back at the hang: the hop is over
            }
        } else if (ledge_pull) {
            vel = pos.z < ledge_top
                ? fx3{fx::zero(), fx::zero(), p.climb_speed * fx::from_int(2)}
                : fx3{-st.climb_normal.x * p.climb_speed, -st.climb_normal.y * p.climb_speed, fx::zero()};
        }
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
        // COYOTE TIME over: a body that fell rather than jumped has spent its
        // ground jump, and keeps only the air jumps max_jumps leaves. Counted
        // here, not written into jumps_used: what watches that counter (a
        // jump's sound) must not hear a fall as a jump.
        const u8 used = (!st.grounded && st.jumps_used == 0 && st.coyote_timer <= fx::zero())
            ? u8{1} : st.jumps_used;
        if (jump_pressed && used < p.max_jumps && !st.grappling) {
            vel.z = p.jump_impulse;
            st.jumps_used = static_cast<u8>(used + 1);
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
    // Coyote time: full on the ground and on a wall (both are ground enough to
    // jump from), running down in the air.
    st.coyote_timer = (now_grounded || st.climbing) ? p.coyote_time : decay(st.coyote_timer, dt);
}

}  // namespace dnp::physics
