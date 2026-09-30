//
// THE GROUND IS WALKABLE BECAUSE OF ITS NUMBERS, NOT BECAUSE SOMEBODY WALKED IT.
//
// Two properties carry the whole design, and neither can be checked by looking:
//
//   1. adjacent tiles never differ by more than one step, so there is no hill a
//      body cannot climb ANYWHERE in the world -- not "none found", none
//      possible;
//   2. the height at a world point depends only on that point, so two cells
//      computing their shared boundary agree without having seen each other.
//
// The second is what replaces the corridor's negotiation. Get it wrong and the
// ground has a cliff along every cell edge.
//
import { heightAt, waterAt, TILE, STEP } from './terrain.mjs'

let checks = 0, fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// (1) NO CLIFFS, over a span far wider than any cell.
{
  let worst = 0, lo = Infinity, hi = -Infinity
  for (let y = -900; y <= 900; y += TILE) {
    for (let x = -900; x <= 900; x += TILE) {
      const h = heightAt(x, y)
      worst = Math.max(worst, Math.abs(heightAt(x + TILE, y) - h),
                              Math.abs(heightAt(x, y + TILE) - h))
      lo = Math.min(lo, h); hi = Math.max(hi, h)
    }
  }
  check(worst <= STEP, `no tile is more than one step from its neighbour (worst ${worst})`)
  // CONTROL: the ground is not flat. A constant field satisfies the check above
  // perfectly and is not a landscape.
  check(hi - lo >= 2, `CONTROL: and it is not flat (${lo} .. ${hi})`)
  // Every height is a whole number of steps, so a step is exactly a step.
  let ragged = 0
  for (let y = 0; y < 200; y += TILE) for (let x = 0; x < 200; x += TILE) {
    if (Math.abs(heightAt(x, y) / STEP - Math.round(heightAt(x, y) / STEP)) > 1e-9) ragged++
  }
  check(ragged === 0, 'and every height is a whole number of steps')
}

// (2) THE SAME POINT IS THE SAME HEIGHT, whoever asks.
//
// This is the cross-cell agreement, stated as what it actually is: the function
// has no hidden state and no dependence on which cell called it. Asked in a
// different order, interleaved with other queries, it answers the same.
{
  const pitch = 224
  let disagreements = 0
  for (let t = 0; t <= pitch; t += TILE) {
    // The boundary between cell (3,1) and cell (4,1), evaluated as each of them
    // would: once as "my local x = pitch", once as "my local x = 0".
    const west = heightAt(3 * pitch + pitch, 1 * pitch + t)
    const east = heightAt(4 * pitch + 0, 1 * pitch + t)
    if (west !== east) disagreements++
  }
  check(disagreements === 0, 'two cells agree along their shared boundary')
  // CONTROL: the boundary is not simply the same number all the way along --
  // otherwise "they agree" is satisfied by a flat edge.
  const along = new Set()
  for (let t = 0; t <= pitch; t += TILE) along.add(heightAt(4 * pitch, 1 * pitch + t))
  check(along.size > 2, `CONTROL: and that boundary has ${along.size} different heights on it`)
}

// (3) WATER IS A RIBBON, not a lake and not a drizzle.
{
  let wet = 0, n = 0
  for (let y = 0; y < 800; y += TILE) for (let x = 0; x < 800; x += TILE) { if (waterAt(x, y)) wet++; n++ }
  const pct = 100 * wet / n
  check(pct > 3 && pct < 30, `water covers ${pct.toFixed(1)}% of the ground`)
}

console.log(`\nterrain: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
