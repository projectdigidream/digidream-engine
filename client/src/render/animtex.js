//
// Animated textures: nukage, water, lava, and the walls that drip.
//
// Doom cycles certain textures through a fixed sequence, and a map never names
// the frames -- it names the FIRST one and the engine animates from there. In
// Doom the sequences were compiled into the executable (P_InitPicAnims); here
// they are assets/animations.txt, for the same reason the prop catalogue is a
// text file: adding an animation should not mean rebuilding a binary.
//
// CLIENT ONLY, and the only catalogue the server does not read. Which picture
// is on a wall changes nothing the simulation can observe, so none of this is
// hashed, replicated or predicted. A client without the file draws the first
// frame of everything and is otherwise correct -- which is the right failure
// for a purely visual feature.
//
// The material samples through a `map` UNIFORM rather than material.map, so a
// frame change is a uniform write: no shader recompile, no material swap, and
// the cost of animating a hundred surfaces is a hundred pointer stores a frame.
//
import { parseCompositeLine } from './composite.js'

// SPRITE SHEETS: one picture holding every frame, stacked top to bottom
// (`vertical`) or side by side (`horizontal`) -- what qPainter exports.
//
//   sheet <NAME> vertical|horizontal <frames> [fps <n>]
//
// NAME is the texture maps use, one PNG. The client cuts it into its frames
// when it loads (render/textures.js sheetFrame), so each frame tiles across a
// wall as an ordinary texture does, and animates exactly as an `anim` group:
// the sheet becomes the group [NAME, NAME#1, ..., NAME#n-1].
export const SHEET_MAX_FRAMES = 256
export const SHEET_DEFAULT_FPS = 8

// COMPOSITE TEXTURES (render/composite.js, docs/COMPOSITE_TEXTURES.md): one name
// made of several pictures, laid out so the repeat stops showing.
//
//   composite <NAME> <TEX> [weight] <TEX> [weight] ... [flip [x|y]] [turn]
//             [shift | patches [size n]] [seam dither|blend]
//             [overlay <TEX> [scale n] [strength s]] [seed n]

/** Read the `anim`, `sheet` and `composite` records out of the catalogue text. */
export function parseAnimations (text) {
  const groups = []
  const sheets = new Map()
  const composites = new Map()
  const warnings = []
  text.split('\n').forEach((raw, i) => {
    const line = raw.split('#')[0].trim()
    if (!line) return
    const tok = line.split(/\s+/)
    if (tok[0] === 'composite') {
      const r = parseCompositeLine(tok)
      if (r.error) { warnings.push(`line ${i + 1}: ${r.error}`); return }
      // A later line for the same name wins, as for a sheet.
      composites.set(r.def.name, { ...r.def, line: i + 1 })
      return
    }
    if (tok[0] === 'sheet') {
      const name = (tok[1] ?? '').toUpperCase()
      const layout = tok[2]
      const frames = Number(tok[3])
      let fps = SHEET_DEFAULT_FPS
      if (tok[4] !== undefined) {
        if (tok[4] !== 'fps' || !(Number(tok[5]) > 0)) {
          warnings.push(`line ${i + 1}: after the frame count, only 'fps <n>' (got '${tok.slice(4).join(' ')}')`)
          return
        }
        fps = Number(tok[5])
      }
      if (!name) { warnings.push(`line ${i + 1}: a sheet needs a texture name`); return }
      if (layout !== 'vertical' && layout !== 'horizontal') {
        warnings.push(`line ${i + 1}: ${name}: a sheet is 'vertical' or 'horizontal', not '${layout}'`)
        return
      }
      if (!Number.isInteger(frames) || frames < 2 || frames > SHEET_MAX_FRAMES) {
        warnings.push(`line ${i + 1}: ${name}: a sheet has 2 to ${SHEET_MAX_FRAMES} frames, not '${tok[3]}'`)
        return
      }
      // A later line for the same sheet wins, as a later module's file would.
      if (sheets.has(name)) {
        const at = groups.findIndex((g) => g.frames[0] === name)
        if (at >= 0) groups.splice(at, 1)
      }
      sheets.set(name, { name, layout, frames, fps })
      groups.push({ fps, frames: [name, ...Array.from({ length: frames - 1 }, (_, k) => `${name}#${k + 1}`)], sheet: name })
      return
    }
    if (tok[0] !== 'anim') return
    const fps = Number(tok[1])
    const frames = tok.slice(2).map((n) => n.toUpperCase())
    if (!Number.isFinite(fps) || fps <= 0) {
      warnings.push(`line ${i + 1}: '${tok[1]}' is not a frame rate`)
      return
    }
    if (frames.length < 2) {
      warnings.push(`line ${i + 1}: ${frames[0] ?? '(nothing)'} has fewer than two frames`)
      return
    }
    groups.push({ fps, frames })
  })
  // Only once every line is read, because a sheet may be declared after the
  // composite that names it. A member must be one still picture: a sheet's
  // frames or another composite's tiles have no single picture to put in a tile.
  for (const c of composites.values()) {
    const keep = c.members.filter((m) => {
      const why = composites.has(m.name) ? 'a composite' : sheets.has(m.name) ? 'a sheet' : null
      if (why) warnings.push(`line ${c.line}: ${c.name}: ${m.name} is ${why}, which cannot be one of its textures; left out`)
      return !why
    })
    if (keep.length === 0) {
      warnings.push(`line ${c.line}: ${c.name}: none of its textures is a still picture; not a composite`)
      composites.delete(c.name)
      continue
    }
    c.members = keep
    if (c.overlay && (composites.has(c.overlay.name) || sheets.has(c.overlay.name))) {
      warnings.push(`line ${c.line}: ${c.name}: the overlay ${c.overlay.name} is not a still picture; no overlay`)
      c.overlay = null
    }
    if (sheets.has(c.name)) warnings.push(`line ${c.line}: ${c.name} is a sheet as well; the composite wins`)
  }
  return { groups, sheets, composites, warnings }
}

/**
 * Which frame of a sequence is showing at time `t`.
 *
 * Separate from everything that draws, so the one thing with an answer a test
 * can assert is asserted. Wraps rather than holding: an animated flat is a
 * loop, unlike a death or an explosion.
 */
export function animFrameAt (fps, count, seconds) {
  if (count <= 1 || !(fps > 0)) return 0
  const i = Math.floor(seconds * fps) % count
  return i < 0 ? i + count : i
}

/**
 * Swaps materials between the frames of their sequence.
 *
 * Built against the map's own texture list: a sequence whose first frame the
 * map never uses is not animated, because there is no material to animate. And
 * a frame whose IMAGE the client does not have is dropped from the sequence
 * rather than shown as a missing texture -- a partial extraction should cost
 * motion, not the wall.
 */
export class TextureAnimator {
  /**
   * `names` is the map's texture list, `materials` the array parallel to it,
   * and `frameTexture(name)` returns a loaded texture for a name or null.
   */
  constructor ({ groups, names, materials, frameTexture }) {
    this.tracks = []
    for (const g of groups) {
      const slot = names.indexOf(g.frames[0])
      if (slot < 0) continue                       // this map does not use it
      const material = materials[slot]
      if (!material?.uniforms?.map) continue
      // A composite's map is its atlas, not a frame to swap out.
      if (material.defines?.COMPOSITE !== undefined) continue

      const textures = []
      for (const f of g.frames) {
        const t = frameTexture(f)
        if (t) textures.push(t)
      }
      // One frame is not an animation. Skipping keeps the material exactly as
      // it was rather than pinning it to a single frame it might not be.
      if (textures.length < 2) continue
      this.tracks.push({ material, textures, fps: g.fps, shown: -1 })
    }
  }

  /** Advance every sequence to `seconds`. One clock for all of them. */
  update (seconds) {
    for (const t of this.tracks) {
      const i = animFrameAt(t.fps, t.textures.length, seconds)
      // Only on a CHANGE. A uniform write is cheap but not free, and at 144 Hz
      // against a 4.4 Hz animation thirty-two of every thirty-three writes
      // would be setting the value it already had.
      if (i === t.shown) continue
      t.shown = i
      t.material.uniforms.map.value = t.textures[i]
    }
  }

  get count () { return this.tracks.length }
}
