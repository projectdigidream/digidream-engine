#!/usr/bin/env bash
#
# Bring the whole thing up: server, WebTransport edge, dev web server.
#
#   scripts/run_stack.sh --module starter        the starter module (engine/modules/starter)
#   scripts/run_stack.sh [map] [--script <lua>]
#   scripts/run_stack.sh --map a.dnpm --map b.dnpm --script <lua>
#
# SOME OF THE ENGINE'S OWN TEST MAPS NEED --module doom2, a private module
# (with the DOOM fixtures) that a public checkout does not have:
#
#   scripts/run_stack.sh --module doom2 maps/doors.dnpm
#
# doors, phase3, test and monsters spawn keys, barrels and zombiemen, and every
# one of those is DOOM art whose catalogue now lives in the module that owns it
# (docs/MODULES.md). Without the module the server loads a catalogue with no
# props in it and the map's script fails on its first spawn -- loudly, naming
# the prop, which is the one mercy in it. This is the honest state: those maps
# always depended on Doom's art, and the dependency used to be invisible
# because the engine carried it for everybody.
#
# Several --map arguments make a PROGRESSION: an exit in the first takes the
# player who touched it -- and only that player -- to the next.
#
# Then open http://localhost:8080/?server=https://127.0.0.1:34433/dnp
#
# Three processes rather than one because that is genuinely what the
# architecture is: the C++ server is transport-agnostic and speaks a local
# framed socket, the edge terminates WebTransport, and the page is static. The
# split was a deliberate Phase 1 decision (there is no batteries-included
# WebTransport server for C++) and hiding it behind a single binary would make
# the deployment story less honest, not simpler.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
# PATHS AS TYPED, from wherever this was run. The stack runs in the engine
# folder, so a map or module path given relative to the caller -- the usual
# `engine/scripts/run_stack.sh --module doom2 modules/doom2/maps/map07.dnpm`
# from the checkout -- is made absolute before the cd, or it would be looked
# for under engine/.
CALLER="$PWD"
TYPED=()
for a in "$@"; do
  if [[ "$a" != /* && "$a" != -* && -e "$CALLER/$a" ]]; then TYPED+=("$CALLER/$a"); else TYPED+=("$a"); fi
done
cd "$ROOT"

ARCH="$(uname -m)"

# Modules named on the command line reach BOTH the server (maps, scripts, the
# prop catalogue) and the web server (sprites, textures, the catalogue's
# renderer half). Repeatable; see docs/MODULES.md.
#
# PULLED OUT FIRST, before the map argument is worked out, because the two
# interact: `--module doom2` alone left nothing on the line, the map logic below
# saw no arguments, and helpfully substituted the default test level. Which is
# how asking for DOOM II got you a grey room.
MODULE_ARGS=()
REST=()
i=0
ARGV=(${TYPED[@]+"${TYPED[@]}"})
while [[ $i -lt ${#ARGV[@]} ]]; do
  if [[ "${ARGV[$i]}" == "--module" && $((i + 1)) -lt ${#ARGV[@]} ]]; then
    MODULE_ARGS+=(--module "${ARGV[$((i + 1))]}")
    i=$((i + 2))
  else
    REST+=("${ARGV[$i]}")
    i=$((i + 1))
  fi
done
set -- ${REST[@]+"${REST[@]}"}

# A bare first argument is still the map, because that is how everyone has been
# invoking this. Anything starting with `--` is passed straight through, which
# is what a progression looks like: --map a --map b --map c.
ARGS=()
if [[ $# -gt 0 && "$1" != --* ]]; then
  ARGS+=(--map "$1")
  MAP="$1"
  shift
elif [[ "$*" != *--map* && ${#MODULE_ARGS[@]} -eq 0 ]]; then
  # The default, and ONLY when no module was named. A module brings its own
  # maps -- the server discovers them -- so substituting a test level here
  # would override the very thing that was asked for.
  ARGS+=(--map "build/$ARCH/test.dnpm")
  MAP="build/$ARCH/test.dnpm"
else
  MAP=""
fi

# EVERYTHING IS LOGGED, BY DEFAULT.
#
# Three processes share this terminal and the interesting line is almost always
# the one that scrolled away: a Lua error at the moment a door opened, the
# server refusing an edge, a crash that left only a signal. Asking somebody to
# re-run "but with a log this time" means asking them to reproduce it, and the
# bugs worth catching here are the ones that do not reproduce on demand.
#
# tee, not redirection: the terminal still shows everything live. The previous
# run is kept as .prev, so "it just did the thing" is still recoverable after
# you have restarted once out of habit.
#
#   DNP_LOG=/path/to/file   log somewhere else
#   DNP_LOG=none            do not log
LOG="${DNP_LOG:-$ROOT/build/dnp-stack.log}"
if [[ "$LOG" != "none" ]]; then
  mkdir -p "$(dirname "$LOG")"
  [[ -f "$LOG" ]] && mv -f "$LOG" "$LOG.prev"
  # Line-buffered through tee so a crash does not take the last few lines --
  # which are the ones that say why -- down with it.
  exec > >(tee "$LOG") 2>&1
  echo "[stack] logging to $LOG  (previous run: $LOG.prev)"
fi

LINK_PORT="${DNP_LINK_PORT:-34500}"
WT_PORT="${DNP_WT_PORT:-34433}"
HTTP_PORT="${DNP_HTTP_PORT:-8080}"

# scripts/build_server.sh writes to build/server, not to the per-architecture
# directory the determinism gate uses -- the server is not part of that gate.
# ALREADY RUNNING? SAY SO AND STOP.
#
# Starting a second stack on the same ports is a confusing failure, not an
# obvious one. The second server cannot bind, so it exits -- but the second
# EDGE starts happily and connects to the FIRST stack's server, which accepts
# the socket and immediately closes it (one edge at a time, server/src/net/
# link.cpp). The edge reconnects half a second later and is closed again, for
# ever:
#
#   [edge] upstream connected 127.0.0.1:34500
#   [edge] upstream lost; dropping all sessions
#
# That reads like a crashing server. It is a second copy of this script. The
# first stack, meanwhile, is fine and playing -- which makes it worse, because
# the symptom appears in a terminal where nothing is wrong.
#
# /dev/tcp rather than lsof: it is bash itself, so there is nothing to install
# and nothing that differs between macOS and Linux.
port_busy () { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && exec 3<&- && return 0; return 1; }
for probe in "$LINK_PORT:the game server" "$HTTP_PORT:the web server"; do
  p="${probe%%:*}"; what="${probe#*:}"
  if port_busy "$p"; then
    echo "port $p is already in use, which is where $what goes." >&2
    echo >&2
    echo "  A stack is probably already running. Find it with:" >&2
    echo "      pgrep -fl run_stack.sh" >&2
    echo "  and stop it, or give this one its own ports:" >&2
    echo "      DNP_LINK_PORT=34600 DNP_WT_PORT=34633 DNP_HTTP_PORT=8081 \\" >&2
    echo "          $0 <your usual arguments>" >&2
    exit 1
  fi
done

SERVER="build/server/dnp-server"
[[ -x "$SERVER" ]] || { echo "no server binary; run scripts/build_server.sh"; exit 1; }
[[ -z "$MAP" || -f "$MAP" ]] || { echo "no map at $MAP; run mapc first"; exit 1; }
[[ -f server/edge/certs/cert-hash.json ]] || {
  echo "no dev certificate; run: (cd server/edge && npm run certs)"; exit 1; }
[[ -f client/wasm/dist/dnp_core.mjs ]] || { echo "no wasm build; run scripts/build_wasm.sh"; exit 1; }

pids=()
cleanup () {
  for p in ${pids[@]+"${pids[@]}"}; do
    # Children first, then the process itself: `exec` above makes this
    # unnecessary for the edge, but the rule should not depend on remembering
    # to use exec everywhere, and a leaked process here is invisible until it
    # breaks the NEXT run.
    pkill -P "$p" 2>/dev/null || true
    kill "$p" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

echo "[stack] server  : ${MAP:-see --map arguments}  (link 127.0.0.1:$LINK_PORT)"
"$SERVER" --port "$LINK_PORT" ${ARGS[@]+"${ARGS[@]}"} \
    ${MODULE_ARGS[@]+"${MODULE_ARGS[@]}"} "$@" &
pids+=($!)

sleep 0.6

echo "[stack] edge    : https://127.0.0.1:$WT_PORT/dnp"
# `exec` MATTERS HERE, and its absence leaked an edge on every single run.
#
# `( cd ... && node ... ) &` records the SUBSHELL's pid in $!, not node's. So
# cleanup killed the subshell and left `node src/index.js` orphaned -- and an
# orphaned edge keeps trying to connect to the link port for ever. The next
# stack's server then accepts the STALE edge first, refuses the new one (one
# edge at a time), and the new edge logs
#
#   [edge] upstream connected 127.0.0.1:34500
#   [edge] upstream lost; dropping all sessions
#
# twice a second until you stop it. That reads like a crashing server; it is a
# process from a run you finished an hour ago. Three of them had accumulated
# before this was found.
#
# `exec` replaces the subshell with node, so $! IS node and killing it kills it.
( cd server/edge && exec node src/index.js --port "$WT_PORT" --upstream-port "$LINK_PORT" ) &
pids+=($!)

echo "[stack] web     : http://localhost:$HTTP_PORT/"
DNP_HTTP_PORT="$HTTP_PORT" node tools/devserver.js \
  ${MODULE_ARGS[@]+"${MODULE_ARGS[@]}"} &
pids+=($!)

echo
echo "  open  http://localhost:$HTTP_PORT/?server=https://127.0.0.1:$WT_PORT/dnp"
echo "  (no ?server= gives the solo renderer, which needs none of the above)"
echo
wait
