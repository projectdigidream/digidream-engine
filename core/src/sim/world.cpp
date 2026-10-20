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
        weapons_[i] = WeaponState{};
        swing_over_[i] = WSwing{};
        swing_req_[i] = SwingRequest{};
        teleports_[i] = 0;
        // Generations start at 1, not 0: handle raw 0 means "no entity", and a
        // slot-0 agent at generation 0 would encode to exactly that.
        generation_[i] = 1;
    }
    prop_high_water_ = 0;
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
    // The broadphase report and its test switch. Cleared here so a World reused
    // between scenarios does not carry one scenario's mode into the next.
    agrid_fast_ = 0;
    agrid_full_ = 0;
    agrid_off_ = false;
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
            weapons_[i] = WeaponState{};
            swing_over_[i] = WSwing{};
            swing_req_[i] = SwingRequest{};
            teleports_[i] = 0;

            if (i + 1 > agent_high_water_) agent_high_water_ = i + 1;

            // HEIGHT-AWARE. find_sector picks by footprint alone, and with
            // stacked sectors -- a bridge over a path -- both contain the point:
            // it returned whichever was smaller, so a body spawned on the bridge
            // was placed in the path underneath and fell through it.
            agents_[i].mv.sector =
                map_ ? geom::find_sector_at(*map_, fx2{pos.x, pos.y}, pos.z,
                                            params_[i].height)
                     : geom::NO_SECTOR;
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
    weapons_[index] = WeaponState{};
    swing_over_[index] = WSwing{};
    swing_req_[index] = SwingRequest{};
    teleports_[index] = 0;
}

// The effects the server's weapon runtime would send for agent i's events this
// tick (mobs.lua's acts.hitscan tracer, weapons.lua's acts.vfx), worked out in
// THIS world -- for a client, against the bodies it mirrors.
void World::present_weapon(u32 i, const WeaponTable& t) noexcept {
    const AgentState& a = agents_[i];
    const auto push = [&](u16 kind, fx3 pos, brad dir, u32 param) {
        if (weapon_vfx_count_ >= sizeof(weapon_vfx_) / sizeof(weapon_vfx_[0])) return;
        PendingVfx& v = weapon_vfx_[weapon_vfx_count_++];
        v.kind = kind; v.pos = pos; v.dir = dir; v.param = param; v.skip_for = 0;
    };
    for (u32 k = 0; k < weapon_events_.count(); ++k) {
        const WeaponEvent& e = weapon_events_.at(k);
        if (e.weapon >= t.weapon_count || e.phase >= t.weapons[e.weapon].phase_count) continue;
        const WPhase& ph = t.weapons[e.weapon].phases[e.phase];
        if (e.step >= ph.step_count) continue;
        const WStep& st = ph.steps[e.step];
        if (e.action == WAct::Hitscan && st.effect != 0) {
            const fx slope = e.slope_set ? e.slope : fx{};
            for (u32 p = 0; p < e.count; ++p) {
                const brad dir = e.aim + brad(static_cast<u32>(e.offsets[p]));
                const ShotHit hit = hitscan(i, dir, slope, st.range);
                // The tracer carries its start, direction and length; its rise
                // rides in the same param (mobs.lua's tracer_param).
                const fx len = length(fx2{hit.at.x - a.pos.x, hit.at.y - a.pos.y});
                const fx rise = e.slope_set ? slope * len : fx{};
                i64 n = (len * fx::from_int(256)).raw >> 32;
                n = n < 0 ? 0 : (n > 65535 ? 65535 : n);
                i64 r = 32768 + ((rise * fx::from_int(256)).raw >> 32);
                r = r < 0 ? 0 : (r > 65535 ? 65535 : r);
                push(st.effect, fx3{a.pos.x, a.pos.y, hit.at.z - rise}, dir,
                     static_cast<u32>(r) * 65536u + static_cast<u32>(n));
            }
        } else if (e.action == WAct::Launch && st.missile.kind != 0) {
            // mobs.lua's launch(): ahead of the body by `offset` along the
            // missile's own heading, `z_offset` above its feet.
            const fx slope = e.slope_set ? e.slope : fx{};
            for (u32 p = 0; p < e.count && weapon_launch_count_ < MAX_PELLETS; ++p) {
                WeaponLaunch& l = weapon_launches_[weapon_launch_count_++];
                l.weapon = e.weapon; l.phase = e.phase; l.step = e.step;
                l.aim = e.aim + brad(static_cast<u32>(e.offsets[p]));
                l.dir = fx_dir(l.aim);
                // Its `z_offset`, or the body's middle when the missile gave none.
            const fx zo = st.missile.z_offset == MISSILE_AT_MIDDLE ? params_[i].height / 2
                                                                   : st.missile.z_offset;
            l.pos = fx3{a.pos.x + l.dir.x * st.missile.offset, a.pos.y + l.dir.y * st.missile.offset,
                        a.pos.z + zo};
                l.slope = slope;
            }
        } else if (e.action == WAct::Vfx && st.effect != 0) {
            // At the body, along its facing; an attached one names the body by
            // its index on the server, which is what a client follows.
            push(st.effect, a.pos, a.facing, st.attach ? e.agent : 0u);
        }
    }
}

World::ShotHit World::hitscan(u32 i, brad aim, fx slope, fx range) const noexcept {
    ShotHit out;
    const AgentState& sh = agents_[i];
    // From the middle of the body, not its feet: a shot from ankle height is
    // stopped by every step in the map. A FIRST-PERSON body (move_relative,
    // which the fps scheme sets) shoots from its eye, where its camera is, so
    // the ray leaves along the line the player is looking down.
    const fx eye = params_[i].move_relative ? sh.pos.z + params_[i].height * EYE_FRACTION
                                            : sh.pos.z + params_[i].height / 2;
    const fx2 from{sh.pos.x, sh.pos.y};
    out.from = fx3{from.x, from.y, eye};
    const fx2 dir = fx_dir(aim);
    const fx2 to = from + dir * range;

    // Nearest BODY along the ray, by the "in front and within the corridor"
    // test a swing uses -- a cylinder test rather than an exact capsule
    // intersection, which for bodies this size differs only where a shot
    // grazes an edge.
    i32  best = -1;
    fx   best_t = fx::one() + fx::one();
    const auto consider = [&](fx2 p, fx r, fx z0, fx z1, i32 entity) {
        const fx2 toward = p - from;
        const fx along = dot(dir, toward);
        if (along <= fx::zero() || along > range) return;
        // Where the ray is by the time it gets there: a sloped shot has to
        // know how far along the body is before it can say how high it is.
        const fx ray_z = eye + slope * along;
        if (ray_z < z0 || ray_z > z1) return;
        const fx2 closest = from + dir * along;
        const u128 off_sq = len_sq_raw(p - closest);
        if (off_sq > static_cast<u128>(static_cast<i128>(r.raw) * r.raw)) return;
        const fx t = along / range;
        if (t >= best_t) return;
        best_t = t;
        best = entity;
    };
    for (u32 j = 0; j < agent_high_water_; ++j) {
        if (j == i || !agents_[j].active) continue;
        fx3 q{};
        if (!query_body(j, q)) continue;
        consider(fx2{q.x, q.y}, params_[j].radius, q.z, q.z + params_[j].height,
                 static_cast<i32>(j));
    }
    for (u32 k = 0; k < prop_high_water_; ++k) {
        if (!props_[k].active) continue;
        const PropState& p = props_[k];
        consider(fx2{p.pos.x, p.pos.y}, p.radius, p.pos.z, p.pos.z + p.height,
                 static_cast<i32>(MAX_AGENTS + k));
    }

    // And the geometry, which OCCLUDES: a wall between the shooter and a body
    // stops the shot, so it is consulted even when a body was found.
    fx2 end = to;
    if (map_ != nullptr) {
        const physics::RayHit wall =
            physics::raycast(*map_, sh.mv.sector, from, dir, range, 0, eye, eye + slope * range);
        if (wall.blocked) {
            const fx wall_t = wall.distance / range;
            if (best < 0 || wall_t < best_t) { best = -1; best_t = wall_t; }
            end = wall.point;
        }
    }
    if (best >= 0) end = from + dir * (range * best_t);
    out.entity = best;
    // The impact's height is where the ray was when it stopped. `best_t` stays
    // at two when nothing was reached, so one comparison tells them apart.
    const fx end_d = best_t <= fx::one() ? range * best_t : range;
    out.at = fx3{end.x, end.y, eye + slope * end_d};
    return out;
}

bool World::autoaim(u32 i, fx range, u32 cone, brad& heading, fx& slope, bool& slope_set) const noexcept {
    if (cone == 0 || i >= MAX_AGENTS || !agents_[i].active || map_ == nullptr) return false;
    const AgentState& me = agents_[i];
    const fx my_eye = me.pos.z + params_[i].height / 2;
    const fx3 eye{me.pos.x, me.pos.y, my_eye};
    bool found = false;
    fx best_d{};
    brad best_ang{};
    fx best_eye{};
    // NEAREST WINS, then the sight test for the one currently winning -- the
    // ray is the expensive part (mobs.lua's M.autoaim, rule for rule).
    const auto consider = [&](fx3 q, fx height) {
        const fx d = length(fx2{q.x - me.pos.x, q.y - me.pos.y});
        if (d > range || d <= fx::zero()) return;
        if (found && d >= best_d) return;
        const brad ang = fx_atan2(q.y - me.pos.y, q.x - me.pos.x);
        // Signed, wrapped, in brad: a target a degree left of north is one
        // degree away, not 359.
        const i64 off = static_cast<i64>(static_cast<i32>(ang.raw - me.facing.raw));
        if (off > static_cast<i64>(cone) || off < -static_cast<i64>(cone)) return;
        if (!physics::can_see(*map_, me.mv.sector, eye, fx2{q.x, q.y}, q.z, q.z + height)) return;
        found = true;
        best_d = d;
        best_ang = ang;
        best_eye = q.z + height / 2;
    };
    for (u32 j = 0; j < agent_high_water_; ++j) {
        if (j == i || !agents_[j].active || !agents_[j].solid) continue;
        fx3 q{};
        if (!query_body(j, q)) continue;
        consider(q, params_[j].height);
    }
    // A client's mirrored bodies: the band spawn_prop never allocates in.
    for (u32 k = SPAWNABLE_PROPS; k < prop_high_water_; ++k) {
        if (!props_[k].active) continue;
        const PropState& p = props_[k];
        consider(p.pos, p.height);
    }
    if (!found) return false;
    heading = best_ang;
    // slope_to: rise from our middle to theirs, per unit of run.
    slope = (best_eye - my_eye) / best_d;
    slope_set = true;
    return true;
}

u32 World::step_weapon(u32 i, const WeaponTable& table, u8 want, u32 tick, u64 seed,
                       WeaponEventLog& log, u32 stream) noexcept {
    if (i >= MAX_AGENTS || !agents_[i].active) return 0;
    AgentState& a = agents_[i];
    WeaponBody body;
    body.grounded = a.mv.grounded;
    body.vel = a.vel;
    body.facing = a.facing;
    body.pitch = a.pitch;
    const u16 b = live_input(i).buttons;
    const u8 triggers = static_cast<u8>(((b & proto::button::ATTACK_PRIMARY) ? 1 : 0) |
                                        ((b & proto::button::ATTACK_SECONDARY) ? 2 : 0));
    const u32 first = log.count();
    const u32 hops = sim::step_weapon(table, weapons_[i], body, triggers, want, tick,
                                      stream < MAX_AGENTS ? stream : i, seed, log);

    // THE AIM of each shot this tick, decided here so both sides agree on it
    // (docs/WEAPON_PREDICTION.md): mobs.lua's for a player, rule for rule.
    // A first-person body (move_relative, which the fps scheme sets) shoots
    // where it looks, up and down included; any other is corrected towards
    // the nearest body in the cone. A weapon may say otherwise (its
    // `autoaim`): on even for a first-person body, whose shot then climbs to
    // the target's exact middle and keeps its view when the cone is empty, or
    // off for anyone. Its own `autoaim_cone` replaces the table's. A missile
    // names no range, so its search uses the table's.
    for (u32 k = first; k < log.count(); ++k) {
        WeaponEvent& e = log.at_mut(k);
        if (e.action != WAct::Hitscan && e.action != WAct::Launch) continue;
        e.aim = a.facing;
        e.slope = fx{};
        e.slope_set = false;
        const WeaponDef* wd = e.weapon < table.weapon_count ? &table.weapons[e.weapon] : nullptr;
        const u8 mode = wd != nullptr ? wd->autoaim : WAIM_DEFAULT;
        const bool view = params_[i].move_relative;
        if (mode == WAIM_ON || (mode == WAIM_DEFAULT && !view)) {
            fx range = table.autoaim_range;
            if (e.action == WAct::Hitscan && wd != nullptr && e.phase < wd->phase_count &&
                e.step < wd->phases[e.phase].step_count) {
                range = wd->phases[e.phase].steps[e.step].range;
            }
            const u32 cone = wd != nullptr && wd->autoaim_cone != 0 ? wd->autoaim_cone : table.autoaim_cone;
            brad heading{};
            fx slope{};
            bool slope_set = false;
            if (autoaim(i, range, cone, heading, slope, slope_set)) {
                e.aim = heading;
                e.slope = slope;
                e.slope_set = slope_set;
                continue;
            }
        }
        if (view && a.pitch != 0) {
            const fx2 d = fx_dir(brad(static_cast<u32>(static_cast<u16>(a.pitch)) << 16));
            e.slope = d.y / d.x;
            e.slope_set = true;
        }
    }
    // A `swing` step: started (or refused) by this tick's step_agent, with the
    // step's own shape. The machine names the step; the table, which this
    // tick holds, says what it gave.
    if (body.swing) {
        SwingRequest& rq = swing_req_[i];
        rq.want = true;
        rq.from_step = true;
        rq.weapon = body.swing_weapon;
        rq.phase = body.swing_phase;
        rq.step = body.swing_step;
        rq.over = table.step_swing(body.swing_weapon, body.swing_phase, body.swing_step);
    }
    // add_impulse, which is what the Lua path's e:add_impulse lands in: a
    // vertical shove also leaves the ground.
    if (body.impulse.x.raw | body.impulse.y.raw | body.impulse.z.raw) add_impulse(i, body.impulse);
    if (body.anim_set) a.animation_state = body.anim;
    return hops;
}

World::SwingShape World::swing_shape(u32 i) const noexcept {
    const MoveParams& p = params_[i];
    SwingShape s;
    s.reach = p.attack_reach;           s.half_arc = p.attack_half_arc;
    s.sweep_from = p.attack_sweep_from; s.sweep_to = p.attack_sweep_to;
    s.min_reach = p.attack_min_reach;
    s.pitch_from = p.attack_pitch_from; s.pitch_to = p.attack_pitch_to;
    s.half_pitch = p.attack_half_pitch;
    s.ticks = p.attack_ticks;           s.cooldown = p.attack_cooldown;
    const WSwing& o = swing_over_[i];
    if (!o.present) return s;
    namespace f = swing_field;
    if (o.set & f::REACH)      s.reach = o.reach;
    if (o.set & f::HALF_ARC)   s.half_arc = o.half_arc;
    if (o.set & f::SWEEP_FROM) s.sweep_from = o.sweep_from;
    if (o.set & f::SWEEP_TO)   s.sweep_to = o.sweep_to;
    if (o.set & f::MIN_REACH)  s.min_reach = o.min_reach;
    if (o.set & f::PITCH_FROM) s.pitch_from = o.pitch_from;
    if (o.set & f::PITCH_TO)   s.pitch_to = o.pitch_to;
    if (o.set & f::HALF_PITCH) s.half_pitch = o.half_pitch;
    if (o.set & f::TICKS)      s.ticks = o.ticks;
    if (o.set & f::COOLDOWN)   s.cooldown = o.cooldown;
    return s;
}

bool World::request_swing(u32 i) noexcept {
    if (i >= MAX_AGENTS || !agents_[i].active) return false;
    swing_req_[i] = SwingRequest{};
    swing_req_[i].want = true;
    return true;
}

void World::adopt_swing(u32 i, const WeaponTable& table) noexcept {
    if (i >= MAX_AGENTS) return;
    const WeaponState& k = weapons_[i];
    const WSwing* over = table.step_swing(k.swung_weapon, k.swung_phase, k.swung_step);
    swing_over_[i] = over != nullptr ? *over : WSwing{};
}

// A swing begins: as long as its shape says, touching nobody yet.
void World::start_swing(u32 i) noexcept {
    AgentState& a = agents_[i];
    const u8 ticks = swing_shape(i).ticks;
    a.mv.attack_left = ticks == 0 ? 1 : ticks;
    for (u64& w : a.mv.attack_hit_agents) w = 0;
    for (u64& w : a.mv.attack_hit_props) w = 0;
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
    MoveCommand cmd = to_command(live_input(i));

    // AIM SNAPPED TO N DIRECTIONS, before anything reads it.
    //
    // Here rather than on the client, because which directions a weapon allows
    // is a game RULE and rules are authoritative -- and because in the shared
    // controller both sides snap identically, so the snap is predicted rather
    // than corrected. input_scale is the same shape of decision: a replicated
    // parameter that changes how input is INTERPRETED.
    //
    // Exact in brad, with no trigonometry: a turn is 2^32, so the nearest of N
    // evenly spaced headings is a rounded division. The half-step added before
    // dividing is what makes it round rather than floor -- without it every
    // heading is biased anticlockwise by half a sector, which reads as aiming
    // slightly off rather than as snapping.
    if (const u8 n = params_[i].aim_snap; n > 1) {
        const u64 turn = 0x100000000ull;
        const u64 sector = turn / n;
        const u64 aim = static_cast<u64>(cmd.aim.raw);
        const u64 snapped = ((aim + sector / 2) / sector) * sector;
        cmd.aim = brad(static_cast<u32>(snapped & 0xFFFFFFFFull));
    }

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

    // THE VERTICAL AIM, straight from the input and clamped. No rate: a look
    // up is the view, and the view is the player's.
    {
        const i16 p = inputs_[i].pitch;
        a.pitch = p > proto::MAX_PITCH ? proto::MAX_PITCH : (p < -proto::MAX_PITCH ? -proto::MAX_PITCH : p);
    }

    // MOVEMENT RELATIVE TO THE FACING (MoveParams::move_relative): the push's
    // y is forward along the body's facing and its x a sidestep to its right.
    // After the turn, so a strafe that is also turning moves along the new
    // heading -- the same order a first-person view draws it in.
    if (params_[i].move_relative) {
        const fx2 fwd = fx_dir(a.facing);
        const fx2 right{fwd.y, -fwd.x};
        const fx2 m = cmd.move;
        cmd.move = fx2{right.x * m.x + fwd.x * m.y, right.y * m.x + fwd.y * m.y};
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

        // A SWING ASKED FOR -- a weapon's `swing` step, a monster's, a script's
        // -- before the button, and by its own rule: it is somebody saying
        // "now", not a player mashing. A swing still sweeping is REPLACED: it
        // stops where it is, starts no cooldown (it never ended), and the new
        // one runs from its own beginning with a fresh list of who it has hit,
        // so what the first one hit the second can hit again -- two blows.
        // Inside the last swing's cooldown it does not happen, and is not
        // kept for later: a step is a moment, and a swing late is a different
        // attack. docs/SWINGS.md says the same for the studio's preview.
        SwingRequest& rq = swing_req_[i];
        if (rq.want) {
            const bool from_step = rq.from_step;
            const u8 weapon = rq.weapon, phase = rq.phase, step = rq.step;
            const WSwing* over = rq.over;
            rq = SwingRequest{};
            if (a.mv.attack_cooldown_left == 0) {
                swing_over_[i] = over != nullptr ? *over : WSwing{};
                WeaponState& k = weapons_[i];
                k.swung_weapon = from_step ? weapon : NO_WEAPON;
                k.swung_phase = from_step ? phase : NO_PHASE;
                k.swung_step = from_step ? step : 0;
                start_swing(i);
            }
        // attack_enabled is what a ranged weapon sets to zero, and it is checked
        // HERE rather than by refusing to report the hit later: a swing that
        // started and connected with nothing still spends a cooldown, still
        // holds the button's edge, and still animates. The press has to do
        // nothing at all, and the client has to predict that it does. A weapon
        // that swings from its steps sets it to zero too: its button runs its
        // phases, and the steps say when.
        } else if (swing && params_[i].attack_enabled &&
                   !a.mv.attack_held && a.mv.attack_left == 0 &&
                   a.mv.attack_cooldown_left == 0) {
            swing_over_[i] = WSwing{};
            WeaponState& k = weapons_[i];
            k.swung_weapon = NO_WEAPON;
            k.swung_phase = NO_PHASE;
            k.swung_step = 0;
            start_swing(i);
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
            if (a.mv.attack_left == 0) a.mv.attack_cooldown_left = swing_shape(i).cooldown;
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
            if (mi >= 0) {
                // FROM WHERE IT WAS STANDING, so stepping off a turntable
                // throws you along the tangent at your own radius rather than
                // at the sector's. The two are the same number for everything
                // that does not turn.
                const SectorMotion& mm = motions_.at(static_cast<u32>(mi));
                a.vel += mm.carry_delta(fx2{a.pos.x, a.pos.y}) * fx::from_int(TICK_RATE_HZ);
            }
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
    // To the high-water mark, not the full table: see agent_high_water_ in
    // world.hpp. Every loop below skips inactive agents, and a slot at or above
    // the mark has never been active, so the work is identical and the state
    // hash does not move.
    for (u32 i = 0; i < agent_high_water_; ++i) {
        AgentState& a = agents_[i];
        if (!a.active || !a.mv.anchored || a.mv.anchor < 0) continue;
        const i32 mi = motions_.find(static_cast<u32>(a.mv.anchor));
        if (mi < 0) continue;
        // The floor's displacement, not the sector's raw offset: a rider is
        // carried by the surface under its feet, and a descending ceiling moves
        // no surface anybody is standing on.
        //
        // AND OF ITS OWN POINT, not of the sector as a whole. That is the same
        // number for a lift or a slider and is not for a turntable, where a
        // rider near the pivot travels a hair and one at the rim travels the
        // circumference. carry_delta reduces to floor_delta exactly when
        // nothing turned, so the common path is unchanged.
        a.pos += motions_.at(static_cast<u32>(mi))
                     .carry_delta(fx2{a.pos.x, a.pos.y});
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
        if (!motions_.active(k)) continue;
        const SectorMotion& m = motions_.at(k);
        const fx3 d = m.floor_delta();
        // Only a footprint that MOVED sweeps anything. A lift travelling
        // straight up displaces no horizontal space, and running the test for it
        // would push every agent standing beside the shaft away from a wall that
        // never came at them.
        //
        // A TURN counts as moving even when the offset did not: a sector
        // spinning about its own centre travels nowhere and sweeps the room
        // anyway, which is most of what makes a rotating platform interesting
        // and all of what makes it dangerous.
        if (d.x.raw == 0 && d.y.raw == 0 && !m.turning()) continue;

        for (u32 i = 0; i < agent_high_water_; ++i) {
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
    for (u32 i = 0; i < agent_high_water_; ++i) {
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
    teleports_[index] = static_cast<u8>(teleports_[index] + 1);

    // Re-derive the sector. Without this the next tick tests the new position
    // against the old sector's walls, which is how a teleported entity ends up
    // inside geometry or falling through a floor that is not there.
    // Height-aware, for the same reason spawn is: a teleport onto a bridge
    // must not land in the path beneath it.
    a.mv.sector = map_ ? geom::find_sector_at(*map_, fx2{pos.x, pos.y}, pos.z,
                                              params_[index].height)
                       : geom::NO_SECTOR;

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

void World::step_idle() noexcept {
    // The same event clear step() does, so a caller reading events after an
    // idle tick sees an empty set rather than the last occupied tick's.
    crushes_.clear();
    interacts_.clear();
    prop_events_.clear();
    attacks_.clear();
    weapon_events_.clear();

    // Sectors still move. A lift in an empty level goes on travelling, so the
    // first client to arrive computes it exactly where the server has it -- and
    // parametric motion is a function of this tick, which is why the tick has
    // to keep counting even when nothing else happens.
    if (store_) motions_.apply(*store_, tick_);

    // A swing asked for goes with the tick, as in step(): no agent ran to take
    // it, and a weapon step's points into a table only this tick promised.
    for (u32 i = 0; i < agent_high_water_; ++i) swing_req_[i] = SwingRequest{};

    ++tick_;
}

void World::step() noexcept {
    phase(0);
    // Events describe the tick about to run, so the previous tick's are dropped
    // here rather than at the end -- a caller reading them after step() sees
    // exactly what this tick raised.
    crushes_.clear();
    interacts_.clear();
    prop_events_.clear();
    attacks_.clear();
    weapon_events_.clear();
    weapon_vfx_count_ = 0;
    weapon_launch_count_ = 0;

    // A PREDICTING CLIENT's own weapon, first: where the server's weapon
    // runtime steps it, in on_tick, before the flush and the step -- with this
    // tick's input, the body as the last tick left it, and this tick's number.
    // The server never sets this; its Lua calls step_weapon itself.
    if (weapon_table_ != nullptr && weapon_agent_ < MAX_AGENTS &&
        agents_[weapon_agent_].active) {
        step_weapon(weapon_agent_, *weapon_table_, live_input(weapon_agent_).weapon, tick_,
                    weapon_seed_, weapon_events_, weapon_stream_);
        present_weapon(weapon_agent_, *weapon_table_);
    }

    // Sectors move BEFORE agents do. The order matters: an agent riding a lift
    // must be carried by this tick's motion and then run its own physics from
    // the new position, not the other way round -- resolving the agent first
    // would leave it a tick behind the platform, which reads as the platform
    // sliding out from under it every frame.
    phase(1);
    if (store_) motions_.apply(*store_, tick_);
    phase(2);

    carry_anchored();
    phase(3);
    sweep_moving_sectors();
    phase(4);

    // Where every body was before anything moved. Both resolutions read it --
    // see the declaration.
    for (u32 i = 0; i < agent_high_water_; ++i) tick_start_pos_[i] = agents_[i].pos;

    // Ascending index order, always. Any order would be deterministic on its
    // own, but it must be the SAME order on client and server, and index order
    // is the one that cannot drift as the agent set changes.
    for (u32 i = 0; i < agent_high_water_; ++i) {
        if (agents_[i].active && !agents_[i].asleep) step_agent(i);
    }
    // A swing asked for and not taken -- an agent asleep this tick -- goes
    // with the tick: its `over` points into a table only this tick promised.
    for (u32 i = 0; i < agent_high_water_; ++i) swing_req_[i] = SwingRequest{};

    phase(5);
    resolve_agent_collisions();
    phase(6);

    // Last, on settled positions. Running it before the agents moved would
    // report a crush on an agent that was about to walk out from under the
    // ceiling in the same tick, and miss one that walked in.

    detect_vertical_crush();
    phase(7);
    detect_prop_events();
    phase(8);

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
    if (weapons_[i].have != 0) f |= proto::state_flag::ARMED;
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
    // The weapon machine (docs/WEAPON_PREDICTION.md), which the client predicts
    // and SelfBlock restores. Only once held: a body with no weapons hashes as
    // it always did.
    if (weapons_[i].have != 0) hash_weapon_state(h, weapons_[i]);

    // Every field the controller carries between ticks. A replay that started
    // from the wrong dash timer or the wrong jump-edge flag would diverge from
    // the server for reasons no amount of position correction repairs, so the
    // hash has to be able to see all of it.
    h.byte(static_cast<unsigned char>(m.mode));
    h.boolv(m.grounded);
    // By NAME, not index: a client and the server may hold different sets of
    // chunks, so the same sector sits at different indices in their stores.
    // For a one-map store the name is the index.
    h.i32v(sector_name(m.sector));
    h.i32v(sector_name(m.anchor));
    h.boolv(m.anchored);
    h.u32v(a.kind);
    // NOT animation_state, which moved to World::hash below.
    //
    // agent_hash is the PREDICTION comparison and animation_state is written
    // only by Lua -- docs/protocol.md calls it "entirely a client-side cosmetic
    // decision with no gameplay or hit-detection consequence", and nothing in
    // core/ reads it. A field the client cannot reproduce makes every tick after
    // the script touches it a reported mismatch that reconciliation cannot heal,
    // because there is nothing in SelfBlock to heal it from. That was dormant
    // only because nothing had ever set a PLAYER's animation state.
    //
    // Contrast attack_left just below, which the simulation itself reads to gate
    // the next press -- which is exactly why that one had to be replicated
    // instead of removed.
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
    // guaranteed mismatch.
    //
    // This comment used to end "They stay in World::hash, where the determinism
    // gate still covers them", which was not true: the masks were hashed
    // NOWHERE. They are now, beside animation_state below, so the sentence and
    // the code agree. Two swings that connected with different targets are two
    // different simulations even though every position matches, and the gate
    // should be able to see that.
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
        if (i + 1 > prop_high_water_) prop_high_water_ = i + 1;
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
    const u32 slot = entity - MAX_AGENTS;
    if (slot + 1 > prop_high_water_) prop_high_water_ = slot + 1;
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

    // BOX REJECT BEFORE THE CIRCLE, because the circle costs a 128-bit multiply
    // and almost every pair fails. With two hundred monsters on a Doom level
    // this runs forty thousand times a tick and all but a handful are nowhere
    // near each other; the profile had 14% of the whole tick in __udivmodti4
    // and its neighbours, reached to discover exactly that.
    //
    // Exact, not approximate: |dx| >= want implies dx*dx >= want*want implies
    // d_sq >= want_sq, which is precisely the test below. Anything this skips,
    // the circle test would have skipped too -- so the state hash cannot move,
    // and the determinism gate is what says so rather than this comment.
    if (dx >= want || dx <= -want) return;
    if (dy >= want || dy <= -want) return;

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

    for (u32 pi = 0; pi < prop_high_water_; ++pi) {
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

// Bucket every solid agent by the cell its TICK-START position falls in.
//
// Walked from the top DOWN and head-inserted, so each bucket's chain comes back
// out in ASCENDING index order. That is half of what keeps the resolution's
// order intact; the other half is the sort in the caller, which is needed
// anyway because a query reads several buckets.
void World::build_agent_grid() noexcept {
    for (u32 b = 0; b < AGRID_BUCKETS; ++b) agrid_head_[b] = static_cast<u16>(AGRID_EMPTY);
    for (u32 n = agent_high_water_; n-- > 0;) {
        if (!agents_[n].active || !agents_[n].solid) continue;
        const fx3 p = tick_start_pos_[n];
        const u32 b = agrid_bucket(p.x.raw >> AGRID_SHIFT, p.y.raw >> AGRID_SHIFT);
        agrid_next_[n] = agrid_head_[b];
        agrid_head_[b] = static_cast<u16>(n);
    }
}

// The full sweep, in the order the answer is defined by. Both paths below end
// here or match this exactly; it is written once so there is one definition of
// what the fast path has to reproduce.
void World::resolve_agent_against_all(u32 j, fx budget) noexcept {
    AgentState& b = agents_[j];
    for (u32 i = 0; i < agent_high_water_; ++i) {
        if (i == j || !agents_[i].active || !agents_[i].solid) continue;
        const fx3 a = tick_start_pos_[i];
        push_out_of_cylinder(b.pos, b.vel, params_[j].radius, params_[j].height,
                             fx2{a.x, a.y}, params_[i].radius, a.z,
                             a.z + params_[i].height, budget, j > i);
    }
}

void World::resolve_agent_collisions() noexcept {
    const fx3* was = tick_start_pos_;

    // The widest solid body in play, so the query radius can be stated without
    // assuming anything about what a map put in it. One linear pass, and it is
    // the only O(agents) work here outside the resolution itself.
    fx max_radius{};
    for (u32 i = 0; i < agent_high_water_; ++i) {
        if (!agents_[i].active || !agents_[i].solid) continue;
        if (params_[i].radius > max_radius) max_radius = params_[i].radius;
    }
    build_agent_grid();

    u16 cand[AGRID_MAX_CAND];
    agrid_fast_ = 0;
    agrid_full_ = 0;

    for (u32 j = 0; j < agent_high_water_; ++j) {
        if (!agents_[j].active || !agents_[j].solid) continue;
        AgentState& b = agents_[j];
        const fx2 before{b.pos.x, b.pos.y};
        const fx2 travelled{b.pos.x - was[j].x, b.pos.y - was[j].y};
        const fx budget = length(travelled) + DEPENETRATE_PER_TICK;

        // HOW FAR TO LOOK, and it has to be provably enough, because a pair
        // this misses is not a slower answer -- it is a different one.
        //
        // push_out_of_cylinder does nothing unless |b.pos - was[i]| is under
        // radius_j + radius_i, so a query of radius_j + max_radius around
        // b.pos catches everything that can matter AT THIS INSTANT. The catch
        // is that b.pos moves while the inner loop runs: each push shifts it by
        // at most `budget`, and a chain of pushes through a crowd can carry it
        // further than any one of them. So the query is widened by budget plus
        // a cell, and then -- because "plus a cell" is a guess and this file
        // does not get to guess -- the margin is CHECKED below rather than
        // assumed. Leave it, and the body falls back to the full sweep.
        const fx safe = budget + fx::from_int(AGRID_CELL_UNITS);
        const fx reach = params_[j].radius + max_radius + safe;

        u32 n_cand = 0;
        bool overflowed = agrid_off_;
        if (!overflowed) {
            const i64 x0 = (before.x - reach).raw >> AGRID_SHIFT;
            const i64 x1 = (before.x + reach).raw >> AGRID_SHIFT;
            const i64 y0 = (before.y - reach).raw >> AGRID_SHIFT;
            const i64 y1 = (before.y + reach).raw >> AGRID_SHIFT;
            // A body moving absurdly fast, or a map with one enormous body in
            // it, sweeps a cell range wide enough that walking it costs more
            // than the sweep it replaces. Fall back rather than grind.
            if ((x1 - x0 + 1) * (y1 - y0 + 1) > AGRID_MAX_CELLS) {
                overflowed = true;
            } else {
                for (i64 cy = y0; cy <= y1 && !overflowed; ++cy) {
                    for (i64 cx = x0; cx <= x1 && !overflowed; ++cx) {
                        const u32 bk = agrid_bucket(cx, cy);
                        for (u32 i = agrid_head_[bk]; i != AGRID_EMPTY; i = agrid_next_[i]) {
                            if (i == j) continue;
                            if (n_cand >= AGRID_MAX_CAND) { overflowed = true; break; }
                            cand[n_cand++] = static_cast<u16>(i);
                        }
                    }
                }
            }
        }

        if (!overflowed) {
            // Back into ascending index order, because the inner loop mutates
            // b.pos and the sequence of pushes is part of the answer.
            // Insertion sort: the list is short and arrives as a handful of
            // ascending runs, and it allocates nothing, which the tick path
            // does not get to do.
            for (u32 k = 1; k < n_cand; ++k) {
                const u16 v = cand[k];
                u32 m = k;
                while (m > 0 && cand[m - 1] > v) { cand[m] = cand[m - 1]; --m; }
                cand[m] = v;
            }

            // Undo material, for the case where the margin turns out not to
            // have been enough. push_out_of_cylinder writes pos and vel and
            // nothing else, so this restores everything it could have touched.
            const fx3 save_pos = b.pos;
            const fx3 save_vel = b.vel;

            u32 k = 0;
            u16 last = static_cast<u16>(AGRID_EMPTY);
            for (; k < n_cand; ++k) {
                // Two cells can hash to one bucket, so the same agent can be
                // collected twice. Sorted, the duplicates are adjacent; applied
                // twice they would be a second push, which is a different
                // answer rather than a redundant one.
                if (cand[k] == last) continue;
                last = cand[k];

                const u32 i = cand[k];
                const fx3 a = was[i];
                push_out_of_cylinder(b.pos, b.vel, params_[j].radius, params_[j].height,
                                     fx2{a.x, a.y}, params_[i].radius, a.z,
                                     a.z + params_[i].height, budget, j > i);

                // THE MARGIN, CHECKED. If the body is still within `safe` of
                // where the query was centred, then every agent that could have
                // been touching it at any point so far was inside the queried
                // box, and the pairs applied are exactly the pairs the full
                // sweep would have applied. The moment that stops being true,
                // stop -- what has been applied so far is discarded, not
                // patched up.
                const fx dx = b.pos.x - before.x;
                const fx dy = b.pos.y - before.y;
                if (dx > safe || dx < -safe || dy > safe || dy < -safe) break;
            }
            if (k < n_cand) {
                b.pos = save_pos;
                b.vel = save_vel;
                overflowed = true;
            }
        }

        if (overflowed) {
            resolve_agent_against_all(j, budget);
            ++agrid_full_;
        } else {
            ++agrid_fast_;
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
    for (u32 pi = 0; pi < prop_high_water_; ++pi) {
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
    // The shape this swing sweeps: the parameters, and its step's own over them.
    const SwingShape sh = swing_shape(agent);

    // Where the collider is THIS tick.
    //
    // A swing sweeps: it starts at one angle relative to the aim and ends at
    // another, and a target that steps out of the way part-way through is
    // missed. That is the whole difference between this and the one-tick arc
    // test it replaces, and it is why the parameters describe a path rather
    // than a shape.
    //
    // Phase runs 0..1 INCLUSIVE across the swing. attack_left counts DOWN, so
    // the first live tick is the last value and phase is derived from the total.
    //
    // The divisor is total - 1, not total, and that is the whole of a bug that
    // was reported as "the physics doesn't reach the end of the arc". `elapsed`
    // runs 0 .. total-1, so dividing by `total` gives a phase that reaches
    // (total-1)/total and never 1 -- a sixteen-tick swing stopping at 0.9375,
    // which is 0.109 of a turn against the 0.125 it was told to sweep to. The
    // collider covered 168.8 degrees of the 180 its parameters described, every
    // time, and the missing sliver was blamed on the drawing.
    //
    // N ticks visit N positions, and to have the first at `from` and the last
    // at `to` the step is 1/(N-1). A one-tick swing has no step at all, so it
    // takes the middle of the range rather than an arbitrary end.
    const u8 total = sh.ticks == 0 ? 1 : sh.ticks;
    const u8 elapsed = static_cast<u8>(total - a.mv.attack_left);
    const fx phase = total > 1 ? fx::from_frac(elapsed, total - 1)
                               : fx::from_frac(1, 2);
    const fx offset = fx_lerp(sh.sweep_from, sh.sweep_to, phase);

    // Turn fractions to brad. A turn is 2^32, so a fraction of a turn is the
    // fraction scaled by 2^32 -- and fx is already Q32.32, so the raw value IS
    // the answer once the integer part is discarded by the wrap.
    const brad centre = brad(a.facing.raw + static_cast<u32>(offset.raw));
    const u32  half   = static_cast<u32>(sh.half_arc.raw);

    const fx2 from{a.pos.x, a.pos.y};
    const fx  a_top = a.pos.z + mp.height;

    const auto in_arc = [&](fx2 toward) {
        if (toward.x.raw == 0 && toward.y.raw == 0) return true;   // inside us
        const brad to = fx_atan2(toward.y, toward.x);
        const u32 diff = to.raw - centre.raw;   // wraps, deliberately
        return diff <= half || diff >= (0u - half);
    };

    // AND THE VERTICAL HALF, if the weapon asked for one.
    //
    // `attack_half_pitch` of zero means "do not check", which is every swing
    // written before this existed: they stay bounded by the z band below and
    // sweep exactly as they did. A non-zero value makes the sweep a path
    // through BOTH angles, so an overhead chop misses something at your feet.
    //
    // Measured from the attacker's MIDDLE to the target's middle, not foot to
    // foot: a downward swing aimed at a body should connect with its chest, and
    // feet are where the floor is.
    const u32  half_pitch = static_cast<u32>(sh.half_pitch.raw);
    const fx   pitch = fx_lerp(sh.pitch_from, sh.pitch_to, phase);
    const brad pitch_centre = brad(static_cast<u32>(pitch.raw));
    const fx   a_mid = a.pos.z + mp.height / fx::from_int(2);

    const auto in_pitch = [&](fx2 toward, fx target_mid) {
        if (half_pitch == 0) return true;
        const fx flat = length(toward);
        const fx rise = target_mid - a_mid;
        if (flat.raw == 0 && rise.raw == 0) return true;           // inside us
        const brad to = fx_atan2(rise, flat);
        const u32 diff = to.raw - pitch_centre.raw;
        return diff <= half_pitch || diff >= (0u - half_pitch);
    };
    // IS THERE A WALL IN THE WAY?
    //
    // The tests above are distance and angle, which describe a shape and say
    // nothing about the level -- so a swing reached through walls, and a player
    // standing on the far side of a door could kill what was behind it. That is
    // the same mistake sight made: "is it close and in front" is not "can I
    // touch it".
    //
    // The FLAT cast, deliberately, unlike line of sight.
    //
    // The height-aware version is the better question in general -- a balcony
    // and a shut door are two-sided lines and sight has to treat them as walls
    // -- but at a swing's range on converted Doom geometry it produced false
    // negatives I have not fully diagnosed: a player stopped dead against a
    // barrel, swinging at it from half a unit away, came back as having a wall
    // in between. Whatever that is, refusing to let somebody hit a thing they
    // are touching is worse than letting a swing pass over a low sill.
    //
    // So this stops at one-sided walls and BLOCKING sides, which is what the
    // reported bug was: killing enemies through a wall. What remains is
    // reaching over a waist-high divider -- two units of reach rather than two
    // thousand -- and it is recorded here rather than left as a surprise.
    const auto clear_path = [&](fx2 toward) {
        if (map_ == nullptr) return true;
        const fx dist = length(toward);
        if (dist <= fx::zero()) return true;
        const fx2 dir{toward.x / dist, toward.y / dist};
        const physics::RayHit hit =
            physics::raycast(*map_, a.mv.sector, from, dir, dist, 0);
        // Blocked BEFORE the target is a wall between them. Blocked at or past
        // it is the wall behind it, which is not in the way of anything.
        //
        // And an UNRESOLVED ray does not veto a swing. raycast fails closed --
        // it reports blocked when it runs out of hops or starts from a sector
        // it cannot place -- which is right for line of sight, where the cost
        // of a wrong yes is a monster shooting through a wall. At swing range
        // the costs are the other way round: the cost of a wrong no is being
        // unable to hit a barrel you are standing against, which is what
        // happened on MAP02. A refusal with no obstruction to point at is not
        // evidence of a wall.
        if (!hit.blocked) return true;
        return hit.distance <= fx::zero() || hit.distance >= dist;
    };

    const auto reaches = [&](fx2 toward, fx surface_radius) {
        const u128 d_sq = len_sq_raw(toward);
        const fx limit = sh.reach + surface_radius;
        if (d_sq > static_cast<u128>(static_cast<i128>(limit.raw) * limit.raw)) return false;
        // And the INNER edge, which is what makes a spear a spear: too close
        // and the weapon misses. Measured to the target's surface like the
        // outer edge, so a wide target is reachable from further in than a
        // narrow one -- the alternative is a halberd that cannot hit a wall.
        if (sh.min_reach > fx::zero()) {
            const fx inner = sh.min_reach + surface_radius;
            if (d_sq < static_cast<u128>(static_cast<i128>(inner.raw) * inner.raw)) return false;
        }
        return true;
    };

    for (u32 t = 0; t < agent_high_water_; ++t) {
        if (t == agent || !agents_[t].active) continue;
        if ((a.mv.attack_hit_agents[t / 64] >> (t % 64)) & 1u) continue;   // already hit
        if (a_top + sh.reach < agents_[t].pos.z) continue;
        if (a.pos.z > agents_[t].pos.z + params_[t].height + sh.reach) continue;
        const fx2 toward = fx2{agents_[t].pos.x, agents_[t].pos.y} - from;
        if (!reaches(toward, params_[t].radius)) continue;
        if (!in_arc(toward)) continue;
        if (!in_pitch(toward, agents_[t].pos.z + params_[t].height / fx::from_int(2))) continue;
        if (!clear_path(toward)) continue;
        a.mv.attack_hit_agents[t / 64] |= (static_cast<u64>(1) << (t % 64));
        attacks_.push(AttackEvent{agent, t});
    }

    for (u32 pi = 0; pi < prop_high_water_; ++pi) {
        const PropState& p = props_[pi];
        if (!p.active) continue;
        // A prop's own bit, or -- for a client's mirrored body, in the band
        // above the spawnable slots -- the bit of the agent it mirrors.
        u64& word = pi < SPAWNABLE_PROPS ? a.mv.attack_hit_props[pi / 64]
                                         : a.mv.attack_hit_agents[(pi - SPAWNABLE_PROPS) / 64];
        const u64 bit = static_cast<u64>(1) << ((pi < SPAWNABLE_PROPS ? pi : pi - SPAWNABLE_PROPS) % 64);
        if (word & bit) continue;
        if (a_top + sh.reach < p.pos.z) continue;
        if (a.pos.z > p.pos.z + p.height + sh.reach) continue;
        const fx2 toward = fx2{p.pos.x, p.pos.y} - from;
        if (!reaches(toward, p.radius)) continue;
        if (!in_arc(toward)) continue;
        if (!in_pitch(toward, p.pos.z + p.height / fx::from_int(2))) continue;
        if (!clear_path(toward)) continue;
        word |= bit;
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
//
// THROUGH A GRID, and with the same answer as the loop it replaced. That was
// every prop against every agent, every tick: on DOOMWORLD -- thousands of
// props, hundreds of monsters in one shared world -- the single largest cost
// in a server tick, measured. Agents are bucketed by where they stand NOW, and
// each prop asks only the cells its reach covers; the candidates are put back
// in ascending index order, so the events come out prop by prop and agent by
// agent exactly as before. A prop so wide its reach spans too many cells, or a
// cell crowded past the candidate cap, falls back to the full loop for that
// prop: the fallback is the definition, so it only ever costs time.
void World::detect_prop_events() noexcept {
    // The widest body, so a prop's reach is stated rather than assumed.
    fx max_radius{};
    for (u32 a = 0; a < agent_high_water_; ++a) {
        if (agents_[a].active && params_[a].radius > max_radius) max_radius = params_[a].radius;
    }
    // Every active agent at its settled position -- the solid-only grid the
    // collision pass built is from the tick's start, and is not this question.
    for (u32 b = 0; b < AGRID_BUCKETS; ++b) agrid_head_[b] = static_cast<u16>(AGRID_EMPTY);
    for (u32 n = agent_high_water_; n-- > 0;) {
        if (!agents_[n].active) continue;
        const fx3 q = agents_[n].pos;
        const u32 bk = agrid_bucket(q.x.raw >> AGRID_SHIFT, q.y.raw >> AGRID_SHIFT);
        agrid_next_[n] = agrid_head_[bk];
        agrid_head_[bk] = static_cast<u16>(n);
    }

    const auto touch = [&](u32 pi, const PropState& p, u32 a) {
        const fx3& ap = agents_[a].pos;
        const fx a_top = ap.z + params_[a].height;
        if (a_top < p.pos.z) return;
        if (ap.z > p.pos.z + p.height) return;
        const fx dx = ap.x - p.pos.x;
        const fx dy = ap.y - p.pos.y;
        const fx r = params_[a].radius + p.radius;
        const i128 d_sq = static_cast<i128>(dx.raw) * dx.raw + static_cast<i128>(dy.raw) * dy.raw;
        const i128 r_sq = static_cast<i128>(r.raw) * r.raw;
        if (d_sq > r_sq) return;
        prop_events_.push(PropEvent{a, MAX_AGENTS + pi, PropEventKind::Touch});
    };

    u16 cand[AGRID_MAX_CAND];
    for (u32 pi = 0; pi < prop_high_water_; ++pi) {
        const PropState& p = props_[pi];
        if (!p.active) continue;
        const fx reach = p.radius + max_radius;
        const i64 x0 = (p.pos.x - reach).raw >> AGRID_SHIFT;
        const i64 x1 = (p.pos.x + reach).raw >> AGRID_SHIFT;
        const i64 y0 = (p.pos.y - reach).raw >> AGRID_SHIFT;
        const i64 y1 = (p.pos.y + reach).raw >> AGRID_SHIFT;
        u32 n_cand = 0;
        bool full = (x1 - x0 + 1) * (y1 - y0 + 1) > AGRID_MAX_CELLS;
        for (i64 cy = y0; cy <= y1 && !full; ++cy) {
            for (i64 cx = x0; cx <= x1 && !full; ++cx) {
                for (u32 i = agrid_head_[agrid_bucket(cx, cy)]; i != AGRID_EMPTY; i = agrid_next_[i]) {
                    if (n_cand >= AGRID_MAX_CAND) { full = true; break; }
                    cand[n_cand++] = static_cast<u16>(i);
                }
            }
        }
        if (full) {
            for (u32 a = 0; a < agent_high_water_; ++a) if (agents_[a].active) touch(pi, p, a);
            continue;
        }
        // Ascending, and each agent once: two cells can share a bucket.
        for (u32 k = 1; k < n_cand; ++k) {
            const u16 v = cand[k];
            u32 m = k;
            while (m > 0 && cand[m - 1] > v) { cand[m] = cand[m - 1]; --m; }
            cand[m] = v;
        }
        u32 last = AGRID_EMPTY;
        for (u32 k = 0; k < n_cand; ++k) {
            if (cand[k] == last) continue;
            last = cand[k];
            touch(pi, p, cand[k]);
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
        // Asleep is the server's decision about who is simulated at all, which
        // the gate must see and the prediction comparison must not: no client
        // predicts a body that is not its own.
        h.boolv(agents_[i].asleep);
        // Agent fields the DETERMINISM GATE covers and the PREDICTION comparison
        // must not. Both are things the client cannot reproduce -- a script's
        // animation choice, and which targets a swing has already connected with
        // -- so agent_hash excludes them and says why. They still belong to the
        // world's state: two runs that differ here differ, and the gate is the
        // one reader that should notice.
        h.u32v(agents_[i].animation_state);
        for (u64 w : agents_[i].mv.attack_hit_agents) h.u64v(w);
        for (u64 w : agents_[i].mv.attack_hit_props) h.u64v(w);
        h.u32v(i);
        h.byte(generation_[i]);
        h.u64v(agent_hash(i));
    }
    return h.value();
}

// A swept sphere along a segment, against entity cylinders and the map.
//
// The geometry first, because a wall bounds everything after it: an entity
// beyond the wall cannot be reached, so the wall's distance caps the search
// and most entities are then rejected by one comparison.
//
// The entity test is the standard one, arranged to keep the numbers small.
// For a centre c, with m = c - from and dir a unit vector:
//
//   t_c  = m . dir                    how far along the ray it is closest
//   d2   = |m|^2 - t_c^2              how far off the ray it is, squared
//   dt   = sqrt(R^2 - d2)             half the chord through the cylinder
//   t    = t_c - dt                   where the sphere first touches it
//
// Solving the quadratic in the b^2 - c form instead would square coordinates
// that can be a thousand units from the origin; this squares a projection and
// a radius, which are small. Same answer, and it does not saturate.
World::SweepHit World::sweep_entities(fx3 from, fx2 dir, fx dist, fx radius,
                                      i32 ignore_a, i32 ignore_b,
                                      i32 sector, fx slope) const noexcept {
    SweepHit out;
    if (dist <= fx::zero()) return out;

    // The map. `raycast` fails CLOSED -- it reports blocked when it runs out of
    // hops -- which is right here: a missile that cannot be placed should stop
    // rather than fly through the level.
    fx limit = dist;
    if (map_ != nullptr) {
        const i32 s = sector >= 0
            ? sector
            : geom::find_sector_at(*map_, fx2{from.x, from.y}, from.z, radius);
        // A sloped missile's line is tested at the height it crosses each
        // boundary, so a step, a balcony or a low ceiling stops it.
        const bool sloped = slope != physics::RAY_NO_HEIGHT;
        const physics::RayHit wall = sloped
            ? physics::raycast(*map_, s, fx2{from.x, from.y}, dir, dist, 0,
                               from.z, from.z + slope * dist)
            : physics::raycast(*map_, s, fx2{from.x, from.y}, dir, dist, 0);
        if (wall.blocked && wall.distance >= fx::zero() && wall.distance < limit) {
            limit = wall.distance;
            out.hit = true;
            out.distance = wall.distance;
            out.normal = wall.normal;
            out.point = fx3{from.x + dir.x * wall.distance, from.y + dir.y * wall.distance,
                            sloped ? from.z + slope * wall.distance : from.z};
        }
        // AND THE FLOOR AND CEILING OF WHERE IT ENDS UP. The boundaries are
        // the cast's; inside one sector only the planes can stop it -- which
        // is the rocket fired at your own feet, and without this it went down
        // through the floor and on until it met a wall.
        if (sloped && slope != fx::zero()) {
            const fx2 end{from.x + dir.x * limit, from.y + dir.y * limit};
            const fx end_z = from.z + slope * limit;
            // AT ITS HEIGHT, not by footprint. Under a platform the smallest
            // footprint is the platform's, whose floor is over the missile's
            // head -- so a missile flying beneath one, descending at all, was
            // "through" that floor and burst where it was, on nothing.
            const i32 es = geom::find_sector_at(*map_, end, end_z, radius);
            if (es >= 0) {
                const geom::Sector& sec = map_->sector(static_cast<u32>(es));
                const fx plane = slope < fx::zero() ? sec.floor_z : sec.ceil_z;
                const bool through = slope < fx::zero() ? end_z < plane : end_z > plane;
                if (through) {
                    fx t = (plane - from.z) / slope;
                    if (t < fx::zero()) t = fx::zero();
                    if (t > limit) t = limit;
                    limit = t;
                    out.hit = true;
                    out.distance = t;
                    out.point = fx3{from.x + dir.x * t, from.y + dir.y * t, plane};
                    out.normal = fx2{};
                    out.plane = slope < fx::zero() ? -1 : 1;
                }
            }
        }
    }

    // One shared test for agents and props: they are both cylinders, and the
    // only difference is which array they live in. Writing it twice is how the
    // two drift.
    const auto consider = [&](u32 entity, fx3 pos, fx r, fx h) {
        // VERTICAL FIRST: one comparison each way, and on a Doom level most
        // entities are on another floor.
        if (from.z + radius < pos.z) return;
        if (from.z - radius > pos.z + h) return;

        const fx2 m{pos.x - from.x, pos.y - from.y};
        const fx  t_c = m.x * dir.x + m.y * dir.y;
        const fx  reach = r + radius;
        // Behind the start, by more than it is wide: it was never in front.
        if (t_c < -reach) return;
        if (t_c - reach > limit) return;

        // Perpendicular offset, squared, in raw 128-bit so a long ray does not
        // saturate what is meant to be a small number.
        const i128 m_sq = static_cast<i128>(len_sq_raw(m));
        const i128 tc_sq = static_cast<i128>(t_c.raw) * t_c.raw;
        const i128 d2 = m_sq - tc_sq;
        const i128 rr = static_cast<i128>(reach.raw) * reach.raw;
        if (d2 > rr) return;                       // misses to the side

        // Half the chord. d2 <= rr, so this is non-negative.
        const fx dt = fx_sqrt(fx::from_raw(static_cast<i64>((rr - d2) >> 32)));
        fx t = t_c - dt;
        // Already overlapping at the start: it touches immediately.
        if (t < fx::zero()) t = fx::zero();
        if (t > limit) return;

        limit = t;
        out.hit = true;
        out.entity_hit = true;
        out.entity = entity;
        out.normal = fx2{};
        out.plane = 0;
        out.distance = t;
        out.point = fx3{from.x + dir.x * t, from.y + dir.y * t,
                        slope != physics::RAY_NO_HEIGHT ? from.z + slope * t : from.z};
    };

    const auto skipped = [&](u32 e) {
        return (ignore_a >= 0 && static_cast<u32>(ignore_a) == e) ||
               (ignore_b >= 0 && static_cast<u32>(ignore_b) == e);
    };

    // SOLID THINGS ONLY, as Doom's missiles: a rocket flies through a corpse
    // and over a medikit on the floor rather than bursting on them. It used to
    // stop on both -- and a predicting client, which mirrors only solid bodies,
    // could never have agreed with it about where.
    for (u32 i = 0; i < agent_high_water_; ++i) {
        if (!agents_[i].active || !agents_[i].solid || skipped(i)) continue;
        consider(i, agents_[i].pos, params_[i].radius, params_[i].height);
    }
    for (u32 pi = 0; pi < prop_high_water_; ++pi) {
        const PropState& p = props_[pi];
        if (!p.active || (p.flags & prop_flag::SOLID) == 0) continue;
        const u32 entity = MAX_AGENTS + pi;
        if (skipped(entity)) continue;
        consider(entity, p.pos, p.radius, p.height);
    }

    // A wall found first and then beaten by an entity leaves `entity_hit` set
    // and the point on the entity, which is what the caller wants. A wall that
    // survives leaves entity_hit false. Both are already correct above; this
    // is the note that says so rather than a fix.
    return out;
}

}  // namespace dnp::sim
