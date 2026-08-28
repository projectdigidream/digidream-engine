//
// Animated textures (render/animtex.js).
//
// Doom animates a texture from the FIRST frame of a sequence: a map names
// NUKAGE1 and the engine cycles to NUKAGE2 and NUKAGE3 without the map ever
// mentioning them. So the failure mode here is not an error -- it is water that
// does not move, which nobody reports as a bug and no assertion notices unless
// one is written for it.
//
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { parseAnimations, animFrameAt, TextureAnimator } from '../src/render/animtex.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

console.log('anim: textures that move')

const cat = parseAnimations(readFileSync(join(ROOT, 'assets', 'animations.txt'), 'utf8'))
check(cat.groups.length > 0, `the catalogue describes ${cat.groups.length} sequences`)
check(cat.warnings.length === 0, `and parses clean (${cat.warnings.length} warnings)`)

// A sequence of one frame is not a sequence, and a rate of zero is not a rate.
{
  const bad = parseAnimations('anim 4 ONLYONE\nanim 0 A B\nanim x A B\n')
  check(bad.groups.length === 0 && bad.warnings.length === 3,
        `malformed records are refused and named (${bad.warnings.length} warnings)`)
}

// --- the clock ---------------------------------------------------------------

check(animFrameAt(4, 3, 0) === 0, 'a sequence starts at its first frame')
check(animFrameAt(4, 3, 0.26) === 1, 'and advances')
check(animFrameAt(4, 3, 0.75) === 0, 'and WRAPS -- a flat is a loop, not an event')
check(animFrameAt(4, 1, 99) === 0, 'a single frame never moves')
check(animFrameAt(0, 3, 99) === 0, 'and neither does a zero rate')
// Every frame is reachable: an off-by-one in the modulo shows up here as a
// frame that never appears, which on a three-frame pool is easy to miss by eye.
{
  const seen = new Set()
  for (let t = 0; t < 4; t += 0.01) seen.add(animFrameAt(4.375, 4, t))
  check(seen.size === 4, `a full cycle visits all four frames (${seen.size})`)
}

// --- the animator ------------------------------------------------------------

const fakeMaterial = () => ({ uniforms: { map: { value: null } } })

{
  const groups = [{ fps: 4, frames: ['NUKAGE1', 'NUKAGE2', 'NUKAGE3'] },
                  { fps: 4, frames: ['LAVA1', 'LAVA2'] }]
  const names = ['STARTAN3', 'NUKAGE1']          // the map uses nukage, not lava
  const materials = [fakeMaterial(), fakeMaterial()]
  const have = new Map([['NUKAGE1', 'n1'], ['NUKAGE2', 'n2'], ['NUKAGE3', 'n3']])

  const anim = new TextureAnimator({
    groups, names, materials, frameTexture: (n) => have.get(n) ?? null
  })
  check(anim.count === 1, `only sequences this map uses are animated (${anim.count})`)

  anim.update(0)
  check(materials[1].uniforms.map.value === 'n1', 'frame 0 is the first')
  anim.update(0.26)
  check(materials[1].uniforms.map.value === 'n2', 'and it advances')
  anim.update(0.76)
  check(materials[1].uniforms.map.value === 'n1', 'and wraps')
  // The material the map's OTHER texture uses is untouched -- animating by
  // index rather than by name would have written to whichever slot came first.
  check(materials[0].uniforms.map.value === null,
        'a static texture is left alone')
}

// A sequence whose images the client does not have is dropped, not pinned to
// its first frame: a partial extraction should cost motion, not the wall.
{
  const groups = [{ fps: 4, frames: ['LAVA1', 'LAVA2', 'LAVA3'] }]
  const materials = [fakeMaterial()]
  const anim = new TextureAnimator({
    groups, names: ['LAVA1'], materials,
    frameTexture: (n) => (n === 'LAVA1' ? 'l1' : null)
  })
  check(anim.count === 0, 'one available frame is not an animation')
  anim.update(5)
  check(materials[0].uniforms.map.value === null,
        'and the material is left exactly as it was')
}

// --- against the real asset set ----------------------------------------------
//
// The catalogue naming a sequence is worth nothing if the frames were never
// extracted. This is the check that would have caught FWATER shipping with
// frames 1 and 4 and nothing in between.
{
  const dir = join(ROOT, 'assets', 'textures')
  let complete = 0
  let partial = []
  for (const g of cat.groups) {
    if (!existsSync(join(dir, `${g.frames[0]}.png`))) continue   // unused by any map
    const missing = g.frames.filter((f) => !existsSync(join(dir, `${f}.png`)))
    if (missing.length === 0) complete++
    else partial.push(`${g.frames[0]} missing ${missing.join(',')}`)
  }
  console.log(`  ${complete} sequences complete, ${partial.length} partial`)
  for (const p of partial.slice(0, 4)) console.log(`     ${p}`)
  check(complete > 0, `at least one sequence is fully extracted (${complete})`)
  check(partial.length === 0,
        `every sequence whose first frame is present has all of them (${partial.length} partial)`)
}

console.log(`\nanim: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
