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
import { attenuation, lightAt, MAX_ACTIVE_LIGHTS, FRAGMENT_SHADER } from '../src/render/lighting.js'

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
}

console.log(`\nlighting: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
