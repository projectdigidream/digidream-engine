#!/usr/bin/env bash
#
# One module, one APK: the engine, a level and the server, on a phone.
#
#   ./scripts/build_apk.sh                 # starter, its first map, debug-signed
#   ./scripts/build_apk.sh --module mygame --map level2.dnpm
#   ./scripts/build_apk.sh --module mygame --log   # writes Download/Digidream/mygame-log.txt
#
# WHAT IS IN IT. The same client the browser loads, the same authoritative
# server the desktop runs -- cross-compiled for arm64 -- and one module's maps
# and art. The game talks to itself over a loopback socket inside the app, so
# the APK asks for no permissions at all, INTERNET included. Nothing it does
# leaves the device because it has no way to.
#
# NO GRADLE, NO ANDROIDX, NO DOWNLOADS. aapt2, javac, d8 and apksigner ship
# with the SDK and are all this needs, which keeps the build the same shape as
# every other build script here: a shell script you can read, with no wrapper
# fetching a toolchain of its own. The one thing AndroidX would have given us
# is WebViewAssetLoader, which is forty lines in MainActivity.
#
# WHAT YOU NEED:
#   ANDROID_HOME or ~/Library/Android/sdk   SDK with build-tools and an NDK
#   a JDK (17 is what this was written against)
#   scripts/build_wasm.sh already run       the client's core is wasm
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

# The starter module unless one is named: the one module every checkout has.
MODULE="starter"
MAP=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --module) MODULE="$2"; shift 2 ;;
    --map) MAP="$2"; shift 2 ;;
    # The level editor in the same app (docs/STUDIO.md).
    # Its plugins come with it (docs/PLUGINS.md).
    --studio) STUDIO=1; shift ;;
    # The suite, the studio's fork with the plugins, became the studio (0.2.0).
    --suite) STUDIO=1; shift ;;
    # Without the packager's kits inside (packager/scripts/build_kits.sh, whose
    # kits must not carry kits).
    --no-kits) NO_KITS=1; shift ;;
    # The studio without its bundled plugins (as build_app.sh --no-plugins).
    --no-plugins) NO_PLUGINS=1; NO_KITS=1; shift ;;
    # The Digidream logo as the launcher icon (branding/icons.mjs), for the
    # studio's own APK. Without it, the engine's plain diamond -- what kits and
    # games keep unless the packager is given an icon.
    --icon) ICON=1; shift ;;
    # A log file the player can open without adb: the server's output and the
    # game page's console, in Download/Digidream/<module>-log.txt (GameLog.java).
    --log) LOG=1; shift ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

STUDIO="${STUDIO:-0}"
NO_KITS="${NO_KITS:-0}"
ICON="${ICON:-0}"
MODULE_DIR="$REPO/modules/$MODULE"
# Else the engine's own (modules/starter ships with it).
[[ -d "$MODULE_DIR" ]] || MODULE_DIR="$ROOT/modules/$MODULE"
[[ -d "$MODULE_DIR" ]] || { echo "[apk] no such module: modules/$MODULE" >&2; exit 2; }
[[ $STUDIO -eq 0 || -f "$REPO/studio/wasm/dist/mapc.mjs" ]] || {
  echo "[apk] --studio needs the map compiler as wasm: run scripts/build_mapc_wasm.sh" >&2; exit 2; }
# No --map: the module's first baked map, in name order -- which is the level
# the server starts a progression on anyway.
if [[ -z "$MAP" ]]; then
  MAP="$(ls "$MODULE_DIR/maps/"*.dnpm 2>/dev/null | sort | head -1 | xargs -n1 basename 2>/dev/null || true)"
fi
[[ -n "$MAP" && -f "$MODULE_DIR/maps/$MAP" ]] || {
  echo "[apk] modules/$MODULE/maps/$MAP is not there. Baked maps in that module:" >&2
  ls "$MODULE_DIR/maps/"*.dnpm 2>/dev/null | xargs -n1 basename >&2 || true
  exit 2; }

# --- the toolchain ------------------------------------------------------------

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
[[ -d "$SDK" ]] || { echo "[apk] no Android SDK at $SDK (set ANDROID_HOME)" >&2; exit 2; }

pick_newest () { ls "$1" 2>/dev/null | sort -V | tail -1; }
BT_VER="${DNP_BUILD_TOOLS:-$(pick_newest "$SDK/build-tools")}"
BT="$SDK/build-tools/$BT_VER"
NDK_VER="${DNP_NDK:-$(pick_newest "$SDK/ndk")}"
NDK="$SDK/ndk/$NDK_VER"
# `android-34` rather than the newest: the newest platform is the one whose
# `aapt2` has opinions about attributes that do not exist on a phone anybody
# owns yet, and nothing here needs an API newer than this.
PLATFORM_JAR=""
for p in android-34 android-35 android-36 $(pick_newest "$SDK/platforms"); do
  [[ -f "$SDK/platforms/$p/android.jar" ]] && { PLATFORM_JAR="$SDK/platforms/$p/android.jar"; break; }
done

for t in "$BT/aapt2" "$BT/d8" "$BT/apksigner" "$BT/zipalign"; do
  [[ -x "$t" ]] || { echo "[apk] missing $t" >&2; exit 2; }
done
[[ -n "$PLATFORM_JAR" ]] || { echo "[apk] no android.jar under $SDK/platforms" >&2; exit 2; }
command -v javac >/dev/null || { echo "[apk] no javac on PATH" >&2; exit 2; }

# The NDK ships one prebuilt per host OS, and on Apple Silicon it is still the
# directory called darwin-x86_64 -- the binaries in it are universal. Looked up
# rather than assumed so this says something useful on Linux.
TC="$(ls -d "$NDK"/toolchains/llvm/prebuilt/* 2>/dev/null | head -1)"
[[ -d "$TC" ]] || { echo "[apk] no NDK toolchain under $NDK" >&2; exit 2; }
# API 26 IS THE FLOOR, and it is set by one function. server/src/main.cpp has
# to call mallopt() to turn off Android's heap pointer tagging -- LuaJIT cannot
# see past the tag, and without it there is no scripting at all -- and mallopt
# entered the NDK's libc stubs at 26. Android 8, which is older than any phone
# with a WebView that runs this client anyway.
API=26
CC="$TC/bin/aarch64-linux-android$API-clang"
CXX="$TC/bin/aarch64-linux-android$API-clang++"
[[ -x "$CXX" ]] || { echo "[apk] no $CXX" >&2; exit 2; }

OUT="$ROOT/build/android"
STAGE="$OUT/stage"
rm -rf "$STAGE"
mkdir -p "$OUT" "$STAGE/assets/web" "$STAGE/assets/game" "$STAGE/lib/arm64-v8a" "$STAGE/classes"

echo "[apk] sdk      $SDK  (build-tools $BT_VER, ndk $NDK_VER)"

# --- 1. LuaJIT, for the phone -------------------------------------------------
#
# A SEPARATE TREE from third_party/luajit, which holds the host build the
# desktop server links. LuaJIT builds in-tree, so sharing the directory would
# mean each build silently replacing the other's objects -- and the failure
# would be a host binary with arm64 objects in it, which does not link and does
# not say why.
LUA_SRC="$ROOT/third_party/luajit"
LUA_AND="$OUT/luajit"
[[ -d "$LUA_SRC/src" ]] || { echo "[apk] run scripts/setup_luajit.sh first" >&2; exit 2; }

if [[ ! -f "$LUA_AND/src/libluajit.a" ]]; then
  echo "[apk] luajit   cross-compiling for arm64"
  rm -rf "$LUA_AND"
  mkdir -p "$(dirname "$LUA_AND")"
  cp -R "$LUA_SRC" "$LUA_AND"
  (cd "$LUA_AND" && git clean -xfdq 2>/dev/null || true)
  # HOST_CC builds the bytecode generator that runs HERE; the CROSS toolchain
  # builds what runs on the phone. Both are 64-bit, so HOST_CC needs no -m32 --
  # which is the flag every LuaJIT cross-compiling recipe on the internet has
  # and which fails on a Mac that has no 32-bit toolchain at all.
  make -C "$LUA_AND" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" amalg \
    HOST_CC="cc" \
    CROSS="$TC/bin/llvm-" \
    STATIC_CC="$CC" \
    DYNAMIC_CC="$CC -fPIC" \
    TARGET_LD="$CC" \
    TARGET_AR="$TC/bin/llvm-ar rcus" \
    TARGET_STRIP="$TC/bin/llvm-strip" \
    TARGET_SYS=Linux >/dev/null
else
  echo "[apk] luajit   already built"
fi

# --- 2. the server, for the phone ---------------------------------------------
#
# THE SOURCE LIST IS scripts/build_server.sh's, read out of it rather than
# copied. Two hand-maintained lists of the same twenty files is how one of them
# goes stale, and the failure would be an APK whose server is missing whatever
# was added last -- which presents as a level that does not load.
echo "[apk] server   cross-compiling for arm64"
# `read` in a loop rather than `mapfile`: macOS ships bash 3.2, which has no
# mapfile at all, and this script has to run on the machine the engine is
# developed on.
SRCS=()
while IFS= read -r f; do SRCS+=("$f"); done < <(
  sed -n 's|^  "\$ROOT/\(.*\.cpp\)" \\$|\1|p' "$ROOT/scripts/build_server.sh")
(( ${#SRCS[@]} > 10 )) || {
  echo "[apk] could not read the source list out of scripts/build_server.sh" >&2
  echo "      (found ${#SRCS[@]} files; its shape must have changed)" >&2
  exit 2; }

"$CXX" -std=c++17 -O2 -Wall -Wextra \
  -I "$ROOT/core/include" -I "$ROOT/server/src" -I "$LUA_AND/src" \
  "${SRCS[@]}" "$LUA_AND/src/libluajit.a" \
  -lm -ldl -static-libstdc++ \
  -o "$STAGE/lib/arm64-v8a/libdnpserver.so"
"$TC/bin/llvm-strip" "$STAGE/lib/arm64-v8a/libdnpserver.so"
echo "[apk] server   $(du -h "$STAGE/lib/arm64-v8a/libdnpserver.so" | cut -f1)"

# --- 3. the client, laid out the way it is served -----------------------------
#
# The URLs are tools/devserver.js's, because they are the client's: main.js
# fetches /props.txt, /textures/NAME.png, /maps/x.dnpm. Serving the same names
# from the APK is what lets the SAME client run in all three shells, and it is
# why this stages a tree rather than teaching the client where it is.
W="$STAGE/assets/web"
echo "[apk] client   staging"
cp -R "$ROOT/client/index.html" "$ROOT/client/src" "$W/"
mkdir -p "$W/vendor/three" "$W/wasm" "$W/maps"
cp -R "$ROOT/client/node_modules/three/build/." "$W/vendor/three/" 2>/dev/null || {
  echo "[apk] three.js is not installed (client/node_modules/three)" >&2; exit 2; }
cp -R "$ROOT/client/wasm/." "$W/wasm/"
[[ -f "$W/wasm/dist/dnp_core.wasm" ]] || {
  echo "[apk] no wasm core -- run scripts/build_wasm.sh" >&2; exit 2; }

# THE SHELL'S HOOK. client/shell.js is empty on purpose and says why; this is
# the file it makes room for. Staged over it rather than edited into the page,
# because the page is the browser's and stays the browser's.
cp "$ROOT/shells/android/web/shell.js" "$W/shell.js"
# The link framing, COPIED from the edge rather than reimplemented -- exactly
# as scripts/build_app.sh copies it into the desktop shell. One function, three
# runtimes; see android/web/shell.js.
cp "$ROOT/server/edge/src/framing.js" "$W/framing.js"

# Art, with the module winning per FILE -- which is devserver.js's rule and has
# to be, or a module that overrides ten textures would have to ship all two
# thousand.
for kind in textures sprites music sounds ui; do
  mkdir -p "$W/$kind"
  [[ -d "$ROOT/assets/$kind" ]] && cp -R "$ROOT/assets/$kind/." "$W/$kind/" 2>/dev/null || true
  [[ -d "$MODULE_DIR/assets/$kind" ]] && cp -R "$MODULE_DIR/assets/$kind/." "$W/$kind/" 2>/dev/null || true
done
# CONCATENATED, not chosen between: the engine contributes its transient
# effects and the module contributes its props, ids are absolute, and the
# server refuses to start on a collision. Same rule, same order as devserver.
for f in props.txt animations.txt; do
  : > "$W/$f"
  [[ -f "$ROOT/assets/$f" ]] && cat "$ROOT/assets/$f" >> "$W/$f"
  [[ -f "$MODULE_DIR/assets/$f" ]] && { printf '\n# --- %s ---\n' "$MODULE" >> "$W/$f"
                                        cat "$MODULE_DIR/assets/$f" >> "$W/$f"; }
done
cp "$MODULE_DIR/maps/"*.dnpm "$W/maps/" 2>/dev/null || true

# The synth and its instruments, when they are built. Optional: a module with
# no soundtrack is a module, and a missing synth must not fail the build. But
# BUILT, not merely present: tool/dist/ is build output, not in git, and the
# worklet without the dist/midisynth.mjs it imports is an APK whose music
# fails to load on the phone ("the audio worklet failed to load") -- which is
# what a release built from a clean export shipped. As build_app.sh does: the
# synth whole, or not at all, and said.
if [[ -f "$REPO/midisynth/tool/dist/midisynth.mjs" ]]; then
  mkdir -p "$W/synth" "$W/synth-content"
  cp -R "$REPO/midisynth/tool/." "$W/synth/" 2>/dev/null || true
  cp -R "$REPO/midisynth/content/." "$W/synth-content/" 2>/dev/null || true
else
  echo "[apk] NO SYNTH: midisynth/tool/dist/midisynth.mjs is missing; build it with" >&2
  echo "      midisynth/scripts/build_wasm.sh, or this APK has no music" >&2
fi
# The sound-effects engine (docs/SOUNDS.md): one file, served at /sfx/. Optional
# like the synth -- without it the game has music and no sound effects -- and
# said, because silence is what a missing sound file looks like.
if [[ -f "$REPO/sound/tool/dist/dsound.mjs" ]]; then
  mkdir -p "$W/sfx"
  cp "$REPO/sound/tool/dist/dsound.mjs" "$W/sfx/"
else
  echo "[apk] NO SOUND ENGINE: sound/tool/dist/dsound.mjs is missing; build it with" >&2
  echo "      sound/scripts/build_wasm.sh, or this APK has no sound effects" >&2
fi

# --- 4. what the SERVER reads, as files ---------------------------------------
#
# Separate from the web tree and unpacked at first launch, because the server
# opens these by path and an asset inside an APK is not a file. Only what it
# reads: its maps, its module scripts and the two catalogues.
G="$STAGE/assets/game"
mkdir -p "$G/assets" "$G/modules/$MODULE"
[[ -f "$ROOT/assets/props.txt" ]] && cp "$ROOT/assets/props.txt" "$G/assets/"
[[ -f "$ROOT/assets/animations.txt" ]] && cp "$ROOT/assets/animations.txt" "$G/assets/"
mkdir -p "$G/modules/$MODULE/assets"
cp "$MODULE_DIR/assets/props.txt" "$G/modules/$MODULE/assets/" 2>/dev/null || true
# maps/ whole: the .dnpm AND the .lua beside it, which is what mapc's copies
# are for and what scripts/check_map_scripts.sh guards. A baked map without
# the scripts it declares kills the server at startup.
cp -R "$MODULE_DIR/maps" "$G/modules/$MODULE/"
# And module.txt, which the server reads for the module's `game` scripts and
# its `persist` switches (docs/MODULES.md). Without it the weapons ran once per
# level on a phone and the module's persistence was silently off.
cp "$MODULE_DIR/module.txt" "$G/modules/$MODULE/" 2>/dev/null || true
# With the studio, the module's documents and pictures too: the studio imports
# the shipped module from this tree on first launch (docs/STUDIO.md), and a
# module without its sheets and textures would import as an empty one.
if [[ $STUDIO -eq 1 ]]; then
  [[ -f "$MODULE_DIR/module.studio.json" ]] && cp "$MODULE_DIR/module.studio.json" "$G/modules/$MODULE/"
  [[ -d "$MODULE_DIR/assets" ]] && cp -R "$MODULE_DIR/assets/." "$G/modules/$MODULE/assets/"
fi
# WHICH MODULE AND WHICH LEVEL, stated where the app can read it.
#
# MainActivity used to hold both as constants, so this script's --map argument
# was checked and then thrown away: every APK started map01 whatever was asked
# for. Written as a file rather than compiled in, so the same classes.dex
# serves every module and changing the level is a rebuild of the assets.
cat > "$G/start.txt" <<EOF
# Written by scripts/build_apk.sh. Read by MainActivity at first launch.
module=$MODULE
map=$MAP
studio=$STUDIO
log=${LOG:-0}
built=$(date '+%Y-%m-%d %H:%M')
EOF

# THE STUDIO, when asked for: its pages, the map compiler as wasm, and the
# runtime Lua and vocabularies it bakes and validates against -- under
# web/studio/, which is where the page fetches them from (docs/STUDIO.md).
if [[ $STUDIO -eq 1 ]]; then
  # With its vendored plugins beside its pages (docs/PLUGINS.md).
  ED=studio
  mkdir -p "$W/$ED/wasm/dist" "$W/$ED/runtime"
  cp "$REPO/$ED/index.html" "$W/$ED/"
  cp -R "$REPO/$ED/src" "$W/$ED/src"
  cp -R "$REPO/$ED/vendor" "$W/$ED/vendor"
  cp "$REPO/studio/wasm/dist/mapc.mjs" "$REPO/studio/wasm/dist/mapc.wasm" "$W/$ED/wasm/dist/"
  if [[ ${NO_PLUGINS:-0} -eq 1 ]]; then
    mkdir -p "$W/studio/plugins"
    echo '{ "plugins": [] }' > "$W/studio/plugins/index.json"
  else
    node "$REPO/studio/tools/vendor_plugins.mjs" --check >&2 || { echo "[apk] the studio's plugins are not as vendored: node studio/tools/vendor_plugins.mjs" >&2; exit 1; }
    cp -R "$REPO/studio/plugins" "$W/studio/plugins"
  fi
  cp "$REPO/studio/sw.js" "$REPO/studio/VERSION" "$W/studio/"
  if [[ $NO_KITS -eq 0 ]]; then
    # The packager plugin's small kits, where its page looks (as build_app.sh):
    # Android and the Tauri desktops, ~24 MB; the Electron ones stay out.
    mkdir -p "$W/studio/packager-kits"
    for k in android tauri-macos-arm64 tauri-macos-x64 tauri-linux tauri-windows; do
      [[ -f "$REPO/packager/build/kits/$k.zip" ]] && cp "$REPO/packager/build/kits/$k.zip" "$W/studio/packager-kits/"
    done
    echo "[apk] studio: $(ls "$W/studio/packager-kits" | wc -l | tr -d ' ') packager kit(s) preloaded"
  fi
  for f in mob_vocab.lua mobs.lua trigger_vocab.lua triggers.lua weapon_vocab.lua weapons.lua \
           graph_vocab.lua graphs.lua player_sounds.lua \
           trigger_vocab.json mob_vocab.json weapon_vocab.json graph_vocab.json; do
    cp "$ROOT/maps/scripts/$f" "$W/$ED/runtime/"
  done
  echo "[apk] $ED   staged"
fi

# Nothing the server will not open. A .map is the authored source and an
# .editor.json is the editor's sidecar; both are megabytes of nothing here.
find "$G" \( -name '*.map' -o -name '*.editor.json' \) -delete

echo "[apk] assets   web $(du -sh "$W" | cut -f1), game $(du -sh "$G" | cut -f1)" \
     "-- $(ls "$W/maps/"*.dnpm 2>/dev/null | wc -l | tr -d ' ') level(s), starting on $MAP"

# --- 5. resources and the manifest --------------------------------------------

RES="$OUT/res"
rm -rf "$RES"; mkdir -p "$RES/mipmap-anydpi-v26" "$RES/drawable" "$RES/values"
# A launcher icon, drawn rather than shipped as a binary: this repository has
# no business carrying a png it cannot diff, and a vector is six lines.
cat > "$RES/drawable/ic_fg.xml" <<'XML'
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="108dp" android:height="108dp"
    android:viewportWidth="108" android:viewportHeight="108">
  <path android:fillColor="#cfe3ff"
        android:pathData="M54,26 L82,54 L54,82 L26,54 Z" />
  <path android:fillColor="#5b8fd6"
        android:pathData="M54,38 L70,54 L54,70 L38,54 Z" />
</vector>
XML
cat > "$RES/values/colors.xml" <<'XML'
<resources><color name="ic_bg">#0b0d12</color></resources>
XML
cat > "$RES/mipmap-anydpi-v26/ic_launcher.xml" <<'XML'
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
  <background android:drawable="@color/ic_bg" />
  <foreground android:drawable="@drawable/ic_fg" />
</adaptive-icon>
XML
# A raster fallback for launchers older than the adaptive icon, which aapt2
# needs to resolve @mipmap/ic_launcher on every density it compiles for.
mkdir -p "$RES/mipmap"
cp "$RES/drawable/ic_fg.xml" "$RES/mipmap/ic_launcher.xml"
# THE LOGO instead (--icon): the same resource names, now bitmaps -- the
# packager swaps a game's icon by those names, so an APK built either way
# takes one.
if [[ $ICON -eq 1 ]]; then
  node "$REPO/branding/icons.mjs" android "$RES" | sed 's/^/[apk] /'
fi

echo "[apk] res      compiling"
"$BT/aapt2" compile --dir "$RES" -o "$OUT/res.zip" >/dev/null

# A STUDIO carries its version (0.4.1 -> 40100, 0.4.1.1 -> 40101), so a later
# APK installs over an earlier one. A studio may also install its own update
# (MainActivity selfUpdate): its manifest asks for that; a game's never does.
# Which update it takes is the studio's (updates.js: a stable build only a
# stable one, a dev build the newest of either).
MANIFEST="$OUT/AndroidManifest.xml"
cp "$ROOT/shells/android/AndroidManifest.xml" "$MANIFEST"
VER_ARGS=()
if [[ $STUDIO -eq 1 ]]; then
  SV=$(tr -d ' \n' < "$REPO/studio/VERSION")
  IFS=. read -r v1 v2 v3 v4 <<< "$SV"
  VER_ARGS=(--version-name "$SV" --version-code $(( v1 * 1000000 + v2 * 10000 + v3 * 100 + ${v4:-0} )))
  sed -i '' 's|<uses-permission android:name="android.permission.INTERNET" />|&\
  <uses-permission android:name="android.permission.REQUEST_INSTALL_PACKAGES" />|' "$MANIFEST"
  echo "[apk] studio   $SV ($([[ -n "$v4" ]] && echo dev || echo stable)): it updates itself"

fi

"$BT/aapt2" link \
  -I "$PLATFORM_JAR" \
  --manifest "$MANIFEST" \
  --java "$OUT/gen" \
  --min-sdk-version 26 --target-sdk-version 34 "${VER_ARGS[@]}" \
  -o "$OUT/base.apk" \
  "$OUT/res.zip" >/dev/null
mkdir -p "$OUT/gen"

# --- 6. java ------------------------------------------------------------------

echo "[apk] java     compiling"
mkdir -p "$OUT/gen"
find "$ROOT/shells/android/java" "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
# SOURCE AND TARGET 8, against android.jar as the bootclasspath.
#
# Not a preference: a modern JDK refuses `-bootclasspath` for any target above
# 8, and without it javac compiles against the DESKTOP's java.lang -- which
# links, and then fails on the phone at the first method Android does not have.
# The Java here is deliberately Java 8 (lambdas and try-with-resources, no
# `var`, no records) so that the floor costs nothing.
# core-lambda-stubs.jar ON THE BOOTCLASSPATH TOO, and it is not optional.
# android.jar has no java.lang.invoke.LambdaMetafactory -- the runtime never
# needs one, because d8 desugars every lambda into a class before it ships --
# but javac insists on seeing it at compile time. The stubs are what the real
# Android toolchain passes for exactly this, and they ship in build-tools.
LAMBDA_STUBS="$BT/core-lambda-stubs.jar"
[[ -f "$LAMBDA_STUBS" ]] || { echo "[apk] no $LAMBDA_STUBS" >&2; exit 2; }

javac -source 8 -target 8 -nowarn -Xlint:none \
      -bootclasspath "$PLATFORM_JAR:$LAMBDA_STUBS" \
      -d "$STAGE/classes" @"$OUT/sources.txt" 2>&1 \
  | grep -vE "^(Note:|warning:|[0-9]+ warning)" || true
[[ -f "$STAGE/classes/com/digidream/app/MainActivity.class" ]] || {
  echo "[apk] javac produced no MainActivity" >&2; exit 2; }

"$BT/d8" --min-api 26 --output "$STAGE" \
  $(find "$STAGE/classes" -name '*.class') >/dev/null
rm -rf "$STAGE/classes"

# --- 7. package, align, sign ---------------------------------------------------

echo "[apk] packing"
cd "$STAGE"
# The manifest and resources come from aapt2's output; everything else is added
# here. `zip -X` keeps the archive free of the extra attributes apksigner
# objects to on some platforms.
cp "$OUT/base.apk" "$OUT/unsigned.apk"
zip -q -X -r "$OUT/unsigned.apk" classes.dex assets lib
cd "$ROOT"

# A DEBUG KEY, generated once and kept. An APK has to be signed to install at
# all; this is not a release key and is not pretending to be one. It lives in
# build/ and is gitignored, so it is regenerated on a fresh clone and every
# build after the first is installable over the previous one.
KEY="$OUT/debug.keystore"
if [[ ! -f "$KEY" ]]; then
  echo "[apk] key      generating a debug keystore"
  keytool -genkeypair -v -keystore "$KEY" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10950 \
    -dname "CN=Digidream Debug,O=Digidream,C=GB" >/dev/null 2>&1
fi

"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
APK="$OUT/digidream-$MODULE.apk"
"$BT/apksigner" sign --ks "$KEY" --ks-pass pass:android --key-pass pass:android \
  --out "$APK" "$OUT/aligned.apk"
rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk" "$OUT/base.apk" "$OUT/res.zip"

echo
echo "[apk] $APK  ($(du -h "$APK" | cut -f1))"
echo
echo "  install:  $SDK/platform-tools/adb install -r $APK"
echo "  log:      $SDK/platform-tools/adb logcat -s digidream"
