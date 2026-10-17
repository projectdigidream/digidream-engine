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
// The core's fx multiply: truncated toward zero, not floored -- a flight
// heading west would otherwise drift from the server's by an ulp a tick.
const mul = (a, b) => { const p = a * b; return p < 0n ? -((-p) >> 32n) : p >> 32n }
const toF = (v) => Number(v) / 4294967296
// WHERE A PROP CAN BE: the server's missile is a prop, and a prop's position is
// stored at the wire's precision (fx_to_fxq, 2^-12 of a unit, truncated toward
// zero) by spawn_prop and by every set_pos. Its flight steps from there each
// tick, so the ghost has to stand where the prop can, or the burst -- and the
// shove it gives -- is a hair from the server's.
const quant = (v) => (v < 0n ? -((-v) >> 20n) : v >> 20n) << 20n
const quant3 = (p) => ({ x: quant(p.x), y: quant(p.y), z: quant(p.z) })
// The floor of a square root, of a non-negative BigInt: the core's isqrt128,
// which has one right answer, so any exact method agrees with it.
const isqrt = (n) => {
  if (n < 2n) return n
  let x = BigInt(Math.floor(Math.sqrt(Number(n))))
  while (x * x > n) x--
  while ((x + 1n) * (x + 1n) <= n) x++
  return x
}
// fx_sqrt: sqrt(raw * 2^32), for x >= 0.
const fxSqrt = (v) => (v <= 0n ? 0n : isqrt(v << 32n))
// The core's fx divide: (a << 32) / b, truncated toward zero, as BigInt's is.
const div = (a, b) => (b === 0n ? 0n : (a << 32n) / b)

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
   * @param selfBlast (impact, blast) when a ghost whose blast shoves its owner
   *                 bursts: the owner's shove, predicted (a rocket jump)
   * @param onBlast  (impact, blast) when any ghost with a blast bursts: its
   *                 shove of the bodies around it, predicted (render/knock.js)
   */
  constructor ({ world, agent, draw, selfBlast = null, onBlast = null, now = () => performance.now() }) {
    this.world = world
    this.selfBlast = selfBlast
    this.onBlast = onBlast
    this.agent = agent
    this.draw = draw
    this.now = now
    this.flying = []        // { pos, dir, slope, aim, m, age, mesh, burstLeft, impact }
    this.blasts = []        // { x, y, z, kinds:Set, at }
    this.claims = new Map() // server entity -> { ghost, strayTicks }
    this.seen = new Set()   // server entities already judged
    this.ids = 0            // each ghost's own number, for what follows it (its trail)
  }

  /** A predicted launch (WasmWorld.weaponFrame().launches). */
  launch (l) {
    const m = l.missile
    const g = { id: ++this.ids, pos: quant3(l.pos), dir: l.dir, slope: l.slope, aim: l.aim, m, age: 0,
                mesh: this.draw.make(m.kind), burstLeft: -1, impact: null, start: { ...l.pos } }
    // A FALLING MISSILE starts along the aim, its vertical speed the slope
    // times its speed, as mobs.lua's throw_vz does for a player's launch.
    // THROWN at a launch speed (`throw`), the speed is along the aim, and
    // the horizontal is what the slope leaves of it -- throw_at_speed's
    // speed / sqrt(1 + slope^2), in the same integer steps.
    if (m.gravity) {
      const slope = l.slope ?? 0n
      if (m.throws) {
        g.speed = div(m.speed, fxSqrt(FX_ONE + mul(slope, slope)))
        g.vz = mul(g.speed, slope)
      } else {
        g.vz = mul(slope, m.speed)
      }
    }
    this.flying.push(g)
    this.#place(g)
  }

  /**
   * One simulation tick: every ghost moves, as the server's flight does.
   *
   * BEFORE the tick's step, as the server's on_tick runs before its world
   * steps: a missile then meets the world as it was after the last step, and
   * the shove of a burst lands in the step that follows -- on both sides.
   */
  tick () {
    for (const g of this.flying) {
      if (g.burstLeft >= 0) { g.burstLeft--; continue }
      g.age++
      if (g.age > g.m.life) { g.burstLeft = 0; g.expired = true; continue }
      const step = (g.speed ?? g.m.speed) / 60n
      // At its slope, as mobs.lua's flight sweeps: the floor, a ceiling and
      // a step stop it where its line meets them. A falling one along this
      // tick's line, gravity taken off first -- the same integer divisions as
      // the server's fx, which truncate as BigInt's do.
      let climb = g.slope
      let dz = null
      if (g.vz !== undefined) {
        g.vz -= g.m.gravity / 60n
        dz = g.vz / 60n
        climb = step > 0n ? (dz << 32n) / step : 0n
      }
      const hit = this.world.sweep(g.pos, g.dir, step, g.m.radius, this.agent, climb)
      if (hit) {
        // dnp.sweep_hit's impact: a tenth short of what it met, at the height
        // it met it -- the world computes it, on the server's bits.
        if (hit.impact) {
          g.pos = { ...hit.impact }
        } else {
          const back = FX_ONE / 10n
          const d = hit.distance > back ? hit.distance - back : 0n
          g.pos = { x: g.pos.x + mul(g.dir.x, d), y: g.pos.y + mul(g.dir.y, d), z: hit.point.z }
        }
        this.#burst(g)
      } else {
        g.pos = quant3({ x: g.pos.x + mul(g.dir.x, step), y: g.pos.y + mul(g.dir.y, step),
                         z: g.pos.z + (dz ?? mul(g.slope, step)) })
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
    // Its owner thrown, before anything is drawn: the rocket jump.
    if (g.m.blast?.shovesOwner && this.selfBlast) this.selfBlast(g.impact, g.m.blast)
    if (g.m.blast && this.onBlast) this.onBlast(g.impact, g.m.blast)
    // The impact's effects, as mobs.lua's acts.vfx sends them: at the impact,
    // facing back along the flight, the radius in 256ths.
    const back = ((g.aim >>> 0) + 2147483648) % 4294967296
    for (const v of g.m.vfx) {
      this.draw.blast({ kind: v.kind, pos: g.impact, dir: back, param: Number((v.radius * 256n) >> 32n) })
    }
    this.blasts.push({ x: toF(g.impact.x), y: toF(g.impact.y), z: toF(g.impact.z),
                       kinds: new Set(g.m.vfx.map((v) => v.kind)), at: this.now() })
  }

  /**
   * Where ghost `id` is, in world units ({x, y, z} numbers), while it flies;
   * null once it has burst, or for no such ghost. What its trail comes out of.
   */
  positionOf (id) {
    for (const g of this.flying) {
      if (g.id === id) return g.burstLeft >= 0 || g.done ? null : { x: toF(g.pos.x), y: toF(g.pos.y), z: toF(g.pos.z) }
    }
    return null
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
