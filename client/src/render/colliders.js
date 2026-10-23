//
// COLLIDERS, DRAWN OVER THE FRAME (a debug view: ?colliders=1, or F4).
//
// Every body the simulation collides with is a vertical cylinder -- a radius
// and a height standing on its feet -- so each is drawn as one: a ring at the
// feet, a ring at the head and four uprights, in a colour for what it is, and
// over everything (no depth test), so a collider inside a wall or under a
// floor is still seen. The shapes are the simulation's numbers, not the
// sprite's: what this shows is what a body actually meets.
//
//   player   the player's own body (green)
//   solid    another body, or a solid prop (orange)
//   ghost    a prop nothing collides with -- a pickup, a corpse (grey)
//
// One shared unit cylinder, scaled per body; the lines are pooled, so a
// frame allocates nothing once the pool is as big as the busiest one.
//
const SEGMENTS = 24
const COLOURS = { player: 0x4cff6a, solid: 0xffa040, ghost: 0x9aa4b8 }

/** The unit cylinder's edges (radius 1, height 1, feet at the origin), as line-segment pairs. */
export function cylinderEdges (segments = SEGMENTS) {
  const out = []
  const ring = (y) => {
    for (let i = 0; i < segments; i++) {
      const a = (i / segments) * Math.PI * 2
      const b = ((i + 1) / segments) * Math.PI * 2
      out.push(Math.cos(a), y, Math.sin(a), Math.cos(b), y, Math.sin(b))
    }
  }
  ring(0)
  ring(1)
  for (let i = 0; i < 4; i++) {
    const a = (i / 4) * Math.PI * 2
    out.push(Math.cos(a), 0, Math.sin(a), Math.cos(a), 1, Math.sin(a))
  }
  return new Float32Array(out)
}

export class ColliderView {
  constructor ({ THREE, scene }) {
    this.THREE = THREE
    this.group = new THREE.Group()
    this.group.visible = false
    scene.add(this.group)
    this.geo = new THREE.BufferGeometry()
    this.geo.setAttribute('position', new THREE.BufferAttribute(cylinderEdges(), 3))
    this.mats = {}
    for (const [k, c] of Object.entries(COLOURS)) {
      this.mats[k] = new THREE.LineBasicMaterial({ color: c, depthTest: false, depthWrite: false, transparent: true, opacity: 0.9 })
    }
    this.pool = []
    this.shown = false
  }

  setShown (on) {
    this.shown = !!on
    this.group.visible = this.shown
  }

  /**
   * This frame's bodies, in THREE's frame: [{ x, y (the feet), z, radius,
   * height, kind: 'player' | 'solid' | 'ghost' }]. Lines beyond them are hidden.
   */
  update (bodies) {
    if (!this.shown) return
    const THREE = this.THREE
    while (this.pool.length < bodies.length) {
      const line = new THREE.LineSegments(this.geo, this.mats.solid)
      line.renderOrder = 1e6   // after everything, with no depth test: on top
      line.frustumCulled = false
      this.group.add(line)
      this.pool.push(line)
    }
    for (let i = 0; i < this.pool.length; i++) {
      const line = this.pool[i]
      const b = bodies[i]
      line.visible = !!b
      if (!b) continue
      line.position.set(b.x, b.y, b.z)
      line.scale.set(b.radius, Math.max(b.height, 1e-3), b.radius)
      line.material = this.mats[b.kind] ?? this.mats.solid
    }
  }
}
