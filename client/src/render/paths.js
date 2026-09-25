//
// Camera paths, drawn.
//
// A debug view of geometry that is otherwise completely invisible: a camera
// path affects nothing in the world, moves nothing, collides with nothing, and
// is only ever seen indirectly as where the camera happens to be standing. An
// author typing waypoints into a .map file has no other way to find out what
// they typed.
//
// THREE LINES PER PATH, and the second is the point of the overlay:
//
//   the CURVE, which is what the camera actually rides;
//   the POLYLINE through the authored points, dimmer;
//   the POINTS themselves, as markers.
//
// Without the polyline this cannot answer the only question it exists for --
// "the curve is not what I typed". Centripetal Catmull-Rom bulges off the
// chord, by 0.65 units on the corner in campath.map, and an author looking at
// one smooth line has no way to tell whether the bulge is the curve doing its
// job or a waypoint in the wrong place.
//
// WHAT THE COLOUR SAYS. A path whose points are anchored to a scripted-discrete
// mover is drawn differently, because it is a trap rather than a style: such a
// sector's offset has no closed form and is STREAMED every snapshot, at half
// the tick rate over a lossy channel, so a camera riding it judders exactly the
// way camera.js's header warns about -- arriving through the map instead of
// through the subject. It is the one authoring mistake this overlay can catch
// before it ships, so it is worth a colour.
//
// Off by default, and by the opt-IN spelling (?paths=1) rather than the
// opt-out one the swing view uses. A map may hold sixteen paths and each is a
// spline sampled per frame; that is nothing beside a level's geometry, but it
// is not nothing beside zero, and nobody wants it who is not authoring a shot.
//
import { buildSpline } from '../spline.js'

// Samples per segment for the DRAWING. Independent of the arc-length table's
// resolution -- this is about how smooth the line looks, not how accurately it
// is measured -- and lower, because a screen cannot show the difference.
const DRAW_SAMPLES = 24

const COLOR = {
  curve: 0x66ccff,        // what the camera rides
  curveActive: 0xffffff,  // ...and the one it is riding now
  polyline: 0x336688,     // what was typed
  point: 0xffcc44,        // the waypoints
  discrete: 0xff5566      // anchored to a sector whose position is streamed
}

export class PathView {
  constructor ({ THREE, scene }) {
    this.THREE = THREE
    this.scene = scene
    this.group = new THREE.Group()
    this.group.renderOrder = 999
    scene.add(this.group)
    this.paths = []
    this.meshes = []
    this.activeTag = -1
    this.discrete = new Set()
  }

  /**
   * The paths to draw, as readPaths() returns them.
   *
   * Engine space in, THREE space out -- converted here rather than by the
   * caller, because this is a renderer and the conversion is the render
   * boundary's business.
   */
  setPaths (paths, toThree) {
    this.clear()
    this.paths = paths
    this.toThree = toThree
    for (const p of paths) this._build(p)
  }

  /**
   * Which sectors move in the unpredictable mode, so a path anchored to one can
   * be coloured as the hazard it is. Built by main.js from the motion type it
   * already has to look at.
   */
  setDiscrete (sectors) {
    this.discrete = sectors instanceof Set ? sectors : new Set(sectors)
    this._recolour()
  }

  /** The path the camera is on now, by tag, or -1. */
  setActive (tag) {
    if (tag === this.activeTag) return
    this.activeTag = tag
    this._recolour()
  }

  _pointsOf (p) {
    const out = []
    for (let i = 0; i < p.pos.length; i += 3) {
      out.push(this.toThree({ x: p.pos[i], y: p.pos[i + 1], z: p.pos[i + 2] }))
    }
    return out
  }

  _line (positions, color, opacity) {
    const T = this.THREE
    const geo = new T.BufferGeometry()
    geo.setAttribute('position', new T.BufferAttribute(new Float32Array(positions), 3))
    const mat = new T.LineBasicMaterial({
      color, transparent: true, opacity, depthTest: false
    })
    const line = new T.Line(geo, mat)
    line.frustumCulled = false
    return line
  }

  _build (p) {
    const T = this.THREE
    const pts = this._pointsOf(p)
    const spline = buildSpline(pts, { loop: p.loop })

    // The curve.
    const n = Math.max(1, spline.segmentCount) * DRAW_SAMPLES
    const curve = new Float32Array((n + 1) * 3)
    for (let i = 0; i <= n; i++) {
      const q = spline.pointAt((i / n) * spline.length)
      curve[i * 3] = q.x; curve[i * 3 + 1] = q.y; curve[i * 3 + 2] = q.z
    }

    // The polyline, closed when the path loops so the two are comparable all
    // the way round rather than only where the curve happens to be drawn.
    const poly = []
    for (const q of pts) poly.push(q.x, q.y, q.z)
    if (p.loop && pts.length) poly.push(pts[0].x, pts[0].y, pts[0].z)

    const curveLine = this._line(curve, COLOR.curve, 0.9)
    const polyLine = this._line(new Float32Array(poly), COLOR.polyline, 0.5)

    // The points. Small boxes rather than sprites: a marker that always faces
    // the camera hides which way the path turns, and turning is what an author
    // is looking at.
    const markers = new T.Group()
    const geo = new T.BoxGeometry(0.35, 0.35, 0.35)
    for (const q of pts) {
      const m = new T.Mesh(geo, new T.MeshBasicMaterial({
        color: COLOR.point, transparent: true, opacity: 0.8, depthTest: false
      }))
      m.position.set(q.x, q.y, q.z)
      markers.add(m)
    }

    this.group.add(curveLine, polyLine, markers)
    this.meshes.push({ path: p, spline, curveLine, polyLine, markers, pointGeo: geo })
  }

  _recolour () {
    for (const m of this.meshes) {
      const onDiscrete = Array.from(m.path.anchors).some((a) => this.discrete.has(a))
      const active = m.path.tag === this.activeTag
      m.curveLine.material.color.setHex(
        onDiscrete ? COLOR.discrete : (active ? COLOR.curveActive : COLOR.curve))
      m.curveLine.material.opacity = active ? 1 : 0.9
    }
  }

  /**
   * Rebuild the lines of any path whose points have moved.
   *
   * `moved` is the set of path indices refreshPathPositions reported a change
   * for. Nothing else is touched: a static path's buffers are written once, at
   * setPaths, and never again -- which is the whole reason that function
   * returns a flag rather than the caller re-reading everything every frame.
   */
  update (moved) {
    if (!moved || moved.size === 0) return
    for (const m of this.meshes) {
      if (!moved.has(m.path.index)) continue
      const pts = this._pointsOf(m.path)
      m.spline = buildSpline(pts, { loop: m.path.loop })
      const n = Math.max(1, m.spline.segmentCount) * DRAW_SAMPLES
      const arr = m.curveLine.geometry.attributes.position.array
      for (let i = 0; i <= n && i * 3 + 2 < arr.length; i++) {
        const q = m.spline.pointAt((i / n) * m.spline.length)
        arr[i * 3] = q.x; arr[i * 3 + 1] = q.y; arr[i * 3 + 2] = q.z
      }
      m.curveLine.geometry.attributes.position.needsUpdate = true

      const parr = m.polyLine.geometry.attributes.position.array
      for (let i = 0; i < pts.length && i * 3 + 2 < parr.length; i++) {
        parr[i * 3] = pts[i].x; parr[i * 3 + 1] = pts[i].y; parr[i * 3 + 2] = pts[i].z
      }
      if (m.path.loop && pts.length && pts.length * 3 + 2 < parr.length) {
        parr[pts.length * 3] = pts[0].x
        parr[pts.length * 3 + 1] = pts[0].y
        parr[pts.length * 3 + 2] = pts[0].z
      }
      m.polyLine.geometry.attributes.position.needsUpdate = true

      m.markers.children.forEach((child, i) => {
        if (pts[i]) child.position.set(pts[i].x, pts[i].y, pts[i].z)
      })
    }
  }

  clear () {
    for (const m of this.meshes) {
      m.curveLine.geometry.dispose(); m.curveLine.material.dispose()
      m.polyLine.geometry.dispose(); m.polyLine.material.dispose()
      m.markers.children.forEach((c) => c.material.dispose())
      m.pointGeo.dispose()
      this.group.remove(m.curveLine, m.polyLine, m.markers)
    }
    this.meshes = []
  }

  get count () { return this.meshes.length }
}
