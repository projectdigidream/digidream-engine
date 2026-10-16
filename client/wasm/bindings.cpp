//
// Emscripten bindings: the client's window onto the simulation core.
//
// This is deliberately thin. The wasm module owns exactly the code that MUST
// produce the same answer as the server -- the simulation step -- and nothing
// else. Rendering, camera, UI and input capture stay in TypeScript, where a
// wrong last bit is a sub-pixel difference nobody can see.
//
// State crosses the boundary through caller-supplied buffers rather than as i64
// return values. That avoids depending on -sWASM_BIGINT for the ABI, and it
// keeps the raw Q32.32 values intact: JS reads them as BigInt via BigInt64Array,
// so the client's copy of the authoritative state is bit-exact. Rounding to a
// double here would silently break the exact-match reconciliation contract
// (docs/protocol.md R2/R3) at the very boundary it depends on.
//
#include <emscripten/emscripten.h>

#include <map>
#include <memory>
#include <unordered_map>

#include "dnp/geom/mapfile.hpp"
#include "dnp/physics/params_table.hpp"
#include "dnp/sim/blast.hpp"
#include "dnp/sim/testmap.hpp"
#include "dnp/sim/world.hpp"

using namespace dnp;

namespace {

// Layout of the i64 buffer used by read/write_agent.
//
// Carries every fx-valued field the controller keeps between ticks, because
// reconciliation OVERWRITES local state and replays from it: anything omitted
// would survive the overwrite and diverge (docs/protocol.md 5.1).
constexpr int BUF64_POS       = 0;   // 3
constexpr int BUF64_VEL       = 3;   // 3
constexpr int BUF64_HASH      = 6;
constexpr int BUF64_DASH_T    = 7;
constexpr int BUF64_DASH_CD   = 8;
constexpr int BUF64_HOVER_F   = 9;
constexpr int BUF64_FLIGHT_F  = 10;
constexpr int BUF64_GRAP_ANCH = 11;  // 3
constexpr int BUF64_GRAP_DIST = 14;
constexpr int BUF64_DASH_DIR  = 15;  // 2
constexpr int BUF64_CLIMB_N   = 17;  // 2
constexpr int BUF64_LEN       = 19;

// Layout of the u32 buffer.
constexpr int BUF32_FACING    = 0;
constexpr int BUF32_FLAGS     = 1;
constexpr int BUF32_JUMPS     = 2;
constexpr int BUF32_JUMP_HELD = 3;
constexpr int BUF32_ACTIVE    = 4;
constexpr int BUF32_MODE      = 5;
constexpr int BUF32_DASHES    = 6;
constexpr int BUF32_SECTOR    = 7;
constexpr int BUF32_DASH_HELD = 8;
constexpr int BUF32_GRAP_HELD = 9;
constexpr int BUF32_CLIMBING  = 10;
constexpr int BUF32_GRAPPLING = 11;
constexpr int BUF32_GRAP_PULL = 12;
constexpr int BUF32_GROUNDED  = 13;
constexpr int BUF32_ANCHOR    = 14;   // sector + 1, 0 == none
constexpr int BUF32_ANCHORED  = 15;
// Appended, and the reason is worth stating once for the next field: this
// layout is mirrored by hand in client/src/core.js, so a field INSERTED here
// silently renumbers every one after it on the C++ side only, and the two
// halves disagree about which slot means "grounded".
constexpr int BUF32_INTR_HELD = 16;
constexpr int BUF32_ATK_HELD  = 17;
constexpr int BUF32_ATK_LEFT  = 18;
constexpr int BUF32_ATK_CD    = 19;
constexpr int BUF32_LEN       = 20;

// A camera path's header (design 4.1). Its OWN four slots, not appended to the
// agent layout above: a path's points need up to 3 x MAX_MAP_PATH_POINTS i64,
// which the shared scratch buffer is nowhere near big enough for, and growing
// that buffer to fit would put a rarely-read field next to the one the
// predictor touches every tick.
//
// Four slots will not over-read. What the length check exists to catch is the
// other failure: C++ swapping tag and flags while JS keeps reading the old
// order, which is a path silently addressed by the wrong number.
constexpr int PATH32_FIRST = 0;
constexpr int PATH32_COUNT = 1;
constexpr int PATH32_TAG   = 2;
constexpr int PATH32_FLAGS = 3;
constexpr int PATH32_LEN   = 4;

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
int dnp_buf64_len() { return BUF64_LEN; }

EMSCRIPTEN_KEEPALIVE
int dnp_buf32_len() { return BUF32_LEN; }

// Engine constants the client needs to DRAW correctly.
//
// Exported rather than copied into JS: the interact reach decides how far the
// debug arrow reaches, and an arrow that disagrees with the engine is worse
// than no arrow -- it would show you standing in range of a door that will not
// open. Same reason the buffer lengths are checked rather than trusted.
EMSCRIPTEN_KEEPALIVE
void dnp_interact_reach(long long* out64) { out64[0] = sim::INTERACT_REACH.raw; }

// Where a first-person body's eye is, as a fraction of its height: the camera
// sits where the core shoots from (sim::EYE_FRACTION).
EMSCRIPTEN_KEEPALIVE
void dnp_eye_fraction(long long* out64) { out64[0] = sim::EYE_FRACTION.raw; }

// The first prop slot reserved for body mirrors (sim::MIRROR_SLOTS).
//
// Exported for the same reason as the buffer lengths: the client mirrors remote
// bodies into this band, spawn_prop is what guarantees the band is empty, and a
// client that had its own copy of the number would silently overwrite real
// props the day either constant moved. It has happened once already, with the
// agent buffer length, and it presented as 212 mispredictions rather than as
// anything resembling an off-by-one.
EMSCRIPTEN_KEEPALIVE
int dnp_mirror_base() { return static_cast<int>(sim::MAX_AGENTS + sim::SPAWNABLE_PROPS); }

// Point the world at the built-in test map.
//
// Retained as a fallback and for tests. The real path is dnp_map_load() below,
// which parses the bytes the server sent -- so client and server agree on
// geometry because they parse the SAME file, not because they happen to share
// a compiled-in copy of it.
EMSCRIPTEN_KEEPALIVE
void dnp_world_use_test_map(sim::World* w) { w->set_map(&sim::test_map()); }

// --- map loading -----------------------------------------------------------
//
// The store owns the geometry; the MapData view handed to the world points into
// it, so the store must outlive the world. JS keeps both alive together.

EMSCRIPTEN_KEEPALIVE
geom::MapStorage* dnp_map_create() { return new geom::MapStorage(); }

EMSCRIPTEN_KEEPALIVE
void dnp_map_destroy(geom::MapStorage* m) { delete m; }

// Parse `len` bytes. Returns a MapLoadStatus; anything non-zero means the map
// was rejected and the store is left empty rather than half-populated.
EMSCRIPTEN_KEEPALIVE
int dnp_map_load(geom::MapStorage* m, const unsigned char* data, unsigned len) {
    ByteReader r(data, len);
    return static_cast<int>(geom::load_map(r, *m));
}

// Content hash, written to out64[0]. Compared against WELCOME.map_hash: a
// client running different geometry from the server is otherwise
// indistinguishable from a physics bug.
EMSCRIPTEN_KEEPALIVE
void dnp_map_hash(const geom::MapStorage* m, long long* out64) {
    out64[0] = static_cast<long long>(m->hash());
}

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_sector_count(const geom::MapStorage* m) { return m->view().sector_count; }

// The body proportions the map declares, into out64[0..1] as raw fx.
//
// Read from the MAP by both sides rather than replicated to the client, because
// both sides already have the map and it is the map's own statement about
// itself. PARAM_UPDATE still carries radius and height like any other rule, so
// this makes replication a CONFIRMATION rather than the mechanism -- and closes
// the window at the start of a session where the client is predicting with a
// body the server has not corrected yet.
EMSCRIPTEN_KEEPALIVE
EMSCRIPTEN_KEEPALIVE
void dnp_map_texture_scale(const geom::MapStorage* m, long long* out64) {
    out64[0] = m->texture_scale().raw;
}

EMSCRIPTEN_KEEPALIVE
void dnp_map_body(const geom::MapStorage* m, long long* out64) {
    out64[0] = m->body_radius().raw;
    out64[1] = m->body_height().raw;
}

// --- reading the map out, for the renderer ---------------------------------
//
// Per-element accessors filling the caller's scratch buffers, rather than
// handing JS a pointer into the store and a struct layout to mirror. A pointer
// would be faster and would break the first time a field was added or the
// compiler padded differently -- silently, as a wall in the wrong place. These
// run once per map load, so the cost is a few thousand calls and never a frame.
//
// This is also the RENDER BOUNDARY, and the one place fx values are allowed to
// stop being exact. They leave as raw Q32.32 in an i64 buffer; the caller
// converts to double for the GPU. The simulation never reads back what the
// renderer did with them.

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_point_count(const geom::MapStorage* m) { return m->view().point_count; }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_side_count(const geom::MapStorage* m) { return m->view().side_count; }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_tri_index_count(const geom::MapStorage* m) { return m->view().tri_index_count; }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_texture_count(const geom::MapStorage* m) { return m->texture_count(); }

// Which sector contains a point, using the SAME exact predicate the simulation
// does. The renderer needs it to decide whether a shadow lands on a platform or
// out over the void, and re-implementing point-in-sector in JS to answer a
// cosmetic question would put a second, approximate copy of an exact predicate
// in the codebase -- the kind that agrees until it does not.
EMSCRIPTEN_KEEPALIVE
int dnp_map_find_sector(const geom::MapStorage* m, const long long* xy64) {
    return geom::find_sector(m->view(), fx2{fx::from_raw(xy64[0]), fx::from_raw(xy64[1])});
}

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_light_count(const geom::MapStorage* m) { return m->light_count(); }

EMSCRIPTEN_KEEPALIVE
void dnp_map_light(const geom::MapStorage* m, unsigned i, long long* out64, unsigned* out32) {
    if (i >= m->light_count()) { out32[0] = 0; return; }
    const geom::MapLight& l = m->light(i);
    out64[0] = l.pos.x.raw;
    out64[1] = l.pos.y.raw;
    out64[2] = l.pos.z.raw;
    out64[3] = l.radius.raw;
    out64[4] = l.intensity.raw;
    out32[0] = l.r;
    out32[1] = l.g;
    out32[2] = l.b;
}

// --- sector effects (docs/SECTOR_FX.md) ---------------------------------------
//
// The kinds, keys and defaults are core's table (dnp/geom/sector_fx.hpp),
// handed over here rather than copied into JS: one definition, so a key added
// to the format is a key the client can read.

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_fx_count(const geom::MapStorage* m) { return m->effect_count(); }

// out32: sector, kind.
EMSCRIPTEN_KEEPALIVE
void dnp_map_fx(const geom::MapStorage* m, unsigned i, unsigned* out32) {
    if (i >= m->effect_count()) { out32[0] = 0; out32[1] = 0; return; }
    out32[0] = m->effect(i).sector;
    out32[1] = m->effect(i).kind;
}

// One key of one effect: 1 and the authored value when it is set, else 0 and
// the kind's default. A value is an fx raw -- the default is in thousandths and
// is converted here, so the caller sees one unit -- except a colour's, which is
// 0xRRGGBB either way.
EMSCRIPTEN_KEEPALIVE
int dnp_map_fx_param(const geom::MapStorage* m, unsigned i, unsigned key, long long* out64) {
    out64[0] = 0;
    if (i >= m->effect_count() || key < 1 || key > geom::FX_KEY_COUNT) return 0;
    const geom::SectorFx& e = m->effect(i);
    for (unsigned k = 0; k < e.count; ++k) {
        if (e.params[k].key == key) { out64[0] = e.params[k].raw; return 1; }
    }
    const geom::FxKeyDef& d = geom::FX_KEYS[key - 1];
    const long long def = (e.kind >= 1 && e.kind <= geom::FX_KIND_COUNT) ? d.defaults[e.kind - 1] : 0;
    out64[0] = d.is_color ? def : fx::from_frac(def, 1000).raw;
    return 0;
}

// A kind's default for a key, as the map loader falls back to it: what a
// runtime effect (SECTOR_SET) fills in for the settings it does not name.
EMSCRIPTEN_KEEPALIVE
void dnp_fx_default(unsigned kind, unsigned key, long long* out64) {
    out64[0] = 0;
    if (kind < 1 || kind > geom::FX_KIND_COUNT || key < 1 || key > geom::FX_KEY_COUNT) return;
    const geom::FxKeyDef& d = geom::FX_KEYS[key - 1];
    const long long def = d.defaults[kind - 1];
    out64[0] = d.is_color ? def : fx::from_frac(def, 1000).raw;
}

EMSCRIPTEN_KEEPALIVE
unsigned dnp_fx_key_count() { return geom::FX_KEY_COUNT; }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_fx_key_name(unsigned key, char* out, unsigned cap) {
    if (key < 1 || key > geom::FX_KEY_COUNT) return 0;
    const char* s = geom::FX_KEYS[key - 1].name;
    unsigned n = 0;
    while (s[n] != 0 && n < cap) { out[n] = s[n]; ++n; }
    return n;
}

EMSCRIPTEN_KEEPALIVE
int dnp_fx_key_is_color(unsigned key) {
    return key >= 1 && key <= geom::FX_KEY_COUNT && geom::FX_KEYS[key - 1].is_color;
}

EMSCRIPTEN_KEEPALIVE
int dnp_fx_takes(unsigned kind, unsigned key) {
    return geom::fx_takes(static_cast<u8>(kind), static_cast<u8>(key));
}

EMSCRIPTEN_KEEPALIVE
void dnp_map_point(const geom::MapStorage* m, unsigned i, long long* out64) {
    const geom::MapData v = m->view();
    if (i >= v.point_count) { out64[0] = 0; out64[1] = 0; return; }
    out64[0] = v.point(i).x.raw;
    out64[1] = v.point(i).y.raw;
}

// Geometry and rendering in ONE call, deliberately -- they are indexed the same
// way and a caller that fetched them separately could pair sector 3's shape
// with sector 4's light. The two views stay separate in C++, where physics can
// be denied one of them; there is no such thing to protect on this side of the
// boundary, where everything is for drawing.
EMSCRIPTEN_KEEPALIVE
void dnp_map_sector(const geom::MapStorage* m, unsigned i, long long* out64, unsigned* out32) {
    const geom::MapData v = m->view();
    if (i >= v.sector_count) { out32[0] = 0; return; }
    const geom::Sector& s = v.sector(i);
    const geom::SectorRender& r = m->render_view().sector(i);
    out64[0] = s.floor_z.raw;
    out64[1] = s.ceil_z.raw;
    out64[2] = s.thickness.raw;
    out32[0] = s.first_point;
    out32[1] = s.point_count;
    out32[2] = s.first_side;
    out32[3] = s.side_count;
    out32[4] = s.tag;
    out32[5] = s.first_tri;
    out32[6] = s.tri_count;
    out32[7] = r.floor_tex;
    out32[8] = r.ceil_tex;
    out32[9] = r.light_level;
    // HOW EACH FLAT IS PAINTED. Appended, both flats together, for the reason
    // the agent layout above states at length: this is mirrored by hand in
    // client/src/mapdata.js, so a slot INSERTED here renumbers everything after
    // it on one side only.
    out64[3] = r.floor.off_x.raw;
    out64[4] = r.floor.off_y.raw;
    out64[5] = r.floor.scale.raw;
    out64[6] = r.ceil.off_x.raw;
    out64[7] = r.ceil.off_y.raw;
    out64[8] = r.ceil.scale.raw;
    out32[10] = r.floor.angle;
    out32[11] = r.floor.alpha;
    out32[12] = r.ceil.angle;
    out32[13] = r.ceil.alpha;
    out32[14] = r.floor.anchor;
    out32[15] = r.ceil.anchor;
}

EMSCRIPTEN_KEEPALIVE
void dnp_map_side(const geom::MapStorage* m, unsigned i, long long* out64, unsigned* out32) {
    const geom::MapData v = m->view();
    if (i >= v.side_count) { out32[0] = 0; return; }
    const geom::Side& sd = v.side(i);
    const geom::SideRender& r = m->render_view().side(i);
    out64[0] = r.x_offset.raw;
    out64[1] = r.y_offset.raw;
    out32[0] = sd.a;
    out32[1] = sd.b;
    // neighbor is i32 and may be -1; reinterpreted rather than cast so the
    // caller reads it back with the same two's-complement bits it left with.
    out32[2] = static_cast<unsigned>(sd.neighbor);
    out32[3] = sd.flags;
    out32[4] = r.upper;
    out32[5] = r.middle;
    out32[6] = r.lower;
    out32[7] = r.peg_flags;
    out32[8] = r.alpha;
    // The wall's texture angle and scale (map version 15).
    out32[9] = r.angle;
    out64[2] = r.scale.raw;
}

// Bulk, because a per-index call for twelve thousand indices is the one place
// the accessor-per-element choice above would actually cost something.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_tris(const geom::MapStorage* m, unsigned short* out, unsigned cap) {
    const geom::MapData v = m->view();
    const unsigned n = v.tri_index_count < cap ? v.tri_index_count : cap;
    for (unsigned i = 0; i < n; ++i) out[i] = v.tris[i];
    return n;
}

// Writes the name and returns its length, or 0. Not NUL-terminated: the stored
// form is NUL-PADDED, and a name that fills the field has no terminator to
// find.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_texture_name(const geom::MapStorage* m, unsigned i, char* out, unsigned cap) {
    if (i >= m->texture_count()) return 0;
    const geom::TextureName& t = m->texture(i);
    const unsigned n = t.len < cap ? t.len : cap;
    for (unsigned k = 0; k < n; ++k) out[k] = t.name[k];
    return n;
}

// The weapon table each world predicts with (dnp_world_set_weapons), released
// with the world.
static std::map<const sim::World*, std::unique_ptr<sim::WeaponTable>>& weapon_tables() {
    static std::map<const sim::World*, std::unique_ptr<sim::WeaponTable>> t;
    return t;
}

// Attach a loaded map to a world. The view is stored out of line because
// World keeps a pointer to it.
// ONE VIEW PER WORLD, kept for as long as the world lives. It used to be a ring
// of eight shared by everybody, which was fine while a world's map never
// changed; a region's store changes whenever a level is added or dropped, and
// set_map is called again each time -- eight of those and the ring handed this
// world's slot to the next caller. Node-based, so a reference stays valid.
static std::unordered_map<sim::World*, geom::MapData>& world_views() {
    static std::unordered_map<sim::World*, geom::MapData> views;
    return views;
}

EMSCRIPTEN_KEEPALIVE
void dnp_world_set_map(sim::World* w, geom::MapStorage* m) {
    geom::MapData& v = world_views()[w];
    v = m->view();
    w->set_map(&v);
    // Mutable storage too: sector motion writes moved geometry back, and
    // without it every sector on the client would be immovable while the
    // server's moved -- which reads as the rider sinking through the floor.
    w->set_map_storage(m);
}

// --- sector motion ---------------------------------------------------------
//
// The client learns motions from SECTOR_MOTION_SET rather than being configured
// to match the server by hand. That distinction is the whole point: two sides
// that happen to be set up identically would predict correctly today and
// silently diverge the moment a map changed only one of them.

// A sector's NAME (geom::MapStorage::sector_name) and back. The wire carries
// names, because the server's store and this one may hold different maps.
EMSCRIPTEN_KEEPALIVE
int dnp_world_sector_by_name(sim::World* w, int name) { return w->sector_by_name(name); }
EMSCRIPTEN_KEEPALIVE
int dnp_world_sector_name(sim::World* w, int sector) { return w->sector_name(sector); }

// The motion-slot capacity, so the client compares against the core's number
// rather than a copy of it.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_max_moving_sectors() { return sim::MAX_MOVING_SECTORS; }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_add_motion(sim::World* w, geom::MapStorage* m, unsigned sector) {
    return w->motions().add(*m, sector);
}

EMSCRIPTEN_KEEPALIVE
int dnp_world_start_parametric(sim::World* w, unsigned sector, const long long* from64,
                               const long long* to64, unsigned start_tick,
                               unsigned duration_ticks, unsigned loop, unsigned target,
                               const long long* spin64) {
    const fx3 from{fx::from_raw(from64[0]), fx::from_raw(from64[1]), fx::from_raw(from64[2])};
    const fx3 to{fx::from_raw(to64[0]), fx::from_raw(to64[1]), fx::from_raw(to64[2])};
    // `target` decides whether the path moves the floor, the ceiling or both.
    // Dropping it would put a crusher's floor where its ceiling belongs and
    // mispredict every entity standing in the sector.
    //
    // `spin64` is {pivot.x, pivot.y, spin_from, spin_to} and may be null for a
    // motion that only travels -- which is every motion this engine had before
    // turntables, and is what keeps a lift's path one buffer write.
    const fx2 pivot = spin64 ? fx2{fx::from_raw(spin64[0]), fx::from_raw(spin64[1])} : fx2{};
    const long long sf = spin64 ? spin64[2] : 0;
    const long long st = spin64 ? spin64[3] : 0;
    return w->motions().start_parametric(sector, from, to, start_tick, duration_ticks,
                                         static_cast<sim::LoopMode>(loop),
                                         static_cast<sim::MotionTarget>(target),
                                         pivot, sf, st) ? 1 : 0;
}

// Where a moving sector currently IS, beyond its heights: the offset it has
// travelled and the angle it has turned through.
//
// The renderer needs both, and not to draw the polygon -- the points it reads
// are already live -- but to paint a flat that is ANCHORED to its sector. A
// pattern tied to the sector has to be evaluated in the sector's own frame, and
// that frame is exactly "undo the offset, undo the turn".
//
// Returns 0 when the sector has no motion, which is almost every sector, so the
// caller can skip the work rather than subtract zeros.
EMSCRIPTEN_KEEPALIVE
int dnp_world_sector_motion(const sim::World* w, unsigned sector,
                            long long* out64, unsigned* out32) {
    const i32 mi = w->motions().find(sector);
    if (mi < 0) return 0;
    const sim::SectorMotion& m = w->motions().at(static_cast<u32>(mi));
    out64[0] = m.offset.x.raw;
    out64[1] = m.offset.y.raw;
    out64[2] = m.pivot.x.raw;
    out64[3] = m.pivot.y.raw;
    out32[0] = m.angle;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int dnp_world_set_discrete(sim::World* w, unsigned sector, const long long* off64,
                           unsigned target) {
    const fx3 off{fx::from_raw(off64[0]), fx::from_raw(off64[1]), fx::from_raw(off64[2])};
    return w->motions().set_discrete(sector, off, static_cast<sim::MotionTarget>(target)) ? 1 : 0;
}

// Spawn position for a tag, written to out64[0..2]. Falls back to the first
// spawn and then the origin, never failing -- a player who cannot land
// anywhere is worse than one who lands in the wrong place (design 3.3).
EMSCRIPTEN_KEEPALIVE
void dnp_map_spawn_for_tag(const geom::MapStorage* m, const char* tag, unsigned len,
                           long long* out64) {
    const fx3 p = m->spawn_for_tag(tag, len);
    out64[0] = p.x.raw;
    out64[1] = p.y.raw;
    out64[2] = p.z.raw;
}

EMSCRIPTEN_KEEPALIVE
sim::World* dnp_world_create() { return new sim::World(); }

EMSCRIPTEN_KEEPALIVE
void dnp_world_destroy(sim::World* w) {
    world_views().erase(w);
    weapon_tables().erase(w);
    delete w;
}

// A REGION's store on the client: the levels it holds, each at its cell's
// origin, joined where they meet (geom::MapStorage::add_chunk). The world the
// client predicts in is then the same world the server simulates.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_add_chunk(geom::MapStorage* dst, const geom::MapStorage* src, unsigned key,
                           const long long* origin64) {
    return dst->add_chunk(key, *src, fx2{fx::from_raw(origin64[0]), fx::from_raw(origin64[1])});
}
EMSCRIPTEN_KEEPALIVE
int dnp_map_remove_chunk(geom::MapStorage* dst, unsigned key) { return dst->remove_chunk(key) ? 1 : 0; }
// A chunk's sector and side ranges: out32 = {sector first, count, side first,
// count}. 0 when it is not resident.
EMSCRIPTEN_KEEPALIVE
int dnp_map_chunk_range(const geom::MapStorage* m, unsigned key, unsigned* out32) {
    const geom::Chunk* c = m->find_chunk(key);
    if (!c) return 0;
    out32[0] = c->sectors.first; out32[1] = c->sectors.count;
    out32[2] = c->sides.first;   out32[3] = c->sides.count;
    return 1;
}
// Motions of sectors a removed chunk owned.
EMSCRIPTEN_KEEPALIVE
void dnp_world_forget_sectors(sim::World* w, unsigned first, unsigned count) {
    w->motions().forget_sectors(first, count);
}

EMSCRIPTEN_KEEPALIVE
void dnp_world_reset(sim::World* w) { w->reset(); }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_spawn(sim::World* w, const long long* pos64) {
    return w->spawn(fx3{fx::from_raw(pos64[0]), fx::from_raw(pos64[1]), fx::from_raw(pos64[2])});
}

EMSCRIPTEN_KEEPALIVE
void dnp_world_set_input(sim::World* w, unsigned idx, unsigned buttons, int mx, int my, unsigned aim,
                         int pitch, unsigned weapon) {
    proto::InputFrame f;
    f.buttons = static_cast<u16>(buttons);
    f.move_x  = static_cast<i16>(mx);
    f.move_y  = static_cast<i16>(my);
    f.aim     = brad(aim);
    f.pitch   = static_cast<i16>(pitch);
    f.weapon  = static_cast<u8>(weapon);
    w->set_input(idx, f);
}

// --- the weapon machine (docs/WEAPON_PREDICTION.md) --------------------------
//
// The table arrives as WEAPON_DEFS and is decoded HERE, by the core's own
// decoder, into storage this file owns -- one per world, released with it. The
// seed is the table's hash, which is what the server seeds its stream with, so
// holding the table is holding everything the spread is drawn from.

// Decode a table and predict agent `agent`'s weapon with it. 1 on success; 0
// leaves the world predicting nothing, which is the honest answer to a table
// this build cannot read.
EMSCRIPTEN_KEEPALIVE
int dnp_world_set_weapons(sim::World* w, const unsigned char* data, unsigned len, unsigned agent,
                          unsigned stream) {
    auto t = std::make_unique<sim::WeaponTable>();
    ByteReader r(data, len);
    if (!sim::decode_weapon_table(r, *t)) {
        w->predict_weapon(nullptr, 0, sim::MAX_AGENTS, sim::MAX_AGENTS);
        weapon_tables().erase(w);
        return 0;
    }
    const u64 seed = t->hash();
    w->predict_weapon(t.get(), seed, agent, stream);
    weapon_tables()[w] = std::move(t);
    return 1;
}

// Which agent's weapon is predicted, once the table is held, and which of the
// server's agents it is (whose random stream it draws from): after a respawn or
// a move into another world, both are different.
EMSCRIPTEN_KEEPALIVE
void dnp_world_predict_weapon_agent(sim::World* w, unsigned agent, unsigned stream) {
    auto it = weapon_tables().find(w);
    if (it == weapon_tables().end()) return;
    w->predict_weapon(it->second.get(), it->second->hash(), agent, stream);
}

EMSCRIPTEN_KEEPALIVE
int dnp_world_has_weapons(const sim::World* w) { return weapon_tables().count(w) ? 1 : 0; }

// A body's animation state: for this client's own, what its PREDICTED weapon
// set (a phase's `anim`, `set_anim`) -- what the first-person view model plays
// (docs/VIEWMODEL_PHASES.md), the tick it happens rather than a round trip late.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_animation_state(const sim::World* w, unsigned idx) {
    return idx < w->capacity() ? w->agent(idx).animation_state : 0;
}

// The weapon state as 32-bit words, in this order; the length is asked for,
// not assumed, as the agent buffers' are.
constexpr unsigned WEAPON_BUF32_LEN = 6 + 4 + sim::MAX_WEAPON_VARS + sim::MAX_AMMO_POOLS + sim::MAX_WEAPON_COOLDOWNS;

EMSCRIPTEN_KEEPALIVE
unsigned dnp_weapon_buf32_len() { return WEAPON_BUF32_LEN; }

EMSCRIPTEN_KEEPALIVE
void dnp_world_read_weapon(const sim::World* w, unsigned idx, unsigned* out32) {
    const sim::WeaponState& s = w->weapon(idx);
    out32[0] = s.have;
    out32[1] = s.current; out32[2] = s.phase; out32[3] = s.armed;
    out32[4] = s.swing_phase; out32[5] = s.prev_buttons;
    out32[6] = s.entered; out32[7] = s.done;
    out32[8] = s.held_since[0]; out32[9] = s.held_since[1];
    for (u32 i = 0; i < sim::MAX_WEAPON_VARS; ++i) out32[10 + i] = static_cast<unsigned>(s.vars[i]);
    for (u32 i = 0; i < sim::MAX_AMMO_POOLS; ++i) out32[10 + sim::MAX_WEAPON_VARS + i] = s.ammo[i];
    for (u32 i = 0; i < sim::MAX_WEAPON_COOLDOWNS; ++i)
        out32[10 + sim::MAX_WEAPON_VARS + sim::MAX_AMMO_POOLS + i] = static_cast<unsigned>(s.cool[i]);
}

// Adopt the server's (reconciliation step 2, as dnp_world_write_agent).
EMSCRIPTEN_KEEPALIVE
void dnp_world_write_weapon(sim::World* w, unsigned idx, const unsigned* in32) {
    sim::WeaponState& s = w->weapon_mut(idx);
    s.have = in32[0];
    s.current = static_cast<u8>(in32[1]); s.phase = static_cast<u8>(in32[2]);
    s.armed = static_cast<u8>(in32[3]); s.swing_phase = static_cast<u8>(in32[4]);
    s.prev_buttons = static_cast<u8>(in32[5]);
    s.entered = in32[6]; s.done = in32[7];
    s.held_since[0] = in32[8]; s.held_since[1] = in32[9];
    for (u32 i = 0; i < sim::MAX_WEAPON_VARS; ++i) s.vars[i] = static_cast<i32>(in32[10 + i]);
    for (u32 i = 0; i < sim::MAX_AMMO_POOLS; ++i) s.ammo[i] = static_cast<u16>(in32[10 + sim::MAX_WEAPON_VARS + i]);
    for (u32 i = 0; i < sim::MAX_WEAPON_COOLDOWNS; ++i)
        s.cool[i] = static_cast<i32>(in32[10 + sim::MAX_WEAPON_VARS + sim::MAX_AMMO_POOLS + i]);
}

// The weapon held in display slot `n` (1-based: the n-th held weapon by the
// table's `order`), or 255 for none -- what a number key asks for.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_weapon_slot(const sim::World* w, unsigned idx, unsigned n) {
    auto it = weapon_tables().find(w);
    if (it == weapon_tables().end() || n == 0) return sim::NO_WEAPON;
    const sim::WeaponTable& t = *it->second;
    const u32 have = w->weapon(idx).have;
    // Stable: order first, then the index, which is the name's order.
    u8 picked[sim::MAX_WEAPONS];
    u32 count = 0;
    for (u8 k = 0; k < t.weapon_count; ++k) if ((have >> k) & 1u) picked[count++] = k;
    for (u32 a = 1; a < count; ++a) {
        for (u32 b = a; b > 0 && t.weapons[picked[b]].order < t.weapons[picked[b - 1]].order; --b) {
            const u8 x = picked[b]; picked[b] = picked[b - 1]; picked[b - 1] = x;
        }
    }
    return n <= count ? picked[n - 1] : sim::NO_WEAPON;
}

// What the predicted weapon asked the world for on the last step, six words
// each: action, weapon, phase, step, the sound (its wire hash, for a `sound`
// step) and whether that sound is the client's own to play (`predicted`). For
// hearing and feeling one's own shot at once; at most `cap`.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_weapon_events(const sim::World* w, unsigned* out32, unsigned cap) {
    const sim::WeaponEventLog& log = w->weapon_events();
    auto it = weapon_tables().find(w);
    unsigned n = 0;
    for (u32 i = 0; i < log.count() && n < cap; ++i, ++n) {
        const sim::WeaponEvent& e = log.at(i);
        u32 sound = 0, predicted = 0;
        if (it != weapon_tables().end() && e.weapon < it->second->weapon_count) {
            const sim::WeaponDef& d = it->second->weapons[e.weapon];
            if (e.phase < d.phase_count && e.step < d.phases[e.phase].step_count) {
                const sim::WStep& st = d.phases[e.phase].steps[e.step];
                sound = st.sound;
                predicted = st.predicted ? 1u : 0u;
            }
        }
        out32[n * 6 + 0] = static_cast<unsigned>(e.action);
        out32[n * 6 + 1] = e.weapon;
        out32[n * 6 + 2] = e.phase;
        out32[n * 6 + 3] = e.step;
        out32[n * 6 + 4] = sound;
        out32[n * 6 + 5] = predicted;
    }
    return n;
}

// The missiles the predicted weapon launched on the last step, where the
// server's launch() puts them (World::weapon_launch): per launch six i64 --
// position xyz, heading xy, slope -- and four words -- weapon, phase, step and
// the heading as a brad. At most `cap`.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_weapon_launches(const sim::World* w, long long* out64, unsigned* out32, unsigned cap) {
    const u32 n = w->weapon_launch_count() < cap ? w->weapon_launch_count() : cap;
    for (u32 k = 0; k < n; ++k) {
        const sim::World::WeaponLaunch& l = w->weapon_launch(k);
        out64[k * 6 + 0] = l.pos.x.raw; out64[k * 6 + 1] = l.pos.y.raw; out64[k * 6 + 2] = l.pos.z.raw;
        out64[k * 6 + 3] = l.dir.x.raw; out64[k * 6 + 4] = l.dir.y.raw; out64[k * 6 + 5] = l.slope.raw;
        out32[k * 4 + 0] = l.weapon; out32[k * 4 + 1] = l.phase; out32[k * 4 + 2] = l.step;
        out32[k * 4 + 3] = l.aim.raw;
    }
    return n;
}

// The missile a launch step fires (sim::WMissile), for flying it: its kind
// (returned; 0 for one nobody described), and six i64 -- speed, radius,
// z_offset, offset, and the two impact effects' radii -- and six words --
// life, burst state, burst ticks, effect count, the two effect kinds. Then its
// blast: three more i64 (radius, knockback, lift) and a seventh word, bit 0
// "it shoves its owner", bit 1 "it falls off", bit 2 "it has a blast".
EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_weapon_missile(const sim::World* w, unsigned weapon, unsigned phase, unsigned step,
                                  long long* out64, unsigned* out32) {
    auto it = weapon_tables().find(w);
    if (it == weapon_tables().end()) return 0;
    const sim::WeaponTable& t = *it->second;
    if (weapon >= t.weapon_count || phase >= t.weapons[weapon].phase_count ||
        step >= t.weapons[weapon].phases[phase].step_count) return 0;
    const sim::WMissile& m = t.weapons[weapon].phases[phase].steps[step].missile;
    out64[0] = m.speed.raw; out64[1] = m.radius.raw; out64[2] = m.z_offset.raw; out64[3] = m.offset.raw;
    out64[4] = m.vfx_radius[0].raw; out64[5] = m.vfx_radius[1].raw;
    out32[0] = m.life; out32[1] = m.burst_anim; out32[2] = m.burst_ticks; out32[3] = m.vfx_count;
    out32[4] = m.vfx_kind[0]; out32[5] = m.vfx_kind[1];
    out64[6] = m.blast_radius.raw; out64[7] = m.blast_knockback.raw; out64[8] = m.blast_lift.raw;
    out32[6] = (m.self_shove ? 1u : 0u) | (m.blast_falloff ? 2u : 0u) | (m.has_blast ? 4u : 0u);
    return m.kind;
}

// How heavy a monster is, by catalogue kind (WeaponTable::mass_of): 0 when
// the table names none, or there is no table.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_mob_mass(const sim::World* w, unsigned kind) {
    auto it = weapon_tables().find(w);
    if (it == weapon_tables().end()) return 0;
    return it->second->mass_of(static_cast<u16>(kind));
}

// A blast's shove of a body (sim/blast.hpp), the rule the server's damage_area
// runs: in, the centre and the body's position (xyz each), radius, knockback
// and lift as i64, and the body's mass; out, the impulse. Returns whether the
// body is in reach at all -- out of it, there is no shove.
EMSCRIPTEN_KEEPALIVE
int dnp_blast_shove(const long long* in64, int falloff, int mass, long long* out64) {
    const fx3 centre{fx::from_raw(in64[0]), fx::from_raw(in64[1]), fx::from_raw(in64[2])};
    const fx3 at{fx::from_raw(in64[3]), fx::from_raw(in64[4]), fx::from_raw(in64[5])};
    const sim::BlastReach r = sim::blast_reach(centre, at, fx::from_raw(in64[6]), falloff != 0);
    if (!r.reached) return 0;
    const fx3 v = sim::knock_impulse(centre, at, fx::from_raw(in64[7]), fx::from_raw(in64[8]),
                                     sim::knock_scale(r, mass));
    out64[0] = v.x.raw; out64[1] = v.y.raw; out64[2] = v.z.raw;
    return 1;
}

// WHERE A SHOVED BODY GOES: the core's own controller (physics::
// step_controller) run on this world's map for `ticks` steps, from `pos` with
// `vel` just added, no input, and the shove's slide -- ground acceleration and
// friction at `knock_rate` -- for the first `knock_ticks`, as mobs.lua's
// apply_hit sets them. Collides with the walls; not with other bodies. In:
// pos xyz, vel xyz, radius, height, move_speed, knock_rate as i64. Out: x, y, z
// per tick. Returns the ticks written. Display only (render/knock.js).
EMSCRIPTEN_KEEPALIVE
unsigned dnp_knock_path(const sim::World* w, const long long* in64, unsigned knock_ticks,
                        unsigned ticks, long long* out64) {
    const geom::MapData* map = w->map();
    if (map == nullptr) return 0;
    fx3 pos{fx::from_raw(in64[0]), fx::from_raw(in64[1]), fx::from_raw(in64[2])};
    fx3 vel{fx::from_raw(in64[3]), fx::from_raw(in64[4]), fx::from_raw(in64[5])};
    physics::MoveParams p;
    p.radius = fx::from_raw(in64[6]);
    p.height = fx::from_raw(in64[7]);
    if (in64[8] > 0) p.move_speed = fx::from_raw(in64[8]);
    const physics::MoveParams base = p;
    physics::MoveState st;
    st.sector = geom::find_sector_at(*map, fx2{pos.x, pos.y}, pos.z, p.height);
    st.grounded = vel.z.raw <= 0;
    const physics::MoveCommand cmd;
    for (unsigned t = 0; t < ticks; ++t) {
        if (t < knock_ticks) {
            p.ground_accel = fx::from_raw(in64[9]);
            p.friction = fx::from_raw(in64[9]);
        } else {
            p.ground_accel = base.ground_accel;
            p.friction = base.friction;
        }
        physics::step_controller(*map, p, st, cmd, pos, vel, sim::DT);
        out64[t * 3] = pos.x.raw; out64[t * 3 + 1] = pos.y.raw; out64[t * 3 + 2] = pos.z.raw;
    }
    return ticks;
}

// A velocity added to a body, as a script's add_impulse lands on the server
// (World::add_impulse): the predicted half of a shove.
EMSCRIPTEN_KEEPALIVE
void dnp_world_add_impulse(sim::World* w, unsigned agent, const long long* in64) {
    w->add_impulse(agent, fx3{fx::from_raw(in64[0]), fx::from_raw(in64[1]), fx::from_raw(in64[2])});
}

// A sphere moving through this world (World::sweep_entities): what a missile
// meets, the question the server's flight asks of its world. In: from xyz,
// heading xy (a unit vector), distance, radius, slope as i64 -- the slope at
// INT64_MIN for a flat sweep, as RAY_NO_HEIGHT. Out: the distance and
// the point as i64, then the IMPACT as dnp.sweep_hit reports it -- a tenth
// short along the normalised heading, at the hit's height -- so a missile
// flown here stops on the server's bits; hit, entity hit, entity as words.
// Returns whether it hit.
EMSCRIPTEN_KEEPALIVE
int dnp_world_sweep(const sim::World* w, const long long* in64, int ignore, long long* out64,
                    unsigned* out32) {
    const fx3 from{fx::from_raw(in64[0]), fx::from_raw(in64[1]), fx::from_raw(in64[2])};
    // Normalised, as dnp.sweep_hit does: a heading from fx_dir is unit to
    // within an ulp, and the server sweeps the normalised one.
    const fx2 dir = normalize(fx2{fx::from_raw(in64[3]), fx::from_raw(in64[4])});
    const sim::World::SweepHit h = w->sweep_entities(from, dir, fx::from_raw(in64[5]),
                                                     fx::from_raw(in64[6]), ignore, -1, -1,
                                                     fx::from_raw(in64[7]));
    out64[0] = h.distance.raw;
    out64[1] = h.point.x.raw; out64[2] = h.point.y.raw; out64[3] = h.point.z.raw;
    const fx back = fx::from_frac(1, 10);
    const fx d = h.distance > back ? h.distance - back : fx::zero();
    out64[4] = (from.x + dir.x * d).raw; out64[5] = (from.y + dir.y * d).raw; out64[6] = h.point.z.raw;
    out32[0] = h.hit ? 1u : 0u; out32[1] = h.entity_hit ? 1u : 0u; out32[2] = h.entity;
    return h.hit ? 1 : 0;
}

// The effects the predicted weapon would have been sent on the last step
// (World::weapon_vfx), as a snapshot's vfx: per entry three i64 -- the
// position -- and three words -- kind, direction, param. At most `cap`.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_weapon_vfx(const sim::World* w, long long* out64, unsigned* out32, unsigned cap) {
    const u32 n = w->weapon_vfx_count() < cap ? w->weapon_vfx_count() : cap;
    for (u32 k = 0; k < n; ++k) {
        const sim::PendingVfx& v = w->weapon_vfx(k);
        out64[k * 3 + 0] = v.pos.x.raw;
        out64[k * 3 + 1] = v.pos.y.raw;
        out64[k * 3 + 2] = v.pos.z.raw;
        out32[k * 3 + 0] = v.kind;
        out32[k * 3 + 1] = v.dir.raw;
        out32[k * 3 + 2] = v.param;
    }
    return n;
}

EMSCRIPTEN_KEEPALIVE
void dnp_world_step(sim::World* w) { w->step(); }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_tick(sim::World* w) { return w->tick(); }

// Apply one replicated movement parameter (docs/protocol.md 8.4).
//
// The value arrives as an i64 raw through a buffer rather than as a parameter,
// for the same reason dnp_axis_from_buf does: without -sWASM_BIGINT an i64
// argument is legalised into two i32s, and that is an ABI detail no caller
// should have to know.
//
// Returns 0 for an unknown field id, which is version skew rather than an
// error: a newer server may name a parameter this build does not have, and
// ignoring that one field beats refusing the whole update.
EMSCRIPTEN_KEEPALIVE
int dnp_world_set_param_raw(sim::World* w, unsigned idx, unsigned field_id,
                            const long long* raw) {
    if (idx >= w->capacity()) return 0;
    return physics::param_set_raw(w->params_mut(idx), static_cast<u16>(field_id),
                                  static_cast<i64>(raw[0])) ? 1 : 0;
}

// Read one back, so a test can assert the client actually applied what the
// server sent rather than merely not crashing.
EMSCRIPTEN_KEEPALIVE
void dnp_world_get_param_raw(const sim::World* w, unsigned idx, unsigned field_id,
                             long long* out) {
    out[0] = (idx >= w->capacity())
                 ? 0
                 : physics::param_get_raw(w->params(idx), static_cast<u16>(field_id));
}

// Wire id for a parameter name, or -1. Lets the client name fields in tests
// without hardcoding indices that are a wire contract.
EMSCRIPTEN_KEEPALIVE
int dnp_param_id_by_name(const char* name) { return physics::param_id_by_name(name); }

// How many parameters there are. Exported so a test can say "the full set"
// rather than "29" -- a literal that was correct until MoveParams grew five
// fields, and then silently described a different claim.
EMSCRIPTEN_KEEPALIVE
int dnp_param_count() { return physics::PARAM_COUNT; }

// The engine's default for one parameter: what a body has that nothing
// changed. The client keeps OTHER bodies' parameters as the fields the server
// sent (a monster's swing, another player's weapon), and every field it was not
// sent is this.
EMSCRIPTEN_KEEPALIVE
void dnp_param_default_raw(unsigned field_id, long long* out) {
    static const physics::MoveParams defaults{};
    out[0] = physics::param_get_raw(defaults, static_cast<u16>(field_id));
}

// Mirror a replicated prop into the client's own simulation.
//
// A solid prop is PHYSICS: the mover pushes bodies out of it, so a client
// without one walks through what the server stops it against and mispredicts
// every tick it is touching. Placed at the entity index the wire used, because
// that index is the thing's identity.
//
// in64: x, y, z, radius, height. The catalogue supplies the last two and the
// flags; the engine neither knows nor looks them up.
EMSCRIPTEN_KEEPALIVE
int dnp_world_place_prop(sim::World* w, unsigned entity, unsigned kind,
                         const long long* in64, unsigned flags) {
    return w->place_prop(entity, static_cast<sim::PropKind>(kind),
                         fx3{fx::from_raw(in64[0]), fx::from_raw(in64[1]), fx::from_raw(in64[2])},
                         fx::from_raw(in64[3]), fx::from_raw(in64[4]),
                         static_cast<u8>(flags)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void dnp_world_remove_prop(sim::World* w, unsigned entity) { w->despawn_prop(entity); }

// Crush and pin events from the last step (design §3.1).
//
// Exposed even though the client cannot act on them: the consequence is server
// Lua's, and Lua is not predicted. What this is for is diagnosis -- a client
// that computes a different crush set from the server has a determinism bug,
// and being able to see that directly beats inferring it from a position that
// drifts for no visible reason.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_crush_count(const sim::World* w) { return w->crushes().count(); }

EMSCRIPTEN_KEEPALIVE
void dnp_world_crush_at(const sim::World* w, unsigned i, unsigned* out32, long long* out64) {
    const sim::CrushEvent& e = w->crushes().at(i);
    out32[0] = e.entity;
    out32[1] = e.sector;
    out32[2] = static_cast<unsigned>(e.kind);
    out64[0] = e.depth.raw;
}

// Align the client's simulation clock with the server's. Required as soon as
// any sector moves -- see World::set_tick.
EMSCRIPTEN_KEEPALIVE
void dnp_world_set_tick(sim::World* w, unsigned t) { w->set_tick(t); }

EMSCRIPTEN_KEEPALIVE
void dnp_world_read_agent(sim::World* w, unsigned idx, long long* out64, unsigned* out32) {
    const sim::AgentState& a = w->agent(idx);
    const physics::MoveState& m = a.mv;

    out64[BUF64_POS + 0] = a.pos.x.raw;
    out64[BUF64_POS + 1] = a.pos.y.raw;
    out64[BUF64_POS + 2] = a.pos.z.raw;
    out64[BUF64_VEL + 0] = a.vel.x.raw;
    out64[BUF64_VEL + 1] = a.vel.y.raw;
    out64[BUF64_VEL + 2] = a.vel.z.raw;
    out64[BUF64_HASH]    = static_cast<long long>(w->agent_hash(idx));
    out64[BUF64_DASH_T]  = m.dash_timer.raw;
    out64[BUF64_DASH_CD] = m.dash_cooldown_timer.raw;
    out64[BUF64_HOVER_F] = m.hover_fuel.raw;
    out64[BUF64_FLIGHT_F] = m.flight_fuel.raw;
    out64[BUF64_GRAP_ANCH + 0] = m.grapple_anchor.x.raw;
    out64[BUF64_GRAP_ANCH + 1] = m.grapple_anchor.y.raw;
    out64[BUF64_GRAP_ANCH + 2] = m.grapple_anchor.z.raw;
    out64[BUF64_GRAP_DIST] = m.grapple_dist.raw;
    out64[BUF64_DASH_DIR + 0] = m.dash_dir.x.raw;
    out64[BUF64_DASH_DIR + 1] = m.dash_dir.y.raw;
    out64[BUF64_CLIMB_N + 0] = m.climb_normal.x.raw;
    out64[BUF64_CLIMB_N + 1] = m.climb_normal.y.raw;

    out32[BUF32_FACING]    = a.facing.raw;
    out32[BUF32_FLAGS]     = w->state_flags(idx);
    out32[BUF32_JUMPS]     = m.jumps_used;
    out32[BUF32_JUMP_HELD] = m.jump_held ? 1u : 0u;
    out32[BUF32_ACTIVE]    = a.active ? 1u : 0u;
    out32[BUF32_MODE]      = static_cast<unsigned>(m.mode);
    out32[BUF32_DASHES]    = m.dashes_used;
    out32[BUF32_SECTOR]    = static_cast<unsigned>(m.sector);
    out32[BUF32_DASH_HELD] = m.dash_held ? 1u : 0u;
    out32[BUF32_GRAP_HELD] = m.grapple_held ? 1u : 0u;
    out32[BUF32_CLIMBING]  = m.climbing ? 1u : 0u;
    out32[BUF32_GRAPPLING] = m.grappling ? 1u : 0u;
    out32[BUF32_GRAP_PULL] = m.grapple_pulling ? 1u : 0u;
    out32[BUF32_GROUNDED]  = m.grounded ? 1u : 0u;
    out32[BUF32_ANCHOR]    = m.anchor >= 0 ? static_cast<unsigned>(m.anchor) + 1u : 0u;
    out32[BUF32_ANCHORED]  = m.anchored ? 1u : 0u;
    out32[BUF32_INTR_HELD] = m.interact_held ? 1u : 0u;
    out32[BUF32_ATK_HELD]  = m.attack_held ? 1u : 0u;
    out32[BUF32_ATK_LEFT]  = m.attack_left;
    out32[BUF32_ATK_CD]    = m.attack_cooldown_left;
}

// Adopt authoritative state. This is step 2 of reconciliation
// (docs/protocol.md 10): overwrite local state with the server's, then replay.
//
// jump_held is part of what gets adopted, and that is not incidental: it is
// simulation state, not input state, and a replay that started from the wrong
// edge-detect flag would re-trigger or swallow a jump and diverge from the
// server for reasons no amount of position correction would fix.
EMSCRIPTEN_KEEPALIVE
void dnp_world_write_agent(sim::World* w, unsigned idx, const long long* in64, const unsigned* in32) {
    sim::AgentState& a = w->agent_mut(idx);
    physics::MoveState& m = a.mv;

    a.pos = fx3{fx::from_raw(in64[BUF64_POS + 0]), fx::from_raw(in64[BUF64_POS + 1]),
                fx::from_raw(in64[BUF64_POS + 2])};
    a.vel = fx3{fx::from_raw(in64[BUF64_VEL + 0]), fx::from_raw(in64[BUF64_VEL + 1]),
                fx::from_raw(in64[BUF64_VEL + 2])};
    a.facing = brad(in32[BUF32_FACING]);
    a.active = in32[BUF32_ACTIVE] != 0;

    m.dash_timer = fx::from_raw(in64[BUF64_DASH_T]);
    m.dash_cooldown_timer = fx::from_raw(in64[BUF64_DASH_CD]);
    m.hover_fuel = fx::from_raw(in64[BUF64_HOVER_F]);
    m.flight_fuel = fx::from_raw(in64[BUF64_FLIGHT_F]);
    m.grapple_anchor = fx3{fx::from_raw(in64[BUF64_GRAP_ANCH + 0]),
                           fx::from_raw(in64[BUF64_GRAP_ANCH + 1]),
                           fx::from_raw(in64[BUF64_GRAP_ANCH + 2])};
    m.grapple_dist = fx::from_raw(in64[BUF64_GRAP_DIST]);
    m.dash_dir = fx2{fx::from_raw(in64[BUF64_DASH_DIR + 0]), fx::from_raw(in64[BUF64_DASH_DIR + 1])};
    m.climb_normal = fx2{fx::from_raw(in64[BUF64_CLIMB_N + 0]), fx::from_raw(in64[BUF64_CLIMB_N + 1])};

    m.mode = static_cast<physics::ControllerMode>(in32[BUF32_MODE]);
    m.jumps_used = static_cast<u8>(in32[BUF32_JUMPS]);
    m.dashes_used = static_cast<u8>(in32[BUF32_DASHES]);
    m.sector = static_cast<i32>(in32[BUF32_SECTOR]);
    m.jump_held = in32[BUF32_JUMP_HELD] != 0;
    m.dash_held = in32[BUF32_DASH_HELD] != 0;
    m.grapple_held = in32[BUF32_GRAP_HELD] != 0;
    m.climbing = in32[BUF32_CLIMBING] != 0;
    m.grappling = in32[BUF32_GRAPPLING] != 0;
    m.grapple_pulling = in32[BUF32_GRAP_PULL] != 0;
    m.grounded = in32[BUF32_GROUNDED] != 0;
    // Adopted, not inferred. The anchor is simulation state the controller
    // carries between ticks, so a replay that started from the wrong one
    // diverges for reasons position correction cannot repair -- exactly the
    // argument docs/protocol.md §5.1 makes for the ability timers.
    m.anchor = in32[BUF32_ANCHOR] == 0
                   ? -1 : static_cast<i32>(in32[BUF32_ANCHOR]) - 1;
    m.anchored = in32[BUF32_ANCHORED] != 0;
    m.interact_held = in32[BUF32_INTR_HELD] != 0;
    m.attack_held = in32[BUF32_ATK_HELD] != 0;
    m.attack_left = static_cast<u8>(in32[BUF32_ATK_LEFT]);
    m.attack_cooldown_left = static_cast<u8>(in32[BUF32_ATK_CD]);
}

// Input quantisation, exposed so the client uses the SAME function the server
// decodes with (protocol R1). Reimplementing it in JS would be one more place
// for the two sides to disagree, and the disagreement would be one ulp wide and
// present on every single tick.
//
// Takes the value through a buffer rather than as a `long long` parameter:
// without -sWASM_BIGINT an i64 argument is legalised into two i32s, and the
// resulting call signature is an ABI detail no caller should have to know.
EMSCRIPTEN_KEEPALIVE
int dnp_axis_from_buf(const long long* p) {
    return proto::axis_from_fx(fx::from_raw(p[0]));
}

// A snapshot written by the REAL encoder, with every SelfBlock field set to a
// value of its own, for client/test/proto_self.test.js. A reader checked only
// against bytes its own test laid out agrees with itself -- which is how
// proto.js read the self block's swing counters and dash count in the wrong
// order, unnoticed, because all three are zero most of the time.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_test_snapshot(unsigned char* out, unsigned cap, unsigned options) {
    const bool grappling = (options & 1u) != 0;
    const bool armed = (options & 2u) != 0;
    proto::SnapshotHeader h;
    h.server_tick = 1001; h.baseline_tick = 0; h.last_input_tick = 999; h.input_margin = -3; h.flags = 2;
    proto::SelfBlock s;
    s.pos = fx3{fx::from_int(11), fx::from_int(12), fx::from_int(13)};
    s.vel = fx3{fx::from_int(21), fx::from_int(22), fx::from_int(23)};
    s.facing = brad(0x31000000u);
    s.controller_mode = 4;
    s.state_flags = static_cast<u16>(proto::state_flag::GROUNDED |
                                     (grappling ? proto::state_flag::GRAPPLING : 0) |
                                     (armed ? proto::state_flag::ARMED : 0));
    s.weapon.have = 0x00A5u;
    s.weapon.current = 7; s.weapon.phase = 3; s.weapon.armed = 6; s.weapon.swing_phase = 2;
    s.weapon.prev_buttons = 1;
    s.weapon.entered = 70001; s.weapon.done = 0x15;
    s.weapon.held_since[0] = 70003; s.weapon.held_since[1] = 70004;
    for (u32 i = 0; i < sim::MAX_WEAPON_VARS; ++i) s.weapon.vars[i] = -100 - static_cast<i32>(i);
    for (u32 i = 0; i < sim::MAX_AMMO_POOLS; ++i) s.weapon.ammo[i] = static_cast<u16>(200 + i);
    // Five cooldowns and three unset, so the encoder's "up to the last one set"
    // is exercised rather than the full array.
    for (u32 i = 0; i < 5; ++i) s.weapon.cool[i] = 300 + static_cast<i32>(i);
    s.anchor.raw = 0x55;
    s.jumps_used = 1;
    s.attack_left = 2;
    s.attack_cooldown_left = 3;
    s.dashes_used = 5;
    s.sector = 66;
    s.dash_timer = fx::from_int(7);
    s.dash_cooldown = fx::from_int(8);
    s.hover_fuel = fx::from_int(9);
    s.flight_fuel = fx::from_int(10);
    s.grapple_anchor = fx3{fx::from_int(41), fx::from_int(42), fx::from_int(43)};
    s.grapple_dist = fx::from_int(44);
    s.avatar = 77;
    s.animation_state = 88;
    s.param_epoch = 99;
    s.state_hash = 0x0123456789ABCDEFull;
    proto::SnapshotWriter w(out, cap);
    w.begin(h, s, 123);
    return static_cast<unsigned>(w.finish());
}


// --- building a map in memory, for the editor ------------------------------
//
// The other direction through the same door. Everything above reads a map the
// SERVER sent; this writes one the editor is drawing, into the same
// geom::MapStorage, so what the editor shows is meshed from the same
// triangulation the server will collide against.
//
// Every one of these is a MapStorage method that already existed -- see
// core/include/dnp/geom/mapstore.hpp. Nothing here decides anything; if a
// polygon is refused, it is refused by the ear clipper the engine ships, and
// the editor reports what the engine said rather than a second opinion. That
// is the entire reason the editor does not carry its own triangulator: an
// editor whose picture disagrees with the game is worse than no editor.
//
// Arguments follow the conventions established above: fx values cross as raw
// Q32.32 in caller-supplied i64 buffers, never as doubles and never as i64
// parameters (which -sWASM_BIGINT would otherwise be needed for).

EMSCRIPTEN_KEEPALIVE
void dnp_map_clear(geom::MapStorage* m) { m->clear(); }

// pts64 is x,y interleaved; z64 is {floor, ceil}. Returns the sector index, or
// MAX_MAP_SECTORS when the polygon was rejected or a capacity was hit -- the
// caller compares against dnp_map_max_sectors() rather than knowing the number.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_add_sector(geom::MapStorage* m, const long long* pts64, unsigned n,
                            const long long* z64, unsigned tag) {
    // A fixed scratch rather than an allocation, matching core's rule that a
    // count chosen elsewhere must never become an allocation size. n is bounded
    // by the same limit add_sector itself enforces.
    static fx2 pts[geom::MAX_MAP_POINTS];
    if (n > geom::MAX_MAP_POINTS) return geom::MAX_MAP_SECTORS;
    for (unsigned i = 0; i < n; ++i) {
        pts[i] = fx2{fx::from_raw(pts64[i * 2]), fx::from_raw(pts64[i * 2 + 1])};
    }
    return m->add_sector(pts, n, fx::from_raw(z64[0]), fx::from_raw(z64[1]),
                         static_cast<u16>(tag));
}

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_max_sectors() { return geom::MAX_MAP_SECTORS; }

// Did sector `i` mesh into the shape it was asked for?
//
// mapc asks this of every sector it bakes, and the editor has to ask it too or
// it will accept outlines the compiler rejects -- which is the disagreement
// this whole arrangement exists to prevent. "The clipper found an ear every
// time" is a weaker statement than "it meshed the region I meant": a loop that
// touches itself gets bridged across the pinch and filled where there is no
// floor, and nothing downstream notices until somebody walks out over it.
//
// The comparison is exact, in 128-bit integers over the same points, and a
// sector with holes is measured against outer minus holes -- which is the area
// its floor actually has. Written here rather than in JavaScript for the usual
// reason: it is the engine's own predicate, and a second approximate copy of an
// exact test is the kind that agrees until it does not.
//
// Returns 1 when they match, 0 when they do not, and 1 for a sector with no
// triangles at all -- that case is already reported by tri_count and reporting
// it twice would make the caller decide which message to believe.
EMSCRIPTEN_KEEPALIVE
int dnp_map_sector_mesh_matches(const geom::MapStorage* m, unsigned i) {
    const geom::MapData v = m->view();
    if (i >= v.sector_count) return 0;
    const geom::Sector& sec = v.sector(i);
    if (sec.tri_count == 0) return 1;

    i128 mesh = 0;
    for (u16 t = 0; t < sec.tri_count; ++t) {
        const u32 base = sec.first_tri + static_cast<u32>(t) * 3;
        const fx2 a = v.point(v.tris[base + 0]);
        const fx2 b = v.point(v.tris[base + 1]);
        const fx2 c = v.point(v.tris[base + 2]);
        mesh += (static_cast<i128>(b.x.raw) - a.x.raw) * (static_cast<i128>(c.y.raw) - a.y.raw) -
                (static_cast<i128>(b.y.raw) - a.y.raw) * (static_cast<i128>(c.x.raw) - a.x.raw);
    }

    i128 poly = geom::polygon_area_2x(&v.points[sec.first_point], sec.point_count);
    if (poly < 0) poly = -poly;
    for (u16 h = 0; h < sec.hole_count; ++h) {
        const geom::PolyLoop& L = v.hole(sec.first_hole + h);
        i128 ha = geom::polygon_area_2x(&v.points[sec.first_point + L.first], L.count);
        if (ha < 0) ha = -ha;
        poly -= ha;
    }
    return mesh == poly ? 1 : 0;
}

// How many triangles a sector meshed into. Zero is how the store reports a
// polygon it could not triangulate at all -- add_sector returns a valid index
// and leaves the mesh empty, which mapc checks separately and so must this.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_sector_tri_count(const geom::MapStorage* m, unsigned i) {
    const geom::MapData v = m->view();
    return i < v.sector_count ? v.sector(i).tri_count : 0u;
}

// Must be called before the next dnp_map_add_sector: a hole's points sit
// immediately after its sector's outer loop so the triangulator can be handed
// one base pointer. Adding out of order puts another sector's points between
// them, which is why this is a rule and not a preference.
EMSCRIPTEN_KEEPALIVE
int dnp_map_add_hole(geom::MapStorage* m, unsigned sector, const long long* pts64, unsigned n) {
    static fx2 pts[geom::MAX_MAP_POINTS];
    if (n > geom::MAX_MAP_POINTS) return 0;
    for (unsigned i = 0; i < n; ++i) {
        pts[i] = fx2{fx::from_raw(pts64[i * 2]), fx::from_raw(pts64[i * 2 + 1])};
    }
    return m->add_hole(sector, pts, n) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int dnp_map_add_side(geom::MapStorage* m, unsigned sector, unsigned local_a,
                     unsigned local_b, int neighbor, unsigned flags) {
    return m->add_side(sector, static_cast<u16>(local_a), static_cast<u16>(local_b),
                       neighbor, static_cast<u8>(flags)) ? 1 : 0;
}

// The rendering half of a sector and of a side, written after the geometry the
// way mapc writes them. Separate calls because the two halves are separate
// structures in the store, and the separation is what scripts/check_render_
// isolation.sh exists to keep -- collapsing them here would be the first step
// to handing physics a texture id.
EMSCRIPTEN_KEEPALIVE
void dnp_map_set_sector_render(geom::MapStorage* m, unsigned i, unsigned floor_tex,
                               unsigned ceil_tex, unsigned light,
                               const long long* thickness64) {
    if (i >= m->sector_count()) return;
    geom::SectorRender& r = m->sector_render_mut(i);
    r.floor_tex = static_cast<u16>(floor_tex);
    r.ceil_tex = static_cast<u16>(ceil_tex);
    r.light_level = static_cast<u8>(light);
    // Thickness is GEOMETRY -- what a body stands on, and what the renderer
    // draws as a slab edge -- so it goes to the Sector, not to SectorRender.
    // One number, which is what makes the picture and the collision agree.
    m->sector_mut(i).thickness = fx::from_raw(thickness64[0]);
}

// HOW A FLAT IS PAINTED: `which` is 0 for the floor and 1 for the ceiling, and
// off64 is {off_x, off_y, scale} in raw fx.
//
// ITS OWN CALL rather than more arguments on set_sector_render, and the reason
// is the shared scratch buffer: a caller that passed the old argument list
// would leave the new slots holding whatever the last call left there, and a
// sector would come out rotated by a number nobody wrote. A separate entry
// point is one a caller either makes or does not.
EMSCRIPTEN_KEEPALIVE
void dnp_map_set_flat_style(geom::MapStorage* m, unsigned i, unsigned which,
                            unsigned angle, unsigned alpha, unsigned anchor,
                            const long long* off64) {
    if (i >= m->sector_count()) return;
    geom::SectorRender& r = m->sector_render_mut(i);
    geom::SurfaceStyle& st = which == 0 ? r.floor : r.ceil;
    st.angle = angle;
    st.alpha = static_cast<u8>(alpha);
    st.anchor = static_cast<u8>(anchor);
    st.off_x = fx::from_raw(off64[0]);
    st.off_y = fx::from_raw(off64[1]);
    st.scale = fx::from_raw(off64[2]);
    // A scale of zero is a division, and save_map would write a file its own
    // loader refuses. The editor already refuses one at parse; this is the
    // gate that makes the refusal structural.
    if (st.scale.raw <= 0) st.scale = fx::one();
}

EMSCRIPTEN_KEEPALIVE
void dnp_map_set_side_alpha(geom::MapStorage* m, unsigned i, unsigned alpha) {
    if (i >= m->view().side_count) return;
    m->side_render_mut(i).alpha = static_cast<u8>(alpha);
}

EMSCRIPTEN_KEEPALIVE
void dnp_map_set_side_render(geom::MapStorage* m, unsigned i, unsigned upper,
                             unsigned middle, unsigned lower, unsigned peg,
                             const long long* off64) {
    if (i >= m->view().side_count) return;
    geom::SideRender& r = m->side_render_mut(i);
    r.upper = static_cast<u16>(upper);
    r.middle = static_cast<u16>(middle);
    r.lower = static_cast<u16>(lower);
    r.peg_flags = static_cast<u8>(peg);
    r.x_offset = fx::from_raw(off64[0]);
    r.y_offset = fx::from_raw(off64[1]);
}

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_intern_texture(geom::MapStorage* m, const char* name, unsigned len) {
    return m->intern_texture(name, len);
}

EMSCRIPTEN_KEEPALIVE
int dnp_map_add_spawn(geom::MapStorage* m, const char* tag, unsigned len,
                      const long long* pos64) {
    return m->add_spawn(tag, len,
                        fx3{fx::from_raw(pos64[0]), fx::from_raw(pos64[1]),
                            fx::from_raw(pos64[2])}) ? 1 : 0;
}

// in64: x, y, z, radius, intensity.
EMSCRIPTEN_KEEPALIVE
int dnp_map_add_light(geom::MapStorage* m, const long long* in64, unsigned r,
                      unsigned g, unsigned b) {
    geom::MapLight l;
    l.pos = fx3{fx::from_raw(in64[0]), fx::from_raw(in64[1]), fx::from_raw(in64[2])};
    l.radius = fx::from_raw(in64[3]);
    l.intensity = fx::from_raw(in64[4]);
    l.r = static_cast<u8>(r);
    l.g = static_cast<u8>(g);
    l.b = static_cast<u8>(b);
    return m->add_light(l) ? 1 : 0;
}

// A sector effect, for a map built in memory (the old editor's build.js) to
// hash as mapc's bake does. Checked as the loader checks one, so what the
// editor accepts is what the compiler accepts: 0 for a kind, key or colour the
// file format would refuse.
EMSCRIPTEN_KEEPALIVE
int dnp_map_add_effect(geom::MapStorage* m, unsigned sector, unsigned kind, unsigned count,
                       const unsigned* keys, const long long* vals) {
    if (sector >= m->view().sector_count || kind < 1 || kind > geom::FX_KIND_COUNT ||
        count > geom::MAX_FX_PARAMS) return 0;
    geom::SectorFx e;
    e.sector = sector;
    e.kind = static_cast<u8>(kind);
    unsigned seen = 0;
    for (unsigned k = 0; k < count; ++k) {
        const unsigned key = keys[k];
        if (!geom::fx_takes(e.kind, static_cast<u8>(key)) || ((seen >> key) & 1u)) return 0;
        seen |= 1u << key;
        if (geom::FX_KEYS[key - 1].is_color && (vals[k] < 0 || vals[k] > 0xFFFFFF)) return 0;
        e.params[e.count].key = static_cast<u8>(key);
        e.params[e.count].raw = vals[k];
        ++e.count;
    }
    return m->add_effect(e) ? 1 : 0;
}

// The scripts the map declares. Exposed even though the editor never runs one:
// they are part of the content hash, so a store built without them hashes
// differently from the same map baked by mapc -- and that comparison is how the
// editor proves it is reading the file correctly.
EMSCRIPTEN_KEEPALIVE
int dnp_map_add_script(geom::MapStorage* m, const char* name, unsigned len) {
    return m->add_script(name, len) ? 1 : 0;
}

// in64: radius, height. Assigns BOTH, as the store's setter does -- a caller
// changing one passes the other's current value, which is what
// dnp_map_body reads back.
EMSCRIPTEN_KEEPALIVE
// The scale the map's textures are drawn at, for the editor's builder. The
// getter beside dnp_map_body reads it; a map built without it would hash
// differently from the same map baked by mapc, which is what the editor's
// build test asserts and what caught this being one-way.
EMSCRIPTEN_KEEPALIVE
void dnp_map_set_texture_scale(geom::MapStorage* m, const long long* in64) {
    m->set_texture_scale(fx::from_raw(in64[0]));
}

EMSCRIPTEN_KEEPALIVE
void dnp_map_set_body(geom::MapStorage* m, const long long* in64) {
    m->set_body(fx::from_raw(in64[0]), fx::from_raw(in64[1]));
}

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_add_rail(geom::MapStorage* m, unsigned tag, unsigned flags) {
    return m->add_rail(static_cast<u16>(tag), static_cast<u8>(flags));
}

// `anchor` is the sector the waypoint rides, or -1 for a world-fixed one. The
// CALLER decides, exactly as it does for mapc: the anchor is settled once, at
// authoring time, so that a rail's anchor does not change the moment a sector
// moves under it. An editor that wants the inferred answer asks
// dnp_map_find_sector for it and passes what it got back.
EMSCRIPTEN_KEEPALIVE
int dnp_map_add_waypoint(geom::MapStorage* m, unsigned rail, const long long* pos64,
                         int anchor) {
    return m->add_waypoint(rail,
                           fx3{fx::from_raw(pos64[0]), fx::from_raw(pos64[1]),
                               fx::from_raw(pos64[2])}, anchor) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int dnp_map_add_rail_link(geom::MapStorage* m, unsigned rail, unsigned waypoint,
                          unsigned to_rail, unsigned to_waypoint) {
    return m->add_rail_link(rail, static_cast<u16>(waypoint), to_rail,
                            static_cast<u16>(to_waypoint)) ? 1 : 0;
}

// --- camera paths (design 4.1) ----------------------------------------------
//
// The only geometry in the map whose reader is the CLIENT. Rails have write
// bindings here because the editor bakes them; paths need both halves, because
// the camera has to read one every frame.
//
// What is exposed is the points and nothing else. There is no dnp_map_path_at
// or dnp_map_path_nearest, and that is the design rather than an omission: the
// camera rides a Catmull-Rom through these points, and a query answered on the
// POLYLINE would be answering about a different curve -- nearest-on-polyline
// can fall in a different segment from nearest-on-curve, and chord length is
// shorter than arc length through every bend. client/src/spline.js owns all of
// it, which also keeps that arithmetic testable without a wasm build at all.

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_add_path(geom::MapStorage* m, unsigned tag, unsigned flags,
                          const long long* curve_length64) {
    return m->add_path(static_cast<u16>(tag), static_cast<u8>(flags),
                       fx::from_raw(curve_length64[0]));
}

EMSCRIPTEN_KEEPALIVE
int dnp_map_add_path_point(geom::MapStorage* m, unsigned path, const long long* pos64,
                           int anchor) {
    return m->add_path_point(path,
                             fx3{fx::from_raw(pos64[0]), fx::from_raw(pos64[1]),
                                 fx::from_raw(pos64[2])}, anchor) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_path_count(const geom::MapStorage* m) {
    return m->view().path_count;
}

// out32: [first_point, point_count, tag, flags]; out64[0]: curve_length raw.
// Returns 1, or 0 for an index out of range, having written nothing.
EMSCRIPTEN_KEEPALIVE
int dnp_map_path_info(const geom::MapStorage* m, unsigned path,
                      unsigned* out32, long long* out64) {
    const geom::MapData v = m->view();
    if (path >= v.path_count) return 0;
    const geom::Path& p = v.path(path);
    out32[PATH32_FIRST] = p.first_point;
    out32[PATH32_COUNT] = p.point_count;
    out32[PATH32_TAG]   = p.tag;
    out32[PATH32_FLAGS] = p.flags;
    out64[0] = p.curve_length.raw;
    return 1;
}

// LIVE point positions for one path, as x,y,z triples of raw fx. `cap` is in
// TRIPLES; returns triples written, truncating rather than overrunning.
//
// Live, not authored, because sector motion rewrites these every tick -- which
// is the whole reason a camera path can be bolted to a lift. A caller that
// reads once at load and never again has a path that stops moving.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_path_points(const geom::MapStorage* m, unsigned path,
                             long long* out64, unsigned cap) {
    const geom::MapData v = m->view();
    if (path >= v.path_count) return 0;
    const geom::Path& p = v.path(path);
    const unsigned n = p.point_count < cap ? p.point_count : cap;
    for (unsigned i = 0; i < n; ++i) {
        const geom::PathPoint& q = v.path_point(p.first_point + i);
        out64[i * 3 + 0] = q.pos.x.raw;
        out64[i * 3 + 1] = q.pos.y.raw;
        out64[i * 3 + 2] = q.pos.z.raw;
    }
    return n;
}

// The anchor sector per point, or -1. For the debug OVERLAY only: it colours a
// path riding a scripted-discrete mover differently, because such a path's
// points arrive over the network at the snapshot rate and judder. The camera
// itself never asks.
EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_path_anchors(const geom::MapStorage* m, unsigned path,
                              int* out32, unsigned cap) {
    const geom::MapData v = m->view();
    if (path >= v.path_count) return 0;
    const geom::Path& p = v.path(path);
    const unsigned n = p.point_count < cap ? p.point_count : cap;
    for (unsigned i = 0; i < n; ++i) out32[i] = v.path_point(p.first_point + i).anchor;
    return n;
}

EMSCRIPTEN_KEEPALIVE
unsigned dnp_map_max_path_points() { return geom::MAX_MAP_PATH_POINTS; }

EMSCRIPTEN_KEEPALIVE
int dnp_path32_len() { return PATH32_LEN; }

// --- staged shape edits (design 3.1.1) -------------------------------------
//
// The drag-a-vertex loop, and the reason the editor does not need a rebuild per
// frame: stage the moves, commit them together, and the commit is
// TRANSACTIONAL. An edit that makes a sector unmeshable changes nothing and the
// previous triangulation still stands, so a bad drag is refused rather than
// leaving half a map behind.

EMSCRIPTEN_KEEPALIVE
int dnp_map_stage_point(geom::MapStorage* m, unsigned sector, unsigned local_index,
                        const long long* xy64) {
    return m->stage_point(sector, static_cast<u16>(local_index),
                          fx2{fx::from_raw(xy64[0]), fx::from_raw(xy64[1])}) ? 1 : 0;
}

// Returns the CommitStatus: 0 Ok, 1 NoStagedEdits, 2 BadSector,
// 3 TriangulationFailed, 4 OutOfTriangleSpace.
EMSCRIPTEN_KEEPALIVE
int dnp_map_commit_shape(geom::MapStorage* m, unsigned sector) {
    return static_cast<int>(m->commit_shape(sector));
}

EMSCRIPTEN_KEEPALIVE
int dnp_map_has_staged_edits(const geom::MapStorage* m, unsigned sector) {
    return m->has_staged_edits(sector) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void dnp_map_discard_staged_edits(geom::MapStorage* m, unsigned sector) {
    m->discard_staged_edits(sector);
}

// Rebuild the broadphase. Needed after a commit before dnp_map_find_sector is
// worth asking: a moved point marks the grid dirty, and every query falls back
// to a linear scan until this runs -- correct either way, and much slower on a
// converted Doom level.
EMSCRIPTEN_KEEPALIVE
void dnp_map_rebuild_grid(geom::MapStorage* m) { m->rebuild_grid(); }

}  // extern "C"
