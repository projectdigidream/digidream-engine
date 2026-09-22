//
// ThreeSprite atlases: one packed sheet plus a JSON manifest.
//
// ThreeSprite (the sprite editor, a separate program) exports a sprite two
// ways. A `.tsprite` is its PROJECT -- a rig, part images and animation keys --
// and turning one into pixels means evaluating that rig, which is ThreeSprite's
// job and is deliberately not done here (tools/tsprite reads what a project
// SAYS; it does not draw one). An atlas is its RUNTIME export, and its own
// description says so: "for Unity, Godot, Phaser or your own engine". This is
// the reader for that.
//
// WHY THIS IS WORTH HAVING AT ALL. The client draws a sprite today as one
// texture per frame name: doom2 ships 2588 separate PNGs and every entity gets
// its own material. An atlas is one image and one material for a whole
// character, with each frame a rectangle inside it -- which is the shape the
// renderer wanted anyway.
//
// TWO CONVENTIONS THAT ALREADY MATCH, which is why this is a reader and not a
// translation layer:
//
//   ROTATIONS. ThreeSprite numbers views from 1, view 1 facing the camera, then
//   anticlockwise seen from above -- "This matches Doom's rotation convention,
//   so Doom-style exports need no remapping". It is also what rotationFor() in
//   props.js already computes, so view selection stays THAT function with a
//   view count passed in, rather than a second copy of the same arithmetic
//   living here.
//
//   FEET. Every frame is composed on one square canvas with the feet on the
//   bottom row, and this renderer already hangs sprites off their feet. So the
//   only work is arithmetic: a frame is TRIMMED to its opaque pixels, and the
//   quad for it has to be sized and offset to put those pixels back where the
//   untrimmed canvas had them.
//
// Pure: no THREE, no DOM, no fetch. What a frame's quad should be is arithmetic
// and arithmetic is worth asserting rather than screenshotting.
//

/** The manifest's own name for itself, and the version this reads. */
export const ATLAS_FORMAT = 'threesprite-atlas'
export const ATLAS_VERSION = 1

const num = (v) => typeof v === 'number' && Number.isFinite(v)
const rect = (r) => r && num(r.x) && num(r.y) && num(r.w) && num(r.h) && r.w > 0 && r.h > 0

/**
 * Read and check a manifest.
 *
 * LOUD, AND FATAL. A half-understood atlas draws something -- the wrong
 * rectangle of the sheet, the right pixels at the wrong height -- and looks
 * like a renderer bug rather than a bad file. Everything this needs is checked
 * here, once, where the file can be named.
 *
 * `name` is only for the message.
 */
export function parseAtlas (json, name = 'atlas') {
  const bad = (msg) => { throw new Error(`${name}: ${msg}`) }

  if (!json || typeof json !== 'object') bad('not an object')
  if (json.format !== ATLAS_FORMAT) bad(`format is '${json.format}', expected '${ATLAS_FORMAT}'`)
  // Refused rather than attempted. A later version may move a field this reads,
  // and guessing produces the wrong rectangle silently.
  if (json.version !== ATLAS_VERSION) {
    bad(`version ${json.version}, this reads ${ATLAS_VERSION}`)
  }

  const canvas = json.canvas
  if (!canvas || !num(canvas.w) || !num(canvas.h)) bad('canvas is missing w/h')
  // The exporter composes on a square canvas and the feet arithmetic below
  // assumes it. Checked rather than assumed: a non-square canvas would put
  // every sprite at a plausible-looking wrong height.
  if (canvas.w !== canvas.h) bad(`canvas is ${canvas.w}x${canvas.h}; frames are composed square`)

  const size = json.size
  if (!size || !num(size.w) || !num(size.h)) bad('size (the sheet) is missing w/h')

  const views = json.views
  if (!views || !Number.isInteger(views.count) || views.count < 1) bad('views.count is missing or not a positive integer')

  if (!num(json.worldHeight) || json.worldHeight <= 0) {
    bad(`worldHeight is ${json.worldHeight}; it sets how tall the sprite stands in the world`)
  }
  if (typeof json.image !== 'string' || !json.image) bad('image (the sheet file name) is missing')

  const frames = json.frames
  if (!frames || typeof frames !== 'object') bad('frames is missing')
  for (const [key, f] of Object.entries(frames)) {
    if (!rect(f?.rect)) bad(`frame '${key}' has no rect in the sheet`)
    if (!rect(f?.source)) bad(`frame '${key}' has no source rect on the canvas`)
    if (f.rect.x + f.rect.w > size.w || f.rect.y + f.rect.h > size.h) {
      bad(`frame '${key}' runs off the sheet`)
    }
    if (f.source.x + f.source.w > canvas.w || f.source.y + f.source.h > canvas.h) {
      bad(`frame '${key}' has a source rect outside the canvas`)
    }
  }

  const animations = json.animations
  if (!Array.isArray(animations) || animations.length === 0) bad('no animations')
  const byName = new Map()
  for (const a of animations) {
    if (typeof a?.name !== 'string' || !a.name) bad('an animation has no name')
    if (!Array.isArray(a.frames)) bad(`animation '${a.name}' has no frames`)
    for (const fr of a.frames) {
      for (const key of Object.values(fr?.views ?? {})) {
        // THE KEYS ARE READ, NEVER BUILT. The manifest composes them from a
        // slug of the animation's name, and re-deriving that slug here would be
        // a second implementation of somebody else's naming rule -- the exact
        // trap this repository keeps paying for. Every frame is reached through
        // this table instead.
        if (!(key in frames)) bad(`animation '${a.name}' names frame '${key}', which is not in frames`)
      }
    }
    byName.set(a.name, a)
  }

  return {
    name,
    sprite: json.sprite ?? name,
    image: json.image,
    canvas: canvas.w,
    sheet: { w: size.w, h: size.h },
    worldHeight: json.worldHeight,
    views: { count: views.count, mirror: !!views.mirror },
    frames,
    animations,
    byName
  }
}

/**
 * The manifest states the feet twice. Do they agree?
 *
 * `pivot` is the feet point relative to the trimmed rect, normalised to it; it
 * is derivable from `source` and the canvas, which is what makes it worth
 * checking rather than reading. If the two ever disagree, one of them is a bug
 * in the exporter and everything drawn from this sheet stands at the wrong
 * height -- a fault that looks like a content mistake, not a code one.
 *
 * Returns the worst disagreement found, in fractions of a rect. Separate from
 * parseAtlas because it is a claim about the FILE rather than a requirement of
 * the reader, and a tool should be able to report it without refusing to draw.
 */
export function pivotDisagreement (atlas) {
  const n = atlas.canvas
  let worst = 0
  for (const f of Object.values(atlas.frames)) {
    if (!f.pivot) continue
    const s = f.source
    worst = Math.max(worst,
      Math.abs(f.pivot.x - (n / 2 - s.x) / s.w),
      Math.abs(f.pivot.y - (n - s.y) / s.h))
  }
  return worst
}

/** The frame key for an animation, a frame index and a 1-based view, or null. */
export function frameKey (atlas, animName, frameIndex, view) {
  const anim = atlas.byName.get(animName)
  if (!anim) return null
  const n = anim.frames.length
  if (n === 0) return null
  // Wrapped, not clamped: a caller advancing a frame counter past the end is
  // looping, which is the common case and not an error.
  const fr = anim.frames[((frameIndex % n) + n) % n]
  return fr?.views?.[String(view)] ?? null
}

/**
 * The quad for a frame: how big to draw it, and where to put it.
 *
 * A frame is trimmed to its opaque pixels, so the rectangle in the sheet is not
 * the sprite -- it is a piece of a square canvas whose bottom-centre is the
 * feet. Drawing the trimmed rect centred on the entity would put a crouching
 * frame at the same height as a standing one and shift anything asymmetric
 * sideways.
 *
 * So: scale the canvas to the sprite's world height, and offset the quad's
 * CENTRE from the feet by wherever the trim sat.
 *
 *   scale = worldHeight / canvas       world units per canvas pixel
 *   w, h  = the trim, scaled
 *   dx    = how far right of the feet the trim's centre is
 *   dy    = how far ABOVE the feet the trim's centre is
 *
 * `dy` is the number the old path calls yOffset. For an untrimmed, full-canvas
 * frame it comes out at exactly half the height, which is what a sprite hanging
 * off its feet has always used -- and is the cheapest check that this is right.
 */
export function quadFor (atlas, key, worldHeight = atlas.worldHeight) {
  const f = atlas.frames[key]
  if (!f) return null
  const n = atlas.canvas
  const scale = worldHeight / n
  const s = f.source
  return {
    w: s.w * scale,
    h: s.h * scale,
    dx: (s.x + s.w / 2 - n / 2) * scale,
    dy: (n - (s.y + s.h / 2)) * scale
  }
}

/**
 * The frame's corner UVs on the sheet, as THREE wants them for a plane.
 *
 * THE VERTICAL FLIP IS THE WHOLE DIFFICULTY. A rect in the manifest is measured
 * from the TOP of the image, because that is how images are addressed
 * everywhere. A texture's V runs from the BOTTOM, because that is how OpenGL
 * addresses them, and THREE's default `flipY` uploads the image so that holds.
 * So v is 1 minus the manifest's y, and the rect's top edge is the HIGHER v.
 *
 * Getting this backwards draws the sheet upside down, which is obvious, or --
 * far worse on a packed sheet -- draws a DIFFERENT frame, which is not.
 *
 * Returned in PlaneGeometry's vertex order: top-left, top-right, bottom-left,
 * bottom-right.
 */
export function uvFor (atlas, key) {
  const f = atlas.frames[key]
  if (!f) return null
  const { w: W, h: H } = atlas.sheet
  const r = f.rect
  const u0 = r.x / W
  const u1 = (r.x + r.w) / W
  const vTop = 1 - r.y / H
  const vBottom = 1 - (r.y + r.h) / H
  return [u0, vTop, u1, vTop, u0, vBottom, u1, vBottom]
}

/** Every distinct sheet a set of atlases needs, for preloading. */
export function sheetsOf (atlases) {
  return [...new Set(atlases.map((a) => a.image))]
}
