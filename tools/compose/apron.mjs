//
// THE GROUND RIGHT UP TO A LEVEL'S WALLS.
//
// The terrain is square tiles, and a Doom level's edge is anything but square.
// Tiles that would overlap the level were simply left out, which left a strip
// of nothing all the way round every level -- and the tile beside it ended in
// its own wall, as high as the sky. That was the cliff around every building.
//
// So the strip is filled exactly. An APRON is the region between a rectangle on
// the tile grid (the ground stops at its edge) and the level's own outer outline
// (the level starts at its edge). It is cut into triangles, each a sector, and
// every edge of every triangle lies exactly on something: another triangle, a
// tile's edge, or one of the level's outer walls. Those walls then stop being
// walls: they are joined to the ground beside them.
//
// Pure geometry: points in, polygons out. The sectors, heights and links are
// compose.mjs's; this is tested on its own in apron.test.mjs.
//
// TEST SCAFFOLDING, like everything in tools/compose (see compose.mjs).
//

export const EPS = 1e-7

export const key = (p) => `${p.x.toFixed(4)},${p.y.toFixed(4)}`
export const edgeKey = (a, b) => `${key(a)}|${key(b)}`

export function area2 (a, b, c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)
}

export function polygonArea (pts) {
  let s = 0
  for (let i = 0; i < pts.length; i++) {
    const a = pts[i], b = pts[(i + 1) % pts.length]
    s += a.x * b.y - b.x * a.y
  }
  return s / 2
}

/**
 * THE LEVEL'S OUTER OUTLINE(S): the loops made by its walls that have nothing
 * on the far side, traced with the level on the left. Counter-clockwise loops
 * are outer boundaries (one per separate part of the level); clockwise ones are
 * voids inside it, and are dropped.
 *
 * `edges` is every such wall as { a, b, ref } with a -> b the direction its own
 * sector winds (counter-clockwise, so the outside is on the right). `ref` is
 * carried through untouched.
 *
 * AT A PINCH -- a point where two walls leave, two parts of the level
 * touching corner to corner -- the trace takes the sharpest LEFT turn, which
 * keeps each part's loop to itself rather than making one loop that crosses
 * over at the pinch.
 *
 * AN ISLAND -- a part of the level inside a void of another part -- is inside
 * the outline already, and is dropped: the ground does not reach it.
 *
 * Returns null when the walls do not close into loops (a chain that ends),
 * because a wrong outline makes a wrong apron and the caller has a fallback.
 */
export function outerOutlines (edges) {
  const from = new Map()
  for (const e of edges) {
    const k = key(e.a)
    if (!from.has(k)) from.set(k, [])
    from.get(k).push(e)
  }
  const used = new Set()
  const next = (cur) => {
    const outs = (from.get(key(cur.b)) ?? []).filter((e) => !used.has(e))
    if (outs.length <= 1) return outs[0] ?? null
    // Sharpest left: the largest counter-clockwise angle from the reverse of
    // the way we came in... measured as the smallest turn to the left.
    const inx = cur.b.x - cur.a.x, iny = cur.b.y - cur.a.y
    let best = null, bestTurn = -Infinity
    for (const o of outs) {
      const ox = o.b.x - o.a.x, oy = o.b.y - o.a.y
      const turn = Math.atan2(inx * oy - iny * ox, inx * ox + iny * oy)   // +left, -right
      if (turn > bestTurn) { bestTurn = turn; best = o }
    }
    return best
  }
  const loops = []
  for (const e of edges) {
    if (used.has(e)) continue
    const loop = []
    let cur = e
    while (cur && !used.has(cur)) {
      used.add(cur)
      loop.push(cur)
      cur = next(cur)
      if (cur === null && key(loop[loop.length - 1].b) === key(e.a)) cur = e
    }
    if (!cur || key(loop[loop.length - 1].b) !== key(e.a)) return null   // did not close
    loops.push(loop)
  }
  // A LOOP THAT TOUCHES ITSELF -- passes through one point twice -- is two
  // loops sharing that point. Split there: the part that winds the level's
  // way is outline, the part that winds the other way is a void inside it.
  const simple = []
  for (const loop of loops) {
    const stack = []
    const at = new Map()
    for (const e of loop) {
      const k = key(e.a)
      if (at.has(k)) {
        const from = at.get(k)
        const sub = stack.splice(from)
        for (const s of sub) at.delete(key(s.a))
        simple.push(sub)
      }
      at.set(k, stack.length)
      stack.push(e)
    }
    if (stack.length) simple.push(stack)
  }
  const outer = simple
    .map((loop) => ({ edges: loop, pts: loop.map((e) => e.a) }))
    .filter((l) => polygonArea(l.pts) > EPS)
  // Islands: an outer loop inside another.
  return outer.filter((l) => !outer.some((o) => o !== l && contains(o.pts, l.pts)))
}

// Whether polygon `outer` contains polygon `inner`: every point of inner that is
// not on outer's own boundary is inside it (touching loops share points).
function contains (outer, inner) {
  const onOuter = new Set(outer.map(key))
  let tested = 0
  for (const p of inner) {
    if (onOuter.has(key(p))) continue
    ++tested
    if (!pointIn(outer, p)) return false
    if (tested >= 3) break
  }
  return tested > 0 && polygonArea(outer) > polygonArea(inner)
}

function pointIn (poly, p) {
  let inside = false
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const a = poly[i], b = poly[j]
    if ((a.y > p.y) !== (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside
  }
  return inside
}

// Drop repeated and exactly collinear points: the triangulator wants corners.
function corners (pts) {
  let out = pts.slice()
  let changed = true
  while (changed && out.length > 3) {
    changed = false
    for (let i = 0; i < out.length && out.length > 3; i++) {
      const a = out[(i + out.length - 1) % out.length], b = out[i], c = out[(i + 1) % out.length]
      const dup = Math.abs(a.x - b.x) < EPS && Math.abs(a.y - b.y) < EPS
      if (dup || Math.abs(area2(a, b, c)) < EPS) { out.splice(i, 1); changed = true; i-- }
    }
  }
  return out
}

// Whether segments pq and ab cross properly (not merely touch at an end).
function crosses (p, q, a, b) {
  const d1 = area2(p, q, a), d2 = area2(p, q, b), d3 = area2(a, b, p), d4 = area2(a, b, q)
  return ((d1 > EPS && d2 < -EPS) || (d1 < -EPS && d2 > EPS)) &&
         ((d3 > EPS && d4 < -EPS) || (d3 < -EPS && d4 > EPS))
}

function insideStrict (a, b, c, p) {
  return area2(a, b, p) > EPS && area2(b, c, p) > EPS && area2(c, a, p) > EPS
}

function insideTri (a, b, c, p) {
  return area2(a, b, p) >= -EPS && area2(b, c, p) >= -EPS && area2(c, a, p) >= -EPS
}
const same = (p, q) => Math.abs(p.x - q.x) < EPS && Math.abs(p.y - q.y) < EPS

// Join a hole (clockwise) into a polygon (counter-clockwise) by a bridge from
// the hole's rightmost point to a vertex of the polygon it can see.
function bridge (poly, hole) {
  let mi = 0
  for (let i = 1; i < hole.length; i++) if (hole[i].x > hole[mi].x) mi = i
  const m = hole[mi]
  // The nearest polygon edge a ray to +x from m crosses.
  let best = Infinity, pi = -1
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length]
    if ((a.y > m.y) === (b.y > m.y) && !(a.y === m.y && b.y === m.y)) {
      if (!(Math.min(a.y, b.y) <= m.y && m.y <= Math.max(a.y, b.y))) continue
    }
    if (Math.min(a.y, b.y) > m.y || Math.max(a.y, b.y) < m.y || a.y === b.y) continue
    const x = a.x + (m.y - a.y) * (b.x - a.x) / (b.y - a.y)
    if (x < m.x - EPS || x >= best) continue
    best = x
    pi = a.x > b.x ? i : (i + 1) % poly.length
  }
  if (pi < 0) return null
  // Any reflex polygon vertex inside the triangle (m, hit, poly[pi]) would block
  // the view; the one closest in angle to the ray is visible instead.
  const hit = { x: best, y: m.y }
  let p = pi
  let bestTan = Infinity
  for (let i = 0; i < poly.length; i++) {
    const v = poly[i]
    if (i === pi || v.x < m.x - EPS) continue
    const prev = poly[(i + poly.length - 1) % poly.length], next = poly[(i + 1) % poly.length]
    const reflex = area2(prev, v, next) <= 0
    if (!reflex) continue
    if (!insideTri(m, hit, poly[pi], v) && !insideTri(m, poly[pi], hit, v)) continue
    const tan = Math.abs(v.y - m.y) / Math.max(EPS, v.x - m.x)
    if (tan < bestTan) { bestTan = tan; p = i }
  }
  // VERIFIED, not assumed. With several holes already bridged in, the polygon
  // is full of long thin reentrant spikes, and the first-visible rule above can
  // pick a vertex the segment to which crosses one. Then any vertex that IS
  // visible, nearest first.
  const visible = (q) => {
    for (let i = 0; i < poly.length; i++) {
      const a = poly[i], b = poly[(i + 1) % poly.length]
      if (same(a, q) || same(b, q) || same(a, m) || same(b, m)) continue
      if (crosses(m, q, a, b)) return false
    }
    for (let i = 0; i < hole.length; i++) {
      const a = hole[i], b = hole[(i + 1) % hole.length]
      if (same(a, m) || same(b, m)) continue
      if (crosses(m, q, a, b)) return false
    }
    return true
  }
  if (!visible(poly[p])) {
    const order = poly.map((q, i) => ({ i, d: (q.x - m.x) ** 2 + (q.y - m.y) ** 2 }))
      .sort((u, v) => u.d - v.d)
    const found = order.find(({ i }) => visible(poly[i]))
    if (!found) return null
    p = found.i
  }
  // THE RIGHT COPY. A vertex several holes have already been bridged to is in
  // the polygon several times over, and only one of those copies opens onto
  // the side the new hole is on. Bridging to another makes a polygon that folds
  // over itself -- no edge crosses another, but no ear can be clipped either.
  const wedge = (i) => {
    const prev = poly[(i + poly.length - 1) % poly.length], v = poly[i], next = poly[(i + 1) % poly.length]
    if (area2(prev, v, next) >= 0) return area2(prev, v, m) >= 0 && area2(v, next, m) >= 0
    return !(area2(prev, v, m) < 0 && area2(v, next, m) < 0)
  }
  if (!wedge(p)) {
    for (let i = 0; i < poly.length; i++) {
      if (i !== p && same(poly[i], poly[p]) && wedge(i)) { p = i; break }
    }
  }
  const ring = []
  for (let k = 0; k < hole.length; k++) ring.push(hole[(mi + k) % hole.length])
  // ... poly[p], m, (hole round), m', poly[p]' ...
  return [...poly.slice(0, p + 1), ...ring, { ...m }, { ...poly[p] }, ...poly.slice(p + 1)]
}

/**
 * Triangulate a counter-clockwise polygon with clockwise holes. Ear clipping,
 * O(n^2), which is nothing at a few hundred points. Returns triangles as
 * [a, b, c] of the input point objects (or copies at the same place), or null.
 */
export function triangulate (outer, holes) {
  let poly = corners(outer)
  const hs = holes.map(corners).filter((h) => h.length >= 3)
  hs.sort((a, b) => Math.max(...b.map((p) => p.x)) - Math.max(...a.map((p) => p.x)))
  for (const h of hs) {
    poly = bridge(poly, h)
    if (!poly) { triangulate.why = 'a hole could not be bridged in'; return null }
  }
  const tris = []
  const pts = poly.slice()
  let guard = pts.length * pts.length + 10
  // Two passes per ear: first a point ON a candidate's edge blocks it, which
  // is the safe rule; only when nothing can be clipped that way does a point
  // have to be strictly INSIDE to block. Points on an edge are what the seams
  // of bridged-in holes are made of, and the safe rule alone can leave every
  // remaining ear blocked by one.
  const clipOne = (strict) => {
    for (let i = 0; i < pts.length; i++) {
      const a = pts[(i + pts.length - 1) % pts.length], b = pts[i], c = pts[(i + 1) % pts.length]
      if (area2(a, b, c) <= EPS) continue
      let blocked = false
      for (let j = 0; j < pts.length && !blocked; j++) {
        const p = pts[j]
        if (p === a || p === b || p === c || same(p, a) || same(p, b) || same(p, c)) continue
        if (strict ? insideStrict(a, b, c, p) : insideTri(a, b, c, p)) blocked = true
      }
      if (blocked) continue
      tris.push([a, b, c])
      pts.splice(i, 1)
      return true
    }
    return false
  }
  while (pts.length > 3 && guard-- > 0) {
    const clipped = clipOne(false) || clipOne(true)
    if (!clipped) {
      // Nothing clean to clip. A zero-area corner is a leftover of the bridge;
      // dropping it loses nothing.
      let dropped = false
      for (let i = 0; i < pts.length; i++) {
        const a = pts[(i + pts.length - 1) % pts.length], b = pts[i], c = pts[(i + 1) % pts.length]
        if (Math.abs(area2(a, b, c)) <= EPS) { pts.splice(i, 1); dropped = true; break }
      }
      if (!dropped) { triangulate.why = `ear clipping stuck with ${pts.length} points left`; triangulate.stuck = pts.slice(); return null }
    }
  }
  if (pts.length === 3 && area2(pts[0], pts[1], pts[2]) > EPS) tris.push([pts[0], pts[1], pts[2]])
  return tris
}

/**
 * The apron as polygons: `rect` {x0, y0, x1, y1} minus the `outlines` (each
 * counter-clockwise), with every point in `boundary` that lies on a piece's
 * edge put back into it, so each piece edge is exactly one tile edge or one
 * wall of the level. Returns counter-clockwise polygons, or null.
 */
export function apronPieces (rect, outlines, boundary) {
  const outer = [{ x: rect.x0, y: rect.y0 }, { x: rect.x1, y: rect.y0 },
                 { x: rect.x1, y: rect.y1 }, { x: rect.x0, y: rect.y1 }]
  const holes = outlines.map((o) => o.slice().reverse())
  const tris = triangulate(outer, holes)
  if (!tris) return null
  const pieces = []
  for (const t of tris) {
    const poly = []
    for (let e = 0; e < 3; e++) {
      const p = t[e], q = t[(e + 1) % 3]
      poly.push({ x: p.x, y: p.y })
      const dx = q.x - p.x, dy = q.y - p.y
      const len2 = dx * dx + dy * dy
      const on = []
      for (const b of boundary) {
        if (same(b, p) || same(b, q)) continue
        if (Math.abs(area2(p, q, b)) > EPS * Math.sqrt(len2) * 10) continue
        const s = ((b.x - p.x) * dx + (b.y - p.y) * dy) / len2
        if (s <= EPS || s >= 1 - EPS) continue
        on.push({ s, b })
      }
      on.sort((u, v) => u.s - v.s)
      for (const { b } of on) poly.push({ x: b.x, y: b.y })
    }
    pieces.push(poly)
  }
  return pieces
}
