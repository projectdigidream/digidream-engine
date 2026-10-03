#!/usr/bin/env bash
#
# The generated half of a vocabulary's documentation is current.
#
# Each of these documents is two halves with a marker between them: prose a
# person wrote, then a reference emitted from the vocabulary table by
# tools/vocabdoc/emit.lua. This regenerates the second half and diffs it.
#
# WHY A DIFF RATHER THAN A HEADING CHECK. docs/TRIGGERS.md is checked by asking
# whether every vocabulary name has a `### `name`` section, which catches a
# MISSING entry and nothing else: a name documented with last month's arguments
# passes. A diff catches that, and it is only possible because the reference is
# generated.
#
# The prose is not checked and must not be -- it is the half that explains why,
# and no generator writes that.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

LUAJIT=third_party/luajit/src/luajit
if [[ ! -x "$LUAJIT" ]]; then
  echo "vocab docs: no $LUAJIT; skipped"
  exit 0
fi

# The line that separates the two halves. Everything after it is generated.
MARKER='<!-- generated from the vocabulary below this line; see engine/tools/vocabdoc -->'

fails=0
checked=0

check_one() {
  local doc="$1" vocab="$2" global="$3"
  checked=$((checked + 1))

  if ! grep -qF -- "$MARKER" "$doc"; then
    echo "  FAIL $doc has no generator marker"
    echo "       add this line where the reference should begin:"
    echo "       $MARKER"
    fails=$((fails + 1))
    return
  fi

  local tmp_have tmp_want
  tmp_have="$(mktemp)"; tmp_want="$(mktemp)"
  # Everything strictly after the marker, which is what the emitter owns. The
  # blank line the marker is followed by is part of the document, not of the
  # generated text, so it is dropped from both sides rather than argued about.
  awk -v m="$MARKER" 'found { print } index($0, m) { found = 1 }' "$doc" \
    | sed '/./,$!d' > "$tmp_have"
  "$LUAJIT" tools/vocabdoc/emit.lua "$vocab" "$global" > "$tmp_want"

  if diff -q "$tmp_have" "$tmp_want" >/dev/null 2>&1; then
    echo "  ok   $doc matches $vocab"
  else
    echo "  FAIL $doc does not match $vocab"
    diff -u "$tmp_have" "$tmp_want" | head -40 | sed 's/^/       /'
    echo "       regenerate with:"
    echo "       $LUAJIT tools/vocabdoc/emit.lua $vocab $global"
    fails=$((fails + 1))
  fi
  rm -f "$tmp_have" "$tmp_want"
}

echo "vocab docs: the generated reference sections are current"
check_one ../docs/MOBS.md    maps/scripts/mob_vocab.lua    mob_vocab
check_one ../docs/WEAPONS.md maps/scripts/weapon_vocab.lua weapon_vocab
check_one ../docs/GRAPHS.md  maps/scripts/graph_vocab.lua  graph_vocab

# THE CONTROL. A checker that compared nothing would report every document clean,
# and so would one pointed at a file that does not exist. This asserts the diff
# really fires: the mob vocabulary's reference must NOT match the weapon
# vocabulary's, which it trivially does not -- and if it somehow did, every
# result above would be meaningless.
if "$LUAJIT" tools/vocabdoc/emit.lua maps/scripts/mob_vocab.lua mob_vocab \
   | diff -q - <("$LUAJIT" tools/vocabdoc/emit.lua maps/scripts/weapon_vocab.lua \
                 weapon_vocab) >/dev/null 2>&1; then
  echo "  FAIL CONTROL: the two vocabularies emitted identical references, so"
  echo "       the comparison above is not looking at anything"
  fails=$((fails + 1))
else
  echo "  ok   CONTROL: the two vocabularies emit different references"
fi

if [[ $fails -ne 0 ]]; then
  echo "vocab docs: $fails of $checked stale"
  exit 1
fi
echo "vocab docs ok: $checked documents current"
