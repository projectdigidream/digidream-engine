#pragma once
//
// Effects over the movement-parameter table (design §6, §7.2).
//
// The engine provides NO stacking or priority resolution, deliberately, and the
// design says so: modifiers apply in the order they were added and that is the
// whole rule. Two effects that both halve a speed produce a quarter, and if a
// game wants "strongest wins" it implements that in Lua. Building a priority
// system into the engine would make one game's convention everybody's.
//
// What the engine DOES guarantee is that adding and removing an effect leaves
// no residue, and that is not free.
//
// THE DRIFT PROBLEM
// -----------------
// The obvious implementation multiplies the live parameter on apply and divides
// it back on remove. In fixed point that loses:
//
//     move_speed = 8
//     apply x1.5  -> 12
//     remove /1.5 -> 7.999999999   (multiply truncates toward zero)
//
// One cycle is invisible. A speed buff that ticks on and off a thousand times
// over a match walks `move_speed` steadily downward, and the player ends up
// slower than they started for reasons nothing in the game can explain. The
// same applies to every additive-inverse pair the moment rounding enters.
//
// So the live parameters are never edited incrementally. The base values are
// kept untouched, and the effective set is RECOMPUTED FROM BASE whenever the
// modifier list changes. Apply-then-remove is then exactly the identity,
// because the intermediate value never existed.
//
#include "dnp/physics/params_table.hpp"
#include "dnp/sim/world.hpp"

namespace dnp::server::lua {

using namespace dnp;

enum class EffectOp : u8 {
    Mul = 0,   // scale
    Add = 1,   // offset
    Set = 2,   // override, last one in the list wins
};

struct Effect {
    u32      id = 0;          // 0 == free slot
    u32      agent = 0;
    u16      field = 0;       // wire id, see params_table.hpp
    EffectOp op = EffectOp::Mul;
    fx       value{};
    // Ticks remaining, or 0 for "until removed". Expiry is a convenience: the
    // alternative is every script writing the same timer.
    u32      expires_at = 0;
};

inline constexpr u32 MAX_EFFECTS = 256;

class EffectStack {
public:
    void clear() noexcept {
        for (u32 i = 0; i < MAX_EFFECTS; ++i) fx_[i] = Effect{};
        next_id_ = 1;
        count_ = 0;
    }

    // Capture an agent's authored parameters as the base. Called once, when the
    // agent spawns: everything after that is computed from this.
    void set_base(u32 agent, const physics::MoveParams& p) noexcept {
        if (agent < sim::MAX_AGENTS) base_[agent] = p;
    }
    const physics::MoveParams& base(u32 agent) const noexcept { return base_[agent]; }

    // Returns the effect id, or 0 if the table is full.
    u32 add(u32 agent, u16 field, EffectOp op, fx value, u32 expires_at) noexcept {
        if (agent >= sim::MAX_AGENTS || field >= physics::PARAM_COUNT) return 0;
        for (u32 i = 0; i < MAX_EFFECTS; ++i) {
            if (fx_[i].id != 0) continue;
            fx_[i] = Effect{next_id_, agent, field, op, value, expires_at};
            ++count_;
            // Wraps past 2^32, skipping 0. An id colliding with a live effect
            // would need four billion intervening effects on one server.
            if (++next_id_ == 0) next_id_ = 1;
            return fx_[i].id;
        }
        ++dropped_;
        return 0;
    }

    bool remove(u32 id) noexcept {
        if (id == 0) return false;
        for (u32 i = 0; i < MAX_EFFECTS; ++i) {
            if (fx_[i].id != id) continue;
            fx_[i] = Effect{};
            --count_;
            return true;
        }
        return false;
    }

    // Drop everything an agent carries, on death or despawn.
    void clear_agent(u32 agent) noexcept {
        for (u32 i = 0; i < MAX_EFFECTS; ++i) {
            if (fx_[i].id != 0 && fx_[i].agent == agent) { fx_[i] = Effect{}; --count_; }
        }
    }

    // Retire anything whose time is up. Returns true if the set changed.
    bool expire(u32 tick) noexcept {
        bool changed = false;
        for (u32 i = 0; i < MAX_EFFECTS; ++i) {
            if (fx_[i].id == 0 || fx_[i].expires_at == 0) continue;
            if (tick >= fx_[i].expires_at) { fx_[i] = Effect{}; --count_; changed = true; }
        }
        return changed;
    }

    // Compute an agent's effective parameters from its base.
    //
    // Always from base, never from the current value. That is the whole
    // anti-drift mechanism -- see the header comment.
    physics::MoveParams compute(u32 agent) const noexcept {
        physics::MoveParams out = base_[agent];
        // Insertion order, not sorted: the design gives the engine no business
        // deciding which of two effects matters more, and sorting would smuggle
        // in a priority rule nobody asked for.
        for (u32 i = 0; i < MAX_EFFECTS; ++i) {
            const Effect& e = fx_[i];
            if (e.id == 0 || e.agent != agent) continue;
            // TYPE-AWARE, because param_get_raw does not return one kind of
            // number.
            //
            // For an Fx field the "raw" is Q32.32 and arithmetic in that domain
            // is exactly right. For a U8 or a Bool it is a PLAIN INTEGER -- 16
            // means sixteen, not sixteen divided by four billion -- and doing
            // fx arithmetic on it was wrong in both directions at once:
            //
            //   Set attack_ticks to 10  ->  wrote fx(10).raw = 42949672960,
            //   which param_set_raw clamps to 255. A swing meant to last a
            //   sixth of a second lasted four and a quarter, its sweep advanced
            //   1/255 of a turn per tick, and it read as a wedge that had
            //   stopped moving. That is how this was found.
            //
            //   Mul a U8 by 1.5  ->  fx::from_raw(16) is 3.7e-9, so the result
            //   was zero. Every effect on max_jumps, max_dashes, attack_ticks,
            //   attack_cooldown or any Bool has been wrong since effects
            //   existed; nothing had set one until the debug panel did.
            //
            // The effect's value is an fx either way -- a script says 1.5 or 10
            // and means the number, not a bit pattern -- so for integer fields
            // the answer is computed in fx and truncated once, at the end.
            const i64 cur = physics::param_get_raw(out, e.field);
            const bool integral = e.field < physics::PARAM_COUNT &&
                                  physics::PARAM_TABLE[e.field].type != physics::ParamType::Fx;
            i64 next = cur;
            if (integral) {
                const fx now = fx::from_int(cur);
                switch (e.op) {
                    case EffectOp::Mul: next = (now * e.value).trunc_int(); break;
                    case EffectOp::Add: next = (now + e.value).trunc_int(); break;
                    case EffectOp::Set: next = e.value.trunc_int(); break;
                }
            } else {
                switch (e.op) {
                    case EffectOp::Mul: next = (fx::from_raw(cur) * e.value).raw; break;
                    case EffectOp::Add: next = (fx::from_raw(cur) + e.value).raw; break;
                    case EffectOp::Set: next = e.value.raw; break;
                }
            }
            physics::param_set_raw(out, e.field, next);
        }
        return out;
    }

    bool has_any(u32 agent) const noexcept {
        for (u32 i = 0; i < MAX_EFFECTS; ++i) {
            if (fx_[i].id != 0 && fx_[i].agent == agent) return true;
        }
        return false;
    }

    u32 count() const noexcept { return count_; }
    u32 dropped() const noexcept { return dropped_; }

private:
    Effect              fx_[MAX_EFFECTS]{};
    physics::MoveParams base_[sim::MAX_AGENTS]{};
    u32                 next_id_ = 1;
    u32                 count_ = 0;
    u32                 dropped_ = 0;
};

}  // namespace dnp::server::lua
