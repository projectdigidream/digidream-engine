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
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import {
  attenuation, lightAt, occlusionAlpha, MAX_ACTIVE_LIGHTS, FRAGMENT_SHADER
} from '../src/render/lighting.js'

const HERE = dirname(fileURLToPath(import.meta.url))

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
  const edge = on({ x: 0.5, y: 10, z: 15 })
  const outside = on({ x: 2, y: 10, z: 15 })
  check(blocking < edge && edge < outside, `soft-edged (${blocking.toFixed(2)} < ${edge.toFixed(2)} < ${outside.toFixed(2)})`)
  check(outside === 1, 'and fully opaque outside the window')

  // Monotonic across the window, so the edge has no ring in it.
  let mono = true
  let prev = -1
  for (let d = 0; d <= 2; d += 0.05) {
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

  // A CONE, the body's size on screen at every depth. Halfway to the camera a
  // wall 1.5 units off the line of sight is clear of the body on screen: it
  // stays whole (the old 3.5-wide cylinder dithered it before it reached the
  // player). CONTROL: the same offset right in front of the player fades.
  check(on({ x: 1.5, y: 10, z: 15 }) === 1, 'a wall beside the line of sight, halfway, is not faded early')
  check(on({ x: 1.0, y: 3.4, z: 5.1 }) < 1, 'CONTROL: the same kind of offset just in front of the player fades')
  // The same angle off the ray fades alike at any depth: the window follows
  // the body on screen, whatever the zoom.
  const at = (k) => on({ x: 0.4 * k, y: 20 - 20 * k, z: 30 - 30 * k })
  check(Math.abs(at(0.3) - at(0.6)) < 1e-9, `one angle, one fade, near or far (${at(0.3).toFixed(3)} / ${at(0.6).toFixed(3)})`)
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
  check(FRAGMENT_SHADER.includes('float r = occludeRadius * t') && FRAGMENT_SHADER.includes('smoothstep(0.0, r, d)'),
        'and the same cone, with the same soft edge')
}

console.log('\n--- the shader and the material it is used with agree ---')
{
  // A TWIN CHECK, and it is here because the drift actually happened.
  //
  // The occlusion window used to be resolved by alpha blending, so the map
  // material was `transparent: true`. It was changed to a dither and a discard
  // -- which is the whole point, a transparent material sorts per object and
  // gets no early-z -- and the flag was left behind. For months the map paid
  // the cost the dither exists to avoid, in silence, and README claimed the
  // opposite.
  //
  // GLSL is not executed here (see the top of this file), so this cannot check
  // what the shader DOES. It can check that the two files still tell the same
  // story, which is the failure that occurred.
  // Only the MAP material. scene.js also builds a debug arrow that is properly
  // transparent, and a check that fired on that would be read as a false alarm
  // and then ignored. Comments are stripped first for the same reason
  // check_render_isolation.sh strips them: a guard that trips on its own
  // explanation is a guard somebody eventually deletes.
  const sceneSrc = readFileSync(join(HERE, '..', 'src', 'render', 'scene.js'), 'utf8')
    .split('\n').filter((l) => !l.trim().startsWith('//')).join('\n')
  // ONLY #material, which is the one every ordinary surface uses. #clearMaterial
  // sits immediately after it and IS transparent, deliberately, and is checked
  // on its own below -- a slice that ran past it would read the see-through
  // twin's flags as the map's and report exactly the drift it exists to catch.
  const mapMaterial = sceneSrc.slice(sceneSrc.indexOf('new THREE.ShaderMaterial('),
                                     sceneSrc.indexOf('#clearMaterial'))

  // Does the shader ever emit a fragment whose alpha is not 1?
  //
  // Of the OPAQUE compilation, which is what the map material gets: the source
  // also carries a `#ifdef DNP_ALPHA` branch for surfaces a map asked to see
  // through, and the preprocessor removes it from every other draw. Cutting it
  // out here is what makes this a question about the material under test rather
  // than about the file it shares.
  const opaqueOnly = (src) => src.replace(/#ifdef DNP_ALPHA[\s\S]*?#else/, '')
  const writes = [...FRAGMENT_SHADER.matchAll(/gl_FragColor\s*=\s*([^;]+);/g)].map((m) => m[1])
  const needsBlending = (src) =>
    [...src.matchAll(/gl_FragColor\s*=\s*([^;]+);/g)]
      .map((m) => m[1]).some((w) => !/,\s*1\.0\s*\)/.test(w))

  check(writes.length > 0, `the fragment shader writes gl_FragColor (${writes.length} site)`)
  check(!needsBlending(opaqueOnly(FRAGMENT_SHADER)),
        'every opaque write ends at alpha 1.0, so the material needs no blending')
  check(FRAGMENT_SHADER.includes('discard'),
        'because the occlusion window is resolved by discard, not by alpha')
  check(mapMaterial.length > 0 && !/transparent:\s*true/.test(mapMaterial),
        'and the map material is not marked transparent')

  // CONTROL: the cut is a cut. The source really does contain a write that
  // needs blending -- the transparent branch -- so `opaqueOnly` removing
  // nothing would be caught here rather than passing quietly.
  check(needsBlending(FRAGMENT_SHADER),
        'CONTROL: the whole source DOES contain a blending write, inside the ifdef')

  // AND THE SEE-THROUGH TWIN IS THE OPPOSITE, in both files. A grate or a glass
  // floor has to blend and must not write depth, which is exactly the cost the
  // map material refuses -- so the two materials must disagree, on purpose, and
  // a change that made them agree again would break one of them silently.
  const clearMaterial = sceneSrc.slice(sceneSrc.indexOf('#clearMaterial'),
                                       sceneSrc.indexOf('ALPHA 254 IS A CUT-OUT'))
  check(/transparent:\s*true/.test(clearMaterial),
        'the see-through material IS transparent')
  check(/depthWrite:\s*false/.test(clearMaterial),
        'and does not write depth, or what is behind it would be rejected')
  check(/FRAGMENT_SHADER_ALPHA/.test(clearMaterial),
        'and it is the only one compiled with the alpha branch')
  check(!/FRAGMENT_SHADER_ALPHA/.test(mapMaterial),
        'CONTROL: the map material is not')

  // AND ALPHA 254 IS NEITHER: a cut-out, "the picture's own holes". The opaque
  // shader with its holes discarded, writing depth and not blended, so water and
  // everything else see-through is ordered against a plank bridge by depth.
  const cutout = sceneSrc.slice(sceneSrc.indexOf('#cutoutMaterial (texture) {'), sceneSrc.indexOf('materialFor ('))
  check(cutout.length > 0 && /CUTOUT/.test(cutout) && /depthWrite:\s*true/.test(cutout) &&
        !/transparent:\s*true/.test(cutout) && !/FRAGMENT_SHADER_ALPHA/.test(cutout),
        'alpha 254 is a cut-out: its holes discarded, depth written, nothing blended')
  check(/alphaTest = 0\.5/.test(cutout), "and says so to the fog's depth pass, which keeps its holes")
  check(/alpha >= 254 \/ 255/.test(sceneSrc) && /#cutoutMaterial\(texture\)/.test(sceneSrc),
        'materialFor sends 254 there, and below it to the blended one')

  // CONTROL: the drift this exists for. The map material as it used to be must
  // be caught -- otherwise the assertion above is satisfied by a slice that
  // failed to find the material at all.
  check(/transparent:\s*true/.test('side: THREE.FrontSide,\n transparent: true,\n depthWrite: true'),
        'CONTROL: the old map material would be caught')

  // CONTROL: a shader that really does need blending must be classified as
  // needing it. Without this, `needsBlending` could be a function that returns
  // false unconditionally and every assertion above would still pass.
  check(needsBlending('void main(){ gl_FragColor = vec4(rgb, alpha); }'),
        'CONTROL: a shader writing a real alpha is classified as needing blending')
}

console.log(`\nlighting: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
