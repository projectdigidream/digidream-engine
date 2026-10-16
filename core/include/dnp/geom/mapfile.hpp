#pragma once
//
// The baked map format.
//
// Written by tools/mapc, loaded by the server, and shipped to clients over the
// control channel as MAP_DATA. The loader therefore parses bytes that arrive
// from the network, which is why it is built on the same bounds-safe cursors as
// the wire protocol rather than on a struct memcpy: a zero-copy layout would
// couple the format to compiler padding AND hand an attacker a way to fabricate
// out-of-range indices that are never checked.
//
// Every index in a loaded map is validated against the counts it must fall
// within. A map whose side references point 9000 must be REJECTED, not loaded
// and then read out of bounds at the first collision query.
//
#include "dnp/geom/mapstore.hpp"
#include "dnp/proto/bytes.hpp"

namespace dnp::geom {

inline constexpr u32 MAP_MAGIC   = 0x4D504E44u;   // "DNPM"
inline constexpr u16 MAP_VERSION = 15;  // 15 added a wall's texture angle and scale
                                        // (14 added sector effects (docs/SECTOR_FX.md);
                                        // 13 added a flat's texture anchor;
                                        // 12 added flat styles and surface alpha;
                                        // 11 added camera paths (design §4.1);
                                        // 10 added the texture scale a map is drawn at;
                                        //  8 added its body proportions;
                                        //  7 added sector holes)

// THE OLDEST FILE THIS STILL READS, and the only version window the format has
// ever had.
//
// Every earlier bump rewrote every map, because every earlier bump changed what
// a map MEANS: a file without sector holes is not a file whose holes are empty,
// and a file without body proportions was built for a body it cannot name. A
// v11 file is different, and so is a v12 one. Everything 12 to 15 added is a
// rendering attribute whose absence has exactly one correct reading --
// unrotated, unshifted, full size, opaque, painted in world coordinates, no
// rain or fog, and walls drawn as Doom draws them -- which is what those maps
// were already drawn as.
//
// So the window costs two `if`s and saves re-baking every level in the
// repository, which is not free: a bake moves each map's content hash, and
// modules/doom2/assets/music/index.json is keyed by it, so a re-bake silently
// unmaps thirty-two soundtracks. It has done so twice.
//
// It closes the moment a field arrives whose absence is ambiguous. Say so here
// when that happens rather than quietly widening it further.
inline constexpr u16 MAP_MIN_VERSION = 11;

enum class MapLoadStatus : u8 {
    Ok = 0,
    BadMagic,
    BadVersion,
    Truncated,
    TooLarge,        // exceeds a MAX_MAP_* capacity
    BadIndex,        // an index points outside the arrays it must address
    HashMismatch,    // content does not match the hash the file carries

    // The file's baked triangles are not what this build's triangulator
    // produces for the same polygons. That means the map compiler and the
    // runtime have drifted apart, and every update_shape() would silently
    // reshape the sector. Refusing is the cheap failure; loading it and
    // discovering the mismatch after the first edit is the expensive one.
    TriangleMismatch,
};

// Serialise `store` into `w`. Returns false if the writer ran out of room.
bool save_map(const MapStorage& store, ByteWriter& w) noexcept;

// Parse into `store`, which is cleared first. On any failure the store is left
// cleared rather than partially populated: a half-loaded map is worse than none,
// because it looks usable.
MapLoadStatus load_map(ByteReader& r, MapStorage& store) noexcept;

const char* map_load_status_text(MapLoadStatus s) noexcept;

}  // namespace dnp::geom
