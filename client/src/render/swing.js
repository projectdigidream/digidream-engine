//
// The swing collider, drawn.
//
// A debug view of a volume that exists in the simulation and is otherwise
// invisible: a swing is a wedge that sweeps from one angle to another over
// several ticks, and every question about whether it connected -- was it wide
// enough, did it reach, had it swept past yet -- is unanswerable from a screen
// that only shows the result.
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
 * A wedge lying in the XZ plane, centred on the origin, opening along +X.
 *
 * Built once per shape and reused: the swing's angle changes every tick and its
 * SHAPE does not, so rotating a mesh costs nothing while rebuilding one would
 * allocate a buffer per frame for the duration of every swing.
 */
export function makeSwingMesh (THREE, { reachUnits, halfArcTurns, segments = 12 }) {
  const half = halfArcTurns * Math.PI * 2
  const shape = new THREE.Shape()
  shape.moveTo(0, 0)
  for (let i = 0; i <= segments; i++) {
    const a = -half + (2 * half * i) / segments
    shape.lineTo(Math.cos(a) * reachUnits, Math.sin(a) * reachUnits)
  }
  shape.lineTo(0, 0)

  const geo = new THREE.ShapeGeometry(shape)
  // The shape is authored in XY and the world's ground plane is XZ, so it is
  // laid down once here rather than by every caller remembering to.
  geo.rotateX(Math.PI / 2)

  const mat = new THREE.MeshBasicMaterial({
    color: 0xff5544,
    transparent: true,
    opacity: 0.28,
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
