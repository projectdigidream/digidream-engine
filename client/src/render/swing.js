//
// The swing collider, drawn.
//
// A debug view of a volume that exists in the simulation and is otherwise
// invisible: a swing is a wedge that sweeps from one angle to another over
// several ticks, and every question about whether it connected -- was it wide
// enough, did it reach, had it swept past yet -- is unanswerable from a screen
// that only shows the result.
//
// TWO THINGS IT DOES NOT SHOW, both deliberate and both worth knowing when the
// overlay and the outcome disagree:
//
//   Reach is measured to a target's SURFACE, so a barrel of radius 0.21 is hit
//   from 2.21 away while the wedge ends at 2.00. One mesh cannot draw a
//   boundary that depends on what it is being compared against.
//
//   The arc test is against a target's CENTRE, not its silhouette. Something
//   whose middle is just outside the wedge is not hit even though its body
//   overlaps the drawing.
//
// Both make the wedge slightly conservative: things a little outside it can be
// hit, and nothing inside it is missed.
//
// Reconstructed from the SAME numbers the engine uses rather than replicated:
// the swing's shape is in the parameter table, which the client already has
// because it predicts with it, and how far through the swing is in the agent
// state, which the client already has because it predicts that too. So this
// costs nothing on the wire and cannot disagree with the simulation about
// anything except by getting the arithmetic wrong -- which is why the
// arithmetic is a pure function with a test.
//

/**
 * The swing's shape, read from the engine's own parameter table.
 *
 * Read EVERY FRAME rather than once at startup. These are replicated
 * parameters: a script can change any of them mid-session -- the debug panel
 * exists to do exactly that -- and a mesh built once at load shows the shape the
 * weapon had when the page opened. Which looks like the parameters doing
 * nothing, and was reported as exactly that.
 */
export function readSwingShape (world, agent) {
  const par = (n) => Number(world.getParamRaw(agent, world.paramId(n))) / 4294967296
  return {
    reach: par('attack_reach'),
    inner: par('attack_min_reach'),
    halfArc: par('attack_half_arc'),
    from: par('attack_sweep_from'),
    to: par('attack_sweep_to'),
    // U8 parameters are stored as integers, not fixed-point fractions.
    ticks: Number(world.getParamRaw(agent, world.paramId('attack_ticks'))),
    cooldown: Number(world.getParamRaw(agent, world.paramId('attack_cooldown')))
  }
}

/** Whether two shapes would draw differently. */
export function sameSwingShape (a, b) {
  if (!a || !b) return false
  return a.reach === b.reach && a.inner === b.inner && a.halfArc === b.halfArc &&
         a.from === b.from && a.to === b.to
}

/**
 * Where the collider is centred, as a TURN FRACTION offset from the aim.
 *
 * Mirrors World::resolve_attack exactly: the swing runs from `from` to `to`
 * across `ticks`, and `left` counts DOWN, so the first live tick is the last
 * value and the phase has to be derived from the total.
 *
 * Returns null when no swing is running, which is the case the caller draws
 * nothing for.
 *
 * ONE TICK BEHIND, unavoidably: a client reads agent state after the tick has
 * been simulated, and the engine decrements the countdown after resolving. So
 * the angle drawn on a frame is the one the collider will occupy on the next
 * tick, not the one it just swept. At 60 Hz that is 16 ms of lead on a debug
 * overlay, and the alternative -- replicating the pre-decrement value as a
 * second field, or reshaping the engine's loop around what a renderer can
 * observe -- costs more than it is worth. Stated here rather than discovered
 * from a test that counts nine of ten.
 */
export function sweepOffset ({ from, to, ticks, left }) {
  if (!left || left <= 0) return null
  const total = ticks > 0 ? ticks : 1
  const elapsed = total - left
  const phase = Math.max(0, Math.min(1, elapsed / total))
  return from + (to - from) * phase
}

/**
 * The Y rotation that aims the wedge along an engine heading.
 *
 * The wedge is authored in XY and laid flat by rotateX(+pi/2), so its local +X
 * lands at three (cos phi, 0, -sin phi) after a Y rotation of phi. An engine
 * facing of theta is the direction (cos theta, sin theta) in engine x/y, and
 * engine (x, y) maps to three (x, -y) -- so it must land at
 * (cos theta, 0, -sin theta), and phi = +theta.
 *
 * The first version negated it. The wedge was then mirrored about the aim, so
 * it swept the wrong way and pointed at things the swing did not touch -- which
 * is exactly the complaint a debug overlay is supposed to answer rather than
 * cause. Tested against facingDirection, which is the same conversion arrived
 * at independently, because agreeing with a formula restated in its own test
 * would prove nothing.
 *
 * `facingTurns` and `offsetTurns` are turn fractions, matching the engine.
 */
export function swingYaw (facingTurns, offsetTurns) {
  return (facingTurns + offsetTurns) * Math.PI * 2
}

/**
 * A wedge lying in the XZ plane, centred on the origin, opening along +X.
 *
 * Built once per shape and reused: the swing's angle changes every tick and its
 * SHAPE does not, so rotating a mesh costs nothing while rebuilding one would
 * allocate a buffer per frame for the duration of every swing.
 */
export function makeSwingMesh (THREE, { reachUnits, halfArcTurns, innerUnits = 0,
                                        segments = 12, faint = false }) {
  const half = halfArcTurns * Math.PI * 2
  const shape = new THREE.Shape()
  if (innerUnits > 0) {
    // A RING SEGMENT, not a wedge. attack_min_reach is the parameter that
    // changes the collider's shape rather than its size, and drawing it as a
    // full wedge would show a weapon hitting exactly what it cannot: the thing
    // pressed against you, which is the whole point of the parameter.
    for (let i = 0; i <= segments; i++) {
      const a = -half + (2 * half * i) / segments
      const x = Math.cos(a) * innerUnits
      const y = Math.sin(a) * innerUnits
      if (i === 0) shape.moveTo(x, y)
      else shape.lineTo(x, y)
    }
    for (let i = segments; i >= 0; i--) {
      const a = -half + (2 * half * i) / segments
      shape.lineTo(Math.cos(a) * reachUnits, Math.sin(a) * reachUnits)
    }
    shape.lineTo(Math.cos(-half) * innerUnits, Math.sin(-half) * innerUnits)
  } else {
    shape.moveTo(0, 0)
    for (let i = 0; i <= segments; i++) {
      const a = -half + (2 * half * i) / segments
      shape.lineTo(Math.cos(a) * reachUnits, Math.sin(a) * reachUnits)
    }
    shape.lineTo(0, 0)
  }

  const geo = new THREE.ShapeGeometry(shape)
  // The shape is authored in XY and the world's ground plane is XZ, so it is
  // laid down once here rather than by every caller remembering to.
  geo.rotateX(Math.PI / 2)

  const mat = new THREE.MeshBasicMaterial({
    color: faint ? 0x4488ff : 0xff5544,
    transparent: true,
    // Two wedges, and the pairing is what makes a 167 ms sweep readable: the
    // faint one is the whole region the swing can cover and does not move, and
    // the bright one is the collider as it is RIGHT NOW, travelling across it.
    // A single wedge appearing and vanishing inside a sixth of a second reads
    // as instantaneous whatever it is doing.
    opacity: faint ? 0.10 : 0.35,
    side: THREE.DoubleSide,
    // An instrument, not part of the scene: it is drawn over whatever it
    // overlaps, because a collider you can only see in open ground is useless
    // for the case you are actually debugging.
    depthTest: false,
    depthWrite: false
  })
  const mesh = new THREE.Mesh(geo, mat)
  mesh.renderOrder = 998
  mesh.visible = false
  return mesh
}
