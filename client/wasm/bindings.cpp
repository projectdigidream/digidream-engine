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
constexpr int BUF32_LEN       = 19;

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

}  // extern "C"
