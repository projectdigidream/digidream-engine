cmake_minimum_required(VERSION 3.20)
project(DoomNetPlatformer CXX)

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
    server/src/net/link.cpp
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

add_executable(golden_trace core/tests/golden_trace.cpp)
target_link_libraries(golden_trace PRIVATE dnp_core)

# The determinism gate cannot be expressed as a single-binary test: it compares
# the output of separately-built targets. It is driven by the script instead,
# and registered here so `ctest` runs it too.
add_test(NAME determinism COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_determinism.sh)
add_test(NAME no_floats   COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_no_floats.sh)
