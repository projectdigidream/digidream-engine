#!/usr/bin/env bash
# Every mob table in maps/mobs/ loads clean, and every table in maps/mobs/bad/
# is refused.
#
# The second half is the point. A loader that accepts everything passes the
# first half perfectly.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LUAJIT="$ROOT/third_party/luajit/src/luajit"

if [ ! -x "$LUAJIT" ]; then
  echo "  skip (no luajit -- run scripts/setup_luajit.sh)"
  exit 0
fi

cd "$ROOT"
fails=0

for f in maps/mobs/*.lua; do
  [ -e "$f" ] || continue
  if out=$("$LUAJIT" tools/mobcheck/check.lua "$f" 2>&1); then
    echo "  ok   $out"
  else
    echo "  FAIL $f does not load:"; echo "$out" | sed 's/^/       /'
    fails=$((fails + 1))
  fi
done

for f in maps/mobs/bad/*.lua; do
  [ -e "$f" ] || continue
  "$LUAJIT" tools/mobcheck/check.lua "$f" >/dev/null 2>&1
  if [ $? -ne 0 ]; then
    echo "  ok   CONTROL: $(basename "$f") is refused"
  else
    echo "  FAIL CONTROL: $(basename "$f") was ACCEPTED and should not be"
    fails=$((fails + 1))
  fi
done

# And the graph BEHAVES, which is a different claim from "the table is well
# formed" -- a runtime whose transitions never fire passes the first perfectly.
for suite in surface behaviour behaviour2 roster cost; do
  if out=$("$LUAJIT" "tools/mobcheck/$suite.lua" 2>&1); then
    echo "$out" | sed 's/^/  /'
  else
    echo "$out" | sed 's/^/  /'
    fails=$((fails + 1))
  fi
done

echo "mobs: $fails failures"
[ "$fails" -eq 0 ]
