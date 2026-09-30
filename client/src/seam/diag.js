//
// WHAT A CROSSING DID, SAID WHERE SOMEBODY CAN READ IT.
//
// Every line goes to the console. With ?diag it also goes to the dev server's
// /dev-log, which prints it in the terminal running tools/devserver.js: that is
// how client/test/seamcam.test.js reads a headless browser, and how a person
// testing on a phone reads one without a debugger attached.
//
// Nothing here changes what the client does. It measures three things that
// have each been reported as "it jams" or "it twitches" and look identical on
// screen: the view slipping against the body at a crossing, a body with no
// sector, and a body that is pressed and goes nowhere.
//

const DIAG = typeof location !== 'undefined' &&
             new URLSearchParams(location.search).has('diag')

/** One line, to the console and (with ?diag) to the dev server. */
export function report (line, level = 'log') {
  console[level](line)
  if (DIAG) fetch(`/dev-log?${encodeURIComponent(line)}`).catch(() => {})
}

/**
 * DID THE VIEW MOVE WITH THE BODY? A seam is a change of frame and nothing
 * else, so across one the camera and the body move by the same amount and
 * what is on screen does not change. mark() on the crossing; frame() every
 * frame, and on the first frame after a mark it reports both distances.
 */
export class CrossingProbe {
  #armed = false
  #prevEye = null
  #prevBody = null
  #lastDe = 0
  #correction = 0
  #lastDb = 0

  /** @param {number} correction how far the arrival moved the body itself */
  mark (correction = 0) { this.#armed = true; this.#correction = correction }

  frame (eye, body) {
    if (this.#prevEye) {
      const de = Math.hypot(eye.x - this.#prevEye.x, eye.y - this.#prevEye.y, eye.z - this.#prevEye.z)
      const db = Math.hypot(body.x - this.#prevBody.x, body.y - this.#prevBody.y, body.z - this.#prevBody.z)
      if (this.#armed) {
        this.#armed = false
        // The frame before, too: walking, the camera trails the body by a
        // little every frame, and that is not the crossing's doing.
        report(`[seam] crossing: body moved ${db.toFixed(3)}, camera moved ${de.toFixed(3)}` +
               `, slip ${Math.abs(de - db).toFixed(3)}` +
               ` (the frame before: ${Math.abs(this.#lastDe - this.#lastDb).toFixed(3)}` +
               `, correction ${this.#correction.toFixed(3)})`)
      }
      this.#lastDe = de
      this.#lastDb = db
    }
    this.#prevEye = { x: eye.x, y: eye.y, z: eye.z }
    this.#prevBody = { x: body.x, y: body.y, z: body.z }
  }
}

/**
 * IS THE LOCAL BODY STUCK, and which kind of stuck.
 *
 *   no sector      -- the controller does not simulate it; nothing responds.
 *   pressed, still -- on a floor, keys down, going nowhere. The server's last
 *                     applied input tick tells the two causes apart: if it
 *                     stops advancing the inputs are being thrown away there.
 *
 * A jammed body JITTERS rather than standing still -- prediction pushes it and
 * each snapshot pulls it back -- so "not moving" is displacement over a window,
 * not position equality between frames.
 */
export class BodyWatch {
  #noSector = 0
  #anchor = null
  #pressed = 0
  #jammed = 0
  appliedTick = 0
  margin = 0

  /** From each snapshot. */
  server (s) {
    if (s.lastInputTick !== undefined) this.appliedTick = s.lastInputTick
    if (s.inputMargin !== undefined) this.margin = s.inputMargin
  }

  /**
   * @param {{x:number, y:number, sector:number}} at   the body, engine units
   * @param {boolean} pressing                         a direction is held
   * @param {number} clientTick
   */
  frame (at, pressing, clientTick) {
    if (at.sector < 0) {
      if (++this.#noSector === 12) {
        report(`[seam] STUCK: no sector for 12 frames at ${at.x.toFixed(2)},${at.y.toFixed(2)}` +
               ' -- the controls will not respond', 'warn')
      }
      return
    }
    if (this.#noSector >= 12) report(`[seam] unstuck after ${this.#noSector} frames`)
    this.#noSector = 0

    if (!pressing) { this.#anchor = null; this.#pressed = 0; this.#jammed = 0; return }
    if (!this.#anchor) { this.#anchor = { x: at.x, y: at.y }; this.#pressed = 0; return }
    ++this.#pressed
    const gone = Math.hypot(at.x - this.#anchor.x, at.y - this.#anchor.y)
    const where = `applying ${this.appliedTick}, client at ${clientTick}` +
                  ` (behind by ${clientTick - this.appliedTick}), margin ${this.margin}`
    // A walk covers several units a second; under one unit in a second and a
    // half of holding a direction is stuck.
    if (this.#pressed >= 90 && gone < 1) {
      report(`[seam] NOT MOVING: ${this.#pressed} frames pressing, moved ${gone.toFixed(2)}` +
             ` on sector ${at.sector} -- ${where}`, 'warn')
      this.#jammed += this.#pressed
      this.#anchor = { x: at.x, y: at.y }
      this.#pressed = 0
    } else if (gone >= 1) {
      if (this.#jammed) report(`[seam] moving again after ${this.#jammed + this.#pressed} frames -- ${where}`)
      this.#jammed = 0
      this.#anchor = { x: at.x, y: at.y }
      this.#pressed = 0
    }
  }
}
