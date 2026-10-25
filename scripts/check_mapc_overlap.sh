#!/usr/bin/env bash
# Which overlapping footprints mapc takes (tools/mapc, "partially overlaps").
# Two rooms meeting at x = 20 and a third sector across the border: a RAISED
# platform is taken (it stands on both rooms); the same one LEVEL with them, or
# SUNK below them (a pit across a border), is refused -- the control that the
# check still checks. An EFFECT sector (`type effect`) level with them is taken:
# it overlaps anything; one joined to a room, or with a slab, is refused.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAPC="$ROOT/build/$(uname -m)/mapc"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
make_map () {   # $1 = the third sector's floor, $2 its attributes, $3 its first edge
  cat <<MAP
spawn 0 5 8 0
sector floor 0 ceil 20 light 160
  vertex  0  0
  vertex 20  0
  vertex 20 16 neighbor 1
  vertex  0 16
end
sector floor 0 ceil 20 light 160
  vertex 20  0
  vertex 40  0
  vertex 40 16
  vertex 20 16 neighbor 0
end
sector floor $1 ceil 20 light 200 ${2:-thickness 2}
  vertex 15  5 ${3:-open}
  vertex 25  5 open
  vertex 25 10 open
  vertex 15 10 open
end
MAP
}
fails=0
expect () {   # $1 = floor, $2 = ok|refused, $3 attributes, $4 first edge
  make_map "$1" "${3:-}" "${4:-}" > "$tmp/m.map"
  if "$MAPC" "$tmp/m.map" "$tmp/m.dnpm" >/dev/null 2>&1; then got=ok; else got=refused; fi
  if [[ $got == "$2" ]]; then echo "  ok   a sector across two rooms, floor $1 ${3:-}${4:+, $4}: $got"
  else echo "  FAIL a sector across two rooms, floor $1 ${3:-}${4:+, $4}: $got, wanted $2"; fails=$((fails + 1)); fi
}
expect 2 ok
expect 0 refused
expect -2 refused
expect 0 ok "type effect" " "
expect 0 refused "type effect" "neighbor 0"
expect 0 refused "type effect thickness 2" " "
[[ $fails == 0 ]] && echo "mapc overlap ok: a raised platform crosses a border, a level or sunk one does not; an effect sector overlaps"
exit $fails
