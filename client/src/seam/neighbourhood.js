//
// THE LEVELS THIS CLIENT HOLDS BESIDES THE ONE IT IS STANDING IN.
//
// One map, keyed by level index, and one rule (docs/protocol.md §8.5): a level
// is held from the first byte of its map until the server drops it. The server
// keeps the same set by the same rule (server/src/world/neighbours.hpp), so
// there is nothing here to reconcile and nothing to guess -- no matching by
// hash, no level "set aside" waiting to be named, no second table for cells
// still being built. Those were three answers to the question "which level is
// this?", and each of them was wrong at least once.
//
// Everything that touches the scene or a worker is an EFFECT handed in, so the
// bookkeeping is tested in node (client/test/neighbourhood.test.js) against the
// sequences that broke it.
//
// Offsets are the level's origin minus the home level's, in engine units (the
// float for drawing, the raw fixed-point for placing a body exactly).
//

/**
 * @typedef {object} Effects
 * @property {(level: number, token: number, bytes: Uint8Array) => void} build
 *   start building a view for these bytes; answer later with built()
 * @property {(built: object) => object} make    a view from a finished build
 * @property {(view: object, dx: number, dy: number) => void} show
 *   put a view in the scene at this offset (again, if it is there already)
 * @property {(view: object) => void} hide       take a view out of the scene
 * @property {(view: object) => void} dispose    and let it go for good
 * @property {(line: string) => void} [warn]     a protocol violation, said out loud
 */

export class Neighbourhood {
  /** @param {Effects} fx */
  constructor (fx) {
    this.fx = fx
    /** @type {Map<number, {bytes: Uint8Array, view: object|null, token: number,
     *   placed: boolean, dx: number, dy: number, dxRaw: bigint, dyRaw: bigint}>} */
    this.held = new Map()
    this.home = -1
    this.nextToken = 1
    // Counted as well as said, so a test can ask for none.
    this.violations = 0
  }

  #warn (line) {
    this.violations++
    this.fx.warn?.(`[seam] ${line}`)
  }

  /** NEIGHBOUR_MAP and its stream, complete. */
  map (level, dx, dy, dxRaw, dyRaw, bytes) {
    if (level === this.home) { this.#warn(`map for the home level ${level}; ignored`); return }
    const had = this.held.get(level)
    if (had) {
      // By the rule the server never sends a map for a level it believes is
      // held. Replaced rather than refused: the bytes just sent are the newer.
      this.#warn(`map for level ${level}, which is already held; replacing it`)
      if (had.view) this.fx.dispose(had.view)
    }
    const token = this.nextToken++
    this.held.set(level, { bytes, view: null, token, placed: true, dx, dy, dxRaw, dyRaw })
    this.fx.build(level, token, bytes)
  }

  /** NEIGHBOUR_MOVE: held already, somewhere else. */
  move (level, dx, dy, dxRaw, dyRaw) {
    const n = this.held.get(level)
    if (!n) { this.#warn(`move for level ${level}, which is not held`); return }
    Object.assign(n, { dx, dy, dxRaw, dyRaw, placed: true })
    if (n.view) this.fx.show(n.view, dx, dy)
  }

  /** NEIGHBOUR_DROP. */
  drop (level) {
    const n = this.held.get(level)
    if (!n) { this.#warn(`drop for level ${level}, which is not held`); return }
    if (n.view) this.fx.dispose(n.view)
    this.held.delete(level)
  }

  /**
   * A build has finished. Kept only if it is the build of what is held NOW:
   * the level may have been dropped, re-sent, or walked into since.
   */
  built (level, token, built) {
    const n = this.held.get(level)
    if (!n || n.token !== token) return false
    n.view = this.fx.make(built)
    if (n.placed) this.fx.show(n.view, n.dx, n.dy)
    return true
  }

  /**
   * THE CLIENT IS NOW IN `level`. The level it was in becomes held, with the
   * bytes and view it was using; the level entered stops being held and what
   * the client had of it is handed back.
   *
   * Across a seam the level entered was held and its offset is known, so
   * every other cell -- the one just left included -- is re-stated relative to
   * it here, in the same breath, and nothing on screen moves. The server's
   * moves arrive a moment later saying the same thing.
   *
   * Otherwise (an exit, or the join) no offset is known, so every held cell is
   * hidden until the server places or drops it.
   *
   * @param {number} level
   * @param {{bytes: Uint8Array|null, view: object|null}} leaving  the old home
   * @returns {null | {bytes: Uint8Array, view: object|null, placed: boolean,
   *                   dx: number, dy: number, dxRaw: bigint, dyRaw: bigint}}
   *   the level entered, if it was held (for a respawn, the home itself at 0)
   */
  enter (level, leaving) {
    const from = this.home
    this.home = level
    // A respawn in the same level: nothing moved, and the home is what it was.
    if (from === level) {
      if (!leaving?.bytes) return null
      return { bytes: leaving.bytes, view: leaving.view ?? null, placed: true,
               dx: 0, dy: 0, dxRaw: 0n, dyRaw: 0n }
    }
    const entered = this.held.get(level) ?? null
    this.held.delete(level)
    // Held but not placed is a level whose bytes are here and whose offset is
    // not: two jumps in a row between levels that are not next to each other.
    // Usable as a map, not as a seam.
    const known = entered?.placed === true
    for (const n of this.held.values()) {
      if (known) {
        n.dx -= entered.dx; n.dy -= entered.dy
        n.dxRaw -= entered.dxRaw; n.dyRaw -= entered.dyRaw
        if (n.view && n.placed) this.fx.show(n.view, n.dx, n.dy)
      } else {
        n.placed = false
        if (n.view) this.fx.hide(n.view)
      }
    }
    if (from >= 0 && leaving?.bytes) {
      const n = { bytes: leaving.bytes, view: leaving.view ?? null, token: 0, placed: known,
                  dx: 0, dy: 0, dxRaw: 0n, dyRaw: 0n }
      if (known) {
        // The level left sits at minus the offset the level entered had.
        n.dx = -entered.dx; n.dy = -entered.dy
        n.dxRaw = -entered.dxRaw; n.dyRaw = -entered.dyRaw
        if (n.view) this.fx.show(n.view, n.dx, n.dy)
      } else if (n.view) this.fx.hide(n.view)
      this.held.set(from, n)
    } else if (leaving?.view) this.fx.dispose(leaving.view)
    if (!entered) return null
    return { bytes: entered.bytes, view: entered.view, placed: known,
             dx: entered.dx, dy: entered.dy, dxRaw: entered.dxRaw, dyRaw: entered.dyRaw }
  }

  /** Whether a level's bytes are in hand. */
  has (level) { return this.held.has(level) }

  /**
   * The grid pitch, as far as this client can tell: every placed neighbour is
   * one or more whole cells away, so the smallest non-zero offset IS the pitch.
   * Infinity when nothing is placed, which is a world with no seams.
   */
  pitch () {
    let p = Infinity
    for (const n of this.held.values()) {
      if (!n.placed) continue
      if (n.dx) p = Math.min(p, Math.abs(n.dx))
      if (n.dy) p = Math.min(p, Math.abs(n.dy))
    }
    return p
  }

  /** One line: what is held, where, and whether it is drawn. */
  describe () {
    return [...this.held].map(([li, n]) =>
      `${li}@${n.placed ? `${n.dx},${n.dy}` : '?'}${n.view ? '' : '(building)'}`).join(' ') || 'nothing'
  }
}
