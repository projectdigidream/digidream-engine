#!/usr/bin/env bash
#
# A GRAPH THROUGH A REAL SERVER (docs/GRAPHS.md): maps/graph_test.map declares
# graph_test.graph.json as a script; the server must decode it and hand it to
# graphs.load, which accepts it. The control is the same map with its graph made
# invalid: the level must refuse to start, naming the problem.
#
# LOADED, not run: a server does not tick a level nobody is in, so a graph's
# on_tick needs a connected player. Running is test_lua_graphs' job, through the
# same hooks and the same bindings.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ARCH="$(uname -m)"; [[ "$ARCH" == "aarch64" ]] && ARCH=arm64
SERVER=build/server/dnp-server
MAP="build/$ARCH/graph_test.dnpm"
[[ -x "$SERVER" && -f "$MAP" ]] || { echo "graph server: no server or no baked graph_test map; skipped"; exit 0; }

run_for () {   # map, seconds -> the log
  local log; log="$(mktemp)"
  "$SERVER" --port 34617 --map "$1" >"$log" 2>&1 &
  local pid=$!
  sleep "$2"
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  cat "$log"; rm -f "$log"
}

fails=0
out="$(run_for "$MAP" 2)"
if grep -q "a graph, declared by the map" <<<"$out" && grep -q "graph_test.graph.json: 7 nodes, 6 links" <<<"$out"; then
  echo "  ok   the server decoded the map's graph and loaded it"
else
  echo "  FAIL the map's graph did not run:"; grep -E "graph|lua" <<<"$out" | head -10 | sed 's/^/       /'
  fails=$((fails + 1))
fi

# CONTROL: a broken graph beside a copy of the map. The level must not start.
tmp="$(mktemp -d)"
cp "$MAP" "$tmp/graph_test.dnpm"
for s in trigger_vocab.lua triggers.lua graph_vocab.lua graphs.lua; do cp "$(dirname "$MAP")/$s" "$tmp/"; done
sed 's/"type": "once"/"type": "twice"/' "$(dirname "$MAP")/graph_test.graph.json" > "$tmp/graph_test.graph.json"
out="$(run_for "$tmp/graph_test.dnpm" 2)"
rm -rf "$tmp"
if grep -q "no node type 'twice'" <<<"$out" && grep -q "asked for graph" <<<"$out"; then
  echo "  ok   CONTROL: a broken graph is refused, by name, and the level does not start"
else
  echo "  FAIL CONTROL: a broken graph was not refused as it should be"
  fails=$((fails + 1))
fi

# A GRAPH FOR THE WHOLE GAME, beside the map's own. The trigger and graph
# runtimes are `game ... shared` lines: run in the game VM AND kept by every
# level. CONTROL: the same lines without `shared` make the level skip its own
# copies, and the map's graph -- which needs them -- cannot load.
game_module () {   # modifier -> a module dir
  local dir; dir="$(mktemp -d)"
  mkdir -p "$dir/maps"
  for s in trigger_vocab.lua triggers.lua graph_vocab.lua graphs.lua; do
    cp "$(dirname "$MAP")/$s" "$dir/maps/"
    echo "game $s $1" >> "$dir/module.txt"
  done
  cat > "$dir/maps/world.graph.json" <<'JSON'
{ "graph": 1, "name": "world", "vars": { "joined": 0 },
  "nodes": [ { "id": "in", "type": "event", "event": "on_player_enter" },
             { "id": "n",  "type": "add_var", "var": "joined", "value": 1 } ],
  "links": [ ["in.then", "n.in"] ] }
JSON
  echo "game world.graph.json" >> "$dir/module.txt"
  echo "$dir"
}
run_with_module () {   # module dir -> the log
  local log; log="$(mktemp)"
  "$SERVER" --port 34617 --module "$1" --map "$MAP" >"$log" 2>&1 &
  local pid=$!
  sleep 2
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  cat "$log"; rm -f "$log"
}
mod="$(game_module shared)"
out="$(run_with_module "$mod")"
rm -rf "$mod"
if grep -q "world.graph.json: 2 nodes, 1 links" <<<"$out" && grep -q "graph_test.graph.json: 7 nodes, 6 links" <<<"$out"; then
  echo "  ok   a game-wide graph runs, and the map keeps its triggers and its own graph (shared)"
else
  echo "  FAIL with \`game ... shared\`, the game graph and the map's both had to load:"
  grep -E "graph|lua|game" <<<"$out" | head -12 | sed 's/^/       /'
  fails=$((fails + 1))
fi
mod="$(game_module "")"
out="$(run_with_module "$mod")"
rm -rf "$mod"
if ! grep -q "graph_test.graph.json: 7 nodes, 6 links" <<<"$out"; then
  echo "  ok   CONTROL: game-only, the level skips its copies and the map's graph cannot load"
else
  echo "  FAIL CONTROL: without \`shared\` the level still loaded the map's graph -- the check proves nothing"
  fails=$((fails + 1))
fi

if [[ $fails -ne 0 ]]; then echo "graph server: $fails failures"; exit 1; fi
echo "graph server ok"
