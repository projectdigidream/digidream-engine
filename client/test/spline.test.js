//
// Spline tests (design §4.1).
//
// The camera's whole path feature rests on this file being a CURVE that is
// parameterised by ARC LENGTH. Both halves of that are easy to fake and neither
// is visible in a result that merely looks plausible, so most of what follows
// is a control: a claim about the curve is worth nothing without the matching
// demonstration that the polyline, or the raw parameter, fails it.
//
import { buildSpline, ALPHA, SAMPLES_PER_SEGMENT } from '../src/spline.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-6) => Math.abs(a - b) <= eps
const P = (x, y, z) => ({ x, y, z })
const dist = (a, b) => Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z)

// The shape crush.map's floor rail has: chords of 16 and 6 meeting at a right
// angle. Uneven spacing and a hard corner is exactly where a curve differs most
// from its polyline, and where uniform Catmull-Rom misbehaves.
const BENT = [P(2, 2, 1), P(18, 2, 1), P(18, 8, 1)]
const STRAIGHT = [P(0, 0, 0), P(10, 0, 0), P(25, 0, 0)]

const polylineLength = (pts, loop = false) => {
  let n = 0
  const last = loop ? pts.length : pts.length - 1
  for (let i = 0; i < last; i++) n += dist(pts[i], pts[(i + 1) % pts.length])
  return n
}

// Nearest point on the POLYLINE, in the test rather than in the module. The
// module deliberately does not offer this: binding it would invite the belief
// that the polyline and the curve agree about where "nearest" is, and the whole
// reason evaluation lives in JS is that they do not.
const polylineNearest = (pts, q, loop = false) => {
  let best = { distSq: Infinity, along: 0 }
  let acc = 0
  const last = loop ? pts.length : pts.length - 1
  for (let i = 0; i < last; i++) {
    const a = pts[i]
    const b = pts[(i + 1) % pts.length]
    const ab = { x: b.x - a.x, y: b.y - a.y, z: b.z - a.z }
    const L2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z
    let t = L2 === 0 ? 0 : ((q.x - a.x) * ab.x + (q.y - a.y) * ab.y + (q.z - a.z) * ab.z) / L2
    t = Math.max(0, Math.min(1, t))
    const p = { x: a.x + ab.x * t, y: a.y + ab.y * t, z: a.z + ab.z * t }
    const d = (p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2
    if (d < best.distSq) best = { distSq: d, along: acc + Math.sqrt(L2) * t }
    acc += Math.sqrt(L2)
  }
  return best
}

console.log('\n--- it interpolates, and it is NOT the polyline ---')
{
  const s = buildSpline(BENT)
  // Catmull-Rom passes through its control points. Find each one's arc length
  // by walking the segments' endpoints.
  check(near(dist(s.pointAt(0), BENT[0]), 0, 1e-9), 'starts at the first point')
  check(near(dist(s.pointAt(s.length), BENT[2]), 0, 1e-9), 'ends at the last point')

  // Every interior point is a seam, and the curve must interpolate it from BOTH
  // sides. Asserted on the segments directly rather than by scanning arc length:
  // a scan only ever gets near the seam, and an earlier version of this test
  // passed at 1e-6 because a BUG parked the curve on the seam for the width of
  // one sample. A scan cannot tell "passes through" from "stops there".
  let interp = true
  let joins = true
  for (let seg = 0; seg + 1 < s.segmentCount + 1 && seg < s.segmentCount; seg++) {
    if (dist(s.evalSegment(seg, 0), BENT[seg]) > 1e-12) interp = false
    if (dist(s.evalSegment(seg, 1), BENT[seg + 1]) > 1e-12) interp = false
    if (seg + 1 < s.segmentCount &&
        dist(s.evalSegment(seg, 1), s.evalSegment(seg + 1, 0)) > 1e-12) joins = false
  }
  check(interp, 'every segment starts and ends exactly on its control points')
  check(joins, 'and consecutive segments agree at the seam')

  // THE CONTROL. Without this, a buildSpline that quietly returned the polyline
  // passes every other assertion in this file -- it interpolates its points, it
  // has an arc length, it has a nearest. This is the one that says it bends.
  const segLen = dist(BENT[0], BENT[1])
  const chordMid = P((BENT[0].x + BENT[1].x) / 2, (BENT[0].y + BENT[1].y) / 2, 1)
  const curveMid = s.pointAt(segLen / 2)
  check(dist(curveMid, chordMid) > 0.05,
    `CONTROL: the curve at a segment's arc-midpoint is NOT the chord midpoint (off by ${dist(curveMid, chordMid).toFixed(4)})`)
}

console.log('\n--- arc length ---')
{
  const bent = buildSpline(BENT)
  check(bent.length > polylineLength(BENT) + 1e-3,
    `a bend is LONGER than its polyline (${bent.length.toFixed(4)} > ${polylineLength(BENT).toFixed(4)})`)

  // The control, and it is the half that catches a broken parameterisation:
  // smoothing that lengthened a straight line is measuring something else.
  const straight = buildSpline(STRAIGHT)
  check(near(straight.length, polylineLength(STRAIGHT), 1e-6),
    'CONTROL: a collinear path is EXACTLY its polyline length')
}

console.log('\n--- constant speed means constant speed ---')
{
  const s = buildSpline(BENT)
  const cov = (steps) => {
    const mean = steps.reduce((a, b) => a + b, 0) / steps.length
    const varr = steps.reduce((a, b) => a + (b - mean) ** 2, 0) / steps.length
    return Math.sqrt(varr) / mean
  }

  // Marching in ARC LENGTH: every frame covers the same distance.
  const byArc = []
  let prev = s.pointAt(0)
  for (let i = 1; i <= 200; i++) {
    const p = s.pointAt((i / 200) * s.length)
    byArc.push(dist(prev, p)); prev = p
  }
  check(cov(byArc) < 0.005, `marching in arc length is even (CoV ${(cov(byArc) * 100).toFixed(3)}%)`)

  // THE CONTROL: the same march in the curve PARAMETER is not. This is the
  // whole justification for the arc-length table -- without it, a camera runs
  // fast down the straights and slow round the bends.
  const byT = []
  prev = s.evalSegment(0, 0)
  for (let i = 1; i <= 200; i++) {
    const g = (i / 200) * s.segmentCount
    const seg = Math.min(Math.floor(g), s.segmentCount - 1)
    const p = s.evalSegment(seg, g - seg)
    byT.push(dist(prev, p)); prev = p
  }
  check(cov(byT) > 0.10, `CONTROL: marching in the parameter is uneven (CoV ${(cov(byT) * 100).toFixed(2)}%)`)
}

console.log('\n--- nearest is global, and it is not the polyline\'s answer ---')
{
  const s = buildSpline(BENT)
  let worst = 0
  for (let gx = -2; gx <= 24; gx += 2) {
    for (let gy = -2; gy <= 12; gy += 2) {
      const q = P(gx, gy, 1)
      const got = s.nearest(q)
      // Brute force over a far finer sampling than the module's own table.
      let brute = Infinity
      for (let i = 0; i <= 10000; i++) {
        const d = dist(s.pointAt((i / 10000) * s.length), q)
        if (d < brute) brute = d
      }
      worst = Math.max(worst, Math.sqrt(got.distSq) - brute)
    }
  }
  check(worst < 1e-3, `agrees with brute force everywhere (worst excess ${worst.toExponential(2)})`)

  // THE CONTROL, and it is what justifies not binding core's rail_nearest:
  // there exists a query point where the curve's answer and the polyline's are
  // different places. If this ever stops failing, the curve stopped bending and
  // the binding decision was wrong.
  let found = null
  for (let gx = -2; gx <= 24 && !found; gx += 1) {
    for (let gy = -2; gy <= 12 && !found; gy += 1) {
      const q = P(gx, gy, 1)
      const a = s.nearest(q).along
      const b = polylineNearest(BENT, q).along
      if (Math.abs(a - b) > 0.25) found = { q, a, b }
    }
  }
  check(found !== null,
    found ? `CONTROL: at (${found.q.x},${found.q.y}) curve says ${found.a.toFixed(3)} and polyline says ${found.b.toFixed(3)}`
          : 'CONTROL: curve-nearest differs from polyline-nearest somewhere')
}

console.log('\n--- loop and clamp ---')
{
  const open = buildSpline(BENT)
  check(open.pointAt(-5).clamped === true, 'running off the start of an open path reports clamped')
  check(open.pointAt(open.length + 5).clamped === true, 'running off the end reports clamped')
  check(open.pointAt(open.length / 2).clamped === false, 'the middle does not')
  check(near(dist(open.pointAt(open.length + 5), open.pointAt(open.length)), 0, 1e-9),
    'and it stops rather than extrapolating')

  const ring = buildSpline([P(0, 0, 0), P(10, 0, 0), P(10, 10, 0), P(0, 10, 0)], { loop: true })
  check(ring.segmentCount === 4, 'a loop has one segment per point')
  check(ring.pointAt(ring.length * 3.5).clamped === false, 'a loop never clamps')
  check(near(dist(ring.pointAt(0), ring.pointAt(ring.length)), 0, 1e-9),
    'a full lap returns exactly to the start')
}

console.log('\n--- centripetal does not cusp ---')
{
  // Wildly uneven spacing, which is what makes uniform Catmull-Rom fold back.
  const UNEVEN = [P(0, 0, 0), P(20, 0, 0), P(21, 0.5, 0), P(22, 6, 0), P(40, 7, 0)]
  const reversals = (alpha) => {
    const s = buildSpline(UNEVEN, { alpha })
    let n = 0
    let prev = null
    for (let i = 0; i <= 600; i++) {
      const t = s.tangentAt((i / 600) * s.length)
      if (prev) { if (prev.x * t.x + prev.y * t.y + prev.z * t.z < 0) n++ }
      prev = t
    }
    return n
  }
  const cent = reversals(ALPHA.CENTRIPETAL)
  const unif = reversals(ALPHA.UNIFORM)
  check(cent === 0, `centripetal never reverses direction (${cent} reversals)`)
  check(unif > 0, `CONTROL: uniform on the same points does (${unif} reversals)`)
}

console.log('\n--- engine space and THREE space are the same curve ---')
{
  // toThreeX/Y/Z is a signed permutation: (x, y, z) -> (x, z, -y). Linear and
  // an isometry, so the curve and its arc length commute with it. This is what
  // licenses doing the maths in engine space and converting once at the end.
  const toThree = (p) => P(p.x, p.z, -p.y)
  const a = buildSpline(BENT)
  const b = buildSpline(BENT.map(toThree))
  let worst = 0
  for (let i = 0; i <= 100; i++) {
    const f = i / 100
    worst = Math.max(worst, dist(toThree(a.pointAt(f * a.length)), b.pointAt(f * b.length)))
  }
  check(worst < 1e-9, `the permutation commutes with the curve (worst ${worst.toExponential(2)})`)
  check(near(a.length, b.length, 1e-9), 'and with its arc length')

  // THE CONTROL: a nonlinear map does not, so the check above is testing
  // something rather than passing for any map at all.
  const squash = (p) => P(p.x, p.y * p.y, p.z)
  const c = buildSpline(BENT.map(squash))
  let bad = 0
  for (let i = 0; i <= 100; i++) {
    const f = i / 100
    bad = Math.max(bad, dist(squash(a.pointAt(f * a.length)), c.pointAt(f * c.length)))
  }
  check(bad > 1e-6, `CONTROL: a nonlinear map does not commute (worst ${bad.toExponential(2)})`)
}

console.log('\n--- degenerate input does not produce NaN ---')
{
  const finite = (p) => Number.isFinite(p.x) && Number.isFinite(p.y) && Number.isFinite(p.z)

  const two = buildSpline([P(0, 0, 0), P(5, 0, 0)])
  check(two.segmentCount === 1 && near(two.length, 5, 1e-9), 'a two-point path is a straight line of the right length')
  check(finite(two.pointAt(2.5)), 'and evaluates finitely')

  // Duplicate points: a zero knot delta, which is a division by zero without
  // the floor. mapc does not reject these and should not -- a path may pause.
  const dup = buildSpline([P(0, 0, 0), P(5, 0, 0), P(5, 0, 0), P(10, 0, 0)])
  let allFinite = true
  for (let i = 0; i <= 100; i++) if (!finite(dup.pointAt((i / 100) * dup.length))) allFinite = false
  check(allFinite, 'a path with duplicated points evaluates finitely throughout')
  check(Number.isFinite(dup.length) && dup.length > 0, 'and has a finite, positive length')

  // Monotone arc length is the invariant pointAt's binary search depends on.
  const s = buildSpline(BENT)
  let monotone = true
  for (let i = 1; i < s._arc.length; i++) if (s._arc[i] < s._arc[i - 1]) monotone = false
  check(monotone, 'the arc table is monotone')
  check(s._arc.length === s.segmentCount * SAMPLES_PER_SEGMENT + 1,
    'and has the sample count tools/mapc will bake with')
}

console.log(`\nspline: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
