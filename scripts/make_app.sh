#!/usr/bin/env bash
#
# One command: engine + one module -> a standalone application.
#
#   scripts/make_app.sh --module doom2
#   scripts/make_app.sh --module doom2 --package
#   scripts/make_app.sh --module mygame --name "My Game" --out build/app
#
# WHAT THIS IS AND WHY IT IS NOT build_app.sh. build_app.sh STAGES: it copies a
# module, the client, the server binary and the synth into one directory, and it
# deliberately refuses to build the world on your behalf -- its own comment says
# "a packaging script that silently rebuilds the world is a packaging script
# nobody runs twice". That is the right contract for the thing app/test/
# stage.test.js asserts against, and it is the wrong contract for somebody who
# just wants the game in a folder.
#
# So this is the orchestrator and build_app.sh stays the staging step. This one
# checks each prerequisite, builds only what is missing, and says what it is
# doing. Nothing here duplicates what build_app.sh does; it makes build_app.sh's
# preconditions true and then calls it.
#
# THE CHAIN, in order, with what satisfies each link:
#
#   emsdk                 scripts/setup_emsdk.sh, then emsdk/emsdk_env.sh
#   client wasm core      scripts/build_wasm.sh   -> client/wasm/dist/dnp_core.mjs
#   three.js              (cd client && npm install)
#   synth wasm            midisynth/scripts/build_wasm.sh (optional: no synth,
#                         no music, and the app still runs)
#   server binary         scripts/build_server.sh (build_app.sh does this one)
#   staging               scripts/build_app.sh
#   packaging             electron-builder, only with --package
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

MODULES=()
PASS=()            # arguments handed straight to build_app.sh
OUT_BASE="build/app"
APP_ID=""
PACKAGE=0
FORCE=0
STUDIO=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --module)  MODULES+=("$2"); PASS+=(--module "$2"); shift 2 ;;
    --name)    PASS+=(--name "$2"); shift 2 ;;
    --id)      APP_ID="$2"; shift 2 ;;
    --out)     OUT_BASE="$2"; PASS+=(--out "$2"); shift 2 ;;
    --package) PACKAGE=1; shift ;;
    --force)   FORCE=1; shift ;;
    --studio)  STUDIO=1; PASS+=(--studio); shift ;;
    -h|--help) sed -n '2,29p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ ${#MODULES[@]} -gt 0 ]] || {
  echo "usage: $0 --module <name> [--module <name>...] [--package]" >&2; exit 2; }

# The app id decides the output directory, and this needs to know it to package
# what it just staged. Same rule build_app.sh uses -- the first module names the
# app -- and it is passed explicitly rather than left to default, so the two
# scripts cannot disagree about where the thing went.
[[ -n "$APP_ID" ]] || APP_ID="${MODULES[0]}"
PASS+=(--id "$APP_ID")
OUT="$OUT_BASE/$APP_ID"

step () { echo; echo "=== $* ==="; }

# --- emsdk -------------------------------------------------------------------
#
# Sourced rather than demanded. emsdk_env.sh only edits PATH for the shell that
# runs it, so every build_wasm.sh invocation from a fresh terminal needs it
# first -- which is a step everyone forgets once and then reads a "em++ not
# found" that looks like a broken install.
if ! command -v em++ >/dev/null 2>&1; then
  if [[ -f ../emsdk/emsdk_env.sh ]]; then
    step "emsdk"
    # shellcheck disable=SC1091
    source ../emsdk/emsdk_env.sh >/dev/null 2>&1 || true
  fi
fi
command -v em++ >/dev/null 2>&1 || {
  echo "em++ not found. Run ./scripts/setup_emsdk.sh first -- it vendors a" >&2
  echo "pinned Emscripten into ../emsdk/, which is gitignored and one-time." >&2
  exit 1; }

# --- the client's wasm core --------------------------------------------------
if [[ $FORCE -eq 1 || ! -f client/wasm/dist/dnp_core.mjs ]]; then
  step "client wasm core"
  ./scripts/build_wasm.sh
else
  echo "[make] wasm core present (--force to rebuild)"
fi

# --- the studio's map compiler, as wasm --------------------------------------
if [[ $STUDIO -eq 1 && ( $FORCE -eq 1 || ! -f ../studio/wasm/dist/mapc.mjs ) ]]; then
  step "studio map compiler (wasm)"
  ./scripts/build_mapc_wasm.sh
elif [[ $STUDIO -eq 1 ]]; then
  echo "[make] studio map compiler present (--force to rebuild)"
fi

# --- three.js ----------------------------------------------------------------
if [[ ! -d client/node_modules/three/build ]]; then
  step "client dependencies"
  (cd client && npm install)
else
  echo "[make] three.js present"
fi

# --- the synth ---------------------------------------------------------------
#
# OPTIONAL, and loudly so. An app with no synth starts, plays and is silent, and
# a silent game that nobody warned you about reads as a bug in the music code.
if [[ -d midisynth ]]; then
  if [[ $FORCE -eq 1 || ! -f ../midisynth/tool/dist/midisynth.mjs ]]; then
    step "synth wasm"
    ./midisynth/scripts/build_wasm.sh || {
      echo "[make] WARNING: the synth failed to build; the app will have no music" >&2; }
  else
    echo "[make] synth present (--force to rebuild)"
  fi
fi

# --- stage -------------------------------------------------------------------
#
# build_app.sh builds the server itself when it is missing, so that link in the
# chain is its job and not repeated here.
step "staging"
./scripts/build_app.sh "${PASS[@]}"

# --- package -----------------------------------------------------------------
if [[ $PACKAGE -eq 1 ]]; then
  step "packaging"
  # npm install here fetches Electron (~150 MB, cached afterwards) and
  # electron-builder, both of which the staged package.json pins -- see the
  # comment in build_app.sh about why `npx` without a declared dependency is a
  # download of unknown version.
  (cd "$OUT" && npm install && npx electron-builder --dir)
  echo
  echo "[make] packaged -> $OUT/dist"
  # --dir, not a full installer. A DMG, NSIS or AppImage is the same command
  # without --dir, and on macOS an unsigned one warns on first launch unless you
  # have a Developer ID certificate. That is a decision about distribution, not
  # about building, so it is left to whoever is distributing.
  echo "       an installer is the same command without --dir; on macOS that"
  echo "       wants a signing certificate or it warns on first launch."
else
  echo
  echo "[make] run it:  (cd $OUT && npm install && npm start)"
  echo "[make] package: $0 --module ${MODULES[0]} --package"
fi
