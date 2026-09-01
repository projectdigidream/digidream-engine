#!/usr/bin/env bash
#
# maps/doors.map and maps/doors_trig.map are the same map.
#
# The two exist so that the Doom line-action implementation and the trigger
# implementation can be compared on identical ground: doors.test.js drives the
# first, triggers.test.js drives the second, and both assert the same ceiling
# reaches the same height at the same speed. That comparison is worth exactly as
# much as the claim that the geometry is identical -- and a copied file is the
# trap this project has already paid for twice ("two programs, one number").
#
# So it is checked rather than promised. Strip comments, blank lines and the
# `script` declarations -- which are the only thing the two are allowed to
# disagree about -- and the rest must be byte-identical. An edit to one that was
# not made to the other fails here, loudly, instead of quietly turning the
# comparison into two different maps that happen to both pass.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

A="maps/doors.map"
B="maps/doors_trig.map"

strip() {
  # Comments, blank lines, script declarations, and trailing whitespace.
  sed -e 's/#.*$//' -e 's/[[:space:]]*$//' -e '/^[[:space:]]*script[[:space:]]/d' "$1" \
    | grep -v '^$' || true
}

if diff <(strip "$A") <(strip "$B") > /tmp/doors_twin.diff 2>&1; then
  n="$(strip "$A" | wc -l | tr -d ' ')"
  echo "doors twin ok: $A and $B agree on all $n geometry lines"
  exit 0
fi

echo "$A and $B have drifted apart:"
sed 's/^/  /' /tmp/doors_twin.diff
echo
echo "  These two must stay the same map -- server/edge/test/triggers.test.js"
echo "  compares the trigger implementation against doors.test.js's numbers, and"
echo "  that comparison is meaningless the moment the geometry differs."
echo "  Make the same edit to both, or delete the twin and say so."
exit 1
