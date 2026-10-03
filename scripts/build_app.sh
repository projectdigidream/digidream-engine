#!/usr/bin/env bash
#
# Stage one MODULE into a standalone desktop application.
#
#   scripts/build_app.sh --module doom2
#   scripts/build_app.sh --module mygame --name "My Game" --out build/app
#   scripts/build_app.sh --module doom2 --module extras      (several, in order)
#
# The output is a self-contained directory that Electron can run directly:
#
#   build/app/<id>/
#     game.json          what this app IS: its name, and which modules it loads
#     package.json       the Electron manifest; "main" is app/main.js
#     app/               the shell (main, preload, static server, link relay)
#     bin/dnp-server     the authoritative server, for THIS platform
#     assets/props.txt   the engine's catalogue
#     modules/<id>/      the module: its maps, its scripts, its assets
#     client/            index.html, src/, wasm/, vendor/three
#
# WHY A DIRECTORY AND NOT AN INSTALLER. Packaging (electron-builder: NSIS on
# Windows, DMG on macOS, AppImage on Linux) is a separate step with separate
# prerequisites -- a code-signing certificate among them -- and it is the part
# that cannot be verified without them. Staging is the part that can: the tree
# below is complete, testable and runnable today, and `npx electron .` inside it
# starts the game. Wrapping a complete tree is the easy half.
#
# WHAT MAKES THIS PER-MODULE AND NOT PER-BUILD. A module is a ROOT
# (docs/MODULES.md): its maps, its scripts and its assets in one directory. This
# copies exactly the modules named and nothing else, so an app built from a
# module that is not Doom contains no Doom -- which is checked, not asserted, by
# app/test/stage.test.js.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

# The Electron the staged app asks for. One place, so a bump is one edit.
ELECTRON_VERSION="44.0.0"

MODULES=()
OUT_BASE="build/app"
APP_NAME=""
APP_ID=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --module) MODULES+=("$2"); shift 2 ;;
    --out)    OUT_BASE="$2"; shift 2 ;;
    --name)   APP_NAME="$2"; shift 2 ;;
    --id)     APP_ID="$2"; shift 2 ;;
    # The level editor in the same app (docs/STUDIO.md): the studio pages, the
    # map compiler as wasm, and the runtime Lua it bakes against.
    --studio) STUDIO=1; shift ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
STUDIO="${STUDIO:-0}"

[[ ${#MODULES[@]} -gt 0 ]] || { echo "usage: $0 --module <name> [--module <name>...]" >&2; exit 2; }

# The first module names the app unless told otherwise. A one-module app is the
# common case and having to spell the name twice is a way to get it wrong once.
[[ -n "$APP_ID" ]] || APP_ID="${MODULES[0]}"

# --- validate, before building anything --------------------------------------
#
# Every failure here is cheap and every one of them is confusing later: a module
# that does not exist stages an empty tree, and a module with no baked maps
# produces an app that starts and hosts nothing.
for m in "${MODULES[@]}"; do
  d="../modules/$m"
  [[ -d "$d" ]] || { echo "no module at $d" >&2; exit 1; }
  count=$(find "$d/maps" -name '*.dnpm' 2>/dev/null | wc -l | tr -d ' ')
  [[ "$count" -gt 0 ]] || {
    echo "module '$m' has no baked maps in $d/maps -- run mapc first" >&2; exit 1; }
  echo "[app] module $m: $count baked map(s)"
done

if [[ -z "$APP_NAME" ]]; then
  # module.txt's `name` is there to be displayed, which is exactly this.
  # [[:space:]][[:space:]]* rather than \+ -- BSD sed (macOS) does not have \+,
  # and it fails by matching nothing rather than by complaining, so the name
  # quietly fell back to the module id.
  APP_NAME=$(sed -n 's/^name[[:space:]][[:space:]]*//p' "../modules/${MODULES[0]}/module.txt" 2>/dev/null | head -1)
  [[ -n "$APP_NAME" ]] || APP_NAME="${MODULES[0]}"
fi

OUT="$OUT_BASE/$APP_ID"
echo "[app] '$APP_NAME' -> $OUT"

# --- the two compiled artefacts ----------------------------------------------
#
# Built if missing rather than always: the server takes a while and the wasm
# core takes longer, and a packaging script that silently rebuilds the world is
# a packaging script nobody runs twice.
[[ -x build/server/dnp-server ]] || {
  echo "[app] building the server"; ./scripts/build_server.sh; }
[[ -f client/wasm/dist/dnp_core.mjs ]] || {
  echo "[app] no wasm core at client/wasm/dist/dnp_core.mjs" >&2
  echo "      run: source ../emsdk/emsdk_env.sh && ./scripts/build_wasm.sh" >&2
  exit 1; }
[[ -d client/node_modules/three/build ]] || {
  echo "[app] three.js not installed -- run: (cd client && npm install)" >&2; exit 1; }

# --- stage --------------------------------------------------------------------
#
# node_modules SURVIVES the restage. It holds Electron -- 150 MB and a download
# -- and `rm -rf "$OUT"` used to take it every time, so the edit/restage/run
# loop paid for a reinstall on every pass. Moved aside and put back rather than
# kept in place: the point of the rm is that a stale file from a previous stage
# never survives, and node_modules is the one directory this script does not
# produce.
STASH=""
if [[ -d "$OUT/node_modules" ]]; then
  STASH="$(mktemp -d)"
  mv "$OUT/node_modules" "$STASH/node_modules"
fi
rm -rf "$OUT"
mkdir -p "$OUT"/{app,bin,assets,modules,client/wasm,client/vendor/three}
if [[ -n "$STASH" ]]; then
  mv "$STASH/node_modules" "$OUT/node_modules"
  rmdir "$STASH"
  echo "[app] kept the existing node_modules (Electron)"
fi

# The shell.
cp shells/app/main.js shells/app/preload.cjs shells/app/static.js shells/app/relay.js shells/app/studio_files.js \
   shells/app/ensure-electron.cjs "$OUT/app/"

# THE LINK FRAMING, COPIED rather than reimplemented or imported across a
# package boundary. A second implementation of this framing is precisely the
# "two programs, one number" failure this repository keeps paying for, and a
# build-time copy is how mapc already solves the same problem for a map's
# scripts: the staged app is self-contained the way a baked map is.
cp server/edge/src/framing.js server/edge/src/upstream.js "$OUT/app/"

cp build/server/dnp-server "$OUT/bin/dnp-server"

# The ENGINE's assets, WHOLE.
#
# Today that is one nearly-empty catalogue: every prop with a picture was Doom
# art and moved to the module that owns it (docs/MODULES.md). But copying the
# directory rather than two named files is deliberate, and not for tidiness --
# the engine may legitimately ship art of its own later, and a copy that names
# props.txt and animations.txt would silently omit it.
#
# It also makes app/test/stage.test.js's control mean something. That test
# stages a module with no assets and asserts the result contains no images; with
# only two named files copied it would pass no matter WHAT was sitting in
# assets/, which is a control that cannot fail. Copying the directory is what
# ties the assertion to the property.
cp -R assets/. "$OUT/assets/"

# The modules named, whole, and no others.
for m in "${MODULES[@]}"; do
  mkdir -p "$OUT/modules/$m"
  # -R rather than a find/copy of chosen names: a module is a root and what is
  # in it is the module's business. Doom's sprite lumps include names with a
  # backslash in them (VILE\1) and any copy that reconstructs paths by hand gets
  # those wrong.
  cp -R "../modules/$m/." "$OUT/modules/$m/"
  # The .map sources and the editor sidecars are authoring inputs, not runtime
  # ones -- the server reads only the baked .dnpm. Dropping them keeps a doom2
  # app from carrying 32 copies of a file nothing opens.
  find "$OUT/modules/$m/maps" \( -name '*.map' -o -name '*.editor.json' \) -delete 2>/dev/null || true
done

# THE SYNTH, if it has been built.
#
# Optional: midisynth's wasm is produced by its own build script and is not
# committed, so an app staged without it is a game with no music rather than a
# failed build. Said out loud either way -- silence is exactly what a missing
# music file looks like, and a packaging step that drops it quietly is how that
# ships.
#
# tool/ and content/ are copied to flat roots (`synth/`, `synth-content/`)
# because the app serves them at those prefixes; the worklet's relative import
# of ./dist/midisynth.mjs keeps working because dist/ comes with it.
if [[ -f ../midisynth/tool/dist/midisynth.mjs ]]; then
  mkdir -p "$OUT/synth" "$OUT/synth-content"
  cp ../midisynth/tool/synth-worklet.js "$OUT/synth/"
  mkdir -p "$OUT/synth/dist"
  cp ../midisynth/tool/dist/midisynth.mjs "$OUT/synth/dist/"
  cp -R ../midisynth/content/. "$OUT/synth-content/"
  echo "[app] synth staged"
else
  echo "[app] NO SYNTH: ../midisynth/tool/dist/midisynth.mjs is missing, so this" >&2
  echo "      app will have no music. Build it with ../midisynth/scripts/build_wasm.sh" >&2
fi

# THE SOUND-EFFECTS ENGINE (sound/, docs/SOUNDS.md), if it has been built.
#
# Optional for the same reason as the synth: its wasm is produced by
# sound/scripts/build_wasm.sh and not committed. One file, served at /sfx/,
# which client/src/audio/sfx-worklet.js imports. The banks need nothing here:
# each module's assets/sounds travels with the module.
if [[ -f ../sound/tool/dist/dsound.mjs ]]; then
  mkdir -p "$OUT/sfx"
  cp ../sound/tool/dist/dsound.mjs "$OUT/sfx/"
  echo "[app] sound engine staged"
else
  echo "[app] NO SOUND ENGINE: ../sound/tool/dist/dsound.mjs is missing, so this" >&2
  echo "      app will have no sound effects. Build it with ../sound/scripts/build_wasm.sh" >&2
fi

# The client, as served. Same files the browser build runs -- see app/static.js
# for why they are served rather than bundled.
cp client/index.html "$OUT/client/"
cp -R client/src "$OUT/client/src"
cp -R client/wasm/dist "$OUT/client/wasm/dist"
cp client/node_modules/three/build/three.module.js "$OUT/client/vendor/three/"
[[ -f client/node_modules/three/build/three.core.js ]] && \
  cp client/node_modules/three/build/three.core.js "$OUT/client/vendor/three/"

# --- the studio, when asked for -------------------------------------------------
#
# The same files the dev server serves at /studio/, plus the runtime Lua and
# the two vocabularies under studio/runtime/, which is where the studio bakes
# and validates against. app/main.js opens /studio/ instead of the game when
# game.json says so, and seeds the modules into the writable tree.
if [[ $STUDIO -eq 1 ]]; then
  [[ -f ../studio/wasm/dist/mapc.mjs ]] || {
    echo "[app] no studio map compiler at ../studio/wasm/dist/mapc.mjs" >&2
    echo "      run: source ../emsdk/emsdk_env.sh && ./scripts/build_mapc_wasm.sh" >&2
    exit 1; }
  mkdir -p "$OUT/studio/wasm/dist" "$OUT/studio/runtime"
  cp ../studio/index.html "$OUT/studio/"
  cp -R ../studio/src "$OUT/studio/src"
  cp -R ../studio/vendor "$OUT/studio/vendor"
  cp ../studio/wasm/dist/mapc.mjs ../studio/wasm/dist/mapc.wasm "$OUT/studio/wasm/dist/"
  for f in mob_vocab.lua mobs.lua trigger_vocab.lua triggers.lua weapon_vocab.lua weapons.lua \
           graph_vocab.lua graphs.lua player_sounds.lua \
           trigger_vocab.json mob_vocab.json weapon_vocab.json graph_vocab.json; do
    cp "maps/scripts/$f" "$OUT/studio/runtime/"
  done
  echo "[app] studio staged"
fi

# --- what this app IS ---------------------------------------------------------
#
# Generated, and read by app/main.js at startup. A manifest rather than a
# baked-in constant so that the same shell serves every module: the shell is not
# rebuilt per game, only re-staged.
MODULES_JSON=$(printf '"%s",' "${MODULES[@]}"); MODULES_JSON="[${MODULES_JSON%,}]"
cat > "$OUT/game.json" <<EOF
{
  "name": $(printf '%s' "$APP_NAME" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'),
  "id": "$APP_ID",
  "modules": $MODULES_JSON,
  "studio": $([[ $STUDIO -eq 1 ]] && echo true || echo false),
  "builtAt": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "platform": "$(uname -s)-$(uname -m)"
}
EOF

# ELECTRON IS DECLARED HERE, and that is not a formality.
#
# Without a dependency there is nothing for `npx electron .` to run: npx finds
# no local electron, and what it does next depends on its cache and its mood.
# The failure is a bare "Unable to find Electron app", which points at the app
# -- the one thing that is fine -- and says nothing about the missing runtime.
#
# electron-builder is here for the same reason: `npx electron-builder` in a
# directory that does not declare it is a download of unknown version.
cat > "$OUT/package.json" <<EOF
{
  "name": "$APP_ID",
  "productName": $(printf '%s' "$APP_NAME" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'),
  "version": "0.0.0",
  "private": true,
  "type": "module",
  "main": "app/main.js",
  "scripts": {
    "postinstall": "node app/ensure-electron.cjs",
    "start": "node app/ensure-electron.cjs && electron ."
  },
  "devDependencies": {
    "electron": "^$ELECTRON_VERSION",
    "electron-builder": "^26.0.0"
  },
  "build": {
    "appId": "dev.dnp.$APP_ID",
    "productName": $(printf '%s' "$APP_NAME" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'),
    "directories": { "output": "dist" },
    "files": ["app/**", "client/**", "studio/**", "game.json"],
    "extraResources": [
      { "from": "bin", "to": "bin" },
      { "from": "assets", "to": "assets" },
      { "from": "modules", "to": "modules" }
    ]
  }
}
EOF

echo
echo "[app] staged $OUT"
# node_modules EXCLUDED from the size. It holds Electron -- 400 MB of runtime
# that is preserved across restages and is not part of what this script
# produces -- and including it reported a 23 MB game as a 432 MB one, which is
# the sort of number that sends somebody hunting for a packaging bug.
#
# `du -sk` on both and subtract, rather than `du --exclude`: that flag is GNU
# and this runs on BSD du too, where it is silently not an error -- the first
# attempt summed mixed K/M/G strings and reported 547.8M.
APP_KB=$(du -sk "$OUT" | cut -f1)
MOD_KB=0
[[ -d "$OUT/node_modules" ]] && MOD_KB=$(du -sk "$OUT/node_modules" | cut -f1)
echo "      $(find "$OUT" -type f -not -path "*/node_modules/*" | wc -l | tr -d ' ') files, $(( (APP_KB - MOD_KB) / 1024 )) MB (excluding Electron)"
echo
echo "  run it:      (cd $OUT && npm install && npm start)"
echo "               npm install fetches Electron; it is ~150 MB the first time"
echo "               and cached after."
echo "  package it:  (cd $OUT && npm install && npx electron-builder --dir)"
echo
