#!/usr/bin/env bash
#
# Every baked map can find the scripts it declares.
#
# A map names its scripts, mapc copies them NEXT TO the baked output, and the
# server loads them from there -- so a baked map's dependencies are files in its
# own directory, referenced by convention rather than by any text that names
# them. Nothing greps for them, which is exactly how one got deleted as an
# unreferenced duplicate: it was a build artefact checked in beside its output,
# and it was load-bearing.
#
# The failure is fatal and remote from its cause. The server refuses to start
# the level, exits, and the edge -- a separate process -- keeps accepting
# browser connections that will never be answered.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

missing=0
checked=0
for map in maps/*.map modules/*/maps/*.map; do
  [[ -f "$map" ]] || continue
  baked="${map%.map}.dnpm"
  # Only maps that have actually been baked in place. Maps built into build/
  # are checked by the tests that load them.
  [[ -f "$baked" ]] || continue
  dir="$(dirname "$baked")"
  while read -r _ decl; do
    [[ -n "$decl" ]] || continue
    want="$dir/$(basename "$decl")"
    checked=$((checked + 1))
    if [[ ! -f "$want" ]]; then
      echo "  MISSING $want   (declared by $map)"
      missing=$((missing + 1))
    fi
  done < <(grep -E '^\s*script\s+' "$map" || true)
done

if (( missing > 0 )); then
  echo "map scripts: $missing missing of $checked declared"
  echo "  a baked map whose scripts are gone kills the server at startup;"
  echo "  re-run mapc for that map to copy them back."
  exit 1
fi
echo "map scripts ok: $checked declared, all present"
