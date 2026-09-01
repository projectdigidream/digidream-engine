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
  const sat = 22 + (h >> 9) % 20
  // Mid-bright, because these are multiplied by the lighting: a base this dark
  // enough to read as "textured" under full light goes to mud under a lamp at
  // half strength.
  const light = 52 + (h >> 17) % 16

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
/**
 * Swap in real texture images for the names a map uses.
 *
 * Asynchronous and best-effort, deliberately. Placeholders are created first
 * and the map renders immediately; each real texture replaces one as it
 * arrives, and a name with no image keeps its placeholder rather than leaving a
 * hole. An asset set is a client's business (§4) -- the map carries names -- so
 * a missing one is a client that has fewer textures, not a broken map.
 *
 * `pxPerUnit` is how many texture pixels span one world unit, and it is the one
 * number that cannot be guessed: a Doom map converted at 1/48 wants 48, because
 * a 64-pixel Doom texture covers 64 Doom units and therefore 64/48 of ours.
 */
export async function loadRealTextures (THREE, names, materials, {
  baseUrl = '/textures', pxPerUnit = 32, extra = [],
  // Called as each picture arrives, with (done, total). Optional, and a
  // counter rather than bytes: a texture is one request whose size nobody knows
  // until it lands, so counting requests is the honest measure and is the one
  // the loading bar can show without lying about how far along it is.
  onProgress = null
} = {}) {
  let index = null
  try {
    index = await (await fetch(`${baseUrl}/index.json`)).json()
  } catch {
    return { loaded: 0, total: names.length }
  }
  const sizes = index?.sizes ?? {}
  const cutout = index?.cutout ?? {}

  const loader = new THREE.TextureLoader()
  let loaded = 0

  // Shared setup, so a frame loaded for an animation is configured exactly like
  // one loaded for a material. They are the same pictures on the same walls;
  // two code paths differing by a filter setting would show as a texture that
  // changes appearance the moment it starts moving.
  const configure = (tex) => {
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping
    tex.colorSpace = THREE.NoColorSpace
    tex.magFilter = THREE.NearestFilter
    // Mipmaps off. Doom art is 64 to 128 pixels and reads as mush the moment
    // it is filtered down; nearest with no mips is what it was drawn for.
    tex.minFilter = THREE.NearestFilter
    tex.generateMipmaps = false
    tex.flipY = false
    return tex
  }

  // ANIMATION FRAMES the map never names.
  //
  // A map says NUKAGE1; the sequence also needs NUKAGE2 and NUKAGE3, which
  // appear in no texture list and so have no material of their own. Loaded into
  // a by-name table instead, for the animator to swap between.
  const frames = new Map()
  // Both passes count toward the same total: the animation frames below are
  // downloads like any other, and a bar that ignored them would sit still at
  // the start of every Doom map while thirty of them came in.
  const totalRequests = extra.length + names.length
  let done = 0
  const tick = () => { if (onProgress) onProgress(++done, totalRequests) }

  await Promise.all(extra.map((name) => new Promise((resolve) => {
    if (!sizes[name] || names.includes(name)) { tick(); resolve(); return }
    loader.load(`${baseUrl}/${name}.png`, (tex) => {
      frames.set(name, configure(tex))
      tick()
      resolve()
    }, undefined, () => { tick(); resolve() })
  })))
  await Promise.all(names.map((name, i) => new Promise((resolve) => {
    const dim = sizes[name]
    if (!dim || !materials[i]) { tick(); resolve(); return }
    loader.load(`${baseUrl}/${name}.png`, (tex) => {
      configure(tex)
      frames.set(name, tex)
      materials[i].uniforms.map.value = tex
      // Each texture covers its own amount of world, so the divisor is
      // per-material rather than one shared constant.
      materials[i].uniforms.repeat.value.set(pxPerUnit / dim[0], pxPerUnit / dim[1])
      // Only a texture with a transparent pixel gets the discard, and with it
      // the loss of early-z rejection for its whole material.
      if (cutout[name]) materials[i].defines = { ...(materials[i].defines ?? {}), CUTOUT: '' }
      materials[i].needsUpdate = true
      loaded++
      tick()
      resolve()
    }, undefined, () => { tick(); resolve() })
  })))
  return { loaded, total: names.length, frames }
}

export function makeTextureSet (THREE, names) {
  return names.map((name) => {
    const canvas = document.createElement('canvas')
    canvas.width = canvas.height = PIXELS
    paintPlaceholder(canvas.getContext('2d'), name)

    const tex = new THREE.CanvasTexture(canvas)
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping
    // NOT tex.repeat: the custom material samples with an explicit `repeat`
    // uniform, and setting both would divide the world-unit UVs twice. The
    // texture's world size is still known in exactly one place -- it is just
    // this constant, read by the material.
    // A canvas's y runs DOWNWARD and a texture's v runs upward, so three flips
    // canvas sources by default. Our v is world distance measured downward from
    // the field's anchor, which is already in the canvas's direction -- leaving
    // the flip on turns every wall's texture upside down, which is how the
    // first rendered frame came out.
    tex.flipY = false
    // NoColorSpace, not sRGB. The custom material writes gl_FragColor directly
    // and includes none of Three's colour-management chunks, so a texture
    // declared sRGB would be decoded on sampling and never re-encoded on
    // output -- darkening everything by roughly a gamma. Everything here stays
    // in display space, consistently, which is the only arrangement where what
    // a map author picks is what appears.
    tex.colorSpace = THREE.NoColorSpace
    tex.magFilter = THREE.NearestFilter
    tex.name = name
    return tex
  })
}
