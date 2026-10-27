//
// THE MOVEMENT COURSE (modules/movetest, docs/MOVEMENT.md), bay by bay, in the
// real core: the course's own map and module.txt, scripted inputs, and what
// each bay must do -- each with its control.
//
//   test_course <move.dnpm> <module.txt>
//
// The unit tests (test_movestates.cpp) hold each rule to a made-up room; this
// holds the module people actually play to its numbers, so a change to a rule
// that leaves the units green and the course broken -- a slide that no longer
// slides off the walkway, a glide that no longer clears the gap, a vine that no
// longer mantles -- fails here. What the course's script does (move.lua) is
// done by hand where a bay needs it: the trap's ask, the glide found.
//
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "dnp/geom/mapfile.hpp"
#include "dnp/geom/mapstore.hpp"
#include "dnp/physics/movedefs.hpp"
#include "dnp/proto/bytes.hpp"
#include "dnp/sim/world.hpp"

using namespace dnp;
using namespace dnp::sim;
using namespace dnp::physics;

static int g_checks = 0;
static int g_fails = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_fails;                                                    \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static double f(fx v) { return static_cast<double>(v.raw) / 4294967296.0; }

static std::vector<u8> slurp(const char* p) {
    std::vector<u8> b;
    FILE* fp = std::fopen(p, "rb");
    if (!fp) return b;
    int c;
    while ((c = std::fgetc(fp)) != EOF) b.push_back(static_cast<u8>(c));
    std::fclose(fp);
    return b;
}

static geom::MapStorage store;
static geom::MapData view;
static World w;
static MoveDefs defs;

// The course's states, by index in module.txt.
enum : u8 { DEFAULT, SLIDE, ICE, MUD, WADE, SWIM, GLIDE, CLING, STUNNED };
// Its sectors (make_move.py).
enum : i32 { LEDGE = 6, LOOKOUT = 15 };

// A body as the module makes one: its `player` lines (max_dashes 0, climb_auto
// on) and the map's body.
static u32 spawn(double x, double y, double z) {
    w.reset();
    view = store.view();
    w.set_map(&view);
    w.set_map_storage(&store);
    w.set_move_defs(defs);
    const auto q = [](double v) { return fx::from_frac(static_cast<i64>(v * 100), 100); };
    const u32 a = w.spawn(fx3{q(x), q(y), q(z)});
    w.params_mut(a).max_dashes = 0;
    w.params_mut(a).climb_auto = true;
    w.params_mut(a).radius = fx::from_frac(1, 2);
    w.params_mut(a).height = fx::from_int(2);
    return a;
}

static const brad EAST{0u}, NORTH{1u << 30}, WEST{2u << 30};
static const i16 FULL = 32767;
namespace b = proto::button;

static proto::InputFrame in(i16 mx, i16 my, u16 buttons, brad aim) {
    proto::InputFrame i;
    i.move_x = mx;
    i.move_y = my;
    i.buttons = buttons;
    i.aim = aim;
    return i;
}

static u8 state(u32 a) { return w.agent(a).mv.move_state; }
static double speed(u32 a) { return f(length(fx2{w.agent(a).vel.x, w.agent(a).vel.y})); }
static double zof(u32 a) { return f(w.agent(a).pos.z); }

static void slide_and_ice_and_mud() {
    std::printf("course: the walkway's slide, the ice, the mud\n");
    {   // Run east, C: a slide, fast, still sliding after 2.5 s.
        const u32 a = spawn(2, 2, 0);
        u8 seen = 0;
        double top = 0;
        for (int t = 0; t < 150; ++t) {
            w.set_input(a, in(t < 50 ? FULL : 0, 0, t == 50 ? b::CROUCH : 0, EAST));
            w.step();
            if (t == 52) { seen = state(a); top = speed(a); }
        }
        std::printf("  slide: %u at %.1f u/s just after C\n", seen, top);
        CHECK(seen == SLIDE && top > 10);
    }
    {   // North onto the ice, let go: it carries. CONTROL: the walkway stops you.
        const auto carry = [&](double y0, int push) {
            const u32 a = spawn(10, y0, 0);
            double at = 0;
            u8 s = 0;
            for (int t = 0; t < push + 120; ++t) {
                w.set_input(a, in(0, t < push ? FULL : 0, 0, NORTH));
                w.step();
                if (t == push - 1) { at = f(w.agent(a).pos.y); s = state(a); }
            }
            return std::make_pair(s, f(w.agent(a).pos.y) - at);
        };
        const auto ice = carry(2, 60);
        const auto walk = carry(1, 20);
        std::printf("  ice: state %u, carried %.2f; CONTROL the walkway %.2f\n", ice.first, ice.second, walk.second);
        CHECK(ice.first == ICE && ice.second > 3);
        CHECK(walk.second < 1);
    }
    {   // Half stick into the mud: slow, and a low jump.
        const u32 a = spawn(24, 2, 0);
        double slow = 0, peak = 0;
        u8 s = 0;
        for (int t = 0; t < 150; ++t) {
            w.set_input(a, in(0, t < 120 ? FULL / 2 : 0, t == 125 ? b::JUMP : 0, NORTH));
            w.step();
            if (t == 100) { s = state(a); slow = speed(a); }
            if (t > 125) peak = std::max(peak, zof(a));
        }
        std::printf("  mud: state %u at %.1f u/s, its jump peaks at %.2f (a plain jump: 2.5)\n", s, slow, peak);
        CHECK(s == MUD && slow < 2 && peak > 0.3 && peak < 1.2);
    }
}

static void the_pool() {
    std::printf("course: the pool -- wade, swim, the air, up from a dive\n");
    {   // In over the margin, into the pit, held down: wade, swim, the air drains.
        const u32 a = spawn(40, 2, 0);
        u8 wading = 0, swimming = 0;
        double air_low = 99;
        for (int t = 0; t < 900; ++t) {
            w.set_input(a, in(0, t < 240 ? FULL / 2 : 0, t >= 300 ? b::CROUCH : 0, NORTH));
            w.step();
            if (t == 70) wading = state(a);
            if (t == 290) swimming = state(a);
            air_low = std::min(air_low, f(w.agent(a).mv.meters[1]));
        }
        std::printf("  margin %u, pit %u at z %.2f; held down 10 s, the air fell to %.2f of 10\n",
                    wading, swimming, zof(a), air_low);
        CHECK(wading == WADE && swimming == SWIM);
        CHECK(air_low < 1);                          // drains under the water to the eyes
    }
    {   // Up from the bottom on buoyancy: fast, and the surface (0.4) stops it.
        const u32 a = spawn(40, 10, 0);
        double top = -99, rise = 0;
        bool swam = true;
        for (int t = 0; t < 600; ++t) {
            w.set_input(a, in(0, 0, (t >= 120 && t < 220) ? b::CROUCH : 0, NORTH));
            w.step();
            if (t >= 220) {
                top = std::max(top, zof(a));
                rise = std::max(rise, f(w.agent(a).vel.z));
                if (state(a) != SWIM) swam = false;
            }
        }
        std::printf("  up from a dive: rose at up to %.2f u/s, feet highest %.2f, swimming throughout %d\n", rise, top, swam);
        CHECK(rise > 2);                             // CONTROL: it really came up fast
        CHECK(top < 0.4 && swam);
    }
}

static void pad_glide_chimney() {
    std::printf("course: the pad, the glide over the gap, the chimney\n");
    {   // North onto the pad: thrown up and onto the ledge.
        const u32 a = spawn(54, 2, 0);
        double peak = 0;
        for (int t = 0; t < 200; ++t) {
            w.set_input(a, in(0, t < 160 ? FULL : 0, 0, NORTH));
            w.step();
            peak = std::max(peak, zof(a));
        }
        std::printf("  pad: thrown to %.2f, now at y %.2f z %.2f (the ledge: y 9..16, z 6)\n",
                    peak, f(w.agent(a).pos.y), zof(a));
        CHECK(peak > 6 && f(w.agent(a).pos.y) > 9 && zof(a) > 5.99 && zof(a) < 6.01);
    }
    {   // Off the ledge with SPACE held: the glide clears the gap. CONTROL: a jump
        // tapped falls into it. And until it is found the glide is not had.
        const auto cross = [&](bool hold, bool found) {
            const u32 a = spawn(51, 12, 6);
            if (found) w.set_movement_state_enabled(a, GLIDE, true);   // move.lua, on the ledge
            bool glided = false;
            for (int t = 0; t < 300; ++t) {
                w.set_input(a, in(t < 240 ? FULL : 0, 0, (t == 50 || (hold && t > 50)) ? b::JUMP : 0, EAST));
                w.step();
                if (state(a) == GLIDE) glided = true;
            }
            return std::make_tuple(glided, f(w.agent(a).pos.x), zof(a));
        };
        const auto g = cross(true, true), tapped = cross(false, true), locked = cross(true, false);
        std::printf("  glide: glided %d, ended x %.1f z %.1f (the landing: x 74..90, z 2); "
                    "CONTROL tapped x %.1f z %.1f; before it is found: glided %d\n",
                    std::get<0>(g), std::get<1>(g), std::get<2>(g), std::get<1>(tapped), std::get<2>(tapped), std::get<0>(locked));
        CHECK(std::get<0>(g) && std::get<1>(g) > 74 && std::get<2>(g) > 1.99 && std::get<2>(g) < 2.01);
        CHECK(std::get<2>(tapped) < 0.01);           // in the gap
        CHECK(!std::get<0>(locked));                 // `flags off`
    }
    {   // The chimney: G held, kicking from wall to wall to the top of a pillar.
        const u32 a = spawn(97.5, 10, 0);
        int clings = 0, last = 0;
        i16 way = -FULL;
        bool on_top = false;
        for (int t = 0; t < 900 && !on_top; ++t) {
            const int now = state(a);
            if (now == CLING && last != CLING) ++clings;
            u16 btn = b::CLIMB;
            if ((w.agent(a).mv.grounded && t % 40 == 0) || (now == CLING && w.agent(a).mv.state_ticks == 8)) {
                btn |= b::JUMP;
                if (now == CLING) way = static_cast<i16>(-way);
            }
            w.set_input(a, in(way, 0, btn, way > 0 ? EAST : WEST));
            w.step();
            last = now;
            on_top = w.agent(a).mv.grounded && zof(a) > 7.9;
        }
        std::printf("  chimney: clung %d times, on top of a pillar %d\n", clings, on_top);
        CHECK(clings >= 2 && on_top);
    }
}

static void touch_trap_vine_dash() {
    std::printf("course: touch's ATK, the trap, the vine, the dash\n");
    {   // On a touch screen ATK slides off a run, and pushes down in the deep.
        const u32 a = spawn(2, 2, 0);
        u8 seen = 0;
        for (int t = 0; t < 60; ++t) {
            w.set_input(a, in(t < 50 ? FULL : 0, 0, t == 50 ? b::ATTACK_PRIMARY : 0, EAST));
            w.step();
            if (t == 52) seen = state(a);
        }
        const u32 c = spawn(40, 10, 0);
        double z0 = 0, low = 99;
        for (int t = 0; t < 200; ++t) {
            w.set_input(c, in(0, 0, t == 120 ? b::ATTACK_PRIMARY : 0, NORTH));
            w.step();
            if (t == 119) z0 = zof(c);
            if (t > 120) low = std::min(low, zof(c));
        }
        std::printf("  touch: ATK off a run: state %u; in the pit at %.2f, ATK pushed down to %.2f\n", seen, z0, low);
        CHECK(seen == SLIDE && low < z0 - 1);
    }
    {   // The trap: the script asks for `stunned`; a second and a half later, out.
        const u32 a = spawn(110, 2, 0);
        w.set_input(a, in(0, 0, 0, EAST));
        w.step();
        CHECK(w.ask_movement_state(a, STUNNED));
        w.step();
        const u8 asked = state(a);
        for (int t = 0; t < 95; ++t) w.step();
        std::printf("  trap: asked, %u; 1.6 s later %u\n", asked, state(a));
        CHECK(asked == STUNNED && state(a) == DEFAULT);
    }
    {   // North into the vines, pushing on: up ten and over onto the lookout.
        // CONTROL: without climb_auto (and no G) the same push climbs nothing.
        const auto push = [&](bool autograb) {
            const u32 a = spawn(124, 8, 0);
            w.params_mut(a).climb_auto = autograb;
            bool climbed = false;
            for (int t = 0; t < 600; ++t) {
                w.set_input(a, in(0, FULL, 0, NORTH));
                w.step();
                climbed = climbed || w.agent(a).mv.climbing;
            }
            return std::make_tuple(climbed, zof(a), w.agent(a).mv.sector, w.agent(a).mv.grounded);
        };
        const auto up = push(true), bare = push(false);
        std::printf("  vine: climbed %d, now z %.2f in sector %d grounded %d; CONTROL no climb_auto: climbed %d\n",
                    std::get<0>(up), std::get<1>(up), std::get<2>(up), std::get<3>(up), std::get<0>(bare));
        CHECK(std::get<0>(up) && std::get<2>(up) == LOOKOUT && std::get<3>(up) && std::get<1>(up) > 9.99);
        CHECK(!std::get<0>(bare) && std::get<1>(bare) < 0.01);
    }
    {   // SHIFT, every 8 ticks: dashes at 18, each costing stamina 30, until it runs short.
        // Then the mud's switch: the dash off, SHIFT does nothing.
        const u32 a = spawn(60, 2, 0);
        int dashes = 0, prev = 0;
        double fast = 0;
        for (int t = 0; t < 40; ++t) {
            w.set_input(a, in(-FULL, 0, (t % 8 == 0) ? b::DASH : 0, WEST));
            w.step();
            fast = std::max(fast, speed(a));
            const int run = w.agent(a).mv.moves[0].move;
            if (run && !prev) ++dashes;
            prev = run;
        }
        const double stamina = f(w.agent(a).mv.meters[0]);
        const u32 m = spawn(24, 8, 0);
        w.set_move_enabled(m, 0, false);               // move.lua, in the mud
        w.set_input(m, in(0, 0, b::DASH, NORTH));
        w.step();
        const bool ran_off = w.agent(m).mv.moves[0].move != 0;
        std::printf("  dash: %d dashes, top speed %.1f, stamina left %.0f; switched off: ran %d\n",
                    dashes, fast, stamina, ran_off);
        CHECK(dashes >= 2 && dashes <= 3 && fast > 17.5 && stamina < 50);
        CHECK(!ran_off);
    }
}

int main(int argc, char** argv) {
    std::printf("== the movement course (modules/movetest) ==\n");
    if (argc < 3) { std::printf("usage: test_course <move.dnpm> <module.txt>\n"); return 2; }
    const auto map = slurp(argv[1]);
    const auto txt = slurp(argv[2]);
    ByteReader r(map.data(), map.size());
    CHECK(!map.empty() && geom::load_map(r, store) == geom::MapLoadStatus::Ok);
    char err[200] = {0};
    CHECK(parse_move_defs(defs, reinterpret_cast<const char*>(txt.data()), static_cast<u32>(txt.size()), err, sizeof(err)));
    if (err[0]) std::printf("  module.txt: %s\n", err);
    CHECK(defs.state_count == 9 && defs.meter_count == 2 && defs.move_count == 5);
    if (g_fails == 0) {
        slide_and_ice_and_mud();
        the_pool();
        pad_glide_chimney();
        touch_trap_vine_dash();
    }
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
