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

#include "dnp/geom/mapfile.hpp"
#include "dnp/physics/params_table.hpp"
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

// Attach a loaded map to a world. The view is stored out of line because
// World keeps a pointer to it.
EMSCRIPTEN_KEEPALIVE
void dnp_world_set_map(sim::World* w, geom::MapStorage* m) {
    static geom::MapData views[8];
    static int next = 0;
    geom::MapData& v = views[next++ % 8];
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

EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_add_motion(sim::World* w, geom::MapStorage* m, unsigned sector) {
    return w->motions().add(*m, sector);
}

EMSCRIPTEN_KEEPALIVE
int dnp_world_start_parametric(sim::World* w, unsigned sector, const long long* from64,
                               const long long* to64, unsigned start_tick,
                               unsigned duration_ticks, unsigned loop, unsigned target) {
    const fx3 from{fx::from_raw(from64[0]), fx::from_raw(from64[1]), fx::from_raw(from64[2])};
    const fx3 to{fx::from_raw(to64[0]), fx::from_raw(to64[1]), fx::from_raw(to64[2])};
    // `target` decides whether the path moves the floor, the ceiling or both.
    // Dropping it would put a crusher's floor where its ceiling belongs and
    // mispredict every entity standing in the sector.
    return w->motions().start_parametric(sector, from, to, start_tick, duration_ticks,
                                         static_cast<sim::LoopMode>(loop),
                                         static_cast<sim::MotionTarget>(target)) ? 1 : 0;
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
void dnp_world_destroy(sim::World* w) { delete w; }

EMSCRIPTEN_KEEPALIVE
void dnp_world_reset(sim::World* w) { w->reset(); }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_spawn(sim::World* w, const long long* pos64) {
    return w->spawn(fx3{fx::from_raw(pos64[0]), fx::from_raw(pos64[1]), fx::from_raw(pos64[2])});
}

EMSCRIPTEN_KEEPALIVE
void dnp_world_set_input(sim::World* w, unsigned idx, unsigned buttons, int mx, int my, unsigned aim) {
    proto::InputFrame f;
    f.buttons = static_cast<u16>(buttons);
    f.move_x  = static_cast<i16>(mx);
    f.move_y  = static_cast<i16>(my);
    f.aim     = brad(aim);
    w->set_input(idx, f);
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
