#pragma once
//
// Bounds-safe little-endian byte cursors.
//
// Two properties matter more than convenience here:
//
//   1. A ByteReader is fed bytes from the network, which means it is fed bytes
//      from anyone. Every read is bounds-checked and a read past the end is a
//      defined no-op that poisons the cursor, never a buffer overrun. Decoders
//      therefore do not need per-field error handling: parse optimistically,
//      check ok() once at the end, discard the packet if it is false.
//
//   2. A ByteWriter is constructed with a CAPACITY and refuses to exceed it.
//      This is how protocol R10 ("never exceed maxDatagramSize") is enforced
//      structurally rather than by a check someone can forget -- a snapshot
//      encoder physically cannot produce an oversized datagram; it runs out of
//      room and reports it.
//
// Byte order is written out explicitly rather than memcpy'd, so nothing here
// depends on the host's endianness.
//
#include <cstddef>

#include "dnp/math/fixed.hpp"
#include "dnp/math/trig.hpp"
#include "dnp/math/vec.hpp"

namespace dnp {

// Quantised position, Q20.12: 1/4096 unit, range +/-524288 units.
// Used for remote entities only -- the local player's own state is sent at full
// precision because reconciliation compares it against the client's own
// arithmetic (protocol R2).
inline constexpr int FXQ_SHIFT = fx::FRAC - 12;   // 20
inline constexpr i64 FXQ_MAX_RAW = static_cast<i64>(2147483647);
inline constexpr i64 FXQ_MIN_RAW = -FXQ_MAX_RAW - 1;

// Truncates toward zero, matching the rounding rule used everywhere else, so
// that a mirrored world quantises to the mirror of the original.
constexpr i32 fx_to_fxq(fx v) noexcept {
    const i64 t = v.raw < 0 ? -((-v.raw) >> FXQ_SHIFT) : (v.raw >> FXQ_SHIFT);
    if (t > FXQ_MAX_RAW) return static_cast<i32>(FXQ_MAX_RAW);
    if (t < FXQ_MIN_RAW) return static_cast<i32>(FXQ_MIN_RAW);
    return static_cast<i32>(t);
}

constexpr fx fxq_to_fx(i32 q) noexcept {
    return fx::from_raw(asl(static_cast<i64>(q), FXQ_SHIFT));
}

class ByteWriter {
public:
    constexpr ByteWriter(u8* data, size_t capacity) noexcept
        : data_(data), cap_(capacity), pos_(0), ok_(true) {}

    constexpr bool   ok() const noexcept { return ok_; }
    constexpr size_t size() const noexcept { return pos_; }
    constexpr size_t capacity() const noexcept { return cap_; }
    constexpr size_t remaining() const noexcept { return ok_ ? cap_ - pos_ : 0; }

    constexpr void u8v(u8 v) noexcept {
        if (!reserve(1)) return;
        data_[pos_++] = v;
    }

    constexpr void u16v(u16 v) noexcept {
        if (!reserve(2)) return;
        data_[pos_++] = static_cast<u8>(v & 0xFF);
        data_[pos_++] = static_cast<u8>((v >> 8) & 0xFF);
    }

    constexpr void u32v(u32 v) noexcept {
        if (!reserve(4)) return;
        for (int i = 0; i < 4; ++i) data_[pos_++] = static_cast<u8>((v >> (i * 8)) & 0xFF);
    }

    constexpr void u64v(u64 v) noexcept {
        if (!reserve(8)) return;
        for (int i = 0; i < 8; ++i) data_[pos_++] = static_cast<u8>((v >> (i * 8)) & 0xFF);
    }

    constexpr void i8v(i8 v)   noexcept { u8v(static_cast<u8>(v)); }
    constexpr void i16v(i16 v) noexcept { u16v(static_cast<u16>(v)); }
    constexpr void i32v(i32 v) noexcept { u32v(static_cast<u32>(v)); }
    constexpr void i64v(i64 v) noexcept { u64v(static_cast<u64>(v)); }

    // LEB128, unsigned.
    constexpr void varint(u64 v) noexcept {
        for (;;) {
            const u8 byte = static_cast<u8>(v & 0x7F);
            v >>= 7;
            if (v == 0) { u8v(byte); return; }
            u8v(static_cast<u8>(byte | 0x80));
            if (!ok_) return;
        }
    }

    constexpr void bradv(brad a) noexcept { u32v(a.raw); }
    constexpr void fxraw(fx v)   noexcept { i64v(v.raw); }
    constexpr void fxq(fx v)     noexcept { i32v(fx_to_fxq(v)); }

    constexpr void vec3_raw(fx3 v) noexcept { fxraw(v.x); fxraw(v.y); fxraw(v.z); }
    constexpr void vec3_q(fx3 v)   noexcept { fxq(v.x);   fxq(v.y);   fxq(v.z); }

    void bytes(const u8* src, size_t n) noexcept {
        if (!reserve(n)) return;
        for (size_t i = 0; i < n; ++i) data_[pos_ + i] = src[i];
        pos_ += n;
    }

    // Snapshot of the cursor, so a speculative section (one entity, say) can be
    // rolled back cleanly when it does not fit in the remaining budget.
    constexpr size_t mark() const noexcept { return pos_; }
    constexpr void rollback(size_t m) noexcept {
        if (m <= pos_) { pos_ = m; ok_ = true; }
    }

private:
    constexpr bool reserve(size_t n) noexcept {
        if (!ok_ || pos_ + n > cap_) { ok_ = false; return false; }
        return true;
    }

    u8*    data_;
    size_t cap_;
    size_t pos_;
    bool   ok_;
};

class ByteReader {
public:
    constexpr ByteReader(const u8* data, size_t size) noexcept
        : data_(data), size_(size), pos_(0), ok_(true) {}

    constexpr bool   ok() const noexcept { return ok_; }
    constexpr size_t pos() const noexcept { return pos_; }
    constexpr size_t remaining() const noexcept { return ok_ && size_ > pos_ ? size_ - pos_ : 0; }
    constexpr bool   at_end() const noexcept { return pos_ >= size_; }

    constexpr u8 u8v() noexcept {
        if (!require(1)) return 0;
        return data_[pos_++];
    }

    constexpr u16 u16v() noexcept {
        if (!require(2)) return 0;
        const u16 v = static_cast<u16>(data_[pos_]) |
                      static_cast<u16>(static_cast<u16>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return v;
    }

    constexpr u32 u32v() noexcept {
        if (!require(4)) return 0;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(data_[pos_ + i]) << (i * 8);
        pos_ += 4;
        return v;
    }

    constexpr u64 u64v() noexcept {
        if (!require(8)) return 0;
        u64 v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<u64>(data_[pos_ + i]) << (i * 8);
        pos_ += 8;
        return v;
    }

    constexpr i8  i8v()  noexcept { return static_cast<i8>(u8v()); }
    constexpr i16 i16v() noexcept { return static_cast<i16>(u16v()); }
    constexpr i32 i32v() noexcept { return static_cast<i32>(u32v()); }
    constexpr i64 i64v() noexcept { return static_cast<i64>(u64v()); }

    // LEB128. Capped at ten bytes: without the cap, a crafted run of
    // continuation bytes walks the cursor until it happens to stop.
    constexpr u64 varint() noexcept {
        u64 result = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            const u8 byte = u8v();
            if (!ok_) return 0;
            result |= static_cast<u64>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return result;
            shift += 7;
        }
        ok_ = false;
        return 0;
    }

    constexpr brad bradv() noexcept { return brad(u32v()); }
    constexpr fx   fxraw() noexcept { return fx::from_raw(i64v()); }
    constexpr fx   fxq()   noexcept { return fxq_to_fx(i32v()); }

    constexpr fx3 vec3_raw() noexcept {
        // Ordered explicitly: argument evaluation order is unspecified in C++17,
        // so building this inline would decode the components in whatever order
        // the compiler chose -- and potentially a different one per target.
        const fx x = fxraw();
        const fx y = fxraw();
        const fx z = fxraw();
        return fx3{x, y, z};
    }

    constexpr fx3 vec3_q() noexcept {
        const fx x = fxq();
        const fx y = fxq();
        const fx z = fxq();
        return fx3{x, y, z};
    }

    const u8* bytes(size_t n) noexcept {
        if (!require(n)) return nullptr;
        const u8* p = data_ + pos_;
        pos_ += n;
        return p;
    }

private:
    constexpr bool require(size_t n) noexcept {
        if (!ok_ || pos_ + n > size_) { ok_ = false; return false; }
        return true;
    }

    const u8* data_;
    size_t    size_;
    size_t    pos_;
    bool      ok_;
};

}  // namespace dnp
