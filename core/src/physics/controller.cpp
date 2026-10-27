#include "dnp/physics/controller.hpp"
#include "dnp/physics/movedefs.hpp"

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


// THE LAYERED MOVES on this tick's velocity, just before the sweep: each axis
// a move holds is its speed (`set`) or what it was before the state steered
// (`add`, whose impulse came as it started). Not while climbing or on a rope:
// those are the engine's, and hold the body themselves.
void apply_moves(const MoveDefs& d, MoveState& st, fx3& vel, fx3 vel0) noexcept {
    if (st.climbing || st.grappling) return;
    const ActiveMove& h = st.moves[0];
    if (h.move != 0) {
        const LayeredMoveDef& m = d.moves[h.move - 1];
        const fx2 v = m.mix == MoveMix::Set ? h.dir * m.speed : fx2{vel0.x, vel0.y};
        vel.x = v.x;
        vel.y = v.y;
    }
    const ActiveMove& u = st.moves[1];
    if (u.move != 0) {
        const LayeredMoveDef& m = d.moves[u.move - 1];
        if (m.mix == MoveMix::Set) vel.z = m.dir == MoveDir::Up ? m.speed : -m.speed;
        if (m.dir == MoveDir::Up) st.grounded = false;
    }
}

// THE END OF EVERY TICK, whatever moved the body: the swept move, where it is
// now, whether it stands, and what standing gives back. Its own function since
// the module's movement states (docs/MOVEMENT.md) end their ticks here too,
// and the layered moves are laid on here, last, so they hold over both.
void finish_tick(const MapData& map, const MoveParams& p, MoveState& st, fx3& pos, fx3& vel,
                 fx dt, const MoveDefs* defs, fx3 vel0) noexcept {
    if (defs != nullptr && defs->move_count > 0) apply_moves(*defs, st, vel, vel0);
    MoveInput mi;
    mi.pos = pos;
    mi.vel = vel;
    mi.radius = p.radius;
    mi.height = p.height;
    mi.sector = st.sector;
    mi.grounded = st.grounded;

    const MoveResult mr = move(map, mi, dt);
    st.wall_contact = mr.hit_wall;

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
    // The wall's normal is a climber's, and only a climber's: cleared off the
    // wall, so no stale one outlives the climb in the hash. SelfBlock carries
    // it while climbing; off it, both sides agree it is nothing.
    if (!st.climbing) st.climb_normal = fx2{};
}

// The flags `default` carries for the abilities it alone has, cleared on the
// way into one of the module's states so none of them outlives it.
void enter_data_state(MoveState& st) noexcept {
    st.climbing = false;
    st.hanging = false;
    st.hopping = false;
    st.grappling = false;
    st.grapple_pulling = false;
    st.hovering = false;
    st.flying = false;
}

bool holds(const MoveCondition& c, const MapData& map, const MoveState& st, fx3 vel,
           u16 now, u16 pressed, u16 released, u8 depth) noexcept {
    switch (c.kind) {
        case MoveCond::Grounded: return st.grounded;
        case MoveCond::Airborne: return !st.grounded;
        case MoveCond::Rising:   return !st.grounded && vel.z > fx::zero();
        case MoveCond::Falling:  return !st.grounded && vel.z < fx::zero();
        case MoveCond::Pressed:  return (pressed & c.arg) != 0;
        case MoveCond::Held:     return (now & c.arg) != 0;
        case MoveCond::Released: return (released & c.arg) != 0;
        case MoveCond::SpeedAbove:
        case MoveCond::SpeedBelow: {
            const fx s = length(fx2{vel.x, vel.y});
            return c.kind == MoveCond::SpeedAbove ? s > c.value : s < c.value;
        }
        case MoveCond::After:    return st.state_ticks >= c.arg;
        case MoveCond::FloorTag:
            return st.grounded && st.sector >= 0 && static_cast<u32>(st.sector) < map.sector_count &&
                   map.sector(static_cast<u32>(st.sector)).tag == c.arg;
        case MoveCond::DepthAtLeast: return depth >= c.arg;
        case MoveCond::DepthNone:    return depth == 0;
        case MoveCond::DepthBelow:   return depth < c.arg;
        case MoveCond::MeterEmpty:   return st.meters[c.arg] <= fx::zero();
        case MoveCond::MeterFull:    return st.meters[c.arg] >= c.value;
        case MoveCond::MeterBelow:   return st.meters[c.arg] < c.value;
        case MoveCond::MeterAbove:   return st.meters[c.arg] > c.value;
        case MoveCond::Asked:        return st.asked != 0 && st.asked == c.arg + 1;
        case MoveCond::TouchingWall: return st.wall_contact;
        case MoveCond::None: break;
    }
    return false;
}

// START A LAYERED MOVE, if its axis is free or holds one of no higher
// priority, and its cost can be paid. An `add` gives its impulse now.
}  // namespace

bool start_layered_move(const MoveDefs& d, u8 k, MoveState& st, fx3& vel, fx2 facing) noexcept {
    const LayeredMoveDef& m = d.moves[k];
    const bool vertical = m.dir == MoveDir::Up || m.dir == MoveDir::Down;
    ActiveMove& slot = st.moves[vertical ? 1 : 0];
    if (slot.move != 0 && d.moves[slot.move - 1].priority > m.priority) return false;
    if (m.cost_meter != NO_METER) {
        if (st.meters[m.cost_meter] < m.cost) return false;
        st.meters[m.cost_meter] = st.meters[m.cost_meter] - m.cost;
    }
    const fx2 dir = m.dir == MoveDir::Facing ? facing : m.dir == MoveDir::Back ? fx2{-facing.x, -facing.y} : fx2{};
    if (m.mix == MoveMix::Add) {
        if (vertical) vel.z = vel.z + (m.dir == MoveDir::Up ? m.speed : -m.speed);
        else { vel.x = vel.x + dir.x * m.speed; vel.y = vel.y + dir.y * m.speed; }
        if (m.dir == MoveDir::Up) st.grounded = false;
    }
    if (m.ticks > 0) slot = ActiveMove{static_cast<u8>(k + 1), m.ticks, dir};
    return true;
}

namespace {

// THE WAY OUT, if one holds this tick: the state's own transitions before
// `any`'s, each in file order, the first whose every condition holds. `any`
// never re-enters the state already running, which would restart its clock.
void take_transition(const MoveDefs& d, const MapData& map, const MoveParams& p, MoveState& st,
                     fx3& vel, fx2 facing, u16 now, u16 pressed, u16 released, u8 depth) noexcept {
    for (int pass = 0; pass < 2; ++pass) {
        for (u8 k = 0; k < d.transition_count; ++k) {
            const MoveTransition& t = d.transitions[k];
            if (pass == 0 ? t.from != st.move_state : (t.from != MOVE_ANY || t.to == st.move_state)) continue;
            if (t.to >= d.state_count) continue;
            bool all = true;
            for (u8 c = 0; c < t.cond_count && all; ++c) all = holds(t.cond[c], map, st, vel, now, pressed, released, depth);
            if (!all) continue;
            if (st.move_state == 0 && t.to != 0) enter_data_state(st);
            st.move_state = t.to;
            st.state_ticks = 0;
            if (st.asked == t.to + 1) st.asked = 0;   // what was asked for, given
            const MoveStateDef& to = d.states[t.to];
            switch (t.action) {
                case MoveAction::Jump:
                    vel.z = (to.has & move_has::JUMP) ? to.jump : p.jump_impulse;
                    st.grounded = false;
                    if (st.jumps_used == 0) st.jumps_used = 1;
                    break;
                case MoveAction::Stop: vel = fx3::zero(); break;
                case MoveAction::ImpulseUp: vel.z = vel.z + t.value; break;
                case MoveAction::ImpulseFacing: vel.x = vel.x + facing.x * t.value; vel.y = vel.y + facing.y * t.value; break;
                case MoveAction::ImpulseBack: vel.x = vel.x - facing.x * t.value; vel.y = vel.y - facing.y * t.value; break;
                case MoveAction::MeterSet:
                case MoveAction::MeterAdd: {
                    const fx v = t.action == MoveAction::MeterSet ? t.value : st.meters[t.meter] + t.value;
                    st.meters[t.meter] = fx_clamp(v, fx::zero(), d.meters[t.meter].max);
                    break;
                }
                case MoveAction::StartMove: start_layered_move(d, t.meter, st, vel, facing); break;
                case MoveAction::None: break;
            }
            return;
        }
    }
}

// THE METERS, for a tick: each fills or drains at the rate of the first rule
// that holds -- the state's own, then `any`'s, in file order -- or its own
// rate with none, and stays within 0..max.
void run_meters(const MoveDefs& d, const MapData& map, MoveState& st, fx3 vel, u16 now, u16 pressed,
                u16 released, u8 depth, fx dt) noexcept {
    for (u8 m = 0; m < d.meter_count; ++m) {
        fx rate = d.meters[m].rate;
        bool found = false;
        for (int pass = 0; pass < 2 && !found; ++pass) {
            for (u8 k = 0; k < d.meter_rule_count && !found; ++k) {
                const MeterRule& r = d.meter_rules[k];
                if (r.meter != m || r.in != (pass == 0 ? st.move_state : MOVE_ANY)) continue;
                bool all = true;
                for (u8 c = 0; c < r.cond_count && all; ++c) all = holds(r.cond[c], map, st, vel, now, pressed, released, depth);
                if (all) { rate = r.rate; found = true; }
            }
        }
        st.meters[m] = fx_clamp(st.meters[m] + rate * dt, fx::zero(), d.meters[m].max);
    }
}

// The layered moves that start themselves: each whose conditions all hold and
// that is not the one already running on its axis.
void auto_moves(const MoveDefs& d, const MapData& map, MoveState& st, fx3& vel, fx2 facing, u16 now,
                u16 pressed, u16 released, u8 depth) noexcept {
    for (u8 k = 0; k < d.move_count; ++k) {
        const LayeredMoveDef& m = d.moves[k];
        if (m.cond_count == 0) continue;
        const bool vertical = m.dir == MoveDir::Up || m.dir == MoveDir::Down;
        if (st.moves[vertical ? 1 : 0].move == k + 1) continue;
        bool all = true;
        for (u8 c = 0; c < m.cond_count && all; ++c) all = holds(m.cond[c], map, st, vel, now, pressed, released, depth);
        if (all) start_layered_move(d, k, st, vel, facing);
    }
}

// ONE OF THE MODULE'S STATES, for a tick: one rule with its numbers. Velocity
// moves toward (stick x speed) at `accel` while the stick is pushed and eases
// off at `friction` when it is not; gravity x `gravity`, plus `buoyancy`. The
// group says what the stick moves you through. A number the state does not
// set is the player's own.
void step_data_state(const MoveStateDef& s, const MoveParams& p, MoveState& st, const MoveCommand& cmd,
                     fx2 wish, bool has_wish, u16 pressed, u8 depth, fx3& vel, fx dt) noexcept {
    const fx speed    = (s.has & move_has::SPEED)    ? s.speed    : p.move_speed;
    const fx accel    = (s.has & move_has::ACCEL)    ? s.accel    : p.ground_accel;
    const fx friction = (s.has & move_has::FRICTION) ? s.friction : p.friction;
    const fx gravity  = (s.has & move_has::GRAVITY)  ? s.gravity  : p.gravity_scale;
    const fx jump     = (s.has & move_has::JUMP)     ? s.jump     : p.jump_impulse;
    const fx buoyancy = (s.has & move_has::BUOYANCY) ? s.buoyancy : fx::zero();
    const bool ground = st.grounded || (s.flags & move_flag::COUNTS_AS_GROUND) != 0;

    switch (s.group) {
        case MoveGroup::Floor:
        case MoveGroup::Air: {
            // On the ground a floor state accelerates and brakes as walking
            // does; in the air -- off a ledge mid-slide -- and always for an
            // air state, it steers as a fall does.
            const bool on_floor = s.group == MoveGroup::Floor && st.grounded;
            const fx air = (s.has & move_has::ACCEL) ? accel : p.air_accel;
            const fx rate = on_floor ? (has_wish ? accel : friction) : air;
            const fx2 v2 = approach2(fx2{vel.x, vel.y}, wish * speed, rate, dt);
            vel.x = v2.x;
            vel.y = v2.y;
            if ((pressed & act::JUMP) && !(s.flags & move_flag::NO_JUMP) && ground) {
                vel.z = jump;
                st.grounded = false;
                st.jumps_used = 1;
            }
            break;
        }
        case MoveGroup::Liquid: {
            // In 3D: the stick on the level, JUMP up and CROUCH down (cmd.up).
            // Up stops at the surface: with the water no higher than the waist
            // a swimmer is at the top, and leaving it is a transition's `do`.
            const fx up = (cmd.up > fx::zero() && depth < 3) ? fx::zero() : cmd.up;
            const fx3 target{wish.x * speed, wish.y * speed, up * speed};
            const fx rate = (has_wish || cmd.up != fx::zero()) ? accel : friction;
            vel.x = approach(vel.x, target.x, rate, dt);
            vel.y = approach(vel.y, target.y, rate, dt);
            vel.z = approach(vel.z, target.z, rate, dt);
            // Buoyancy lifts only what is in the water to the waist, so a body
            // left alone floats with the surface about its middle.
            vel.z = vel.z + ((depth >= 2 ? buoyancy : fx::zero()) + sim::GRAVITY * gravity) * dt;
            return;
        }
        case MoveGroup::None: {
            const fx2 v2 = approach2(fx2{vel.x, vel.y}, fx2{}, st.grounded ? friction : p.air_accel, dt);
            vel.x = v2.x;
            vel.y = v2.y;
            break;
        }
        case MoveGroup::Default: break;
    }
    vel.z = vel.z + (sim::GRAVITY * gravity + buoyancy) * dt;
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
                     const MoveCommand& cmd, fx3& pos, fx3& vel, fx dt,
                     const MoveDefs* defs) noexcept {
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
    const bool jump_released   = !jump_down && st.jump_held;
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

    // ------------------------------------------------------- movement states
    //
    // THE MODULE'S MOVEMENT STATES (docs/MOVEMENT.md). Their ways in and out
    // are taken here, once a tick, before anything moves; while one of them
    // runs it is the whole of this tick's movement, and everything below --
    // `default` -- is what runs in state 0. The buttons are remembered every
    // tick, states or none, so `pressed` means pressed this tick.
    const u16 now_buttons = move_buttons(cmd);
    const u16 pressed = static_cast<u16>(now_buttons & ~st.prev_buttons);
    const u16 released = static_cast<u16>(st.prev_buttons & ~now_buttons);
    st.prev_buttons = now_buttons;
    // How deep the body stands in liquid, measured only when something asks.
    const u8 depth = (defs != nullptr && defs->uses_depth) ? liquid_depth(map, pos, p.height) : 0;
    // The layered moves' clocks, before anything can start one this tick.
    for (ActiveMove& am : st.moves) {
        if (am.move != 0 && --am.left == 0) am = ActiveMove{};
    }
    if (defs != nullptr && defs->transition_count > 0) {
        take_transition(*defs, map, p, st, vel, facing, now_buttons, pressed, released, depth);
    }
    if (st.state_ticks < 0xFFFF) ++st.state_ticks;
    if (defs != nullptr && defs->meter_count > 0) {
        run_meters(*defs, map, st, vel, now_buttons, pressed, released, depth, dt);
    }
    if (defs != nullptr && defs->move_count > 0) {
        auto_moves(*defs, map, st, vel, facing, now_buttons, pressed, released, depth);
    }
    // What the velocity was before the state steers: an `add` move keeps it.
    const fx3 vel0 = vel;
    if (defs != nullptr && st.move_state != 0 && st.move_state < defs->state_count) {
        const MoveStateDef& s = defs->states[st.move_state];
        step_data_state(s, p, st, cmd, wish, has_wish, pressed, depth, vel, dt);
        finish_tick(map, p, st, pos, vel, dt, defs, vel0);
        if (s.flags & move_flag::COUNTS_AS_GROUND) { st.jumps_used = 0; st.coyote_timer = p.coyote_time; }
        return;
    }

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
        // VARIABLE JUMP (MoveParams::variable_jump): JUMP let go while the jump
        // still rises cuts the rise, once -- the release is an edge. Only after a
        // jump: jumps_used is 0 after a fall or a launch from the ground, and a
        // ledge's hop is hopping.
        if (p.variable_jump && jump_released && st.jumps_used > 0 && !st.hopping &&
            !st.grappling && !st.grounded && vel.z > fx::zero()) {
            vel.z = vel.z * p.jump_release_keep;
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

    finish_tick(map, p, st, pos, vel, dt, defs, vel0);
}

}  // namespace dnp::physics
