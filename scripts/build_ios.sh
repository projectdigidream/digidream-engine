#!/usr/bin/env bash
#
# Builds the iOS app: the client, the studio, the server LINKED IN, and one
# module -- the same shape as scripts/build_apk.sh, with the tools Xcode ships
# and nothing downloaded.
#
#   scripts/build_ios.sh --module starter --studio --simulator
#   scripts/build_ios.sh --module starter --studio --identity "Apple Development: ..." --profile x.mobileprovision
#
# WHAT IT NEEDS. Xcode.app (the command-line tools alone have no iOS SDK),
# LuaJIT vendored by scripts/setup_luajit.sh, the client wasm from
# scripts/build_wasm.sh and, with --studio, scripts/build_mapc_wasm.sh.
#
# AND ONE THING THAT IS NOT THERE YET: dnp_server_run, the in-process entry
# point in server/src/main.cpp (server/src/embed.cpp hosts it on a thread;
# ios/dnp_bridge.h is what the shell calls). iOS spawns nothing, so the server
# must run inside the app; that change to main.cpp is deferred while the file
# is another workstream's. This script refuses, loudly, until it lands.
# docs/IOS.md.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

MODULE="starter"
MAP=""
STUDIO=0
SIM=0
IDENTITY="${DNP_IOS_IDENTITY:-}"
PROFILE="${DNP_IOS_PROFILE:-}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --module) MODULE="$2"; shift 2 ;;
    --map) MAP="$2"; shift 2 ;;
    --studio) STUDIO=1; shift ;;
    --simulator) SIM=1; shift ;;
    --identity) IDENTITY="$2"; shift 2 ;;
    --profile) PROFILE="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

MODULE_DIR="$REPO/modules/$MODULE"
# Else the engine's own (modules/starter ships with it).
[[ -d "$MODULE_DIR" ]] || MODULE_DIR="$ROOT/modules/$MODULE"
[[ -d "$MODULE_DIR" ]] || { echo "[ios] no such module: modules/$MODULE" >&2; exit 2; }
if [[ -z "$MAP" ]]; then
  MAP="$(ls "$MODULE_DIR/maps/"*.dnpm 2>/dev/null | head -1 | xargs -n1 basename 2>/dev/null || true)"
fi
[[ -n "$MAP" && -f "$MODULE_DIR/maps/$MAP" ]] || { echo "[ios] no baked map in modules/$MODULE/maps" >&2; exit 2; }

# --- the toolchain ------------------------------------------------------------
if ! xcode-select -p >/dev/null 2>&1 || [[ "$(xcode-select -p)" == */CommandLineTools ]]; then
  echo "[ios] Xcode.app is required (the command-line tools have no iOS SDK):" >&2
  echo "      install it, then: sudo xcode-select -s /Applications/Xcode.app" >&2
  exit 2
fi
SDK_NAME=$([[ $SIM -eq 1 ]] && echo iphonesimulator || echo iphoneos)
SDK="$(xcrun --sdk "$SDK_NAME" --show-sdk-path)"
TARGET=$([[ $SIM -eq 1 ]] && echo "arm64-apple-ios15.0-simulator" || echo "arm64-apple-ios15.0")
CC="$(xcrun --sdk "$SDK_NAME" -f clang)"
CXX="$(xcrun --sdk "$SDK_NAME" -f clang++)"
SWIFTC="$(xcrun --sdk "$SDK_NAME" -f swiftc)"
AR="$(xcrun --sdk "$SDK_NAME" -f ar)"
[[ -f "$ROOT/third_party/luajit/src/luajit.h" ]] || { echo "[ios] run scripts/setup_luajit.sh first" >&2; exit 2; }
[[ -f "$ROOT/client/wasm/dist/dnp_core.wasm" ]] || { echo "[ios] no wasm core -- run scripts/build_wasm.sh" >&2; exit 2; }
[[ $STUDIO -eq 0 || -f "$REPO/studio/wasm/dist/mapc.mjs" ]] || { echo "[ios] --studio needs scripts/build_mapc_wasm.sh" >&2; exit 2; }
grep -q 'int dnp_server_run' "$ROOT/server/src/main.cpp" || {
  echo "[ios] server/src/main.cpp does not export dnp_server_run yet: the in-process" >&2
  echo "      entry point (docs/IOS.md). Nothing to link the shell against until it does." >&2
  exit 2; }

OUT="$ROOT/build/ios"
STAGE="$OUT/stage"
APP="$OUT/Digidream.app"
rm -rf "$STAGE" "$APP"
mkdir -p "$OUT" "$STAGE" "$APP"
echo "[ios] sdk      $SDK ($TARGET)"

# --- 1. LuaJIT, interpreter only ----------------------------------------------
#
# A separate tree, as build_apk.sh keeps one: LuaJIT builds in-tree. The JIT
# is off (iOS forbids writable executable pages); the interpreter is what runs
# the map's Lua, which is fine for a level's worth of monsters.
LUA_SRC="$ROOT/third_party/luajit"
LUA_IOS="$OUT/luajit-$SDK_NAME"
if [[ ! -f "$LUA_IOS/src/libluajit.a" ]]; then
  echo "[ios] luajit   cross-compiling ($SDK_NAME, no JIT)"
  rm -rf "$LUA_IOS"
  cp -R "$LUA_SRC" "$LUA_IOS"
  (cd "$LUA_IOS" && git clean -xfdq 2>/dev/null || true)
  make -C "$LUA_IOS" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" amalg \
    HOST_CC="clang" \
    CROSS="$(dirname "$CC")/" \
    TARGET_SYS=iOS \
    TARGET_FLAGS="-isysroot $SDK -target $TARGET" \
    XCFLAGS="-DLUAJIT_DISABLE_JIT" >/dev/null
else
  echo "[ios] luajit   already built"
fi

# --- 2. the server, as a static library ----------------------------------------
#
# THE SOURCE LIST IS scripts/build_server.sh's, read out of it (build_apk.sh
# does the same), plus the embed entry point.
SRCS=()
while IFS= read -r f; do SRCS+=("$ROOT/$f"); done < <(
  sed -n 's|^  "\$ROOT/\(.*\.cpp\)" \\$|\1|p' "$ROOT/scripts/build_server.sh")
(( ${#SRCS[@]} > 10 )) || { echo "[ios] could not read the source list out of scripts/build_server.sh" >&2; exit 2; }
SRCS+=("$ROOT/server/src/embed.cpp")
echo "[ios] server   compiling ${#SRCS[@]} files"
OBJ="$STAGE/obj"; mkdir -p "$OBJ"
for f in "${SRCS[@]}"; do
  "$CXX" -std=c++17 -O2 -Wall -Wextra -target "$TARGET" -isysroot "$SDK" \
    -I"$ROOT/core/include" -I"$ROOT/server/src" -I"$LUA_IOS/src" \
    -c "$f" -o "$OBJ/$(echo "$f" | tr '/' '_').o"
done
"$AR" rcs "$STAGE/libdnpserver.a" "$OBJ"/*.o

# --- 3. the shell ----------------------------------------------------------------
echo "[ios] shell    compiling Shell.swift"
"$SWIFTC" -O -target "$TARGET" -sdk "$SDK" \
  -import-objc-header "$ROOT/shells/ios/dnp_bridge.h" \
  "$ROOT/shells/ios/Shell.swift" \
  "$STAGE/libdnpserver.a" "$LUA_IOS/src/libluajit.a" \
  -lc++ -framework UIKit -framework WebKit -framework Network \
  -o "$APP/Digidream"
cp "$ROOT/shells/ios/Info.plist" "$APP/Info.plist"

# --- 4. the web tree, laid out the way it is served -------------------------------
W="$APP/web"
mkdir -p "$W/vendor/three" "$W/wasm" "$W/maps"
cp -R "$ROOT/client/index.html" "$ROOT/client/src" "$W/"
cp -R "$ROOT/client/node_modules/three/build/." "$W/vendor/three/"
cp -R "$ROOT/client/wasm/." "$W/wasm/"
# THE SAME shell.js AS ANDROID, over the dnpNative shim native.js provides.
cp "$ROOT/shells/android/web/shell.js" "$W/shell.js"
cp "$ROOT/server/edge/src/framing.js" "$W/framing.js"
cp "$ROOT/shells/ios/web/native.js" "$APP/native.js"
for kind in textures sprites music sounds ui; do
  mkdir -p "$W/$kind"
  [[ -d "$ROOT/assets/$kind" ]] && cp -R "$ROOT/assets/$kind/." "$W/$kind/" 2>/dev/null || true
  [[ -d "$MODULE_DIR/assets/$kind" ]] && cp -R "$MODULE_DIR/assets/$kind/." "$W/$kind/" 2>/dev/null || true
done
for f in props.txt animations.txt; do
  : > "$W/$f"
  [[ -f "$ROOT/assets/$f" ]] && cat "$ROOT/assets/$f" >> "$W/$f"
  [[ -f "$MODULE_DIR/assets/$f" ]] && { printf '\n# --- %s ---\n' "$MODULE" >> "$W/$f"; cat "$MODULE_DIR/assets/$f" >> "$W/$f"; }
done
cp "$MODULE_DIR/maps/"*.dnpm "$W/maps/" 2>/dev/null || true
if [[ -f "$REPO/midisynth/tool/dist/midisynth.mjs" ]]; then
  mkdir -p "$W/synth" "$W/synth-content"
  cp -R "$REPO/midisynth/tool/." "$W/synth/"
  cp -R "$REPO/midisynth/content/." "$W/synth-content/"
fi
# The sound-effects engine (docs/SOUNDS.md), served at /sfx/; optional like the synth.
if [[ -f "$REPO/sound/tool/dist/dsound.mjs" ]]; then
  mkdir -p "$W/sfx"
  cp "$REPO/sound/tool/dist/dsound.mjs" "$W/sfx/"
else
  echo "[ios] NO SOUND ENGINE: sound/tool/dist/dsound.mjs is missing; no sound effects" >&2
fi
if [[ $STUDIO -eq 1 ]]; then
  mkdir -p "$W/studio/wasm/dist" "$W/studio/runtime"
  cp "$REPO/studio/index.html" "$W/studio/"
  cp -R "$REPO/studio/src" "$W/studio/src"
  cp -R "$REPO/studio/vendor" "$W/studio/vendor"
  cp "$REPO/studio/wasm/dist/mapc.mjs" "$REPO/studio/wasm/dist/mapc.wasm" "$W/studio/wasm/dist/"
  for f in mob_vocab.lua mobs.lua trigger_vocab.lua triggers.lua weapon_vocab.lua weapons.lua \
           graph_vocab.lua graphs.lua player_sounds.lua \
           trigger_vocab.json mob_vocab.json weapon_vocab.json graph_vocab.json; do
    cp "$ROOT/maps/scripts/$f" "$W/studio/runtime/"
  done
fi

# --- 5. what the SERVER reads, seeded into Documents/game at first launch ----------
G="$APP/game"
mkdir -p "$G/assets" "$G/modules/$MODULE/assets"
[[ -f "$ROOT/assets/props.txt" ]] && cp "$ROOT/assets/props.txt" "$G/assets/"
[[ -f "$ROOT/assets/animations.txt" ]] && cp "$ROOT/assets/animations.txt" "$G/assets/"
cp -R "$MODULE_DIR/." "$G/modules/$MODULE/"
find "$G" \( -name '*.map' -o -name '*.editor.json' \) -delete
cat > "$G/start.txt" <<EOF
# Written by scripts/build_ios.sh. Read by the shell at first launch.
module=$MODULE
map=$MAP
studio=$STUDIO
EOF
date -u +%Y-%m-%dT%H:%M:%SZ > "$G/stamp.txt"

# --- 6. sign ------------------------------------------------------------------------
if [[ $SIM -eq 1 ]]; then
  codesign --force --sign - "$APP" >/dev/null
  echo
  echo "[ios] $APP (simulator)"
  echo "  install:  xcrun simctl install booted \"$APP\" && xcrun simctl launch booted dev.dnp.digidream"
else
  [[ -n "$IDENTITY" && -f "$PROFILE" ]] || {
    echo "[ios] a device build needs --identity and --profile (or DNP_IOS_IDENTITY / DNP_IOS_PROFILE)" >&2; exit 2; }
  cp "$PROFILE" "$APP/embedded.mobileprovision"
  ENT="$OUT/entitlements.plist"
  security cms -D -i "$PROFILE" | plutil -extract Entitlements xml1 -o "$ENT" -
  codesign --force --sign "$IDENTITY" --entitlements "$ENT" "$APP" >/dev/null
  echo
  echo "[ios] $APP"
  echo "  install:  xcrun devicectl device install app --device <id> \"$APP\""
fi
