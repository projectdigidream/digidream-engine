//
// Shadow placement (design §4).
//
// Two kinds with two different jobs, and the tests are about the difference:
//
//   The CONTACT shadow is always directly below, whatever the lighting. In an
//   isometric view height and depth look the same, so without it a jump is
//   genuinely ambiguous -- an entity drawn higher on screen might be above you
//   or behind you. It is the one that makes platforming readable, which is why
//   it must exist in a scene with no lights at all.
//
//   The CAST shadows come from the lamps, are TRAPEZIA, and are cosmetic: they
//   stop a body lit from the side from looking pasted on. Their shape is not a
//   stylistic choice -- projecting a body's feet and its head from a point lamp
//   gives two discs of different size at different distances, and the hull of
//   those is a trapezium. Drawing a disc instead says the light came from
//   directly overhead while the shading says otherwise.
//
import { planShadows, projectToGround, MAX_CAST_SHADOWS } from '../src/render/shadows.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps
const anywhere = () => true
const lamp = (px, py, pz, extra = {}) =>
  ({ px, py, pz, r: 1, g: 1, b: 1, radius: 30, intensity: 1.5, ...extra })

console.log('\n--- the contact shadow ---')
{
  const p = planShadows({ subject: { x: 3, y: 0, z: -4 }, groundY: 0, lights: [], onGround: anywhere })
  check(p.contact !== null, 'exists with NO lights at all')
  check(near(p.contact.x, 3) && near(p.contact.z, -4), 'directly below the body')
  check(p.contact.y > 0, 'lifted off the floor so it does not fight it for depth')
  check(p.casts.length === 0, 'and no cast shadows, because there is nothing casting')
}
{
  // Height is what it communicates: it grows and fades as the body rises.
  const at = (h) => planShadows({ subject: { x: 0, y: h, z: 0 }, groundY: 0, lights: [], onGround: anywhere }).contact
  const low = at(0)
  const mid = at(5)
  check(mid.size > low.size, `grows with height (${low.size.toFixed(2)} -> ${mid.size.toFixed(2)})`)
  check(mid.opacity < low.opacity, `and fades (${low.opacity.toFixed(3)} -> ${mid.opacity.toFixed(3)})`)

  // Monotonic in both, or the cue it gives is ambiguous.
  let grows = true
  let fades = true
  let prev = at(0)
  for (let h = 0.5; h <= 13; h += 0.5) {
    const c = at(h)
    if (c.size < prev.size) grows = false
    if (c.opacity > prev.opacity) fades = false
    prev = c
  }
  check(grows && fades, 'monotonically, so the cue is never ambiguous')
  check(at(40) === null, 'and is gone entirely from far enough up')
}

console.log('\n--- the void has no floor to catch a shadow ---')
{
  const p = planShadows({ subject: { x: 0, y: 5, z: 0 }, groundY: null, lights: [lamp(0, 20, 0)], onGround: anywhere })
  check(p.contact === null && p.casts.length === 0,
        'nothing is drawn when there is no surface below')
}

console.log('\n--- cast shadows fall away from the lamp ---')
{
  // Lamp at x = -10, height 15; body standing on the ground at the origin. Its
  // FEET are on the ground so their shadow is at the feet; its head, being
  // nearer the lamp, throws further.
  const p = planShadows({ subject: { x: 0, y: 0, z: 0 }, groundY: 0, bodyHeight: 2,
                          lights: [lamp(-10, 15, 0)], onGround: anywhere })
  check(p.casts.length === 1, 'one lamp casts one shadow')
  const c = p.casts[0]
  check(near(c.near.x, 0), `the near end sits at the feet (x=${c.near.x.toFixed(2)})`)
  check(c.far.x > c.near.x, `and the far end is thrown away from the lamp (x=${c.far.x.toFixed(2)})`)
  check(near(c.near.z, 0) && near(c.far.z, 0), 'along the lamp\'s own axis')

  // The trapezium: wider at the far end, because the head is nearer the lamp.
  check(c.farHalfWidth > c.nearHalfWidth,
        `wider at the far end (${c.nearHalfWidth.toFixed(2)} -> ${c.farHalfWidth.toFixed(2)})`)
  check(near(c.nearHalfWidth, 0.75),
        'and exactly the body radius at the feet, which are on the ground')

  // A lamp directly overhead: no throw, and the trapezium degenerates toward a
  // rectangle -- which is right, and is why the contact shadow does the work.
  const over = planShadows({ subject: { x: 0, y: 0, z: 0 }, groundY: 0, bodyHeight: 2,
                             lights: [lamp(0, 15, 0)], onGround: anywhere })
  check(near(over.casts[0].far.x, 0) && near(over.casts[0].far.z, 0),
        'a lamp directly overhead throws its shadow nowhere')

  // A LOWER lamp at the same offset throws further, because the ray is more
  // oblique. This is the property that makes the shape carry the light angle.
  const low = planShadows({ subject: { x: 0, y: 0, z: 0 }, groundY: 0, bodyHeight: 2,
                            lights: [lamp(-10, 6, 0, { radius: 60 })], onGround: anywhere })
  check(low.casts[0].far.x > c.far.x,
        `a lower lamp throws further (${c.far.x.toFixed(2)} -> ${low.casts[0].far.x.toFixed(2)})`)
  check(low.casts[0].farHalfWidth > c.farHalfWidth, 'and wider')
}

console.log('\n--- the projection itself ---')
{
  const L = lamp(-10, 15, 0)
  // A point ON the ground is its own shadow.
  const onFloor = projectToGround(L, 4, 0, -3, 0)
  check(near(onFloor.x, 4) && near(onFloor.z, -3) && near(onFloor.t, 1),
        'a point on the ground projects to itself, at magnification 1')

  // A point above it lands further from the lamp, magnified.
  const up = projectToGround(L, 0, 5, 0, 0)
  check(up.t > 1, `a raised point is magnified (t=${up.t.toFixed(3)})`)
  check(up.x > 0, 'and lands beyond the body, away from the lamp')

  // At or above the lamp there is no projection at all.
  check(projectToGround(L, 0, 15, 0, 0) === null, 'a point level with the lamp does not project')
  check(projectToGround(L, 0, 40, 0, 0) === null, 'nor one above it')
}

console.log('\n--- which lamps cast ---')
{
  const below = planShadows({
    subject: { x: 0, y: 10, z: 0 }, groundY: 0, lights: [lamp(0, 2, 0)], onGround: anywhere })
  check(below.casts.length === 0,
        'a lamp BELOW the body casts nothing -- its shadow would go upward')

  const level = planShadows({
    subject: { x: 0, y: 0, z: 0 }, groundY: 0, bodyHeight: 2,
    lights: [lamp(5, 1.5, 0)], onGround: anywhere })
  check(level.casts.length === 0,
        'nor one level with the body, where the head would project to infinity')

  const far = planShadows({
    subject: { x: 0, y: 1, z: 0 }, groundY: 0, lights: [lamp(200, 40, 0)], onGround: anywhere })
  check(far.casts.length === 0, 'and one out of range casts nothing')

  // Strength orders them, and the cap drops the weakest rather than whichever
  // happened to be listed last.
  const many = []
  for (let i = 0; i < MAX_CAST_SHADOWS + 4; i++) many.push(lamp(i * 2, 20, 0, { intensity: 0.2 + i * 0.2 }))
  const p = planShadows({ subject: { x: 0, y: 2, z: 0 }, groundY: 0, lights: many, onGround: anywhere })
  check(p.casts.length === MAX_CAST_SHADOWS, `capped at ${MAX_CAST_SHADOWS} cast shadows`)
  let descending = true
  for (let i = 1; i < p.casts.length; i++) if (p.casts[i].opacity > p.casts[i - 1].opacity) descending = false
  check(descending, 'keeping the strongest, in order')
}

console.log('\n--- a shadow only lands where there is floor ---')
{
  const lights = [lamp(-10, 15, 0)]
  // The platform edge has to be INSIDE the throw or nothing gets clipped and
  // the test passes vacuously. This lamp throws to x = 1.54, so the edge is at
  // 1.0 -- the first version put it at 2.0, beyond the shadow's reach, and
  // compared a number with itself.
  const onPlatform = (x) => x < 1.0

  // The throw is TRUNCATED at the platform's edge rather than dropped whole: an
  // all-or-nothing test makes a shadow blink out the moment its far corner
  // crosses a boundary, which is far more distracting than one that stops.
  const clipped = planShadows({ subject: { x: 0, y: 0, z: 0 }, groundY: 0, bodyHeight: 2,
                                lights, onGround: onPlatform })
  const full = planShadows({ subject: { x: 0, y: 0, z: 0 }, groundY: 0, bodyHeight: 2,
                             lights, onGround: anywhere })
  check(clipped.casts.length === 1, 'a shadow reaching past the edge is still drawn')
  check(clipped.casts[0].far.x < full.casts[0].far.x,
        `but stops short of it (${clipped.casts[0].far.x.toFixed(2)} vs ${full.casts[0].far.x.toFixed(2)})`)
  check(clipped.casts[0].far.x <= 1.0001, 'at the boundary itself')
  check(clipped.contact !== null, 'and the contact shadow, under the body, is unaffected')

  // Standing so that even the feet are over nothing: there is no surface at all.
  const nowhere = planShadows({ subject: { x: 0, y: 0, z: 0 }, groundY: 0, bodyHeight: 2,
                               lights, onGround: () => false })
  check(nowhere.casts.length === 0, 'with no floor under the body, nothing casts')
}

console.log('\n--- strength ---')
{
  const nearLamp = planShadows({ subject: { x: 0, y: 1, z: 0 }, groundY: 0, bodyHeight: 2,
                                 lights: [lamp(0, 8, 0)], onGround: anywhere })
  const farLamp = planShadows({ subject: { x: 0, y: 1, z: 0 }, groundY: 0, bodyHeight: 2,
                                lights: [lamp(0, 25, 0)], onGround: anywhere })
  check(nearLamp.casts[0].opacity > farLamp.casts[0].opacity,
        `a nearer lamp casts a darker shadow (${nearLamp.casts[0].opacity.toFixed(3)} > ${farLamp.casts[0].opacity.toFixed(3)})`)

  // And every cast shadow is FAINTER than the contact shadow, whatever the
  // lamp. The contact shadow is the one carrying information -- where you are
  // about to land -- and a decorative shadow as dark as it would compete with
  // the cue a player is actually reading.
  check(nearLamp.casts[0].opacity < nearLamp.contact.opacity / 2,
        `and stays well under the contact shadow (${nearLamp.casts[0].opacity.toFixed(3)} vs ${nearLamp.contact.opacity.toFixed(3)})`)

  const blinding = planShadows({ subject: { x: 0, y: 0, z: 0 }, groundY: 0, bodyHeight: 2,
                                 lights: [lamp(0, 4, 0, { intensity: 50 })], onGround: anywhere })
  check(blinding.casts[0].opacity < blinding.contact.opacity,
        'even from an absurdly bright lamp, which is what the cap is for')
}

console.log(`\nshadows: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
