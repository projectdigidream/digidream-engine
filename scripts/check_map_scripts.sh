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
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

# AND THAT THE COPY IS THE SOURCE.
#
# Present is not enough. A map that declares `../../../maps/scripts/mobs.lua`
# runs the copy beside its own .dnpm, so editing the source and not re-baking
# leaves the level running the version from before the change -- silently, and
# looking exactly like the change not working. That is this repository's oldest
# trap ("two programs, one number") wearing a build system as a disguise, and
# the fix is one `cp` that nobody remembers to run.
#
# Only declarations with a directory in them are compared: a bare name IS the
# file beside the map and has no source anywhere else.
missing=0
stale=0
checked=0
reported=
for map in maps/*.map ../modules/*/maps/*.map; do
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
      continue
    fi
    [[ "$decl" == */* ]] || continue
    src="$dir/$decl"
    [[ -f "$src" ]] || continue
    if ! cmp -s "$src" "$want"; then
      # Once per copy, not once per map that declares it: thirty-two Doom
      # levels declare the same mobs.lua, and thirty-two identical lines about
      # one file buries the second file that is also stale.
      if [[ " $reported " != *" $want "* ]]; then
        reported="$reported $want"
        echo "  STALE   $want"
        srcnorm="$(cd "$(dirname "$src")" && pwd -P)/$(basename "$src")"
        echo "          differs from ${srcnorm#"$ROOT/"}   (declared by $map among others)"
        stale=$((stale + 1))
      fi
    fi
  done < <(grep -E '^\s*script\s+' "$map" || true)
done

# AND A MODULE'S GAME SCRIPTS (docs/MODULES.md, "Game scripts"): `game <file>`
# in module.txt names a file in the module's maps/ directory, which is the copy
# a packaged game carries -- so it must be there, and where maps/scripts has a
# file of the same name, it must be that file.
for mt in ../modules/*/module.txt; do
  [[ -f "$mt" ]] || continue
  mdir="$(dirname "$mt")/maps"
  while read -r _ gfile _modifier; do
    [[ -n "$gfile" ]] || continue
    want="$mdir/$gfile"
    checked=$((checked + 1))
    if [[ ! -f "$want" ]]; then
      echo "  MISSING $want   (a game script of $mt)"
      missing=$((missing + 1))
      continue
    fi
    src="maps/scripts/$(basename "$gfile")"
    if [[ -f "$src" ]] && ! cmp -s "$src" "$want"; then
      if [[ " $reported " != *" $want "* ]]; then
        reported="$reported $want"
        echo "  STALE   $want"
        echo "          differs from $src   (a game script of $mt)"
        stale=$((stale + 1))
      fi
    fi
  done < <(grep -E '^\s*game\s+' "$mt" || true)
done

if (( missing > 0 || stale > 0 )); then
  (( missing > 0 )) && {
    echo "map scripts: $missing missing of $checked declared"
    echo "  a baked map whose scripts are gone kills the server at startup;"
    echo "  re-run mapc for that map to copy them back."
  }
  (( stale > 0 )) && {
    echo "map scripts: $stale stale of $checked declared"
    echo "  the level runs the copy, so it is running the code from before your"
    echo "  edit; re-run mapc for that map, or copy the source over the copy."
  }
  exit 1
fi
echo "map scripts ok: $checked declared, all present and current"
