cmake_minimum_required(VERSION 3.20)
# The engine's version: VERSION beside this file, read here so the two cannot
# differ. 0.1.0 was the first alpha; features count in the last place
# (0.1.0.1, ...), the user deciding each bump. npm takes only three parts, so
# the package.json files carry the first three.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DNP_VERSION LIMIT_COUNT 1)
project(DoomNetPlatformer VERSION ${DNP_VERSION} LANGUAGES CXX)

# NOTE: scripts/build_native.sh is the dependency-free fallback and must stay in
# sync with the flags below. It exists so the determinism gate can run before
# CMake and emsdk are installed, and so CI has a path that cannot break on a
# toolchain upgrade.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# core: the fixed simulation core. Compiled identically for the native server
# and the wasm client -- this target is the physics contract in build form, so
# it must never gain a platform-conditional source file or a dependency.
# ---------------------------------------------------------------------------
add_library(dnp_core STATIC
  core/src/math/trig.cpp
  core/src/proto/messages.cpp
  core/src/sim/world.cpp
  core/src/sim/testmap.cpp
  core/src/sim/sectormotion.cpp
  core/src/sim/weapon.cpp
  core/src/sim/crush.cpp
  core/src/geom/map.cpp
  core/src/geom/triangulate.cpp
  core/src/geom/mapstore.cpp
  core/src/geom/mapfile.cpp
  core/src/geom/rail.cpp
  core/src/physics/collide.cpp
  core/src/physics/controller.cpp
  core/src/nav/navgraph.cpp
)

target_include_directories(dnp_core PUBLIC core/include)

target_compile_options(dnp_core PRIVATE
  -Wall -Wextra -Wpedantic -Werror
  # core contains no floating point at all. These are here so that if a float
  # ever does appear, it cannot silently acquire fused-multiply-add on one
  # target and not another -- FMA changes results, and it is enabled by default
  # at -O2 on some targets and not others.
  -fno-fast-math
  -ffp-contract=off
  -fno-strict-aliasing
)

# Deny the core the ability to observe anything ambient. Anything the simulation
# reads must come from replicated state.
target_compile_definitions(dnp_core PRIVATE DNP_CORE_BUILD=1)

# ---------------------------------------------------------------------------
# server: authoritative simulation + edge link endpoint.
#
# Unlike core/, this target may use wall clock, sockets and the heap. None of
# that reaches the simulation: the tick loop only decides WHEN a tick happens,
# never what it computes.
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
  add_executable(dnp-server
    server/src/main.cpp
    server/src/platform.cpp
    server/src/net/link.cpp
    server/src/world/metamap.cpp
  )
  target_include_directories(dnp-server PRIVATE server/src)
  target_link_libraries(dnp-server PRIVATE dnp_core)
  target_compile_options(dnp-server PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_math core/tests/test_math.cpp)
target_link_libraries(test_math PRIVATE dnp_core)
add_test(NAME math COMMAND test_math)

add_executable(test_proto core/tests/test_proto.cpp)
target_link_libraries(test_proto PRIVATE dnp_core)
add_test(NAME proto COMMAND test_proto)

add_executable(test_physics core/tests/test_physics.cpp)
target_link_libraries(test_physics PRIVATE dnp_core)
add_test(NAME physics COMMAND test_physics)

add_executable(test_abilities core/tests/test_abilities.cpp)
target_link_libraries(test_abilities PRIVATE dnp_core)
add_test(NAME abilities COMMAND test_abilities)

add_executable(test_triangulate core/tests/test_triangulate.cpp)
target_link_libraries(test_triangulate PRIVATE dnp_core)
add_test(NAME triangulate COMMAND test_triangulate)

add_executable(test_mapfile core/tests/test_mapfile.cpp)
target_link_libraries(test_mapfile PRIVATE dnp_core)
add_test(NAME mapfile COMMAND test_mapfile)

add_executable(test_sectormotion core/tests/test_sectormotion.cpp)
target_link_libraries(test_sectormotion PRIVATE dnp_core)
add_test(NAME sectormotion COMMAND test_sectormotion)

add_executable(test_crush core/tests/test_crush.cpp)
target_link_libraries(test_crush PRIVATE dnp_core)
add_test(NAME crush COMMAND test_crush)

add_executable(test_nav core/tests/test_nav.cpp)
target_link_libraries(test_nav PRIVATE dnp_core)
add_test(NAME nav COMMAND test_nav)

add_executable(test_rail core/tests/test_rail.cpp)
target_link_libraries(test_rail PRIVATE dnp_core)
add_test(NAME rail COMMAND test_rail)
add_executable(test_weapons core/tests/test_weapons.cpp)
target_link_libraries(test_weapons PRIVATE dnp_core)
add_test(NAME weapons COMMAND test_weapons)

add_executable(test_path core/tests/test_path.cpp)
target_link_libraries(test_path PRIVATE dnp_core)
add_test(NAME path COMMAND test_path)

add_executable(mapc tools/mapc/main.cpp)
target_link_libraries(mapc PRIVATE dnp_core)

add_executable(golden_trace core/tests/golden_trace.cpp)
target_link_libraries(golden_trace PRIVATE dnp_core)

# The determinism gate cannot be expressed as a single-binary test: it compares
# the output of separately-built targets. It is driven by the script instead,
# and registered here so `ctest` runs it too.
add_test(NAME determinism COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_determinism.sh)
add_test(NAME no_floats   COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_no_floats.sh)
