//
// THE GROUND BETWEEN THE LEVELS.
//
// A composed world puts each map in its own cell with a wide margin around it,
// and until now that margin was void with one 2.5-unit corridor threaded across
// it. A corridor has to find a wall facing the right way, with a clear route,
// that BOTH sides can agree on without talking -- and four of DOOM II's levels
// offer no such wall at all, while the rest gave a thread you walk along
// looking at nothing.
//
// Terrain has none of those constraints. It fills the margin, so there is
// nothing to thread; it meets the level along whatever wall is opened, so there
// is nothing to find; and the agreement problem has an exact answer:
//
//   THE HEIGHT AT A POINT IS A FUNCTION OF THAT POINT IN WORLD SPACE.
//
// Two cells computing the ground along their shared boundary are evaluating the
// same function at the same coordinates, so they agree to the bit, without
// having seen each other -- the same argument SEAM_Z makes for a corridor's
// floor, generalised from one number to a landscape. No negotiation, no
// protocol, no chance of drift.
//
// Everything here is deterministic and integer-seeded for the same reason
// everything else in this engine is: the same world on every machine, and the
// seed written down.
//

// A body steps up half a unit (sim::constants STEP_HEIGHT). Heights are
// quantised to it so a step is exactly a step and never a hair over.
export const STEP = 0.5

// HOW STEEP THE GROUND MAY GET, and it is not a matter of taste.
//
// Value noise with a smoothstep fade has |gradient| <= 1.5 / wavelength per
// unit of lattice amplitude. Summed over the two octaves below and multiplied
// by the tile size, the worst rise between neighbouring tiles is
//
//   AMPLITUDE * TILE * 1.5 * (0.7/WAVE + 0.3/(WAVE/2))
//
// and that has to stay under HALF a step, not under a step: the heights are
// then rounded to whole steps, and rounding two samples that differ by d can
// put them up to d + STEP apart. Bounding the true difference at STEP/2 is what
// bounds the rounded one at STEP.
//
// Getting that wrong is not a subtle error. The first attempt used the whole
// step as the budget and produced a landscape with one-unit cliffs in it --
// measured, not guessed: `worst step between tiles: 1`. Half of those are
// walls, and a wall that looks like a slope is worse than a wall.
//
// This is what makes the ground WALKABLE BY CONSTRUCTION rather than by
// testing: there is no hill anywhere a body cannot climb, because at these
// numbers there cannot be one.
export const TILE = 8
const WAVE = 128
const AMPLITUDE = 2

// Integer hash, 32-bit, no floating point in the mixing. Two cells must agree
// exactly and a double's last bit is not something to stake that on.
function hash2 (x, y) {
  let h = Math.imul(x | 0, 0x27d4eb2d) ^ Math.imul(y | 0, 0x165667b1)
  h ^= h >>> 15
  h = Math.imul(h, 0x2545f491)
  h ^= h >>> 13
  return (h >>> 0) / 4294967296
}

const fade = (t) => t * t * (3 - 2 * t)

function value (x, y) {
  const ix = Math.floor(x), iy = Math.floor(y)
  const fx = fade(x - ix), fy = fade(y - iy)
  const a = hash2(ix, iy), b = hash2(ix + 1, iy)
  const c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1)
  return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy
}

/** Ground height at a WORLD point, quantised to whole steps. */
export function heightAt (wx, wy) {
  const n = value(wx / WAVE, wy / WAVE) * 0.7 +
            value(wx / (WAVE / 2) + 11.3, wy / (WAVE / 2) + 7.1) * 0.3
  return Math.round((n - 0.5) * 2 * AMPLITUDE / STEP) * STEP
}

// WHERE THE WATER IS. A separate channel, on its own lattice, so a river does
// not merely follow a valley -- it crosses the ground where it likes, which is
// what makes it read as a river rather than as the bottom of a bowl.
const WATER_WAVE = 96
/**
 * Is there water here?
 *
 * A narrow band of its own noise, so a river crosses the ground where it likes
 * rather than merely filling the low parts -- which is what makes it read as a
 * river and not as the bottom of a bowl.
 *
 * FLAT, in this version: the river is a surface, not a trench. Sinking the bed
 * by a step costs a step getting out, and that step lands on top of whatever
 * the ground was already doing -- so the bank becomes a one-and-a-half unit
 * wall exactly where somebody is trying to wade ashore. Doing it properly means
 * levelling the banks to the bed, which is a second pass over the field and a
 * second thing to get wrong; until then the water is something you walk through
 * and see, which is honest and cannot trap anybody.
 */
export function waterAt (wx, wy) {
  const n = value(wx / WATER_WAVE + 41.7, wy / WATER_WAVE + 3.9)
  return Math.abs(n - 0.5) < 0.055
}
