//
// A TYPED SUBTITLE: the line shown a few letters at a time (graphs' subtitle
// node, mode `typed`; docs/GRAPHS.md "Subtitles").
//
// `speed` letters a second, `pack` letters at each step, the first step at
// once. Each step that shows a letter (anything but white space) is one to
// sound; a step of spaces is silent, so a pause between words is a pause.
// Pure arithmetic over a clock, so a frame dropped or a slow phone shows the
// line where it should be by now rather than falling behind.
//
export class Typewriter {
  constructor ({ text, speed, pack = 1, startedAt }) {
    this.text = String(text ?? '')
    this.chars = [...this.text]          // by code point, so an emoji is one letter
    this.speed = Math.max(1, Number(speed) || 1)
    this.pack = Math.max(1, Math.floor(Number(pack) || 1))
    this.startedAt = startedAt
    this.steps = Math.max(1, Math.ceil(this.chars.length / this.pack))
    this.sounded = 0                     // steps whose sound has been played
  }

  /** How many steps have shown by `now` (seconds), 1..steps. */
  stepAt (now) {
    const elapsed = Math.max(0, now - this.startedAt)
    // A hair of tolerance: a clock read as 100.6 - 100 is 0.5999999..., and a
    // step due at exactly that moment must not wait a frame for it.
    return Math.min(this.steps, Math.floor(elapsed * this.speed / this.pack + 1e-9) + 1)
  }

  /** The part of the line shown at `now`. */
  shownAt (now) {
    return this.chars.slice(0, this.stepAt(now) * this.pack).join('')
  }

  /** Whether the whole line is out. */
  doneAt (now) { return this.stepAt(now) >= this.steps }

  /**
   * The steps that came out since the last call and should SOUND (they show a
   * letter), oldest first; each is reported once. A frame that skipped several
   * steps gets them all, and the caller decides how many to play.
   */
  takeSounds (now) {
    const upTo = this.stepAt(now)
    const out = []
    for (let s = this.sounded; s < upTo; s++) {
      const piece = this.chars.slice(s * this.pack, (s + 1) * this.pack).join('')
      if (/\S/.test(piece)) out.push(s)
    }
    this.sounded = upTo
    return out
  }
}
