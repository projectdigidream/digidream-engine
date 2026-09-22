//
// The ThreeSprite atlas reader: what a frame's quad and UVs should be.
//
// No GPU and no sheet. Everything this checks is arithmetic over a manifest --
// where a trimmed frame sits on its canvas, how that becomes a quad hanging off
// the feet, and which rectangle of the sheet it samples.
//
// THE FIXTURES ARE BUILT THE WAY THE EXPORTER BUILDS THEM. ThreeSprite packs in
// Rust (src-tauri/src/atlas.rs): frames are trimmed to their opaque pixels,
// shelf-packed, and each one's pivot is written as the feet point relative to
// the trimmed rect, normalised -- `(n/2 - t.x) / t.w`, `(n - t.y) / t.h`. So
// `manifest()` below derives pivot from source with that same formula, which is
// what makes `pivot agrees with source` a real check of the READER rather than
// a check of the fixture.
//
import { parseAtlas, pivotDisagreement, frameKey, quadFor, uvFor, sheetsOf } from '../src/render/atlas.js'
import { parseProps, rotationFor, writeAtlasFrame } from '../src/render/props.js'
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps

const CANVAS = 64

/** A manifest shaped exactly as the exporter writes one. */
function manifest ({ frames, sheet = { w: 128, h: 128 }, views = 8, worldHeight = 2, canvas = CANVAS } = {}) {
  const out = {}
  const views1 = {}
  for (const [key, source] of Object.entries(frames)) {
    out[key] = {
      // COPIED, NOT REFERENCED. The controls below mutate the manifest they are
      // handed; sharing this object with the module-level fixture let the first
      // of them corrupt every test after it, and they all then "passed" on the
      // same stale error.
      rect: { ...source.rect },
      source: { x: source.x, y: source.y, w: source.w, h: source.h },
      pivot: {
        x: (canvas / 2 - source.x) / source.w,
        y: (canvas - source.y) / source.h
      }
    }
    views1[String(source.view ?? 1)] = key
  }
  return {
    format: 'threesprite-atlas',
    version: 1,
    sprite: 'SPRT',
    canvas: { w: canvas, h: canvas },
    worldHeight,
    image: 'SPRT.png',
    size: sheet,
    views: { count: views, mirror: false, list: [] },
    frames: out,
    animations: [{ name: 'idle', fps: 8, loop: true, frames: [{ index: 0, views: views1 }] }]
  }
}

// A frame that fills the canvas, and one trimmed to a small patch off-centre.
const FULL = { x: 0, y: 0, w: CANVAS, h: CANVAS, rect: { x: 0, y: 0, w: CANVAS, h: CANVAS }, view: 1 }
// OFF-CENTRE ON PURPOSE. A trim centred on the canvas has dx 0, and an
// assertion that dx is 0 passes just as well for a quadFor() that never
// computes dx at all. 16 + 24/2 = 28, four pixels left of the canvas centre.
const TRIM = { x: 16, y: 8, w: 24, h: 40, rect: { x: 64, y: 0, w: 24, h: 40 }, view: 2 }

console.log('--- reading a manifest ---')
{
  const a = parseAtlas(manifest({ frames: { 'idle/0/v1': FULL, 'idle/0/v2': TRIM } }), 'fixture')
  check(a.canvas === CANVAS && a.views.count === 8, `canvas ${a.canvas}, ${a.views.count} views`)
  check(a.worldHeight === 2 && a.image === 'SPRT.png', 'world height and sheet name came through')
  check(sheetsOf([a, a]).length === 1, 'one sheet listed once, however many atlases name it')
}

console.log('\n--- a manifest that cannot be drawn is refused, not guessed at ---')
{
  const refuses = (mutate, why) => {
    const m = manifest({ frames: { 'idle/0/v1': FULL } })
    mutate(m)
    let threw = null
    try { parseAtlas(m, 'fixture') } catch (e) { threw = e.message }
    check(threw !== null, `CONTROL: ${why} -- ${threw ?? 'ACCEPTED, which is the bug'}`)
  }
  refuses((m) => { m.format = 'aseprite' }, 'a different format')
  refuses((m) => { m.version = 2 }, 'a newer version')
  refuses((m) => { m.canvas.w = 32 }, 'a canvas that is not square')
  refuses((m) => { delete m.worldHeight }, 'no world height')
  refuses((m) => { m.frames['idle/0/v1'].rect.x = 120 }, 'a frame running off the sheet')
  refuses((m) => { m.frames['idle/0/v1'].source.h = CANVAS + 8 }, 'a source rect off the canvas')
  refuses((m) => { m.animations[0].frames[0].views['1'] = 'idle/9/v1' }, 'an animation naming a frame that is not there')
}

console.log('\n--- the feet, stated twice, must agree ---')
{
  const a = parseAtlas(manifest({ frames: { 'idle/0/v1': FULL, 'idle/0/v2': TRIM } }), 'fixture')
  check(near(pivotDisagreement(a), 0), `pivot agrees with source (worst ${pivotDisagreement(a).toExponential(1)})`)

  // CONTROL: a manifest whose two statements of the feet disagree must be
  // caught. Without this the check above passes for a reader that ignores
  // pivot entirely -- which is exactly what it would do if it were broken.
  const m = manifest({ frames: { 'idle/0/v1': FULL } })
  m.frames['idle/0/v1'].pivot.y += 0.25
  const b = parseAtlas(m, 'fixture')
  check(near(pivotDisagreement(b), 0.25), `CONTROL: a wrong pivot is reported (${pivotDisagreement(b)})`)
}

console.log('\n--- the quad ---')
{
  const a = parseAtlas(manifest({ frames: { 'idle/0/v1': FULL, 'idle/0/v2': TRIM } }), 'fixture')

  // The cheapest check that the feet arithmetic is right: an untrimmed frame
  // is the whole canvas, so it is worldHeight tall and its centre sits at half
  // that above the feet -- which is the yOffset a sprite has always used.
  const full = quadFor(a, 'idle/0/v1')
  check(near(full.w, 2) && near(full.h, 2), `a full-canvas frame is worldHeight square (${full.w} x ${full.h})`)
  check(near(full.dx, 0) && near(full.dy, 1), `and its centre is half a height above the feet (dy ${full.dy})`)

  // A trim of 24x40 at (20,8) on a 64 canvas, worldHeight 2 -> scale 1/32.
  const t = quadFor(a, 'idle/0/v2')
  check(near(t.w, 24 / 32) && near(t.h, 40 / 32), `a trimmed frame keeps its trimmed size (${t.w} x ${t.h})`)
  check(near(t.dx, (16 + 12 - 32) / 32) && t.dx < 0,
        `offset LEFT of the feet by its trim, and not merely zero (dx ${t.dx})`)
  check(near(t.dy, (64 - 28) / 32), `and lifted to where the trim sat (dy ${t.dy})`)

  // Scaling the sprite scales the whole quad, offsets included. A sprite drawn
  // at twice the height whose feet stayed put would sink into the floor.
  const big = quadFor(a, 'idle/0/v2', 4)
  check(near(big.h, t.h * 2) && near(big.dy, t.dy * 2), 'a taller sprite scales its offset too')
}

console.log('\n--- the sheet rectangle ---')
{
  const a = parseAtlas(manifest({ frames: { 'idle/0/v2': TRIM } }), 'fixture')
  const uv = uvFor(a, 'idle/0/v2')
  // rect (64,0,24,40) on a 128x128 sheet. u: 0.5 .. 0.6875.
  check(near(uv[0], 0.5) && near(uv[2], 0.6875), `u spans the rect (${uv[0]} .. ${uv[2]})`)
  // y 0 is the TOP of the image, which is v = 1 after the upload flip.
  check(near(uv[1], 1), 'the rect top is v 1')
  check(near(uv[5], 1 - 40 / 128), `and its bottom is lower (v ${uv[5]})`)
  check(uv[1] > uv[5], 'top-left sits above bottom-left, which is the flip done right')

  // CONTROL: the flip is the one thing here that is easy to get backwards, and
  // on a PACKED sheet getting it wrong does not draw the image upside down --
  // it draws a different frame. An unflipped reader must disagree.
  const unflipped = [TRIM.rect.y / 128, (TRIM.rect.y + TRIM.rect.h) / 128]
  check(!near(uv[1], unflipped[0]), 'CONTROL: reading v straight off the manifest gives a different answer')
}

console.log('\n--- looking a frame up ---')
{
  const a = parseAtlas(manifest({ frames: { 'idle/0/v1': FULL } }), 'fixture')
  check(frameKey(a, 'idle', 0, 1) === 'idle/0/v1', 'by animation, frame and view')
  check(frameKey(a, 'idle', 4, 1) === 'idle/0/v1', 'a frame counter past the end wraps rather than failing')
  check(frameKey(a, 'walk', 0, 1) === null, 'an animation that is not there is null, not a throw')
  check(frameKey(a, 'idle', 0, 7) === null, 'and so is a view the export does not have')
}

console.log('\n--- the catalogue record ---')
{
  const { kinds, warnings } = parseProps(`
prop 90 spirit radius 0.3 height 1.6 solid
atlas 90 sheet SPRT anim idle
atlas 90 state 2 sheet SPRT anim hurt
`)
  const d = kinds.get(90)
  check(d?.type === 'atlas' && d.sheet === 'SPRT', `an atlas line names its sheet ('${d?.sheet}')`)
  check(d.states.get(0)?.anim === 'idle' && d.states.get(2)?.anim === 'hurt', 'one animation per state')
  check(d.radius === 0.3 && d.flags === 1, 'and the prop line beside it still reaches the simulation')
  check(warnings.length === 0, `no warnings (${warnings.join('; ') || 'none'})`)

  // CONTROL: a line that cannot draw is reported, not half-kept. Without this
  // the checks above pass for a parser that accepts anything at all.
  const bad = parseProps('atlas 91 sheet SPRT\natlas 92 anim idle\n')
  check(bad.warnings.length === 2, `CONTROL: a missing animation and a missing sheet both warn (${bad.warnings.length})`)
  check(!bad.kinds.get(91)?.states.size && !bad.kinds.get(92)?.states.size, 'and neither gets a state')
}

console.log('\n--- picking a view, for sheets that are not eight ---')
{
  // Generalising rotationFor is only safe if the eight-view answer did not
  // move. This is that control: the numbers below are the ones the function
  // returned before it took an argument.
  check(rotationFor(0, 0) === 1 && rotationFor(0, 0.25) === 3 && rotationFor(0, 0.5) === 5,
        'CONTROL: the eight-view answers are unchanged')
  check(rotationFor(0, 0.4) === 4 && rotationFor(0.25, 0.25) === 1, 'CONTROL: including the off-axis ones')

  check(rotationFor(0, 0, 4) === 1 && rotationFor(0, 0.25, 4) === 2 && rotationFor(0, 0.75, 4) === 4,
        'four views split the circle in quarters')
  check(rotationFor(0, 0.49, 1) === 1 && rotationFor(0, 0.99, 1) === 1, 'one view is always view one')
  check(rotationFor(0, 1 / 32, 16) === 2, 'sixteen views round to the nearest, not down')
  check(rotationFor(0.5, 0.5, 16) === 1, 'and it is the angle BETWEEN facing and camera that counts')
}

console.log('\n--- writing a frame into a plane ---')
{
  const a = parseAtlas(manifest({ frames: { 'idle/0/v2': TRIM } }), 'fixture')
  // A stand-in for a BufferGeometry: enough of one to record what was written.
  const attr = (n) => ({ a: new Array(n * 3).fill(0), needsUpdate: false,
    setXYZ (i, x, y, z) { this.a[i * 3] = x; this.a[i * 3 + 1] = y; this.a[i * 3 + 2] = z },
    setXY (i, x, y) { this.a[i * 3] = x; this.a[i * 3 + 1] = y } })
  let spheres = 0
  const geo = { attributes: { position: attr(4), uv: attr(4) }, computeBoundingSphere () { spheres++ } }

  check(writeAtlasFrame(geo, a, 'idle/0/v2') === true, 'the frame was written')
  const p = geo.attributes.position.a
  const q = quadFor(a, 'idle/0/v2')
  // Corner 0 is top-left, corner 3 bottom-right.
  check(near(p[0], q.dx - q.w / 2) && near(p[1], q.dy + q.h / 2), 'corner 0 is the top left of the quad')
  check(near(p[9], q.dx + q.w / 2) && near(p[10], q.dy - q.h / 2), 'corner 3 is the bottom right')
  check(geo.attributes.position.needsUpdate && geo.attributes.uv.needsUpdate, 'both attributes are marked dirty')
  check(spheres === 1, 'and the bounding sphere is recomputed, or the sprite culls while on screen')

  // THE FEET. Every corner of a full-canvas frame sits at or above the origin,
  // and the bottom edge sits exactly on it -- that is what "hangs off its feet"
  // means, and it is the one thing a wrong sign here would break invisibly
  // until something stood on a ledge.
  const b = parseAtlas(manifest({ frames: { 'idle/0/v1': FULL } }), 'fixture')
  const g2 = { attributes: { position: attr(4), uv: attr(4) }, computeBoundingSphere () {} }
  writeAtlasFrame(g2, b, 'idle/0/v1')
  const ys = [1, 4, 7, 10].map((i) => g2.attributes.position.a[i])
  check(near(Math.min(...ys), 0), `the bottom edge sits on the feet (${Math.min(...ys)})`)
  check(near(Math.max(...ys), b.worldHeight), `and the top at full height (${Math.max(...ys)})`)

  check(writeAtlasFrame({ attributes: {} }, b, 'idle/0/v1') === false, 'a geometry with no attributes is refused')
  check(writeAtlasFrame(geo, b, 'nope/0/v1') === false, 'and so is a frame that is not in the sheet')
}

console.log('\n--- the sheet the module actually ships ---')
{
  // The fixtures above prove the READER. This proves it against a real export,
  // which is a different failure: a manifest this cannot draw would pass every
  // fixture and still ship a level of invisible monsters. Skipped loudly rather
  // than failed when the module is not present, the way mapmesh.test.js does.
  const SHEET = join(ROOT, 'modules', 'doom2', 'assets', 'sprites', 'SPIR.json')
  if (!existsSync(SHEET)) {
    console.log('  (skipped: modules/doom2/assets/sprites/SPIR.json is not here)')
  } else {
    const a = parseAtlas(JSON.parse(readFileSync(SHEET, 'utf8')), 'SPIR.json')
    check(a.views.count === 8 && a.animations.length === 4,
          `${Object.keys(a.frames).length} frames, ${a.animations.length} animations, ${a.views.count} views`)
    check(pivotDisagreement(a) === 0, 'its two statements of the feet agree exactly')

    // Every frame, not a sample. A packed sheet has one rect per frame and any
    // one of them can be wrong on its own.
    let bad = 0
    let lowest = Infinity
    let tallest = 0
    for (const key of Object.keys(a.frames)) {
      const q = quadFor(a, key)
      const uv = uvFor(a, key)
      if (!q || !uv || !(q.w > 0 && q.h > 0)) { bad++; continue }
      if (uv.some((v) => v < -1e-9 || v > 1 + 1e-9)) bad++
      lowest = Math.min(lowest, q.dy - q.h / 2)
      tallest = Math.max(tallest, q.dy + q.h / 2)
    }
    check(bad === 0, `every frame gives a quad with UVs inside the sheet (${bad} bad)`)
    // THE FEET, ON REAL ART. Nothing may hang below the origin, and nothing may
    // stand taller than the height the sheet declares -- which together are
    // what "the sprite stands on the floor at the size it says" means.
    check(lowest >= -1e-9, `nothing hangs below the feet (lowest ${lowest.toFixed(4)})`)
    check(tallest <= a.worldHeight + 1e-9,
          `and nothing exceeds its declared height (tallest ${tallest.toFixed(4)} of ${a.worldHeight})`)

    // The catalogue names animations in this sheet, and a typo there draws
    // nothing at all -- silently, because a missing animation is warned once
    // and then skipped.
    const cat = parseProps(readFileSync(join(ROOT, 'modules', 'doom2', 'assets', 'props.txt'), 'utf8'))
    for (const [, def] of cat.kinds) {
      if (def.type !== 'atlas' || def.sheet !== a.sprite) continue
      for (const [stateId, st] of def.states) {
        check(a.byName.has(st.anim),
              `catalogue state ${stateId} names animation '${st.anim}', which the sheet has`)
      }
    }
  }
}

console.log(`\natlas: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
