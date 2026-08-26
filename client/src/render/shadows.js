//
// Blob shadows (design §4).
//
// Two kinds with two different jobs, and two different SHAPES, because the two
// projections are different:
//
//   The CONTACT shadow is a CIRCLE directly below the entity, whatever the
//   lighting -- the vertical projection of an upright cylinder is a circle. It
//   is not cast by any particular lamp: it is the ambient darkening under a
//   body, and its real job is telling a player where they are about to land. In
//   an isometric view height and depth look the same, so without it a jump is
//   genuinely ambiguous -- an entity drawn higher on screen might be above you
//   or behind you. It is the one that makes platforming readable, which is why
//   it exists even in a scene with no lights at all.
//
//   The CAST shadows are TRAPEZIA, one per lamp. Projecting the body's feet and
//   its head from a point lamp gives two discs: the head is nearer the lamp, so
//   its shadow lands further away AND larger. The silhouette is the hull of the
//   two, which is an isosceles trapezium oriented along the throw. Drawing that
//   as a disc instead is what makes a side-lit body look pasted on -- the shape
//   says the light came from directly overhead while the shading says otherwise.
//
// They are faint on purpose. The contact shadow is the one carrying
// information; a cast shadow as dark as it would compete with the cue a player
// is actually reading.
//
// Blobs, not shadow maps. A shadow map would cost a depth pass per light and
// buy accuracy an isometric camera at this scale cannot show -- and it would
// put the light count into the frame budget, where a map author would then have
// to think about it.
//
import { attenuation } from './lighting.js'

// Lifted off the ground plane so a shadow does not fight the floor for depth.
const LIFT = 0.02

// Beyond this many units above the ground a contact shadow has faded out. Also
// where a jump stops being readable, which is the same number for the reason.
const FADE_HEIGHT = 14

export const MAX_CAST_SHADOWS = 4

// Peak darkness of each kind. The gap is deliberate: the contact shadow is a
// gameplay cue and the cast shadows are decoration, so they must not compete.
const CONTACT_OPACITY = 0.55
const CAST_OPACITY = 0.2

// How far a cast shadow is followed before it is given up on. The throw is
// sampled so that a shadow running off the edge of a platform STOPS at the
// edge rather than vanishing entirely -- an all-or-nothing test makes a shadow
// blink out as its far corner crosses a boundary.
const EDGE_SAMPLES = 8

/**
 * Project a point onto the ground plane along the ray from a lamp.
 *
 * Returns the landing point and the magnification `t`: a horizontal disc of
 * radius r at that height casts a disc of radius r * t. t > 1 always, for a
 * lamp above the point, which is why a head's shadow is larger than a foot's.
 */
export function projectToGround (lamp, px, py, pz, groundY) {
  const dy = py - lamp.py
  if (dy >= 0) return null            // the point is at or above the lamp
  const t = (groundY - lamp.py) / dy
  return { x: lamp.px + (px - lamp.px) * t, z: lamp.pz + (pz - lamp.pz) * t, t }
}

/**
 * Where an entity's shadows land, how dark each is, and what shape.
 *
 * Pure: numbers in, numbers out. Separated from the meshes because this is the
 * part with decisions in it -- which lamps cast, where their shadows fall, how
 * far they reach before running out of floor -- and every one of those is
 * arithmetic that can be asserted rather than looked at.
 */
export function planShadows ({ subject, groundY, lights, onGround, radius = 0.75,
                               bodyHeight = 2, max = MAX_CAST_SHADOWS }) {
  // Out over the void with nothing beneath: no surface, no shadow. Drawing one
  // at some default height would put a disc in mid-air.
  if (groundY === null || groundY === undefined) return { contact: null, casts: [] }

  const height = Math.max(0, subject.y - groundY)
  const fade = Math.max(0, 1 - height / FADE_HEIGHT)

  // Contact: a circle, directly below, always. Grows and fades with height --
  // the growth is what makes rising read as rising rather than as moving away,
  // which an isometric camera cannot otherwise distinguish.
  const contact = fade > 0.01
    ? {
        x: subject.x, y: groundY + LIFT, z: subject.z,
        size: radius * 2 * (1 + height * 0.06),
        opacity: CONTACT_OPACITY * fade
      }
    : null

  const ranked = []
  for (const L of lights) {
    // A lamp below the body throws its shadow upward, onto nothing this
    // renderer draws.
    if (L.py <= subject.y + bodyHeight + 0.001) continue

    const dist = Math.hypot(subject.x - L.px, subject.y - L.py, subject.z - L.pz)
    const strength = attenuation(dist, L.radius) * L.intensity
    if (strength <= 0.01) continue

    const near = projectToGround(L, subject.x, subject.y, subject.z, groundY)
    const far = projectToGround(L, subject.x, subject.y + bodyHeight, subject.z, groundY)
    if (!near || !far) continue

    ranked.push({ near, far, strength })
  }
  ranked.sort((a, b) => b.strength - a.strength)

  const casts = []
  for (const c of ranked) {
    if (casts.length >= max) break

    // The near end is under the body, so if THAT is off the floor there is
    // nothing to draw on. Otherwise the throw is walked outward and truncated
    // at the last point still over floor: a shadow reaching past a platform's
    // edge stops there instead of blinking out whole.
    if (onGround && !onGround(c.near.x, c.near.z)) continue

    let end = c.far
    let endT = c.far.t
    if (onGround) {
      let lastOk = 0
      for (let i = 1; i <= EDGE_SAMPLES; i++) {
        const f = i / EDGE_SAMPLES
        const x = c.near.x + (c.far.x - c.near.x) * f
        const z = c.near.z + (c.far.z - c.near.z) * f
        if (!onGround(x, z)) break
        lastOk = f
      }
      if (lastOk <= 0) continue
      end = {
        x: c.near.x + (c.far.x - c.near.x) * lastOk,
        z: c.near.z + (c.far.z - c.near.z) * lastOk
      }
      endT = c.near.t + (c.far.t - c.near.t) * lastOk
    }

    casts.push({
      y: groundY + LIFT,
      near: { x: c.near.x, z: c.near.z },
      far: { x: end.x, z: end.z },
      // Half-widths, from the magnification at each end. This is the whole of
      // the trapezium: near narrow, far wide.
      nearHalfWidth: radius * c.near.t,
      farHalfWidth: radius * endT,
      opacity: Math.min(CAST_OPACITY, CAST_OPACITY * c.strength) * fade
    })
  }
  return { contact, casts }
}

function blobTexture (THREE) {
  const N = 128
  const canvas = document.createElement('canvas')
  canvas.width = canvas.height = N
  const ctx = canvas.getContext('2d')
  const g = ctx.createRadialGradient(N / 2, N / 2, 0, N / 2, N / 2, N / 2)
  // Soft-edged rather than a hard disc. At this size the softness is most of
  // what makes it read as a shadow instead of a painted circle -- and on a
  // trapezium it is what hides the straight sides.
  g.addColorStop(0, 'rgba(0,0,0,0.9)')
  g.addColorStop(0.55, 'rgba(0,0,0,0.5)')
  g.addColorStop(1, 'rgba(0,0,0,0)')
  ctx.fillStyle = g
  ctx.fillRect(0, 0, N, N)
  const tex = new THREE.CanvasTexture(canvas)
  tex.colorSpace = THREE.NoColorSpace
  return tex
}

export class ShadowCaster {
  constructor ({ THREE, scene, radius = 0.75, bodyHeight = 2 }) {
    this.THREE = THREE
    this.scene = scene
    this.radius = radius
    this.bodyHeight = bodyHeight
    this.tex = blobTexture(THREE)

    const material = () => new THREE.MeshBasicMaterial({
      map: this.tex,
      transparent: true,
      depthWrite: false,            // never occlude what is drawn after it
      polygonOffset: true,
      polygonOffsetFactor: -1,
      polygonOffsetUnits: -1
    })

    const disc = new THREE.PlaneGeometry(1, 1)
    disc.rotateX(-Math.PI / 2)      // a plane is upright by default; a shadow lies flat
    this.contact = new THREE.Mesh(disc, material())
    this.contact.visible = false
    this.contact.frustumCulled = false
    scene.add(this.contact)

    // Cast shadows get their vertices rewritten each frame rather than a
    // transform: a trapezium's PROPORTIONS change as the lamp's angle does, and
    // no scale or rotation of a fixed quad can express that.
    this.cast = []
    for (let i = 0; i < MAX_CAST_SHADOWS; i++) {
      const geo = new THREE.BufferGeometry()
      geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(12), 3))
      geo.setAttribute('uv', new THREE.BufferAttribute(
        new Float32Array([0, 0, 1, 0, 1, 1, 0, 1]), 2))
      geo.setIndex([0, 1, 2, 0, 2, 3])
      const mesh = new THREE.Mesh(geo, material())
      mesh.visible = false
      mesh.frustumCulled = false
      scene.add(mesh)
      this.cast.push(mesh)
    }
  }

  update (subject, groundY, lights, onGround) {
    const plan = planShadows({
      subject, groundY, lights, onGround,
      radius: this.radius, bodyHeight: this.bodyHeight, max: this.cast.length
    })

    this.contact.visible = plan.contact !== null
    if (plan.contact) {
      const c = plan.contact
      this.contact.scale.set(c.size, 1, c.size)
      this.contact.position.set(c.x, c.y, c.z)
      this.contact.material.opacity = c.opacity
    }

    for (let i = 0; i < this.cast.length; i++) {
      const c = plan.casts[i]
      const m = this.cast[i]
      m.visible = c !== undefined
      if (!c) continue

      // Along the throw, and across it.
      let ax = c.far.x - c.near.x
      let az = c.far.z - c.near.z
      const len = Math.hypot(ax, az)
      if (len < 1e-6) { ax = 1; az = 0 } else { ax /= len; az /= len }
      const px = -az
      const pz = ax

      const y = c.y + LIFT * i          // stacked, so overlapping shadows do not z-fight
      const p = m.geometry.getAttribute('position')
      const nw = c.nearHalfWidth
      const fw = c.farHalfWidth
      p.setXYZ(0, c.near.x - px * nw, y, c.near.z - pz * nw)
      p.setXYZ(1, c.near.x + px * nw, y, c.near.z + pz * nw)
      p.setXYZ(2, c.far.x + px * fw, y, c.far.z + pz * fw)
      p.setXYZ(3, c.far.x - px * fw, y, c.far.z - pz * fw)
      p.needsUpdate = true
      m.geometry.computeBoundingSphere()
      m.material.opacity = c.opacity
    }
  }
}
