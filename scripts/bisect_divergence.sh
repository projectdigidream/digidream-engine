#!/usr/bin/env bash
#
# Locates the first tick at which two hash streams disagree, then dumps the full
# world state from both sides at that tick and diffs it field by field.
#
# The hash tells you THAT the simulation diverged; this tells you WHICH VALUE
# diverged, which is the difference between a one-hour fix and a one-week hunt.
# Because the trace is a pure function of its seed, both sides can simply be
# re-run to the offending tick rather than needing to have recorded state.
#
#   scripts/bisect_divergence.sh <trace_a.txt> <trace_b.txt> [bin_a] [bin_b]
#
set -euo pipefail

A="${1:?usage: bisect_divergence.sh <trace_a> <trace_b> [bin_a] [bin_b]}"
B="${2:?usage: bisect_divergence.sh <trace_a> <trace_b> [bin_a] [bin_b]}"
BIN_A="${3:-}"
BIN_B="${4:-}"

# Traces are "<tick> <hash>" per line, so the first differing line IS the first
# differing tick -- no binary search needed. The name is kept because that is
# what this step is called, and because a future streaming trace may need one.
#
# awk rather than `diff --unchanged-line-format`: that flag is GNU-only and the
# BSD diff on macOS rejects it, which would leave developers on macOS with a
# broken debugging tool at exactly the moment they needed it.
# `found` guards the END block: awk runs END even after `exit`, so without it a
# divergence on line 1 would print twice and the caller would get "1\n2".
FIRST_LINE=$(awk 'NR==FNR { a[FNR] = $0; n = FNR; next }
                  { if (FNR > n || $0 != a[FNR]) { print FNR; found = 1; exit } }
                  END { if (!found && FNR < n) print FNR + 1 }' "$A" "$B")

if [[ -z "$FIRST_LINE" ]]; then
  echo "no divergence: $A and $B are identical"
  exit 0
fi

TICK=$(sed -n "${FIRST_LINE}p" "$A" | awk '{print $1}')
echo "first divergence at tick $TICK (trace line $FIRST_LINE)"
echo "  $(basename "$A"): $(sed -n "${FIRST_LINE}p" "$A")"
echo "  $(basename "$B"): $(sed -n "${FIRST_LINE}p" "$B")"

if [[ -n "$BIN_A" && -n "$BIN_B" ]]; then
  echo
  echo "--- differing state fields at tick $TICK ---"
  "$BIN_A" --dump "$TICK" > /tmp/dnp_dump_a.txt
  "$BIN_B" --dump "$TICK" > /tmp/dnp_dump_b.txt
  diff /tmp/dnp_dump_a.txt /tmp/dnp_dump_b.txt || true
  echo
  echo "state at the last GOOD tick $((TICK - 1)):"
  "$BIN_A" --dump "$((TICK - 1))" > /tmp/dnp_dump_prev.txt
  diff /tmp/dnp_dump_prev.txt /tmp/dnp_dump_a.txt | head -20 || true
else
  echo
  echo "pass the two binaries as arguments 3 and 4 to see the differing fields"
fi
