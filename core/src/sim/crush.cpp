#include "dnp/sim/crush.hpp"

namespace dnp::sim {

void CrushLog::hash_into(StateHasher& h) const noexcept {
    // Hashed even though the events never reach a client. They are a pure
    // function of state the gate already covers, so this adds no information
    // about the world -- what it adds is coverage of the DETECTION, which is
    // otherwise the one piece of tick logic no target comparison would ever
    // exercise. A crush rule that resolved differently under wasm would
    // otherwise be found by a player, on a server, months later.
    h.u32v(count_);
    h.u32v(dropped_);
    for (u32 i = 0; i < count_; ++i) {
        h.u32v(events_[i].entity);
        h.u32v(events_[i].sector);
        h.byte(static_cast<unsigned char>(events_[i].kind));
        h.fxv(events_[i].depth);
    }
}

}  // namespace dnp::sim
