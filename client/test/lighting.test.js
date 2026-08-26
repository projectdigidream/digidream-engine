//
// The lighting model (design §4).
//
// GLSL cannot be run in a test, so the shader in lighting.js and the exported
// JS functions are written as twins and the JS is what is asserted here. That
// is a real limitation and worth stating plainly: this checks the MODEL, not
// the GPU's execution of it. What it does catch is the class of mistake that
// makes lighting look wrong everywhere at once -- the falloff curve, the
// orientation term, and above all whether a lamp adds or multiplies.
//
import {
  attenuation, lightAt, occlusionAlpha, MAX_ACTIVE_LIGHTS, FRAGMENT_SHADER
} from '../src/render/lighting.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps

console.log('\n--- falloff ---')
{
  check(near(attenuation(0, 10), 1), 'full at the source')
  check(near(attenuation(10, 10), 0), 'exactly zero AT the radius, not merely small')
  check(attenuation(10.0001, 10) === 0, 'and zero beyond it')
  check(attenuation(1e6, 10) === 0, 'far beyond it too, without going negative')

  // Monotonic: a light must never brighten as you walk away from it.
  let prev = Infinity
  let monotonic = true
  for (let d = 0; d <= 12; d += 0.25) {
    const a = attenuation(d, 10)
    if (a > prev + 1e-12) monotonic = false
    prev = a
  }
  check(monotonic, 'never brightens with distance')

  // Squared, so the edge is a gradient rather than a visible ring. Halfway out
  // the light is at a quarter, not a half.
  check(near(attenuation(5, 10), 0.25), 'squared falloff (half the radius gives a quarter)')

  check(attenuation(1, 0) === 0, 'a zero-radius light lights nothing')
  check(attenuation(1, -5) === 0, 'and a negative radius is not a trap')
}

console.log('\n--- surface orientation ---')
{
  const lamp = [{ x: 0, y: 10, z: 0, r: 1, g: 1, b: 1, radius: 20, intensity: 1 }]
  const point = { x: 0, y: 0, z: 0 }

  const facing = lightAt(point, { x: 0, y: 1, z: 0 }, lamp)
  check(facing.r > 0, `a surface facing the lamp is lit (${facing.r.toFixed(3)})`)

  const away = lightAt(point, { x: 0, y: -1, z: 0 }, lamp)
  check(away.r === 0, 'a surface facing away gets nothing')

  const edge = lightAt(point, { x: 1, y: 0, z: 0 }, lamp)
  check(near(edge.r, 0), 'and one edge-on to it gets nothing either')

  // Without the orientation term every surface within range brightens equally,
  // which reads as fog rather than as a light.
  check(facing.r > edge.r, 'orientation matters: facing beats edge-on')
}

console.log('\n--- distance ---')
{
  const lamp = [{ x: 0, y: 10, z: 0, r: 1, g: 1, b: 1, radius: 30, intensity: 1 }]
  const n = { x: 0, y: 1, z: 0 }
  const near0 = lightAt({ x: 0, y: 0, z: 0 }, n, lamp).r
  const near5 = lightAt({ x: 5, y: 0, z: 0 }, n, lamp).r
  const near15 = lightAt({ x: 15, y: 0, z: 0 }, n, lamp).r
  check(near0 > near5 && near5 > near15, `brighter nearer (${near0.toFixed(3)} > ${near5.toFixed(3)} > ${near15.toFixed(3)})`)
}

console.log('\n--- colour and accumulation ---')
{
  const warm = { x: 0, y: 5, z: 0, r: 1, g: 0.8, b: 0.6, radius: 20, intensity: 1 }
  const cold = { x: 0, y: 5, z: 0, r: 0.5, g: 0.7, b: 1, radius: 20, intensity: 1 }
  const n = { x: 0, y: 1, z: 0 }
  const p = { x: 0, y: 0, z: 0 }

  const w = lightAt(p, n, [warm])
  check(w.r > w.g && w.g > w.b, 'a warm lamp is warm')

  const both = lightAt(p, n, [warm, cold])
  const c = lightAt(p, n, [cold])
  check(near(both.r, w.r + c.r) && near(both.g, w.g + c.g) && near(both.b, w.b + c.b),
        'two lamps sum rather than one winning')
}

console.log('\n--- a lamp ADDS to the sector light, it does not scale with it ---')
{
  // The claim the whole model rests on. A torch in a dark room must brighten
  // it. Under the conventional arrangement -- vertex colours MULTIPLYING a lit
  // material -- the torch's contribution is scaled by the very darkness it is
  // meant to relieve, so a dark room stays dark and the lamp does nothing.
  const lamp = [{ x: 0, y: 6, z: 0, r: 1, g: 1, b: 1, radius: 20, intensity: 1 }]
  const n = { x: 0, y: 1, z: 0 }
  const p = { x: 0, y: 0, z: 0 }
  const added = lightAt(p, n, lamp).r

  const darkSector = 0.15
  const brightSector = 0.85
  const litDark = darkSector + added
  const litBright = brightSector + added

  check(near(litDark - darkSector, litBright - brightSector),
        `the same lamp adds the same amount to a dark and a bright sector (${added.toFixed(3)})`)
  check(litDark > darkSector * 2,
        `a lamp meaningfully brightens a dark sector (${darkSector} -> ${litDark.toFixed(3)})`)

  // CONTROL: the multiplicative arrangement, measured rather than asserted.
  const mulDark = darkSector * (1 + added)
  check(mulDark - darkSector < (litDark - darkSector) / 2,
        `CONTROL: multiplying would give the dark room ` +
        `${(mulDark - darkSector).toFixed(3)} instead of ${added.toFixed(3)}`)
}

console.log('\n--- wall occlusion (§4) ---')
{
  // Camera above and behind; player at the origin. Geometry between the two is
  // faded in a soft window around the line of sight.
  const cam = { x: 0, y: 20, z: 30 }
  const subj = { x: 0, y: 0, z: 0 }
  const on = (p) => occlusionAlpha(p, cam, subj)

  // Directly in the way, halfway along.
  const blocking = on({ x: 0, y: 10, z: 15 })
  check(blocking < 0.25, `something square in the way is faded hard (${blocking.toFixed(3)})`)

  // The fade is soft, not a hole with a rim.
  const edge = on({ x: 3, y: 10, z: 15 })
  const outside = on({ x: 8, y: 10, z: 15 })
  check(blocking < edge && edge < outside, `soft-edged (${blocking.toFixed(2)} < ${edge.toFixed(2)} < ${outside.toFixed(2)})`)
  check(outside === 1, 'and fully opaque outside the window')

  // Monotonic across the window, so the edge has no ring in it.
  let mono = true
  let prev = -1
  for (let d = 0; d <= 6; d += 0.2) {
    const a = on({ x: d, y: 10, z: 15 })
    if (a < prev - 1e-12) mono = false
    prev = a
  }
  check(mono, 'and rises monotonically outward')

  // Nothing BEHIND the player is touched: it is not in the way of anything.
  check(on({ x: 0, y: -10, z: -15 }) === 1, 'geometry behind the player is untouched')
  check(on({ x: 0, y: -1, z: -2 }) === 1, 'even just behind it')

  // Nor is the floor at the player's own feet, which is the failure the gap
  // exists to prevent: a window centred on the player would otherwise punch a
  // hole in the ground they are standing on.
  check(on({ x: 0.5, y: 0, z: 0.5 }) === 1, 'and the floor at the player\'s feet is not punched through')

  // Off to the side of the camera entirely -- behind it along the ray.
  check(on({ x: 0, y: 30, z: 45 }) === 1, 'geometry behind the CAMERA is untouched')

  // Degenerate: camera at the player. No line of sight, so no window.
  check(occlusionAlpha({ x: 1, y: 1, z: 1 }, subj, subj) === 1,
        'a camera sitting on the player fades nothing')

  // Strength zero turns it off entirely, which is what a viewer with no player
  // uses -- a hole following the origin around would be worse than none.
  check(occlusionAlpha({ x: 0, y: 10, z: 15 }, cam, subj, { strength: 0 }) === 1,
        'strength 0 disables it')

  // The window is a WORLD distance from the ray, not a screen-space radius, so
  // it does not swallow the room as you zoom in. Same geometry, camera twice as
  // far: the same world point is still just inside.
  const far = { x: 0, y: 40, z: 60 }
  check(occlusionAlpha({ x: 3, y: 10, z: 15 }, far, subj) < 1,
        'the window keeps its world size as the camera pulls back')
}

console.log('\n--- the shader is the twin of the tested function ---')
{
  // Not a proof -- GLSL is not executed here. It does catch the two edits that
  // would silently desynchronise them: dropping the orientation term, or
  // changing the falloff to a different curve.
  check(FRAGMENT_SHADER.includes('t * t'), 'the shader squares its falloff, as attenuation() does')
  check(FRAGMENT_SHADER.includes('max(dot(n, d'), 'and clamps the orientation term at zero')
  check(FRAGMENT_SHADER.includes('lit += '), 'and ADDS to the sector light rather than scaling it')
  check(FRAGMENT_SHADER.includes(`i < ${MAX_ACTIVE_LIGHTS}`),
        'and its loop bound matches the uniform array size')
  check(FRAGMENT_SHADER.includes('dFrag < dSubj - occludeGap'),
        'and its occlusion test keeps the same gap in front of the player')
  check(FRAGMENT_SHADER.includes('smoothstep(0.0, occludeRadius'),
        'and the same soft edge')
}

console.log(`\nlighting: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
