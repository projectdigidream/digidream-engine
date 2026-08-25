#include "dnp/sim/sectormotion.hpp"

#include "dnp/sim/state_hash.hpp"

namespace dnp::sim {

fx3 parametric_offset(const SectorMotion& m, u32 tick) noexcept {
    if (m.duration_ticks == 0) return m.to;

    // Before the start tick the motion has not begun. Unsigned tick arithmetic
    // would wrap here and place a not-yet-started sector somewhere arbitrary,
    // so the comparison is explicit.
    if (tick <= m.start_tick) return m.from;

    const u32 elapsed = tick - m.start_tick;
    u32 t = 0;
    switch (m.loop) {
        case LoopMode::Once:
            t = elapsed >= m.duration_ticks ? m.duration_ticks : elapsed;
            break;
        case LoopMode::Loop:
            t = elapsed % m.duration_ticks;
            break;
        case LoopMode::PingPong: {
            const u32 cycle = elapsed % (m.duration_ticks * 2);
            t = cycle <= m.duration_ticks ? cycle : (m.duration_ticks * 2 - cycle);
            break;
        }
    }

    // fx_lerp returns exactly `from` at 0 and exactly `to` at 1
    // (docs/physics-contract.md rule 4). That is why a lift settles precisely on
    // its endpoint instead of stopping one ulp short and reporting itself as
    // still moving forever.
    const fx phase = fx::from_frac(static_cast<i64>(t), static_cast<i64>(m.duration_ticks));
    return fx3{fx_lerp(m.from.x, m.to.x, phase),
               fx_lerp(m.from.y, m.to.y, phase),
               fx_lerp(m.from.z, m.to.z, phase)};
}

u32 SectorMotionSet::add(const geom::MapStorage& map, u32 sector) noexcept {
    if (count_ >= MAX_MOVING_SECTORS) return MAX_MOVING_SECTORS;
    const i32 existing = find(sector);
    if (existing >= 0) return static_cast<u32>(existing);

    const geom::MapData m = map.view();
    if (sector >= m.sector_count) return MAX_MOVING_SECTORS;
    const geom::Sector& s = m.sector(sector);
    if (s.point_count > geom::MAX_POLY_POINTS) return MAX_MOVING_SECTORS;

    SectorMotion& sm = motions_[count_];
    sm = SectorMotion{};
    sm.sector = sector;
    sm.base_point_count = s.point_count;
    for (u16 i = 0; i < s.point_count; ++i) sm.base_points[i] = m.point(s.first_point + i);
    sm.base_floor = s.floor_z;
    sm.base_ceil = s.ceil_z;
    return count_++;
}

i32 SectorMotionSet::find(u32 sector) const noexcept {
    for (u32 i = 0; i < count_; ++i) {
        if (motions_[i].sector == sector) return static_cast<i32>(i);
    }
    return -1;
}

bool SectorMotionSet::start_parametric(u32 sector, fx3 from, fx3 to, u32 start_tick,
                                       u32 duration_ticks, LoopMode loop) noexcept {
    const i32 i = find(sector);
    if (i < 0) return false;
    SectorMotion& m = motions_[static_cast<u32>(i)];
    m.type = MotionType::Parametric;
    m.from = from;
    m.to = to;
    m.start_tick = start_tick;
    m.duration_ticks = duration_ticks;
    m.loop = loop;
    return true;
}

bool SectorMotionSet::set_discrete(u32 sector, fx3 offset) noexcept {
    const i32 i = find(sector);
    if (i < 0) return false;
    SectorMotion& m = motions_[static_cast<u32>(i)];
    m.type = MotionType::ScriptedDiscrete;
    m.offset = offset;
    return true;
}

void SectorMotionSet::apply(geom::MapStorage& map, u32 tick) noexcept {
    for (u32 i = 0; i < count_; ++i) {
        SectorMotion& m = motions_[i];
        m.prev_offset = m.offset;

        if (m.type == MotionType::Parametric) {
            m.offset = parametric_offset(m, tick);
        }
        // ScriptedDiscrete keeps whatever set_discrete() last wrote; Static
        // keeps a zero offset.

        geom::Sector& s = map.sector_mut(m.sector);

        // Written as base + offset every tick, never as an accumulated
        // increment. Accumulating would let a sector drift by the rounding of
        // each step, and a platform that ends a round trip a few ulp from where
        // it started is a platform that eventually is not where the level
        // author put it.
        s.floor_z = m.base_floor + m.offset.z;
        s.ceil_z = m.base_ceil + m.offset.z;

        if (m.offset.x.raw != 0 || m.offset.y.raw != 0 ||
            m.prev_offset.x.raw != 0 || m.prev_offset.y.raw != 0) {
            // Horizontal motion moves the footprint, so the points have to be
            // rewritten. The triangulation is unaffected -- translating a
            // polygon cannot change which vertices are ears -- so this is a
            // pure vertex update, exactly as §3.1.1 describes.
            for (u16 k = 0; k < m.base_point_count; ++k) {
                map.point_mut(s.first_point + k) =
                    fx2{m.base_points[k].x + m.offset.x, m.base_points[k].y + m.offset.y};
            }
        }
    }
}

void SectorMotionSet::hash_into(StateHasher& h) const noexcept {
    h.u32v(count_);
    for (u32 i = 0; i < count_; ++i) {
        const SectorMotion& m = motions_[i];
        h.u32v(m.sector);
        h.byte(static_cast<unsigned char>(m.type));
        h.vec3(m.offset);
        h.vec3(m.prev_offset);
        h.vec3(m.from);
        h.vec3(m.to);
        h.u32v(m.start_tick);
        h.u32v(m.duration_ticks);
        h.byte(static_cast<unsigned char>(m.loop));
    }
}

}  // namespace dnp::sim
