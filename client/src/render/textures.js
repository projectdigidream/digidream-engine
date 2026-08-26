//
// Placeholder textures, generated from the map's own texture NAMES.
//
// There is no art. The map says BRICK1 and the client resolves it -- that
// separation is the point of shipping names rather than images (§4) -- so this
// is one client's asset set, and it happens to be a generator rather than a
// folder of PNGs.
//
// Generated rather than a handful of flat colours on purpose. A flat-shaded
// world hides exactly the mistakes this phase is most likely to make: a texture
// with no direction cannot show that a wall's UVs are rotated, and one with no
// scale cannot show that pegging anchored a field to the wrong surface. These
// have a visible grid, a distinguishable up, and their own name written across
// them, so a wrong surface looks wrong instead of looking like a colour.
//

// Every texture is this many world units square unless told otherwise. Wall
// heights in the test maps are single digits, so 4 puts a couple of tiles on a
// typical wall -- enough to see tiling and alignment without turning the screen
// into a grid.
export const DEFAULT_WORLD_SIZE = 4

const PIXELS = 128

function hashName (name) {
  let h = 2166136261 >>> 0
  for (let i = 0; i < name.length; i++) {
    h ^= name.charCodeAt(i)
    h = Math.imul(h, 16777619) >>> 0
  }
  return h
}

/**
 * Draw one placeholder texture into a canvas.
 *
 * Exported separately from the THREE.Texture wrapper so it can be exercised
 * without a GPU or a DOM -- any canvas-like object with a 2d context will do.
 */
export function paintPlaceholder (ctx, name, { pixels = PIXELS } = {}) {
  const h = hashName(name)
  const hue = h % 360
  const sat = 24 + (h >> 9) % 22
  const light = 38 + (h >> 17) % 18

  ctx.fillStyle = `hsl(${hue} ${sat}% ${light}%)`
  ctx.fillRect(0, 0, pixels, pixels)

  // A brick-ish course, offset row by row. Rows give the surface a clear "up",
  // which is what makes a vertically mis-anchored texture obvious.
  const rows = 4
  const cols = 2
  const rh = pixels / rows
  const cw = pixels / cols
  ctx.strokeStyle = `hsl(${hue} ${sat}% ${Math.max(8, light - 16)}%)`
  ctx.lineWidth = 2
  for (let r = 0; r < rows; r++) {
    const y = r * rh
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(pixels, y); ctx.stroke()
    const shift = (r % 2) * (cw / 2)
    for (let c = 0; c < cols; c++) {
      const x = c * cw + shift
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x, y + rh); ctx.stroke()
    }
  }

  // A bright corner marker: one unambiguous origin, so a rotated or mirrored
  // UV set is visible at a glance rather than only as "the pattern looks odd".
  ctx.fillStyle = `hsl(${(hue + 40) % 360} 70% 70%)`
  ctx.fillRect(2, 2, pixels / 10, pixels / 10)

  ctx.fillStyle = `hsl(${hue} 20% ${Math.min(92, light + 45)}%)`
  ctx.font = `bold ${Math.round(pixels / 9)}px monospace`
  ctx.textAlign = 'center'
  ctx.textBaseline = 'middle'
  ctx.fillText(name, pixels / 2, pixels / 2)
  return ctx
}

/**
 * Build a THREE.Texture per name.
 *
 * `THREE` is passed in rather than imported, so this module stays loadable in
 * node -- the painting above is the part worth testing and it does not need a
 * renderer.
 */
export function makeTextureSet (THREE, names) {
  return names.map((name) => {
    const canvas = document.createElement('canvas')
    canvas.width = canvas.height = PIXELS
    paintPlaceholder(canvas.getContext('2d'), name)

    const tex = new THREE.CanvasTexture(canvas)
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping
    // UVs arrive in WORLD UNITS (see mapgeom.js), so the repeat is what turns
    // them into texture space. This is the one place the texture's world size
    // is known, which is exactly why the mesh does not need to know it -- and
    // why the same mesh serves clients whose textures are different sizes.
    tex.repeat.set(1 / DEFAULT_WORLD_SIZE, 1 / DEFAULT_WORLD_SIZE)
    // A canvas's y runs DOWNWARD and a texture's v runs upward, so three flips
    // canvas sources by default. Our v is world distance measured downward from
    // the field's anchor, which is already in the canvas's direction -- leaving
    // the flip on turns every wall's texture upside down, which is how the
    // first rendered frame came out.
    tex.flipY = false
    tex.colorSpace = THREE.SRGBColorSpace
    tex.magFilter = THREE.NearestFilter
    tex.name = name
    return tex
  })
}
