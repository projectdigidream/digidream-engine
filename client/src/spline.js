//
// Centripetal Catmull-Rom through authored points (design §4.1).
//
// A camera path is stored in the map as a POLYLINE -- `path` in a .map file,
// `Path`/`PathPoint` in core -- and the curve through it lives here, in
// JavaScript, at frame rate. The engine never sees the curve.
//
// That split is not laziness. A camera is never hashed and never predicted, so
// a curve in core would buy no agreement that the polyline does not already
// buy: both sides load the same points from the same hashed file, and the only
// thing derived from them is where a cosmetic camera sits this frame. Fixed
// point earns its cost where a wrong last bit is a divergence; here it is a
// sub-pixel difference nobody can see.
//
// THE ONE THING THAT MUST MATCH: `length`. tools/mapc bakes the arc length of
// this curve into the map so a script can work out how many ticks a flythrough
// takes -- the engine has no completion event, so a cutscene times itself the
// way COND.sector_at_rest does, by arithmetic done in advance. If mapc and this
// file disagreed about how long a path is, every cinematic would end at the
// wrong moment and nothing would say why. SAMPLES_PER_SEGMENT and ALPHA below
// are that agreement; mapc reads these same numbers.
//

// Knot exponent. 0 is uniform Catmull-Rom, 1 chordal, 0.5 centripetal.
//
// CENTRIPETAL, and this is the one parameter that is not a matter of taste.
// Authored points are unevenly spaced by construction -- a path is typed by
// hand, and a corner is wherever the author wanted one. Uniform Catmull-Rom
// cusps and self-intersects when the spacing is uneven, and a cusp is a camera
// that reverses direction for one frame: the most visible artefact this file
// could produce. Centripetal provably has neither.
//
// It stays a parameter because UNIFORM IS THE CONTROL. The no-cusp test asserts
// centripetal does not reverse and that uniform, on the same points, does --
// a claim about this curve that a test of this curve alone cannot make.
export const ALPHA = { UNIFORM: 0, CENTRIPETAL: 0.5, CHORDAL: 1 }

// Samples per segment in the arc-length table. Shared with tools/mapc: see the
// header.
//
// Measured on a right-angled corner with chords of 16 and 6 -- the shape a
// hand-authored path actually contains, and the worst case for a chord
// approximation. Step-length variation when marching at constant speed, and the
// arc length each count converges on:
//
//     m=8   3.33%   22.3818        m=32  0.65%   22.4046
//     m=16  1.52%   22.4000        m=64  0.20%   22.4058
//
// Sixty-four, because the table is built ONCE per path -- or per frame only for
// a path riding a mover -- and a few hundred evaluations buys a camera whose
// speed is steady to a fifth of a percent. Sixteen was the first guess and is
// visible in the variance, which is what the constant-speed test measures.
export const SAMPLES_PER_SEGMENT = 64

// Two points in the same place give a zero knot delta and a division by zero.
// mapc does not reject duplicate points -- and should not, since a path may
// legitimately pause -- so the floor lives here.
const KNOT_FLOOR = 1e-9

function sub (a, b) { return { x: a.x - b.x, y: a.y - b.y, z: a.z - b.z } }
function add (a, b) { return { x: a.x + b.x, y: a.y + b.y, z: a.z + b.z } }
function len (a) { return Math.hypot(a.x, a.y, a.z) }

function dist (a, b) { return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z) }

function distSq (a, b) {
  const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z
  return dx * dx + dy * dy + dz * dz
}

// Barry-Goldman: three rounds of linear interpolation on the knot sequence.
//
// Written this way rather than as a basis matrix because the matrix form is
// only valid for UNIFORM knots -- it is where a centripetal implementation
// usually goes wrong, by deriving the coefficients once and then varying alpha
// as though they still applied.
function barryGoldman (p0, p1, p2, p3, t0, t1, t2, t3, t) {
  const lerp = (a, b, ta, tb) => {
    const d = tb - ta
    const w = d === 0 ? 0 : (t - ta) / d
    return {
      x: a.x + (b.x - a.x) * w,
      y: a.y + (b.y - a.y) * w,
      z: a.z + (b.z - a.z) * w
    }
  }
  const a1 = lerp(p0, p1, t0, t1)
  const a2 = lerp(p1, p2, t1, t2)
  const a3 = lerp(p2, p3, t2, t3)
  const b1 = lerp(a1, a2, t0, t2)
  const b2 = lerp(a2, a3, t1, t3)
  return lerp(b1, b2, t1, t2)
}

class Spline {
  constructor (points, loop, alpha, samplesPerSegment) {
    this.points = points
    this.loop = loop
    this.alpha = alpha
    this.samplesPerSegment = samplesPerSegment

    const n = points.length
    // One fewer than the points, or one per point when it loops -- the same
    // rule geom::rail_segment_count states, so the two systems describe a
    // closed path the same way.
    this.segmentCount = n < 2 ? 0 : (loop ? n : n - 1)

    this._buildKnots()
    this._buildArcTable()
  }

  // The four control points for a segment, and their knots.
  //
  // A looping path wraps, so every segment has real neighbours. A non-looping
  // one REFLECTS at each end -- p(-1) = p0 + (p0 - p1) -- which makes the curve
  // leave the first point along the chord to the second. The alternative,
  // duplicating the endpoint, gives a zero knot delta and a curve that starts
  // by standing still.
  _controls (seg) {
    const p = this.points
    const n = p.length
    if (this.loop) {
      return [p[(seg - 1 + n) % n], p[seg % n], p[(seg + 1) % n], p[(seg + 2) % n]]
    }
    const p0 = seg === 0 ? add(p[0], sub(p[0], p[1])) : p[seg - 1]
    const p1 = p[seg]
    const p2 = p[seg + 1]
    const p3 = seg + 2 < n ? p[seg + 2] : add(p[n - 1], sub(p[n - 1], p[n - 2]))
    return [p0, p1, p2, p3]
  }

  _buildKnots () {
    this._knots = []
    for (let s = 0; s < this.segmentCount; s++) {
      const [p0, p1, p2, p3] = this._controls(s)
      const k = (a, b) => Math.max(Math.pow(len(sub(b, a)), this.alpha), KNOT_FLOOR)
      const t0 = 0
      const t1 = t0 + k(p0, p1)
      const t2 = t1 + k(p1, p2)
      const t3 = t2 + k(p2, p3)
      this._knots.push([t0, t1, t2, t3])
    }
  }

  /** The curve point in segment `seg` at local parameter u in [0, 1]. */
  evalSegment (seg, u) {
    const [p0, p1, p2, p3] = this._controls(seg)
    const [t0, t1, t2, t3] = this._knots[seg]
    return barryGoldman(p0, p1, p2, p3, t0, t1, t2, t3, t1 + (t2 - t1) * u)
  }

  // Cumulative chord length over a uniform sampling in u.
  //
  // `along` is measured on THIS table, and so is everything that reads it --
  // pointAt and nearest both. That they share one table is not an optimisation:
  // if nearest reported a distance-along that pointAt resolved to a different
  // place, every attach would jump by the disagreement.
  _buildArcTable () {
    const m = this.samplesPerSegment
    this._arc = [0]         // cumulative length at each sample
    this._sample = []       // {seg, u, p} per sample, parallel to _arc
    if (this.segmentCount === 0) {
      this._sample.push({ seg: 0, u: 0, p: this.points[0] || { x: 0, y: 0, z: 0 } })
      this.length = 0
      return
    }
    let acc = 0
    let prev = this.evalSegment(0, 0)
    this._sample.push({ seg: 0, u: 0, p: prev })
    for (let s = 0; s < this.segmentCount; s++) {
      for (let i = 1; i <= m; i++) {
        const u = i / m
        const q = this.evalSegment(s, u)
        acc += dist(prev, q)
        this._arc.push(acc)
        this._sample.push({ seg: s, u, p: q })
        prev = q
      }
    }
    this.length = acc
  }

  // Wrap or clamp a distance-along, reporting which happened.
  //
  // A looping path wraps; a non-looping one CLAMPS AND SAYS SO. Clamping
  // silently would make "the ride reached the end" indistinguishable from "the
  // ride is somewhere in the middle", and the end is exactly where a cinematic
  // hands the camera back -- the same contract geom::rail_point_at states.
  _normalise (along) {
    if (this.length <= 0) return { along: 0, clamped: !this.loop }
    if (this.loop) {
      let a = along % this.length
      if (a < 0) a += this.length
      return { along: a, clamped: false }
    }
    if (along < 0) return { along: 0, clamped: true }
    if (along > this.length) return { along: this.length, clamped: true }
    return { along, clamped: false }
  }

  // The sample index at or before `a`. Binary search over _arc.
  _bracket (a) {
    let lo = 0
    let hi = this._arc.length - 1
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1
      if (this._arc[mid] <= a) lo = mid
      else hi = mid - 1
    }
    return lo
  }

  /**
   * Position at `along` units from the start, measured ALONG THE CURVE.
   *
   * Arc length, not the curve parameter, and that is the whole point of the
   * table: a camera marching in `t` runs fast down the straights and slow round
   * the bends, because the chord of a bent segment is shorter than its arc.
   * "Twelve units a second" has to mean twelve units.
   */
  pointAt (along) {
    const nrm = this._normalise(along)
    if (this.segmentCount === 0) {
      const p = this._sample[0].p
      return { x: p.x, y: p.y, z: p.z, segment: 0, t: 0, clamped: nrm.clamped }
    }
    const i = this._bracket(nrm.along)
    const last = this._arc.length - 1
    if (i >= last) {
      const s = this._sample[last]
      const p = this.evalSegment(s.seg, s.u)
      return { x: p.x, y: p.y, z: p.z, segment: s.seg, t: s.u, clamped: nrm.clamped }
    }
    const span = this._arc[i + 1] - this._arc[i]
    const f = span === 0 ? 0 : (nrm.along - this._arc[i]) / span
    const a = this._sample[i]
    const b = this._sample[i + 1]
    // Within a segment the two samples share `seg` and u interpolates directly.
    //
    // ACROSS A SEAM they do not: a is the last sample of one segment (u = 1)
    // and b the first of the next (u = 1/m). Those name the SAME point at the
    // join, so the interval between them lies wholly inside b's segment, from
    // u = 0. Interpolating in a's segment instead pins u at 1 for the width of
    // one sample -- the camera stops dead at every corner and then covers two
    // samples in the next step. It is invisible in a still and obvious in the
    // step-length variance, which is how the test found it.
    const sameSeg = a.seg === b.seg
    const seg = sameSeg ? a.seg : b.seg
    const u = sameSeg ? a.u + (b.u - a.u) * f : b.u * f
    const p = this.evalSegment(seg, u)
    return { x: p.x, y: p.y, z: p.z, segment: seg, t: u, clamped: nrm.clamped }
  }

  /**
   * Unit tangent at `along`, pointing the way `along` increases.
   *
   * A central difference rather than the analytic derivative: the derivative of
   * the Barry-Goldman form is a third thing to keep in step with the knots, and
   * this is a direction for a camera to look down, not a quantity anything is
   * measured against.
   */
  tangentAt (along) {
    if (this.length <= 0) return { x: 1, y: 0, z: 0 }
    const h = Math.max(this.length / 4096, 1e-6)
    const a = this.pointAt(along - h)
    const b = this.pointAt(along + h)
    const d = sub(b, a)
    const m = len(d)
    if (m === 0) return { x: 1, y: 0, z: 0 }
    return { x: d.x / m, y: d.y / m, z: d.z / m }
  }

  /**
   * The point on the curve nearest `p`, and how far along it that is.
   *
   * A GLOBAL scan of the sample table, then refinement. Not a local descent
   * from a seed: a Catmull-Rom through unevenly spaced points bulges off the
   * chord, so at a query point outside a corner the true minimum can sit in a
   * different segment from the one a seeded search would fall into -- and the
   * camera would snap the moment the global minimum crossed over.
   *
   * `from` and `window` restrict the scan to a band of arc length around where
   * the camera already was. That is not an optimisation either: on a path that
   * doubles back, two far-apart places are near the same point, and a camera
   * choosing freshly every frame flips between them.
   */
  nearest (p, opts = {}) {
    const { from = null, window = null } = opts
    const nSamples = this._sample.length
    let best = -1
    let bestD = Infinity

    const inWindow = (a) => {
      if (from === null || window === null) return true
      let d = Math.abs(a - from)
      // On a loop the short way round may cross the seam.
      if (this.loop && this.length > 0) d = Math.min(d, this.length - d)
      return d <= window
    }

    for (let i = 0; i < nSamples; i++) {
      if (!inWindow(this._arc[i])) continue
      const d = distSq(this._sample[i].p, p)
      // STRICTLY less, so equal distances resolve to the EARLIER sample. A
      // corner is equidistant from a point on its bisector, and a tie that
      // resolved by array order would still be a tie that flip-flopped.
      if (d < bestD) { bestD = d; best = i }
    }

    // The window can exclude everything -- a camera that was teleported, or a
    // first attach with a stale `from`. Fall back to the whole path rather than
    // returning nothing, since "nowhere near" is still somewhere.
    if (best < 0) return this.nearest(p, {})

    // Refine on arc length within one sample either side, by ternary search.
    // The bracket is guaranteed to contain the minimum of a unimodal stretch,
    // and one sample is short enough that it is unimodal.
    const step = this.length / Math.max(nSamples - 1, 1)
    let lo = this._arc[best] - step
    let hi = this._arc[best] + step
    if (!this.loop) { lo = Math.max(lo, 0); hi = Math.min(hi, this.length) }
    for (let k = 0; k < 40 && hi - lo > 1e-12; k++) {
      const m1 = lo + (hi - lo) / 3
      const m2 = hi - (hi - lo) / 3
      if (distSq(this.pointAt(m1), p) <= distSq(this.pointAt(m2), p)) hi = m2
      else lo = m1
    }
    const along = this._normalise((lo + hi) / 2).along
    const at = this.pointAt(along)
    return {
      x: at.x, y: at.y, z: at.z,
      along,
      segment: at.segment,
      t: at.t,
      distSq: distSq(at, p)
    }
  }
}

/**
 * Build a spline through `points`.
 *
 * `points` is an array of {x, y, z} in ENGINE space. It stays engine space
 * deliberately: toThreeX/Y/Z is a signed permutation, so it is linear and an
 * isometry, and both this curve and its nearest-point commute with it. Doing
 * the maths here and converting once at the end is therefore identical to
 * converting first -- which is worth a test rather than a comment, and has one.
 */
export function buildSpline (points, opts = {}) {
  const {
    loop = false,
    alpha = ALPHA.CENTRIPETAL,
    samplesPerSegment = SAMPLES_PER_SEGMENT
  } = opts
  const pts = (points || []).map((p) => ({ x: p.x, y: p.y, z: p.z }))
  return new Spline(pts, !!loop && pts.length >= 3, alpha, Math.max(1, samplesPerSegment | 0))
}
