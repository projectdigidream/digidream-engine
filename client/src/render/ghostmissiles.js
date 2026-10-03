//
// OUR OWN MISSILES, flown by this client (docs/WEAPON_PREDICTION.md).
//
// A rocket is a real thing the server creates and flies, so it reached the
// screen a snapshot after the trigger -- already a unit and a half out of the
// barrel -- and its explosion a snapshot after the impact. Both read as lag.
// So when the predicted weapon launches one, this flies a GHOST of it the
// server's way: the same start (World::weapon_launch, the server's launch()),
// the same speed, and the same sweep through this client's own world
// (World::sweep_entities) each tick; at the impact it bursts and draws the
// impact's effects at once.
//
// And the server's copies are not drawn twice:
//   - the server's rocket, when it appears near a ghost of the same kind, is
//     CLAIMED and kept hidden -- unless, after the ghost has burst, it is still
//     flying somewhere else, which means the prediction was wrong (a monster
//     moved) and the truth is shown;
//   - the server's impact effects near a ghost's impact are swallowed.
//
// Everything here is in fx raw BigInts, as the core is; only drawing converts.
//
const FX_ONE = 1n << 32n
const mul = (a, b) => (a * b) >> 32n
const toF = (v) => Number(v) / 4294967296

export const CLAIM_DIST = 3          // units: a server rocket this near a ghost is its copy
export const BLAST_DIST = 2.5        // units: a server effect this near a ghost blast is a copy
export const BLAST_MS = 1500         // ...within this long
export const STRAY_TICKS = 24        // a claimed rocket still flying this long after the burst is shown
const FORGET_TICKS = 600

export class GhostMissiles {
  /**
   * @param world    a WasmWorld (sweep)
   * @param agent    our body's index in this world, which the sweep ignores
   * @param draw     { make(kind), place(mesh, {x,y,z} floats, aimTurns),
   *                   burst(mesh, state), remove(mesh), blast(vfx) }
   */
  constructor ({ world, agent, draw, now = () => performance.now() }) {
    this.world = world
    this.agent = agent
    this.draw = draw
    this.now = now
    this.flying = []        // { pos, dir, slope, aim, m, age, mesh, burstLeft, impact }
    this.blasts = []        // { x, y, z, kinds:Set, at }
    this.claims = new Map() // server entity -> { ghost, strayTicks }
    this.seen = new Set()   // server entities already judged
  }

  /** A predicted launch (WasmWorld.weaponFrame().launches). */
  launch (l) {
    const m = l.missile
    const g = { pos: { ...l.pos }, dir: l.dir, slope: l.slope, aim: l.aim, m, age: 0,
                mesh: this.draw.make(m.kind), burstLeft: -1, impact: null, start: { ...l.pos } }
    this.flying.push(g)
    this.#place(g)
  }

  /** One simulation tick: every ghost moves, as the server's flight does. */
  tick () {
    for (const g of this.flying) {
      if (g.burstLeft >= 0) { g.burstLeft--; continue }
      g.age++
      if (g.age > g.m.life) { g.burstLeft = 0; g.expired = true; continue }
      const step = g.m.speed / 60n
      // At its slope, as mobs.lua's flight sweeps: the floor, a ceiling and
      // a step stop it where its line meets them.
      const hit = this.world.sweep(g.pos, g.dir, step, g.m.radius, this.agent, g.slope)
      if (hit) {
        // dnp.sweep_hit: a tenth short of what it met, at the height it met it.
        const back = FX_ONE / 10n
        const d = hit.distance > back ? hit.distance - back : 0n
        g.pos = { x: g.pos.x + mul(g.dir.x, d), y: g.pos.y + mul(g.dir.y, d), z: hit.point.z }
        this.#burst(g)
      } else {
        g.pos = { x: g.pos.x + mul(g.dir.x, step), y: g.pos.y + mul(g.dir.y, step),
                  z: g.pos.z + mul(g.slope, step) }
      }
      this.#place(g)
    }
    // Gone once burst out (or expired): the mesh with them.
    this.flying = this.flying.filter((g) => {
      if (g.burstLeft === 0 || (g.expired && g.burstLeft <= 0)) { this.draw.remove(g.mesh); g.done = true; return false }
      return true
    })
    for (const [key, c] of this.claims) {
      if (c.ghost.done || c.ghost.burstLeft >= 0) c.strayTicks++
      if (c.strayTicks > FORGET_TICKS) this.claims.delete(key)
    }
    // Nothing of ours in the air and nothing claimed: what was judged no longer
    // matters, and the set does not grow for the length of a session.
    if (this.flying.length === 0 && this.claims.size === 0) this.seen.clear()
  }

  #burst (g) {
    g.impact = { ...g.pos }
    g.burstLeft = Math.max(1, g.m.burstTicks)
    if (g.m.burstAnim !== 0xFFFF) this.draw.burst(g.mesh, g.m.burstAnim)
    // The impact's effects, as mobs.lua's acts.vfx sends them: at the impact,
    // facing back along the flight, the radius in 256ths.
    const back = ((g.aim >>> 0) + 2147483648) % 4294967296
    for (const v of g.m.vfx) {
      this.draw.blast({ kind: v.kind, pos: g.impact, dir: back, param: Number((v.radius * 256n) >> 32n) })
    }
    this.blasts.push({ x: toF(g.impact.x), y: toF(g.impact.y), z: toF(g.impact.z),
                       kinds: new Set(g.m.vfx.map((v) => v.kind)), at: this.now() })
  }

  #place (g) {
    this.draw.place(g.mesh, { x: toF(g.pos.x), y: toF(g.pos.y), z: toF(g.pos.z) }, (g.aim >>> 0) / 4294967296)
  }

  /**
   * Is this server effect a copy of one a ghost already drew? Consumed if so,
   * so a second real blast in the same place is still shown.
   */
  swallow (v) {
    const t = this.now()
    this.blasts = this.blasts.filter((b) => t - b.at < BLAST_MS)
    const x = toF(v.pos.x)
    const y = toF(v.pos.y)
    const z = toF(v.pos.z)
    for (const b of this.blasts) {
      if (!b.kinds.has(v.kind)) continue
      if (Math.hypot(x - b.x, y - b.y, z - b.z) > BLAST_DIST) continue
      b.kinds.delete(v.kind)
      return true
    }
    return false
  }

  /**
   * Should the server's entity `key` (catalogue `kind`, at world `pos` --
   * floats, {x, y} in the map's plane, as the renderer has it) be hidden?
   *
   * `key` is the entity, not its slot: index AND generation. The server hands
   * every rocket the slot the last one freed, so by index alone the second
   * rocket was "already judged" and still tied to the first one's ghost. A new one near a live ghost of its kind is that ghost's copy.
   * A claimed one shows again if it outlives its ghost's burst by STRAY_TICKS
   * while far from where the ghost burst: the prediction was wrong.
   */
  hides (key, kind, pos) {
    let c = this.claims.get(key)
    if (!c && !this.seen.has(key)) {
      this.seen.add(key)
      const { x, y } = pos
      for (const g of this.flying) {
        if (g.m.kind !== kind || g.claimed) continue
        const near = (p) => Math.hypot(toF(p.x) - x, toF(p.y) - y) < CLAIM_DIST
        if (near(g.pos) || near(g.start)) {
          g.claimed = true
          c = { ghost: g, strayTicks: 0 }
          this.claims.set(key, c)
          break
        }
      }
    }
    if (!c) return false
    const g = c.ghost
    if (c.strayTicks > STRAY_TICKS && g.impact) {
      const far = Math.hypot(pos.x - toF(g.impact.x), pos.y - toF(g.impact.y)) > BLAST_DIST
      if (far) return false
    }
    return true
  }

  /** The server's entity is gone: forget it. */
  forget (key) { this.claims.delete(key); this.seen.delete(key) }

  /** A new level: nothing of ours flies there. */
  clear () {
    for (const g of this.flying) this.draw.remove(g.mesh)
    this.flying = []
    this.blasts = []
    this.claims.clear()
    this.seen.clear()
  }
}
