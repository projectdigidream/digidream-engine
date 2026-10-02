#!/usr/bin/env bash
#
# Rendering data must never reach the simulation.
#
# The primary guarantee is structural: MapData -- the only view physics and the
# nav graph are ever handed -- does not contain textures, pegging or light
# level. Those live in a parallel RenderData that collision code is never given,
# so `map.sector(s).light_level` does not compile. That is the real defence, and
# it needs no script.
#
# This guards the OTHER direction: someone adding a RenderData parameter to a
# physics routine because it was convenient. That compiles fine, and nothing
# else in the build would object. It would also be an immediate divergence
# between the client and the server the first time a retexture happened -- the
# renderer's data is not in the state hash, so a physics rule reading it would
# desync without the determinism gate ever noticing, which is the worst
# available failure mode.
#
# Scope: the simulation half of core/. The renderer, the map compiler and the
# loader all legitimately touch this data.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
cd "$ROOT"

# Directories whose code runs inside the tick and must be geometry-only.
SIM_DIRS=(core/src/physics core/src/sim core/src/nav)
SIM_HEADERS=(core/include/dnp/physics core/include/dnp/sim core/include/dnp/nav)

# The render-only vocabulary. If one of these appears in the simulation, either
# a rule is reading cosmetic data or a name has been reused for two things --
# and both are worth stopping for.
BANNED='RenderData|SectorRender|SideRender|render_view|light_level|peg_flags|peg_flag|floor_tex|ceil_tex|NO_TEXTURE|intern_texture|MapLight|add_light|light_count'

fail=0
for d in "${SIM_DIRS[@]}" "${SIM_HEADERS[@]}"; do
  [[ -d "$d" ]] || continue
  # Strip comments before scanning, preserving line numbers. A guard that fires
  # on its own documentation is a guard someone eventually disables.
  while IFS= read -r f; do
    stripped="$(sed -e 's://.*::' "$f")"
    hits="$(printf '%s\n' "$stripped" | grep -nE "$BANNED" || true)"
    if [[ -n "$hits" ]]; then
      echo "  FAIL $f references rendering data:"
      printf '%s\n' "$hits" | sed 's/^/    /'
      fail=1
    fi
  done < <(find "$d" -name '*.cpp' -o -name '*.hpp')
done

if [[ $fail -ne 0 ]]; then
  echo
  echo "Rendering attributes are not simulation state. They are not in the state"
  echo "hash, so a physics rule that reads one desyncs without the determinism"
  echo "gate seeing it. Pass the geometry the rule actually needs instead."
  exit 1
fi

echo "  ok   rendering data is absent from physics, sim and nav"
