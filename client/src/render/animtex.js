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

/** Read the `anim` records out of the catalogue text. */
export function parseAnimations (text) {
  const groups = []
  const warnings = []
  text.split('\n').forEach((raw, i) => {
    const line = raw.split('#')[0].trim()
    if (!line) return
    const tok = line.split(/\s+/)
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
  return { groups, warnings }
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
