#!/usr/bin/env bash
#
# Guards the "no floating point in core/" rule.
#
# The determinism gate (check_determinism.sh) catches divergence that a trace
# actually exercises. This catches the same class of bug earlier and more
# cheaply, by refusing the construct outright: a float that only diverges on a
# code path the golden trace happens not to hit would otherwise sail through CI
# and surface in production as an unreproducible desync.
#
# Comments are stripped before scanning (line numbers preserved), because the
# files that most need this rule are also the ones that explain it -- a guard
# that fires on its own documentation gets switched off, and then guards nothing.
#
# core/tests/ is exempt: tests legitimately use doubles as an accuracy oracle
# and never link into the simulation.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAILED=0

# Blank out /* ... */ blocks (keeping their newlines, so line numbers still
# match the real file) and // comments.
strip_comments() {
  perl -0777 -pe 's{/\*(.*?)\*/}{ my $m = $1; $m =~ tr/\n//cd; $m }gse; s{//[^\n]*}{}g' "$1"
}

FILES=$(find "$ROOT/core/include" "$ROOT/core/src" \
             \( -name '*.hpp' -o -name '*.cpp' \) -type f | sort)

scan() {
  local label="$1" pattern="$2"
  local any=0
  local f hits
  for f in $FILES; do
    hits=$(strip_comments "$f" | grep -nE "$pattern" || true)
    if [[ -n "$hits" ]]; then
      if [[ "$any" -eq 0 ]]; then echo "FAIL: $label"; any=1; FAILED=1; fi
      echo "$hits" | sed "s|^|    ${f#"$ROOT/"}:|"
    fi
  done
}

# Floating-point types. The \b-equivalent guards keep this from firing on
# identifiers that merely contain the word (float_buffer, double_click), which
# would make the guard noisy enough that someone would disable it.
scan "floating-point type in core/" \
     '(^|[^[:alnum:]_])(float|double|long[[:space:]]+double)([^[:alnum:]_]|$)'

# libm. Not correctly-rounded, and results differ across platforms and libc
# versions -- the exact reason core has its own CORDIC.
scan "libm call in core/" \
     '(^|[^[:alnum:]_.>])(sinf?|cosf?|tanf?|atan2f?|sqrtf?|powf?|expf?|logf?|floorf?|ceilf?|fabsf?|fmodf?)[[:space:]]*\('

# Wall-clock and ambient entropy. Simulation time is a tick counter, and all
# randomness comes from Rng, whose state is part of the replicated world state.
scan "wall-clock or ambient randomness in core/" \
     '(^|[^[:alnum:]_])(rand|srand|time|clock|gettimeofday|random_device|mt19937|chrono)([^[:alnum:]_]|$)'

# std::sort is not stable and the relative order of equal elements is
# unspecified -- it genuinely differs between libc++ and libstdc++. Anything the
# simulation sorts must use stable_sort or a total-order comparator.
scan "unstable sort in core/" \
     '(^|[^[:alnum:]_])(std::)?sort[[:space:]]*\('

# Pointer-value ordering. Iterating a container keyed by address, or sorting by
# pointer, produces an order that depends on the allocator -- reproducible on
# one machine, different on another.
scan "ordering by pointer value in core/" \
     '(unordered_map|unordered_set)[[:space:]]*<[[:space:]]*[A-Za-z_][A-Za-z0-9_:]*[[:space:]]*\*'

if [[ "$FAILED" -ne 0 ]]; then
  echo
  echo "core/ must contain only deterministic integer code (docs/physics-contract.md)."
  exit 1
fi

echo "no-floats guard ok: core/include and core/src are integer-only"
