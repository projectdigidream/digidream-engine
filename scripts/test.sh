#!/usr/bin/env bash
#
# Runs everything CI runs, in the order that fails cheapest-first.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== 1/5  purity guards ==="
./scripts/check_no_floats.sh
# Lua belongs in the server build and nowhere else. Until Phase 4 the six core
# source lists were identical by construction and a mistake showed up as a link
# error; now they are required to differ in one specific way, which no link
# error can check for you.
./scripts/check_source_lists.sh
# Rendering attributes arrived in Phase 5. They are structurally out of
# MapData; this catches the other direction -- a physics routine handed a
# RenderData because it was convenient.
./scripts/check_render_isolation.sh
# maps/doors.map and maps/doors_trig.map are the same map with two different
# implementations of its behaviour. The comparison between them is worth nothing
# once the geometry drifts, so it is checked rather than promised.
./scripts/check_doors_twin.sh

echo
echo "=== 2/5  core unit + property tests ==="
HOST_ARCH="$(uname -m)"
./scripts/build_native.sh "$HOST_ARCH" >/dev/null
"./build/$HOST_ARCH/test_math"
"./build/$HOST_ARCH/test_proto"
"./build/$HOST_ARCH/test_physics"
"./build/$HOST_ARCH/test_abilities"
"./build/$HOST_ARCH/test_triangulate"
"./build/$HOST_ARCH/test_mapfile"
"./build/$HOST_ARCH/test_sectormotion"
"./build/$HOST_ARCH/test_crush"
"./build/$HOST_ARCH/test_interact"
"./build/$HOST_ARCH/test_props"
"./build/$HOST_ARCH/test_bodies"
"./build/$HOST_ARCH/test_sweep"
"./build/$HOST_ARCH/test_sight"
"./build/$HOST_ARCH/test_turning"
"./build/$HOST_ARCH/test_nav"
"./build/$HOST_ARCH/test_rail"
"./build/$HOST_ARCH/test_path"

# The map compiler must round-trip the authored map: mapc bakes it, and the
# loader re-triangulates and compares, so this also proves the compiler and the
# runtime have not drifted apart.
"./build/$HOST_ARCH/mapc" maps/test.map  "build/$HOST_ARCH/test.dnpm"
"./build/$HOST_ARCH/mapc" maps/crush.map  "build/$HOST_ARCH/crush.dnpm"
"./build/$HOST_ARCH/mapc" maps/phase3.map "build/$HOST_ARCH/phase3.dnpm"
# Camera paths (design §4.1). The only map that has any, and the fixture the
# client's path tests load: a static path, one on a parametric lift, a loop, a
# two-point minimum, a duplicated tag, and one on the discrete mover.
"./build/$HOST_ARCH/mapc" maps/campath.map "build/$HOST_ARCH/campath.dnpm"
# The studio's starter module, written by the studio's own emitter from its
# documents (docs/STUDIO.md): if this map stops baking, the studio's .map
# writer has drifted from mapc's grammar.
"./build/$HOST_ARCH/mapc" modules/starter/maps/first.map "build/$HOST_ARCH/starter_first.dnpm"
./scripts/check_map_scripts.sh

# Every mob table loads clean, and every deliberately broken one is refused.
# The second half is the point: a loader that accepts everything passes the
# first half perfectly.
./scripts/check_mobs.sh
"./build/$HOST_ARCH/mapc" maps/doors.map  "build/$HOST_ARCH/doors.dnpm"
# The same map, its behaviour written as triggers instead of as Doom line
# actions. Both are baked and both are driven, so the two implementations are
# compared rather than one replacing the other.
"./build/$HOST_ARCH/mapc" maps/doors_trig.map "build/$HOST_ARCH/doors_trig.dnpm"
"./build/$HOST_ARCH/mapc" maps/monsters.map "build/$HOST_ARCH/monsters.dnpm"
# The scaling fixture: 144 generated sectors and a script that fills them with
# as many monsters as it is asked for. Used by the snapshot-ordering test below
# and by tools/mobperf/run.js, which is not part of the suite because it takes
# a minute and answers a question rather than asserting one.
"./build/$HOST_ARCH/mapc" maps/mobperf.map "build/$HOST_ARCH/mobperf.dnpm"
# And the vocabulary fixture, whose monster uses the parts of the mob
# vocabulary maps/mobs/doom_monsters.lua does not. tools/mobdiff runs it
# through both mob runtimes and compares them tick by tick.
"./build/$HOST_ARCH/mapc" maps/mobvocab.map "build/$HOST_ARCH/mobvocab.dnpm"
"./build/$HOST_ARCH/mapc" maps/level_a.map "build/$HOST_ARCH/level_a.dnpm"
"./build/$HOST_ARCH/mapc" maps/level_b.map "build/$HOST_ARCH/level_b.dnpm"
# Floating platforms with open edges -- the only map here that is not a sealed
# box, and the only one where walking off an edge is a fall rather than a wall.
"./build/$HOST_ARCH/mapc" maps/void.map   "build/$HOST_ARCH/void.dnpm"

# A generated stress map: a few hundred irregular concave sectors, regenerated
# from a fixed seed so it is the same map every run. The hand-authored maps have
# a dozen sectors and most of them are boxes, which exercises neither the
# triangulator's reflex-vertex handling nor the broadphase nor a MAP_DATA
# transfer long enough to need more than one chunk.
node tools/mapgen/mapgen.js --cells 16 --seed 1 --out maps/big.map
"./build/$HOST_ARCH/mapc" maps/big.map    "build/$HOST_ARCH/big.dnpm"

# The level editor's reader, against the maps themselves.
#
# Early on purpose. editor/src/mapfmt.js is a SECOND implementation of the .map
# grammar, whose only definition is tools/mapc/main.cpp -- the trap this project
# has paid for before -- and the check is that every map in the repository, read
# and written back out, bakes to a .dnpm with the same content hash. It needs
# mapc and nothing else, so it runs here rather than behind the stack's
# preconditions where a missing certificate would silently skip it.
node editor/test/mapfmt.test.js   | tail -2
node editor/test/entities.test.js | tail -2
node editor/test/server.test.js   | tail -2
# Neighbour inference, against the whole corpus. The assertion is a negative
# one: OPENING a map must not edit it. The first version of that code re-derived
# every link after every drag, could not pair a hole's rim with the sector
# inside it, and sealed a pit in maps/doom/map01.map shut -- which surfaced much
# later as "sector 51 partially overlaps sector 26".
node editor/test/topology.test.js | tail -2
# Undo, which here is parse(emit(doc)) -- so it rides on the same fixed point
# the corpus above proves, and this adds the case that one does not have: the
# shapes the EDITOR draws, whose numbers no author ever typed.
node editor/test/history.test.js  | tail -2
# Which way round a sector goes. The renderer takes counter-clockwise as fact
# and derives both the outward normal and the direction u runs from it, so a
# sector drawn the other way has its walls facing into the void with mirrored
# textures. Physics does not care, which is why it is invisible until you look
# at the wall from the other side.
node editor/test/winding.test.js  | tail -2
# Deleting sectors, corners and pits. A sector is named by its POSITION in the
# file, so removing one renumbers every reference after it -- and a pit is two
# things, a hole and the sector standing in it, which have to go together.
node editor/test/edit.test.js     | tail -2
# The prop catalogue. assets/props.txt is one file that two programs read, so
# the check is that after a round trip through the editor the CLIENT's own
# parser sees exactly the same catalogue -- not that the text looks similar.
node editor/test/props.test.js    | tail -2
# Its VISUAL half: the sprite lines, and the frames they name. Regenerated from
# parsed fields now rather than carried as text, so every catalogue in the repo
# has to come back byte-identical -- column alignment included, or one edit
# becomes a diff across the whole file.
#
# The frame checker has an ORACLE: the loop client/src/main.js actually runs to
# build its preload set. Reading `rot8` off the LINE instead of the KIND made it
# report 128 problems on a catalogue that works, and the oracle is what says
# which of the two is wrong. What survives is real -- three projectiles whose
# impact bursts have never drawn.
node editor/test/propart.test.js  | tail -2
# What a map says about itself: scripts, body, spawns, lamps. Each has a way of
# being wrong that the map still compiles with -- a script mapc cannot find is
# printed and then baked anyway -- so the checks are about what the records
# MEAN, and each ends at mapc.
node editor/test/globals.test.js  | tail -2
# Which panel the inspector shows. A state machine rather than a DOM: the two
# menus could once only be left by pressing the button that opened them, so
# picking a tool did the thing and left the menu up, and the editor looked stuck
# when one variable was.
node editor/test/mode.test.js     | tail -2
# The mob window's file surgery. A mob file is Lua with helper functions, so the
# editor substitutes characters at a known offset rather than reading it -- and
# the control is that a write which lands one character out is refused, which
# comparing field NAMES cannot see and comparing the VALUE can.
node editor/test/mobfile.test.js  | tail -2
# Mobs AUTHORED in the editor, checked by the loader that will read them. The
# emitter has an ORACLE -- tools/mobcheck/check.lua under LuaJIT, the real one
# -- so the claim is "the loader accepts it", not "the Lua looks right". It
# caught a default graph the editor called clean and the loader refused, and an
# argument name the vocabulary does not have. Skips loudly without LuaJIT.
node editor/test/mobgraph.test.js | tail -2
# ONE LIST out of the two files that define a kind. A mob is a prop with a
# brain, so the entity window shows one row per kind and behaviour is a
# property of the row -- it used to be two lists in that window plus the
# catalogue in a panel of the other one, which made a monster three steps in
# two places. What this checks is the cases that are NOT one-to-one: a record
# with no mob (most of a catalogue), a mob that wears another record's body
# (six in maps/mobs/exercise.lua), a name defined twice (a map that does not
# load), and a mob whose body is missing (a monster spawned invisible).
#
# Every one has its control, and the load-bearing one is that with no
# catalogue fetched yet NOTHING is accused -- the window renders before that
# lands, so reading it unconditionally would flag the whole module on open.
node editor/test/entitylist.test.js | tail -2
# Where the boxes go in the AI graph. Pure arithmetic, which is the half of a
# visual editor that can be checked at all -- the claims are that the picture is
# deterministic, that it draws every edge the RUNTIME follows, and that it
# accuses exactly the states the loader would.
#
# Its oracle is maps/scripts/mobs.lua's own reachability walk, which counts
# `goto` actions and exempts pain and death. Without that the first version
# called both halves of a correct monster broken and drew no goto arrow at all.
node editor/test/graphlayout.test.js | tail -2
# Walking round a monster in the 3D preview. A rot8 sprite names only a BASE
# and the renderer picks one of eight views from the angle, so the check is that
# a full orbit reaches all eight -- if it did not, the preview would show some of
# a monster's art and silently hide the rest.
#
# It calls the CLIENT's rotationFor() and asserts they agree at every angle.
# props.js says this renderer "has now shipped two mirrored angle conventions";
# a preview with one of its own would be the third.
node editor/test/orbit.test.js | tail -2
# The URLs the editor fetches art at, against a server that has to serve them.
# The art lives in modules now, and the viewport was still using
# loadRealTextures's default of /textures -- the ENGINE's directory -- so every
# wall of every DOOM II level drew as a placeholder. Nothing reported it:
# loadRealTextures returns { loaded: 0 } instead of throwing, so the .catch()
# around that call could never fire.
#
# It starts the real server and fetches what editor/src/assets.js builds, with
# the control that the old path still 404s.
node editor/test/assets.test.js | tail -2
# Where the engine is. The editor server's root used to be fixed to its own
# parent, which meant it could only ever be tested against the real repository
# -- the browser tests build a throwaway module inside modules/ for exactly that
# reason. The decisive check here is ISOLATION: a server pointed at a scratch
# tree lists that tree's maps and none of the real ones. The same resolver is
# what a VS Code extension will read a dnp.engineRoot setting into.
node editor/test/engineroot.test.js | tail -2
# POST /api/newmap, the command palette's New map. The claim that matters is the
# oracle: what it writes, mapc BAKES -- one sector, one spawn -- so Test works
# before a wall has been drawn. Against a scratch engine through
# DNP_ENGINE_ROOT; nothing touches the real maps/. It found the route creating
# modules/nosuch/ for a module that did not exist, on its first run.
node editor/test/newmap.test.js | tail -2
# The VS Code extension, driven without VS Code: a stub host, and a REAL editor
# server underneath. Double-click a map and the claim is that the tab would have
# shown a live editor pointed at the right module -- read off the path -- and
# that deactivate() takes the server down. It found a server bug on its first
# run: IS_MAIN compared a URL to argv[1], and a symlinked script exited 0.
node editor/test/extension.test.js | tail -2
# The INSTALLABLE form: build the .vsix with the real script, unpack it where no
# tools/ is anywhere near, and drive that extension.cjs the same way. The
# in-place test cannot make this claim -- the sibling it needs is always there.
# Needs the network once for npx to fetch vsce; skips out loud without it.
node editor/test/vsix.test.js | tail -2

# The Lua runtime, built separately from the core tests on purpose: those
# compile against core sources only, which is what lets the determinism gate run
# before any third-party toolchain is installed.
if [[ -f third_party/luajit/src/libluajit.a ]]; then
  ./scripts/build_lua_tests.sh >/dev/null
  "./build/lua/test_lua_vm"
  "./build/lua/test_lua_fx"
  "./build/lua/test_lua_effects"
  # The trigger interpreter, against a world made of test doubles: dispatch,
  # validation, tick deadlines and the bounded `run`. It is also the only place
  # a syntax error in maps/scripts/triggers.lua is caught without standing up a
  # server, and it is where docs/TRIGGERS.md is checked against the vocabulary
  # it is written from.
  "./build/lua/test_lua_triggers"
  # The mob graph as C++ compiles it, walked against the tables it came from.
  # Everything the C++ mob runtime will decide rests on those arrays saying
  # what mobs.lua's `defs` said, and this walks the two together in the
  # TABLE's terms -- state names and condition names, never the indices the
  # compiler chose for itself, which would agree with themselves.
  "./build/lua/test_mob_graph" .
  # And the loop that spends it, against a real sim::World on a real room: the
  # sequence fires at the ticks it declares, `goto` stops the rest of it, the
  # six actions the loop runs never reach the sink and the rest do -- exactly
  # once, in order. The control is a run with no player in the level, where
  # nothing may advance at all.
  "./build/lua/test_mob_loop" .
  # The two mob runtimes against each other: the same scenario through each,
  # world hash and RNG state compared at every tick. On maps/mobvocab.map,
  # whose monster exists to use the parts of the vocabulary the Doom set does
  # not -- wander, strafe, chance, jitter, angle offsets -- because a
  # vocabulary entry neither runtime's fixture touches is unproven however
  # green the run. It found three real bugs the behaviour tests could not.
  node tools/mobdiff/run.js --ticks 600 --map "build/$HOST_ARCH/mobvocab.dnpm" | tail -2
  # The shooting range: a room with a player in the middle and hitscan monsters
  # all round it, asserting every one of them lands a shot. The ONLY test that
  # drives the real world bindings -- every other Lua test stubs `dnp` with a
  # table, and four bugs this phase came from a stub being more generous than
  # the engine and one from a parameter setter that never existed.
  "./build/lua/test_lua_shootrange"
  # The server hosted on a thread and restarted with a different module, the
  # way the phone shells play-test (docs/IOS.md). Present only once main.cpp
  # exports dnp_server_run; the control is a run whose stop flag is never set.
  [[ -x ./build/lua/test_server_run ]] && ./build/lua/test_server_run . | tail -2
  # The editor's trigger-table WRITER, checked by the runtime that has to read
  # it. It runs here rather than beside the other editor tests because its
  # oracle is build/lua/triggercheck, which is built one line above -- and a
  # test whose oracle is missing skips loudly rather than passing.
  node editor/test/triggerlua.test.js | tail -2
  # And where a map's triggers live: the sidecar is the source, the map's .lua
  # holds a generated block. Same oracle -- what it splices into that file has
  # to load in the real runtime, not merely look like Lua.
  node editor/test/triggerstore.test.js | tail -2
  # The trigger panel's operations. The panel itself is DOM and is covered by
  # the browser smoke test; this is everything its buttons DO -- and the last
  # assertion runs the resulting graph through the runtime, so "the editor can
  # build it" means the engine takes it.
  node editor/test/triggeredit.test.js | tail -2
  # The studio's trigger writer and its presets, through the same runtime: a
  # door, a lift, a switch, a teleport, an ambush, an exit and a crusher must
  # all load, and the control strips the staleness guards the emitter writes
  # beside every side index -- the runtime must then refuse the table.
  node studio/test/lua_emit.test.js | tail -2
  # And its monsters, through tools/mobcheck: a brain the cards wrote loads,
  # and a corpse with no death state is refused by the studio and the runtime
  # alike. The body half is read back by the client's own catalogue parser.
  node studio/test/mobs.test.js | tail -2
else
  echo "  skip lua tests (run: scripts/setup_luajit.sh)"
fi

echo
echo "=== 3/5  sanitizers (ASan + UBSan) ==="
# Not redundant with the tests above. UBSan is what catches undefined behaviour
# that currently happens to compile into the intended instruction -- shifting a
# negative value left, signed overflow -- which is invisible until a compiler
# upgrade resolves it differently on one target and silently breaks prediction.
./scripts/check_sanitizers.sh

echo
echo "=== 4/5  cross-target determinism gate ==="
./scripts/check_determinism.sh

echo
echo "=== 5/5  transport + full stack ==="
if [[ ! -d server/edge/node_modules ]]; then
  echo "  skip (run: cd server/edge && npm install)"
elif [[ ! -f server/edge/certs/cert.pem ]]; then
  echo "  skip (run: cd server/edge && npm run certs)"
else
  # The dev certificate is valid for 13 days by design; an expired one fails the
  # handshake with no useful diagnostic, so say so plainly here instead.
  if ! openssl x509 -in server/edge/certs/cert.pem -checkend 0 -noout >/dev/null 2>&1; then
    echo "  dev certificate has EXPIRED -- regenerating"
    (cd server/edge && npm run --silent certs >/dev/null)
  fi
  # ITS OWN BINARY, not the one a stack might be running.
  #
  # build_server.sh writes build/server/dnp-server by default, which is what
  # scripts/run_stack.sh executes -- and relinking a file while a process is
  # running it replaces the image underneath and kills it. Running the tests
  # while playing therefore dropped the session, and the symptom was an edge
  # logging "upstream lost" over and over, which reads as a networking fault.
  #
  # The tests find it through DNP_SERVER_BIN (server/edge/test/serverbin.js).
  export DNP_SERVER_OUT="$PWD/build/test-server"
  export DNP_SERVER_BIN="$DNP_SERVER_OUT/dnp-server"
  ./scripts/build_server.sh >/dev/null

  # The wasm core, BEFORE the tests that load a map through it.
  #
  # It used to be built further down, next to the prediction tests, and the map
  # transfer test quietly ran against whatever binary happened to be on disk. A
  # map format change then failed here with "status 2" and no hint that the
  # client was simply older than the file it was parsing -- which is the exact
  # confusion the format version exists to prevent, reintroduced by the harness.
  if command -v em++ >/dev/null 2>&1; then
    ./scripts/build_wasm.sh >/dev/null
  fi

  node client/test/snapshots.test.js
  # Pure: no wasm, no GPU. The geometry rules of a Doom-style renderer are
  # arithmetic, and arithmetic is worth asserting rather than screenshotting.
  node client/test/mapgeom.test.js | tail -2
  node client/test/camera.test.js | tail -2
  # Which animation a body should be playing. Pure, and the seam a mesh renderer
  # would later consume unchanged -- so the state machine is decided once here
  # rather than twice, once per renderer, and found to disagree.
  node client/test/animstate.test.js | tail -2
  # The camera's path curve. Pure arithmetic, and the half of the path feature
  # that has no other way to be checked: a curve that quietly returned its
  # polyline would look right in every screenshot, so most of this file is the
  # control that says it bends.
  node client/test/spline.test.js | tail -2
  # And the same curve fed the points a .map file actually holds, through the
  # wasm module the client predicts with. The claim it alone can make: the arc
  # length tools/mapc baked into the file is the one this client rides at, which
  # is what every cutscene's timing rests on.
  node client/test/paths.test.js | tail -2
  # CAMERA_SET's field ids are positional across C++ and JavaScript with no
  # shared header. Getting them out of step changes the pitch when a map asks
  # for a lead, in every level already shipped, and nothing else would notice.
  node client/test/proto_camera.test.js | tail -2
  # TOUCH, which is arithmetic and a state machine: where the stick's origin
  # is, what counts as a push, who owns which finger. The claims worth having
  # are the ones with a wrong answer you could ship -- pushing your thumb up
  # must walk you up (the DOM's y grows downward), a thumb resting on the glass
  # must not walk, a corner must not be 41% faster than a straight push, and a
  # CANCELLED pointer must stop you. That last one is the phone's version of
  # main.js clearing `held` on blur, and on a phone it is not an edge case: a
  # call, a notification, the app backgrounded mid-jump.
  #
  # It also asserts the stick and the keyboard hand the SAME world vector to
  # camera.js -- one implementation of "a push on screen is a direction in the
  # world", with a control that the comparison can fail.
  node client/test/touch.test.js | tail -2
  # THE CLIENT, LOADED IN A BROWSER. tdz.test.js above checks main.js's
  # declaration order statically and says why that was all it could do:
  # main.js "wants a DOM, a WebGL context, three.js and a live WebTransport
  # session, so it is the one file in the client with no automated coverage at
  # all -- and it is the file that changes most". This is the other half. It
  # asserts the module reached the end of its start-up and mounted what it
  # should have, with controls both ways: ?touch=0 gets the keyboard legend
  # and no thumbstick, and a fine-pointer browser with no override gets none
  # either -- so wantsTouch reads the browser rather than defaulting to yes.
  #
  # Self-gated like the editor's: it launches headless Chrome, so it skips
  # loudly without DNP_CLIENT_PAGE_TEST=1 rather than costing every run.
  node client/test/page.test.js | tail -2
  node client/test/entities.test.js | tail -2
  node client/test/lighting.test.js | tail -2
  node client/test/shadows.test.js | tail -2
  # Static, and about the one client file nothing else loads.
  node client/test/tdz.test.js | tail -2
  node client/test/props.test.js | tail -2
  # ThreeSprite sheets: where a trimmed frame sits on its canvas, how that
  # becomes a quad hanging off the feet, and which rectangle of the packed
  # image it samples. Pure arithmetic over a manifest -- no sheet, no GPU.
  node client/test/atlas.test.js | tail -2
  node client/test/swing.test.js | tail -2
  node client/test/vfx.test.js | tail -2
  # Panels: the client's decoder against real bytes from the C++ encoder.
  node client/test/ui.test.js | tail -2
  # The MUSIC message: a cue name, a fade and a moment survive the wire as the
  # server writes them, and a moment the protocol does not know is refused.
  node client/test/proto_music.test.js | tail -2
  # Animated textures, and that every sequence a map uses was fully extracted.
  node client/test/animtex.test.js | tail -2
  # The synth: instruments, a baked track, and audio that is not silence.
  # Skips itself when midisynth's wasm has not been built.
  node client/test/music.test.js | tail -2
  # And the same rules applied to the map the server actually ships, read
  # through the same wasm module the client predicts with.
  node client/test/mapmesh.test.js | tail -2
  # What the renderer does PER FRAME, in counts rather than milliseconds -- a
  # count is the same on every machine and a millisecond is whatever the laptop
  # was doing at the time. Guards the dirty check that stops the map being
  # re-derived sixty times a second once a level's first door has opened.
  node client/test/renderbudget.test.js | tail -2
  # THE STUDIO (docs/STUDIO.md). Its compiler is mapc built to wasm, and the
  # first claim is that a bake on the device is a bake on the workstation:
  # byte for byte, over the map corpus, against the native binary. Then the
  # adjacency it derives -- shared walls, split T-junctions, platforms, pits --
  # read back through the engine's own loader, with the control that an
  # unsplit T-junction names no neighbour; the side indices its trigger writer
  # relies on, checked edge by edge; and the pack, against unzip.
  if command -v em++ >/dev/null 2>&1; then
    ./scripts/build_mapc_wasm.sh >/dev/null
  fi
  node studio/test/bake.test.js | tail -2
  node studio/test/topology.test.js | tail -2
  node studio/test/mapfmt.test.js | tail -2
  node studio/test/pack.test.js | tail -2
  # A .tsprite composed by ThreeSprite's own runtime (vendored) into the atlas
  # the client draws: checked by the client's parser and by tools/tsprite's
  # project-versus-atlas comparison.
  node studio/test/sheets.test.js | tail -2
  # The brain as a graph: every way the runtime leaves a state is an arrow,
  # and the automatic arrangement places every state, initial first.
  node studio/test/graph.test.js | tail -2
  # A prop's body and picture through the client's catalogue parser; deleting
  # or renaming one follows it into every map's placements.
  node studio/test/props.test.js | tail -2
  # Camera paths: what the studio draws is what mapc bakes, and the curve
  # length it shows is the one a cutscene is timed against.
  node studio/test/paths.test.js | tail -2
  # A map's music is filed under the hash the client computes for it, and a
  # cue that names a missing track is said, not silently dropped.
  node studio/test/music.test.js | tail -2
  # modules/starter is generated from its documents; a checked-in copy that
  # the emitter would write differently is a stale one.
  node studio/tools/emit_module.mjs modules/starter --check | tail -1
  # The studio in a real browser: opt-in (DNP_STUDIO_PAGE_TEST=1 with a dev
  # server up), skipped loudly otherwise.
  node studio/test/page.test.js | tail -2
  # And the editor's in-memory map against the one mapc bakes: same content
  # hash, same per-sector triangle counts. Here rather than above because it is
  # the one editor test that loads the wasm core.
  node editor/test/build.test.js | tail -2
  # Rooms, platforms and pits: each must bake, and must bake into what the
  # format means by that word. A platform whose edges came out `wall` looks
  # completely correct and plays as a room with a thick floor.
  node editor/test/shapes.test.js | tail -2
  # The page itself, in a real browser. Opt-in: three headless launches with
  # software rasterisation is minutes of CPU for a check the pure tests mostly
  # cover, and a suite everybody waits four minutes for is one people stop
  # running. DNP_EDITOR_PAGE_TEST=1 includes it.
  node editor/test/page.test.js | tail -2
  # Does the editor leak? Asked of Chrome: a repeated stimulus, a forced GC, a
  # metrics read after each round -- a leak grows every round, churn plateaus.
  # The two stimuli are the two that leaked: a render per keystroke in the mob
  # window (+542,000 retained nodes per twenty, and dropped WebGL contexts) and
  # a full rebuild in the viewport (+450 nodes per eight opens, one <img> per
  # stranded texture). Both zero now. Opt-in with the page test above; self-
  # gated, so it says so and exits clean without DNP_EDITOR_PAGE_TEST=1.
  node editor/test/leak.test.js | tail -2

  # --- the desktop build ------------------------------------------------------
  #
  # Staging a module into a standalone app, and the app's own transport.
  #
  # The staging test carries the control that keeps the engine clean: an app
  # built from a module with no assets of its own must contain no images at all.
  # If Doom art ever finds its way back into assets/, that is where it is caught
  # -- nowhere else would notice, because a contaminated build looks perfect.
  #
  # The transport test runs the STAGED tree against a real server: the same
  # relay, framing and client modules the packaged game uses, with only
  # Electron's IPC pipe stubbed. See app/test/local_transport.test.js for why
  # that is the right seam.
  node app/test/stage.test.js | tail -2
  # The studio's writable game tree: files land under it and nowhere else --
  # seven escaping paths are the control -- and seeding the bundled modules
  # never touches one the user made.
  node app/test/studio_files.test.js | tail -2
  node app/test/local_transport.test.js 2>/dev/null | tail -2
  # And the app under a REAL Electron. Opt-in (DNP_APP_LAUNCH_TEST=1): it is a
  # window, a GPU process and a ~150 MB dependency. Worth having anyway --
  # nothing else in the suite ever loads app/main.js under Electron, and the
  # first time anybody did, it failed on its first line.
  node app/test/launch.test.js | tail -2
  (cd server/edge && node test/framing.test.js \
     && node test/relay.test.js | tail -2 \
     && node test/noupstream.test.js 2>/dev/null | tail -2 \
     && node test/ui.test.js 2>/dev/null | tail -2 \
     && node test/e2e.test.js 2>/dev/null | tail -2 \
     && node test/delta.test.js 2>/dev/null | grep -E 'FAIL|failures' \
     && node test/mapdata.test.js 2>/dev/null | tail -2 \
     && DNP_MAP="$ROOT/build/$HOST_ARCH/big.dnpm" DNP_WT_PORT=34471 DNP_LINK_PORT=34541 \
        node test/mapdata.test.js 2>/dev/null | tail -2 \
     && node test/platform.test.js 2>/dev/null | tail -2 \
     && node test/crush.test.js 2>/dev/null | tail -2 \
     && node test/phase3.test.js 2>/dev/null | tail -2 \
     && node test/params.test.js 2>/dev/null | tail -2 \
     && node test/lua.test.js 2>/dev/null | tail -2 \
     && node test/doors.test.js 2>/dev/null | tail -2 \
     && node test/triggers.test.js 2>/dev/null | tail -2 \
     && node test/attack.test.js 2>/dev/null | tail -2 \
     && node test/enemies.test.js 2>/dev/null | tail -2 \
     `# WHICH entities a full snapshot spends its budget on. A datagram holds` \
     `# about 46 and a real level has more, so the choice is the whole question,` \
     `# and docs/protocol.md 5.4 has always specified distance then staleness.` \
     `# It did not do that: it ordered by entity INDEX, which is spawn order, so` \
     `# a player was told about whatever the map placed first however far away it` \
     `# had wandered -- and 211 of 250 monsters were never sent ONCE. The control` \
     `# is in the test: restore index order and four of its seven checks fail.` \
     && node test/snapshot_order.test.js 2>/dev/null | tail -2 \
     && node test/levels.test.js 2>/dev/null | tail -2 \
     && node test/live.test.js 2>/dev/null | tail -2)

  # The control case: a client that ignores the replicated motion parameters
  # MUST mispredict. If it does not, the replication is doing nothing and the
  # test above proves nothing.
  echo
  echo "--- control: ignoring motion parameters must break prediction ---"
  (cd server/edge && DNP_IGNORE_MOTION=1 DNP_WT_PORT=34470 DNP_LINK_PORT=34540 \
     node test/platform.test.js 2>/dev/null | grep -E "CONTROL|platform:")
  (cd server/edge && DNP_IGNORE_MOTION=1 DNP_WT_PORT=34480 DNP_LINK_PORT=34550 \
     node test/crush.test.js 2>/dev/null | grep -E "CONTROL|crush:")
  # Sector motion is the map's business now: run without a script and nothing
  # moves, which is what proves the engine is not still driving it quietly.
  (cd server/edge && DNP_NO_SCRIPT=1 DNP_WT_PORT=34498 DNP_LINK_PORT=34582 \
     node test/phase3.test.js 2>/dev/null | grep -E "CONTROL|phase3:")
  # Doors: the same walk with the button never pressed must leave the door
  # shut, and with no script at all nothing may move. The first proves the door
  # is not on a timer; the second proves the engine reports rather than acts.
  (cd server/edge && DNP_NO_USE=1 DNP_WT_PORT=34493 DNP_LINK_PORT=34567 \
     node test/doors.test.js 2>/dev/null | grep -E "CONTROL|doors:")
  (cd server/edge && DNP_NO_SCRIPT=1 DNP_WT_PORT=34494 DNP_LINK_PORT=34568 \
     node test/doors.test.js 2>/dev/null | grep -E "CONTROL|doors:")
  # The same two controls against the trigger-driven twin of that map. A trigger
  # table that opened the door on a timer, or an engine still moving sectors on
  # its own, would satisfy every positive assertion in triggers.test.js.
  (cd server/edge && DNP_NO_USE=1 DNP_WT_PORT=34701 DNP_LINK_PORT=34711 \
     node test/triggers.test.js 2>/dev/null | grep -E "CONTROL|triggers:")
  (cd server/edge && DNP_NO_SCRIPT=1 DNP_WT_PORT=34702 DNP_LINK_PORT=34712 \
     node test/triggers.test.js 2>/dev/null | grep -E "CONTROL|triggers:")
  # Monsters chase a player. With nobody connected there is nothing to chase, so
  # nothing may advance and nothing may shoot -- otherwise "the monster moved"
  # is satisfied by anything that drifts.
  (cd server/edge && DNP_ENEMY_IDLE=1 DNP_WT_PORT=34487 DNP_LINK_PORT=34587 \
     node test/enemies.test.js 2>/dev/null | grep -E "CONTROL|enemies:")
  # A panel is a script's doing. With the script left out nothing may open one,
  # or "a panel arrived" is satisfied by the engine opening it itself.
  (cd server/edge && DNP_NO_UI_SCRIPT=1 DNP_WT_PORT=34650 DNP_LINK_PORT=34651 \
     node test/ui.test.js 2>/dev/null | grep -E "CONTROL|ui:")
  # A solid prop that stops you, and a swing that removes it: without the swing
  # the same walk must end in the same place, or `solid` is doing nothing.
  (cd server/edge && DNP_NO_ATTACK=1 DNP_WT_PORT=34499 DNP_LINK_PORT=34573 \
     node test/attack.test.js 2>/dev/null | grep -E "CONTROL|attack:")
  # A map change must be a move to the NEXT level, not any reset at all: with
  # only one level loaded the exit has nowhere to go and must move nobody.
  (cd server/edge && DNP_ONE_LEVEL=1 DNP_WT_PORT=34497 DNP_LINK_PORT=34571 \
     node test/levels.test.js 2>/dev/null | grep -E "CONTROL|levels:")
  # And the other half of §3.1.2: a client that discards the STREAMED offsets
  # has nothing for anchor-relative prediction to be relative to.
  (cd server/edge && DNP_IGNORE_OFFSETS=1 DNP_WT_PORT=34486 DNP_LINK_PORT=34556 \
     node test/phase3.test.js 2>/dev/null | grep -E "CONTROL|phase3:")
  # And the rules themselves: a client that discards PARAM_UPDATE runs different
  # physics from the server and must be caught saying so.
  (cd server/edge && DNP_IGNORE_PARAMS=1 DNP_WT_PORT=34488 DNP_LINK_PORT=34558 \
     node test/params.test.js 2>/dev/null | grep -E "CONTROL|params:")
  # Scripted changes: one control proves the flag is not set unconditionally,
  # the other proves a client that ignores it reports a defect.
  (cd server/edge && DNP_NO_SCRIPT=1 DNP_WT_PORT=34492 DNP_LINK_PORT=34572 \
     node test/lua.test.js 2>/dev/null | grep -E "CONTROL|lua:")
  (cd server/edge && DNP_IGNORE_LUA=1 DNP_WT_PORT=34494 DNP_LINK_PORT=34574 \
     node test/lua.test.js 2>/dev/null | grep -E "CONTROL|lua:")
  # A scripted change to the RULES rather than the state: it travels on the
  # reliable channel with an epoch, not as a snapshot flag.
  (cd server/edge && DNP_EFFECTS=1 DNP_WT_PORT=34496 DNP_LINK_PORT=34578 \
     node test/lua.test.js 2>/dev/null | tail -2)

  echo
  echo "--- client prediction against the real server ---"
  if command -v em++ >/dev/null 2>&1; then
    (cd server/edge && node test/prediction.test.js 2>/dev/null | tail -4)

    echo
    echo "--- prediction under simulated network conditions ---"
    node tools/netsim/matrix.js --quick 2>/dev/null | tail -8
  else
    echo "  skip (emsdk not active -- source emsdk/emsdk_env.sh)"
    echo "  NOTE: this is the test that verifies client and server agree bit-for-bit."
  fi
fi

echo
echo "all checks passed"
