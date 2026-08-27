#pragma once
//
// Crushing and horizontal sweep (design §3.1, §3.1.3).
//
// The engine DETECTS; it does not decide. Design §3.1 is explicit that what
// happens to a crushed entity -- damage, death, knockback, nothing at all -- is
// a Lua decision, so this file produces events and stops there. Baking the
// consequence in would make "crushed" mean one fixed thing across every server,
// which is exactly the kind of gameplay rule §2 keeps out of the native core.
//
// Two conditions, and they are the same idea along different axes:
//
//   Vertical    the floor and ceiling of the sector an entity occupies are
//               closer together than the entity is tall. Note the test is on
//               the GEOMETRY, not on the entity's position: the mover already
//               clamps a body between the two surfaces, so by the time it is
//               resolved there is nothing left in the position to notice.
//
//   Horizontal  a sector's footprint has moved into the entity, and the entity
//               has nowhere to be shoved -- static geometry holds it in place
//               while the wall keeps coming (§3.1.3). Pinned, not merely
//               touched: being pushed along by a moving wall is ordinary, and
//               reporting that as a crush would fire the callback on every
//               entity that ever rode past a sliding platform.
//
// The horizontal PUSH is simulation state and is predicted by the client like
// any other position change, because it happens in the shared core. The events
// are not sent to clients at all: they are inputs to server Lua.
//
#include "dnp/math/fixed.hpp"
#include "dnp/sim/state_hash.hpp"

namespace dnp::sim {

using namespace dnp;

enum class CrushKind : u8 {
    Vertical = 1,
    Horizontal = 2,
};

struct CrushEvent {
    u32       entity = 0;
    u32       sector = 0;        // the sector whose geometry did it
    CrushKind kind = CrushKind::Vertical;

    // How badly the space falls short of the body. Vertical: body height minus
    // the gap. Horizontal: the distance the entity had to be shoved and could
    // not be. Reported rather than thresholded here, so a script can scale
    // damage by severity instead of receiving a bare boolean.
    fx depth{};
};

// A tick's worth of events. Fixed capacity, like everything else in the core:
// the count is bounded by the agent count times the moving-sector count, and an
// unbounded queue in the tick path is an allocation the physics contract does
// not allow.
inline constexpr u32 MAX_CRUSH_EVENTS = 64;

class CrushLog {
public:
    void clear() noexcept { count_ = 0; dropped_ = 0; }

    void push(const CrushEvent& e) noexcept {
        if (count_ >= MAX_CRUSH_EVENTS) { ++dropped_; return; }
        events_[count_++] = e;
    }

    u32 count() const noexcept { return count_; }
    const CrushEvent& at(u32 i) const noexcept { return events_[i]; }

    // Protocol R10's rule applied locally: never silently cap. A server that
    // dropped crush events would show up as entities that walk out of a closing
    // door unharmed, and nothing would say why.
    u32 dropped() const noexcept { return dropped_; }

    void hash_into(StateHasher& h) const noexcept;

private:
    CrushEvent events_[MAX_CRUSH_EVENTS]{};
    u32        count_ = 0;
    u32        dropped_ = 0;
};

// An entity reached for a side (design §3.2.2's interaction, and Doom's use key).
//
// Reported, not acted on. What a side DOES when worked -- open, lower, switch,
// nothing -- is a gameplay decision, and gameplay is server Lua. The engine
// answers "who reached for what", which is the same division crushing draws.
enum class TouchKind : u8 {
    // Reached for it deliberately: Doom's use key, a switch, a manual door.
    Use = 0,
    // Walked across it. Doom's W1/WR line specials, and by a wide margin the
    // most common trigger class in its maps -- teleports, remote doors and
    // lifts are nearly all trip-wires rather than switches.
    Cross = 1,
};

struct InteractEvent {
    u32 entity = 0;
    u32 side = 0;
    i32 sector = -1;
    TouchKind kind = TouchKind::Use;
};

inline constexpr u32 MAX_INTERACT_EVENTS = 32;

class InteractLog {
public:
    void clear() noexcept { count_ = 0; dropped_ = 0; }
    void push(const InteractEvent& e) noexcept {
        if (count_ >= MAX_INTERACT_EVENTS) { ++dropped_; return; }
        events_[count_++] = e;
    }
    u32 count() const noexcept { return count_; }
    const InteractEvent& at(u32 i) const noexcept { return events_[i]; }
    u32 dropped() const noexcept { return dropped_; }
    void hash_into(StateHasher& h) const noexcept;

private:
    InteractEvent events_[MAX_INTERACT_EVENTS]{};
    u32 count_ = 0;
    u32 dropped_ = 0;
};

}  // namespace dnp::sim
