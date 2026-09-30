//
// The apron (tools/compose/apron.mjs): the ground cut to meet a level's edge.
//
// What matters is that the pieces TILE the region exactly -- no gap, no
// overlap -- and that every piece edge is exactly one thing: another piece,
// one tile edge, or one wall of the level. A gap is a hole to fall into, an
// overlap is two floors in one place, and an edge that spans two things is a
// wall the engine will not open.
//
//   node tools/compose/apron.test.mjs
//
import { outerOutlines, apronPieces, triangulate, polygonArea, edgeKey } from './apron.mjs'

let pass = 0, fail = 0
const check = (ok, what) => { ok ? ++pass : (++fail, console.log(`  FAIL ${what}`)) }

const P = (x, y) => ({ x, y })

// A level of two rooms, an L: its walls (with the level on the left) as edges.
function lLevel () {
  const a = [P(10, 10), P(20, 10), P(20, 14), P(14, 14), P(14, 20), P(10, 20)]
  const edges = []
  for (let i = 0; i < a.length; i++) edges.push({ a: a[i], b: a[(i + 1) % a.length], ref: i })
  return { pts: a, edges }
}

function coverage (rect, outline, T) {
  const boundary = [...outline]
  for (let x = rect.x0; x <= rect.x1; x += T) boundary.push(P(x, rect.y0), P(x, rect.y1))
  for (let y = rect.y0; y <= rect.y1; y += T) boundary.push(P(rect.x0, y), P(rect.x1, y))
  const pieces = apronPieces(rect, [outline], boundary)
  return { pieces, boundary }
}

{
  const { pts, edges } = lLevel()
  const loops = outerOutlines(edges)
  check(loops && loops.length === 1 && loops[0].pts.length === 6, 'an L-shaped level has one outline')

  const rect = { x0: 8, y0: 8, x1: 24, y1: 24 }
  const { pieces } = coverage(rect, pts, 4)
  check(pieces !== null, 'the apron triangulates')
  const total = pieces.reduce((s, p) => s + polygonArea(p), 0)
  const want = 16 * 16 - polygonArea(pts)
  check(Math.abs(total - want) < 1e-6, `the pieces cover exactly the apron (${total} vs ${want})`)
  check(pieces.every((p) => polygonArea(p) > 0), 'every piece winds counter-clockwise')

  // Every piece edge: another piece's reversed, a wall of the level reversed,
  // or one tile's length of the rectangle.
  const pieceEdges = new Map()
  for (const p of pieces) for (let i = 0; i < p.length; i++) pieceEdges.set(edgeKey(p[i], p[(i + 1) % p.length]), p)
  const walls = new Set(edges.map((e) => edgeKey(e.a, e.b)))
  let bad = 0, toWall = 0, toRect = 0, inner = 0
  for (const p of pieces) {
    for (let i = 0; i < p.length; i++) {
      const a = p[i], b = p[(i + 1) % p.length]
      if (pieceEdges.has(edgeKey(b, a))) { ++inner; continue }
      if (walls.has(edgeKey(b, a))) { ++toWall; continue }
      const onRect = (a.x === b.x && (a.x === rect.x0 || a.x === rect.x1)) ||
                     (a.y === b.y && (a.y === rect.y0 || a.y === rect.y1))
      if (onRect && Math.abs(Math.hypot(b.x - a.x, b.y - a.y) - 4) < 1e-9) { ++toRect; continue }
      ++bad
    }
  }
  check(bad === 0, `every piece edge is exactly one thing (${bad} are not)`)
  check(toWall === 6, `every wall of the level is met exactly once (${toWall} of 6)`)
  check(toRect === 16, `every tile edge round the rectangle is met exactly once (${toRect} of 16)`)
  check(inner > 0, 'and the pieces meet each other')
}

{
  // CONTROL: without putting the tile points back, a piece edge spans several
  // tile edges -- which is the check above able to fail.
  const { pts } = lLevel()
  const rect = { x0: 8, y0: 8, x1: 24, y1: 24 }
  const pieces = apronPieces(rect, [pts], pts)
  let spans = 0
  for (const p of pieces) {
    for (let i = 0; i < p.length; i++) {
      const a = p[i], b = p[(i + 1) % p.length]
      if (a.y === rect.y0 && b.y === rect.y0 && Math.abs(b.x - a.x) > 4) ++spans
    }
  }
  check(spans > 0, `CONTROL: without the tile points, an edge spans several tiles (${spans})`)
}

{
  // A chain that goes nowhere is refused, not guessed at.
  const edges = [{ a: P(0, 0), b: P(1, 0) }, { a: P(1, 0), b: P(2, 0) }]
  check(outerOutlines(edges) === null, 'an outline that does not close is refused')
  // Two rooms touching corner to corner: two loops, not one crossing itself.
  const sq = (x, y) => [P(x, y), P(x + 2, y), P(x + 2, y + 2), P(x, y + 2)]
  const loopEdges = (pts) => pts.map((p, i) => ({ a: p, b: pts[(i + 1) % pts.length] }))
  const pinch = outerOutlines([...loopEdges(sq(0, 0)), ...loopEdges(sq(2, 2))])
  check(pinch && pinch.length === 2 && pinch.every((l) => l.pts.length === 4),
        `a pinch makes two loops (${pinch?.map((l) => l.pts.length).join(', ')})`)
  // A single trace that passes through one point twice -- a level shaped like
  // two rooms joined at a corner, traced as one loop -- comes out as two.
  const eight = [P(0, 0), P(2, 0), P(2, 2), P(4, 2), P(4, 4), P(2, 4), P(2, 2), P(0, 2)]
  const eightLoops = outerOutlines(loopEdges(eight))
  check(eightLoops && eightLoops.length === 2, `a loop through one point twice is split (${eightLoops?.length})`)
  // An island inside a void of the level is not an outline of its own.
  const big = [P(0, 0), P(20, 0), P(20, 20), P(0, 20)]
  const voidLoop = [P(5, 5), P(5, 15), P(15, 15), P(15, 5)]     // clockwise: a void
  const island = [P(8, 8), P(12, 8), P(12, 12), P(8, 12)]
  const nested = outerOutlines([...loopEdges(big), ...loopEdges(voidLoop), ...loopEdges(island)])
  check(nested && nested.length === 1 && nested[0].pts.length === 4 && nested[0].pts[1].x === 20,
        `an island in a void is dropped (${nested?.length} outline(s))`)
  // CONTROL: the same island on its own, outside, is an outline.
  const apart = outerOutlines([...loopEdges(big), ...loopEdges([P(30, 0), P(34, 0), P(34, 4), P(30, 4)])])
  check(apart && apart.length === 2, `CONTROL: a separate part outside is kept (${apart?.length})`)
  // A void inside the level (a clockwise loop of walls) is not an outline.
  const v = [P(0, 0), P(0, 4), P(4, 4), P(4, 0)]
  const ve = v.map((p, i) => ({ a: p, b: v[(i + 1) % 4] }))
  check(outerOutlines(ve).length === 0, 'a void inside the level is not an outline')
}

{
  // Collinear points on the level's edge (Doom splits walls where the far side
  // changes) survive into the pieces, so each wall is still met exactly.
  const a = [P(10, 10), P(15, 10), P(20, 10), P(20, 20), P(10, 20)]
  const edges = a.map((p, i) => ({ a: p, b: a[(i + 1) % a.length] }))
  const rect = { x0: 8, y0: 8, x1: 24, y1: 24 }
  const { pieces } = coverage(rect, a, 4)
  const got = new Set()
  for (const p of pieces) for (let i = 0; i < p.length; i++) got.add(edgeKey(p[(i + 1) % p.length], p[i]))
  check(edges.every((e) => got.has(edgeKey(e.a, e.b))), 'a wall split in two is met as two walls')
}

{
  // A triangulation of a plain square with a square hole, as a sanity floor.
  const tris = triangulate([P(0, 0), P(10, 0), P(10, 10), P(0, 10)],
                           [[P(3, 3), P(3, 7), P(7, 7), P(7, 3)]])
  const total = tris.reduce((s, t) => s + polygonArea(t), 0)
  check(Math.abs(total - 84) < 1e-9, `a square with a hole triangulates to its area (${total})`)
}

console.log(`apron: ${pass} passed, ${fail} failed`)
process.exit(fail === 0 ? 0 : 1)
