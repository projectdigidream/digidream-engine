//
// The tracer, from a wire event to a mesh on screen.
//
// A hitscan shot is drawn as a line for a tenth of a second and then forgotten.
// It has been reported three times as "still no visible raycasts", and each
// time the server was demonstrably sending the events -- so the failure was
// somewhere in the client, in code no test ran, because it lived in main.js
// behind a DOM, a WebGL context and a live session.
//
// So the calculation is out of main.js now and this exercises it directly:
// catalogue -> event -> two endpoints -> a visible mesh -> a fade -> gone. Each
// step has a way of failing that leaves the screen empty and the logs quiet.
//
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { softDisc } from '../src/render/vfx.js'
import { parseVfx, tracerEnds, fade, VfxView, emitBurst, stepParticles,
         rampColour, particleShape, PARTICLE_SHAPES, defaultImageUrls } from '../src/render/vfx.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// Enough of three.js to build a tracer. What is under test is the geometry and
// the lifetime, neither of which needs a GPU.
const THREE = {
  BoxGeometry: class { translate () { return this } },
  MeshBasicMaterial: class {
    constructor (o) { Object.assign(this, o); this.color = { setHex: (h) => { this.hex = h } } }
  },
  Mesh: class {
    constructor (geo, mat) {
      this.geometry = geo
      this.material = mat
      this.visible = false
      this.position = { x: 0, y: 0, z: 0, set (x, y, z) { this.x = x; this.y = y; this.z = z } }
      this.scale = { x: 0, y: 0, z: 0, set (x, y, z) { this.x = x; this.y = y; this.z = z } }
      this.quaternion = { setFromUnitVectors (a, b) { this.a = a; this.b = b } }
    }
  },
  Vector3: class {
    constructor (x = 0, y = 0, z = 0) { this.x = x; this.y = y; this.z = z }
    set (x, y, z) { this.x = x; this.y = y; this.z = z; return this }
  },
  // Enough of the point pipeline for a burst: a geometry with named attributes
  // and a draw range, a material, and a Points to hang them on.
  BufferGeometry: class {
    constructor () { this.attributes = {}; this.drawRange = { start: 0, count: 0 } }
    setAttribute (n, a) { this.attributes[n] = a }
    setDrawRange (s, c) { this.drawRange = { start: s, count: c } }
    dispose () { this.disposed = true }
  },
  Float32BufferAttribute: class {
    constructor (array, itemSize) { this.array = array; this.itemSize = itemSize }
  },
  PointsMaterial: class {
    constructor (o) { Object.assign(this, o) }
    dispose () { this.disposed = true }
  },
  Points: class {
    constructor (geo, mat) { this.geometry = geo; this.material = mat }
  },
  AdditiveBlending: 2,
  NormalBlending: 1
}

const fakeTHREE = () => THREE

const CAT = parseVfx(readFileSync(join(ROOT, 'assets', 'props.txt'), 'utf8'))
console.log('vfx: a hitscan shot becomes a line, and then stops being one')
check(CAT.kinds.size > 0, `the catalogue describes ${CAT.kinds.size} effect kinds`)
check(CAT.warnings.length === 0, `and parses clean (${CAT.warnings.length} warnings)`)

// The catalogue is what makes a tracer configurable without a recompile, so the
// three things a caller can set have to actually arrive.
const one = CAT.kinds.get(1)
check(one !== undefined && one.colour > 0 && one.width > 0 && one.life > 0,
      `kind 1 carries a colour, a width and a life ` +
      `(#${one?.colour?.toString(16)}, ${one?.width}, ${one?.life}s)`)

// --- the geometry ----------------------------------------------------------

const FX = 4294967296n
const fxToFloat = (v) => Number(v) / 4294967296
const at = (x, y, z) => ({ x: BigInt(Math.round(x * 4294967296)),
                           y: BigInt(Math.round(y * 4294967296)),
                           z: BigInt(Math.round(z * 4294967296)) })
const BRAD = (turns) => Math.round(turns * 4294967296) >>> 0
// A tracer's `param`, packed exactly as mobs.lua's tracer_param packs it: the
// horizontal length in the low sixteen bits, the rise over it in the high
// sixteen offset by 32768. Written out here rather than imported because the
// two ends of a wire agreeing by construction proves nothing about the wire.
const PARAM = (len, rise = 0) =>
  (((32768 + Math.round(rise * 256)) * 65536) + Math.round(len * 256)) >>> 0

// Due east: a quarter of the way round is north, so zero must be +x and the
// line must be exactly as long as the event said. A sign error here is the
// single most likely reason a tracer is invisible: it would point away from
// everything and out of the room.
{
  const e = { kind: 1, pos: at(10, 20, 1), dir: BRAD(0), param: PARAM(4) }
  const { from, to, length } = tracerEnds(e, fxToFloat)
  check(Math.abs(from.x - 10) < 1e-6 && Math.abs(from.y - 20) < 1e-6,
        `it starts where the shot started (${from.x.toFixed(2)}, ${from.y.toFixed(2)})`)
  check(Math.abs(to.x - 14) < 1e-6 && Math.abs(to.y - 20) < 1e-6,
        `zero brad points east (${to.x.toFixed(2)}, ${to.y.toFixed(2)})`)
  check(Math.abs(length - 4) < 1e-6, `and it is ${length} units long, as param said`)
  check(Math.abs(to.z - from.z) < 1e-6,
        'a shot with no rise is flat, which is every shot at something level with you')
}

// SLOPED, which is what an auto-aimed shot at a monster on a balcony is.
//
// There is no vertical aim input in this engine, so the slope is chosen for the
// player -- and a tracer drawn from the heading alone would be flat while the
// damage landed three units higher. The rise is signed, so both directions are
// checked: an offset encoding that dropped its sign would pass upwards and
// draw every downward shot into the ceiling.
{
  const up = tracerEnds({ kind: 1, pos: at(0, 0, 1), dir: BRAD(0), param: PARAM(4, 3) },
                        fxToFloat)
  check(Math.abs(up.to.z - 4) < 1e-6, `it climbs by the rise (${up.to.z.toFixed(3)})`)
  check(Math.abs(up.to.x - 4) < 1e-6,
        'and `length` stays the HORIZONTAL run, so the low half is untouched by the high one')
  const down = tracerEnds({ kind: 1, pos: at(0, 0, 10), dir: BRAD(0), param: PARAM(4, -3) },
                          fxToFloat)
  check(Math.abs(down.to.z - 7) < 1e-6, `and falls by a negative one (${down.to.z.toFixed(3)})`)
}

// A quarter turn. Checked as well as east, because a convention that is right
// at zero and mirrored everywhere else is exactly what shipped twice before in
// this renderer -- the sprite billboard and the swing collider.
{
  const e = { kind: 1, pos: at(0, 0, 0), dir: BRAD(0.25), param: PARAM(2) }
  const { to } = tracerEnds(e, fxToFloat)
  check(Math.abs(to.x) < 1e-6 && Math.abs(to.y - 2) < 1e-6,
        `a quarter turn points north (${to.x.toFixed(3)}, ${to.y.toFixed(3)})`)
}

// --- the lifetime ----------------------------------------------------------

check(fade(0, 0.1) === 1, 'a new tracer is fully visible')
// The one that mattered. A tracer spawned LATER in the frame than the timestamp
// update() is given has a negative age, and treating that as expired destroyed
// every tracer on the frame it was created -- which presents as nothing ever
// appearing, with the pool permanently full and no error anywhere.
check(fade(-0.01, 0.1) === 1, 'and one not yet born is visible, not expired')
check(fade(0.05, 0.1) > 0 && fade(0.05, 0.1) < 1, 'half way through it is half faded')
check(fade(0.1, 0.1) === 0, 'and at its life it is gone')
check(fade(0.2, 0.1) === 0, 'and stays gone')

// --- the whole path --------------------------------------------------------

{
  const scene = { added: [], add (o) { this.added.push(o) } }
  const view = new VfxView({ THREE, scene, catalogue: CAT, poolSize: 4 })
  const e = { kind: 1, pos: at(10, 20, 1), dir: BRAD(0), param: PARAM(4) }
  const { from, to } = tracerEnds(e, fxToFloat)

  view.spawn(1, from, to, 100)
  const live = view.live.map((l) => l.mesh)
  check(live.length === 1, `spawning one shot makes one mesh live (${live.length})`)
  check(live[0]?.visible === true, 'and it is visible')
  check(Math.abs((live[0]?.scale.x ?? 0) - 4) < 1e-6,
        `stretched to the shot's length (${live[0]?.scale.x})`)
  check(live[0]?.scale.y === one.width && live[0]?.scale.z === one.width,
        `and to the catalogue's width (${live[0]?.scale.y})`)
  check(live[0]?.material.hex === one.colour, "and painted the catalogue's colour")

  // It fades, and then it goes back in the pool. A tracer that never expires
  // fills the screen; one returned to the pool while still drawn flickers.
  view.update(100 + one.life / 2)
  check(view.live.length === 1 && live[0].material.opacity > 0 &&
        live[0].material.opacity < 1,
        `mid-life it is partly faded (${live[0].material.opacity.toFixed(2)})`)
  view.update(100 + one.life + 0.001)
  check(view.live.length === 0, 'past its life it is retired')
  check(live[0].visible === false, 'and hidden')

  // Spawned at 100, faded against 99.99 -- the frame-clock skew, exactly as it
  // reaches the renderer. It must survive: this is the whole bug, and the
  // original test passed because it only ever faded against a LATER time.
  {
    const scene2 = { added: [], add (o) { this.added.push(o) } }
    const v2 = new VfxView({ THREE, scene: scene2, catalogue: CAT, poolSize: 4 })
    v2.spawn(1, from, to, 100)
    v2.update(99.99)
    check(v2.live.length === 1,
          `a tracer faded against an earlier stamp survives (${v2.live.length} live)`)
    check(v2.pool.length === 3, `and is not returned to the pool (${v2.pool.length})`)
  }

  // The control: an event naming a kind the catalogue does not describe must
  // draw NOTHING rather than a default-sized line at the origin. Without it, a
  // typo in a map script would put a stray line through the middle of the level
  // and nothing would say why.
  view.spawn(9999, from, to, 200)
  check(view.live.length === 0, 'CONTROL: an unknown kind draws nothing')
}

console.log('\n--- particles come out of a cone and fall down it ---')
{
  const def = { count: 400, life: 1, lifeVar: 0, speed: 10, speedVar: 0,
                cone: 0.125, gravity: 20, drag: 0, size: 0.1, sizeEnd: 0,
                colour: 0xff0000, colourEnd: 0x000000 }

  // A quarter of the cone, about +X in three space (the event's heading of 0).
  const b = emitBurst(def, { x: 0, y: 0, z: 0 }, 0, 0)
  check(b.length === 400, `the burst has its declared count (${b.length})`)

  // `cone` is in TURNS, and 0.125 turns is a 45-degree half-angle: every
  // direction must be within that of the axis. Asserted as a dot product rather
  // than per-component, because the axis is a direction and not an axis-aligned
  // one in general.
  let worst = 1
  for (const p of b) {
    const l = Math.hypot(p.vx, p.vy, p.vz)
    worst = Math.min(worst, p.vx / l)     // dot with +X
  }
  const limit = Math.cos(0.125 * 2 * Math.PI)
  check(worst >= limit - 1e-9,
        `every particle is inside the cone (worst ${worst.toFixed(4)} >= ${limit.toFixed(4)})`)

  // CONTROL: a wider cone really does go wider, or `cone` is decorative and the
  // check above passes for a burst that only ever goes straight ahead.
  const wide = emitBurst({ ...def, cone: 0.5 }, { x: 0, y: 0, z: 0 }, 0, 0)
  const behind = wide.filter((p) => p.vx < 0).length
  check(behind > 0, `CONTROL: a full sphere throws ${behind} of 400 backwards`)

  // The heading is obeyed: pointing a quarter turn round puts the cone on -Z,
  // because engine (x, y) maps to three (x, -y).
  const turned = emitBurst(def, { x: 0, y: 0, z: 0 }, 0.25, 0)
  const axis = turned.every((p) => p.vz < 0)
  check(axis, 'a quarter-turn heading throws them along -Z in three space')

  // Speed and lifetime spread: with the variance at zero they are exact, which
  // is what makes the varied case meaningful.
  const speeds = b.map((p) => Math.hypot(p.vx, p.vy, p.vz))
  check(Math.max(...speeds) - Math.min(...speeds) < 1e-9, 'no variance means one speed')
  const varied = emitBurst({ ...def, speedVar: 4 }, { x: 0, y: 0, z: 0 }, 0, 0)
  const vs = varied.map((p) => Math.hypot(p.vx, p.vy, p.vz))
  check(Math.max(...vs) - Math.min(...vs) > 1,
        `CONTROL: speed_var spreads them (${(Math.max(...vs) - Math.min(...vs)).toFixed(2)})`)

  // A radius fills a VOLUME rather than a shell, which is what stops a blast
  // looking like a smoke ring.
  const filled = emitBurst(def, { x: 0, y: 0, z: 0 }, 0, 2)
  const rs = filled.map((p) => Math.hypot(p.x, p.y, p.z))
  check(Math.max(...rs) <= 2 + 1e-9, 'nothing starts outside the emission radius')
  check(Math.min(...rs) < 1, `and they fill it rather than ring it (min ${Math.min(...rs).toFixed(2)})`)
  // CONTROL: a zero radius is a point source.
  const point = emitBurst(def, { x: 5, y: 6, z: 7 }, 0, 0)
  check(point.every((p) => p.x === 5 && p.y === 6 && p.z === 7),
        'CONTROL: no radius means they all start at the event')
}

console.log('\n--- and then they are integrated ---')
{
  const def = { gravity: 10, drag: 0 }
  const parts = [{ x: 0, y: 0, z: 0, vx: 1, vy: 0, vz: 0, age: 0, life: 1 }]
  stepParticles(parts, 0.5, def)
  check(Math.abs(parts[0].x - 0.5) < 1e-9, 'velocity moves it')
  check(parts[0].vy < 0 && Math.abs(parts[0].vy + 5) < 1e-9, 'gravity pulls it DOWN, which is -Y in three space')

  // Expiry, and the reason the draw range exists: the buffer is sized once.
  const dying = [{ x: 0, y: 0, z: 0, vx: 0, vy: 0, vz: 0, age: 0, life: 0.2 },
                 { x: 0, y: 0, z: 0, vx: 0, vy: 0, vz: 0, age: 0, life: 2 }]
  stepParticles(dying, 0.5, def)
  check(dying.length === 1, 'a particle past its life is dropped')
  check(dying[0].life === 2, 'and it is the short one that went')

  // Drag is exponential, so a huge dt cannot push a speed through zero and send
  // a spark backwards -- which `v -= v * drag * dt` does the moment drag*dt > 1.
  const dragged = [{ x: 0, y: 0, z: 0, vx: 10, vy: 0, vz: 0, age: 0, life: 99 }]
  stepParticles(dragged, 5, { gravity: 0, drag: 4 })
  check(dragged[0].vx > 0, `CONTROL: a huge step never reverses it (${dragged[0].vx.toExponential(1)})`)
  check(dragged[0].vx < 10, 'and it really did slow down')

  // A zero or negative step changes nothing, which is what a paused tab gives.
  const still = [{ x: 3, y: 0, z: 0, vx: 1, vy: 0, vz: 0, age: 0, life: 1 }]
  stepParticles(still, 0, def)
  check(still[0].x === 3 && still[0].age === 0, 'a zero step is a no-op')
}

console.log('\n--- the colour ramp is the fade ---')
{
  const c0 = rampColour(0xff0000, 0x000000, 0)
  const c1 = rampColour(0xff0000, 0x000000, 1)
  const ch = rampColour(0xff0000, 0x000000, 0.5)
  check(c0.r === 1 && c0.g === 0 && c0.b === 0, 'it starts at the first colour')
  check(c1.r === 0, 'and ends at the second -- black, which additive blending draws as nothing')
  check(Math.abs(ch.r - 0.5) < 0.01, `and passes through the middle (${ch.r.toFixed(3)})`)
  // Out of range is clamped, because `age/life` can overshoot by a frame.
  check(rampColour(0xff0000, 0x000000, 2).r === 0, 'past the end is clamped')
  check(rampColour(0xff0000, 0x00ff00, -1).g === 0, 'and so is before the start')
}

console.log('\n--- a particle kind draws through the same spawn as everything else ---')
{
  const { kinds } = parseVfx('vfx 9 particles count 8 life 0.3 gravity 0 drag 0')
  const scene = {
    children: [],
    add (o) { this.children.push(o) },
    remove (o) { const i = this.children.indexOf(o); if (i >= 0) this.children.splice(i, 1) }
  }
  const view = new VfxView({ THREE: fakeTHREE(), scene, catalogue: { kinds }, poolSize: 2 })
  view.spawn(9, { x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 }, 0)
  check(view.burstCount === 1, 'the burst is live')
  // NOT a tracer: it must not have taken one of the tracer meshes, or a
  // firefight of sparks starves the thing that draws the shots.
  check(view.live.length === 0, 'CONTROL: and it did not consume a tracer from the pool')
  // Stepped at a real frame rate, not in one leap: `update` clamps dt to 0.1 so
  // a tab regaining focus cannot teleport a burst through its whole life, which
  // means a 0.3s life takes several frames whatever the wall clock says.
  for (let f = 1; f <= 40; f++) view.update(f / 60)
  check(view.burstCount === 0, 'and it is gone once every particle has expired')
  check(scene.children.length === 2,
        `CONTROL: its Points was removed from the scene (${scene.children.length} left, the 2 tracers)`)
}

console.log('\n--- an attached emitter follows the body it came from ---')
{
  const { kinds } = parseVfx(
    'vfx 20 particles attach 1 rate 60 linger 0.2 life 5 speed 0 speed_var 0 gravity 0 drag 0')
  const scene = {
    children: [],
    add (o) { this.children.push(o) },
    remove (o) { const i = this.children.indexOf(o); if (i >= 0) this.children.splice(i, 1) }
  }
  const view = new VfxView({ THREE: fakeTHREE(), scene, catalogue: { kinds }, poolSize: 2 })

  // The body walks along +X. `locate` is what the caller supplies; here it is a
  // moving point, which is the whole case this exists for.
  let bodyX = 0
  const locate = (index) => (index === 7 ? { x: bodyX, y: 0, z: 0 } : null)

  check(view.kindOf(20).attach === 1, 'the kind says it attaches')
  view.attach(20, 7, 0, 0)
  check(view.emitterCount === 1, 'and an emitter starts')
  // NOT a burst, and not a tracer: it must not have taken either pool.
  check(view.burstCount === 0 && view.live.length === 0,
        'CONTROL: it is neither a burst nor a tracer')

  // Walk it a long way while it emits. The particles have no speed and no
  // gravity, so wherever they are is where they were BORN -- which makes the
  // spread of their positions exactly the spread of the emitter's.
  for (let f = 1; f <= 30; f++) {
    bodyX += 0.5
    view.attach(20, 7, 0, f / 60)        // the weapon keeps re-triggering
    view.update(f / 60, locate)
  }
  const em = [...view.emitters.values()][0]
  const xs = em.parts.map((p) => p.x)
  check(xs.length > 0, `it emitted (${xs.length} particles)`)
  check(Math.max(...xs) - Math.min(...xs) > 5,
        `and they were born ALONG the walk, not at one spot (${(Math.max(...xs) - Math.min(...xs)).toFixed(1)} apart)`)
  // THE CLAIM: the newest particles are where the body is NOW. A burst spawned
  // once at the event's position would leave every particle behind at x=0.
  check(Math.abs(Math.max(...xs) - bodyX) < 0.6,
        `the newest are on the body (${Math.max(...xs).toFixed(2)} vs ${bodyX.toFixed(2)})`)

  // LINGER: it keeps going briefly after the last event, then stops emitting.
  const before = em.parts.length
  for (let f = 31; f <= 60; f++) view.update(f / 60, locate)   // no more attach()
  check(em.parts.length > before, 'CONTROL: it kept emitting through the linger')
  const after = em.parts.length
  for (let f = 61; f <= 120; f++) view.update(f / 60, locate)
  check(em.parts.length === after, 'and stopped once the linger ran out')

  // A body that is gone stops it, and the emitter is cleaned up once its last
  // particle has expired.
  // `life_var` is spelled out rather than defaulted: the default is 0.25, which
  // against a life of 0.1 gives particles that expire almost at once -- so an
  // emitter nobody re-triggered was legitimately dead by frame 10, and the
  // assertion below failed five runs in six for a reason that was not the one
  // it names.
  const short = parseVfx('vfx 21 particles attach 1 rate 60 linger 0.05 life 0.1 life_var 0').kinds
  const v2 = new VfxView({ THREE: fakeTHREE(), scene, catalogue: { kinds: short }, poolSize: 1 })
  // Re-triggered every frame, as a held trigger does. An emitter is kept alive
  // by its events, not by existing.
  for (let f = 1; f <= 10; f++) {
    v2.attach(21, 3, 0, f / 60)
    v2.update(f / 60, () => ({ x: 0, y: 0, z: 0 }))
  }
  check(v2.emitterCount === 1, 'an emitter with a body is alive')
  for (let f = 11; f <= 90; f++) v2.update(f / 60, () => null)   // the body died
  check(v2.emitterCount === 0, 'and one whose body is gone is cleaned up')

  // Re-triggering the same kind on the same body REFRESHES rather than stacking
  // -- a chainsaw held for ten seconds must not leave fifty emitters behind.
  const v3 = new VfxView({ THREE: fakeTHREE(), scene, catalogue: { kinds }, poolSize: 1 })
  for (let f = 0; f < 50; f++) v3.attach(20, 7, 0, f / 60)
  check(v3.emitterCount === 1, 'fifty triggers on one body are one emitter')
  v3.attach(20, 8, 0, 1)
  check(v3.emitterCount === 2, 'CONTROL: a different body gets its own')
}

// --- the soft sprite, and a blast thrown out of the wall --------------------------
{
  const n = 32
  const px = softDisc(n)
  const alphaAt = (x, y) => px[(y * n + x) * 4 + 3]
  check(alphaAt(16, 16) > 240 && alphaAt(0, 0) === 0 && alphaAt(0, 16) < 20,
        `a particle is a soft disc: opaque in the middle, nothing at the corners (${alphaAt(16, 16)}, ${alphaAt(0, 0)})`)
  let mono = true
  for (let x = 16; x < n - 1; x++) if (alphaAt(x + 1, 16) > alphaAt(x, 16)) mono = false
  check(mono, 'and it only fades outward')
  // The rocket's blast, with the cone the catalogue gives it, thrown back the
  // way the rocket came (mobs.lua reverses an impact's heading): heading west
  // into a wall at x = 0 means a burst heading east, and nothing goes behind.
  const cat = parseVfx(readFileSync(join(HERE, '..', '..', 'assets', 'props.txt'), 'utf8'))
  const blast = cat.kinds.get(11)
  const parts = emitBurst(blast, { x: 0, y: 1, z: 0 }, 0, 2)   // heading 0 turns: +x
  const behind = parts.filter((p) => p.x < -1e-9 || p.vx < -1e-9).length
  check(parts.length === blast.count && behind === 0,
        `the rocket's blast starts and flies on the open side only (${behind} of ${parts.length} behind)`)
  const sphere = emitBurst({ ...blast, cone: 0.5 }, { x: 0, y: 1, z: 0 }, 0, 2)
  check(sphere.filter((p) => p.vx < 0).length > sphere.length / 4,
        'CONTROL: with the old full-sphere cone, a good part went into the wall')
}

// --- what a particle is drawn as: a shape, or a picture of the module's -----------
console.log('vfx: a particle is drawn as a shape or a picture, tinted by its ramp')
{
  // The words, read back -- for the studio's editor to round-trip.
  const { kinds, warnings } = parseVfx([
    'vfx 30 particles shape star',
    'vfx 31 particles sprite fx/spark',
    'vfx 32 particles shape hexagon sprite ember',
    'vfx 33 particles',
    'vfx 34 particles shape blob',
    'vfx 35 tracer shape disc',
    'vfx 36 particles sprite ../../etc/x'
  ].join('\n'))
  check(kinds.get(30).shape === 'star' && kinds.get(30).sprite === null, 'shape star is read back')
  check(kinds.get(31).sprite === 'fx/spark' && kinds.get(31).shape === 'soft', 'a sprite is read back, its shape the default')
  check(kinds.get(32).shape === 'hexagon' && kinds.get(32).sprite === 'ember', 'both, on one line')
  check(kinds.get(33).shape === 'soft' && kinds.get(33).sprite === null, 'a line that says nothing: soft, no sprite')
  check(warnings.length === 3 && kinds.get(34).shape === 'soft' && kinds.get(35).shape === undefined &&
        kinds.get(36).sprite === null,
        `an unknown shape, a tracer's shape and a path out of the folder are refused (${warnings.join(' | ')})`)

  // THE SHAPES. `soft` is softDisc to the byte: nothing that says nothing changes.
  const n = 32
  const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i])
  check(same(particleShape('soft', n), softDisc(n)), 'soft is the soft disc, byte for byte')
  const alpha = (shape) => { const px = particleShape(shape, n); return (x, y) => px[(y * n + x) * 4 + 3] }
  // At a point given -1..1, y up (rows run bottom to top).
  const at = (f, x, y) => f(Math.min(n - 1, Math.floor((x + 1) / 2 * n)), Math.min(n - 1, Math.floor((y + 1) / 2 * n)))
  const disc = alpha('disc'); const square = alpha('square'); const hex = alpha('hexagon'); const star = alpha('star')
  check(at(disc, 0, 0) === 255 && at(disc, 0.95, 0.95) === 0, 'a disc: opaque in the middle, clear in the corner')
  // HARD, which is the point: no fall-off inside, unlike the soft dot.
  check(at(disc, 0.6, 0) === 255 && alpha('soft')(Math.floor(0.8 * n), n / 2) < 200,
        'and hard: full at 0.6 out, where the soft dot has faded')
  check(at(square, 0.7, 0.7) === 255 && at(square, 0.9, 0) === 0, 'a square: its corner in, past its side out')
  check(at(hex, 0.85, 0) === 255 && at(hex, 0, 0.85) === 0, 'a flat-topped hexagon: a corner to the side, a flat top')
  check(at(star, 0, 0.6) === 255 && at(star, 0, -0.85) === 0 && at(star, 0, 0) === 255,
        'a star: a point straight up, a notch straight down')
  // Anti-aliased rims: some pixel on each shape is neither in nor out.
  const partial = (shape) => { const px = particleShape(shape, n); for (let k = 3; k < px.length; k += 4) if (px[k] > 0 && px[k] < 255) return true; return false }
  check(['disc', 'square', 'hexagon', 'star'].every(partial), 'every hard shape has an anti-aliased rim')
  // White, so the ramp is the colour.
  const white = (shape) => { const px = particleShape(shape, n); for (let k = 0; k < px.length; k += 4) if (px[k] !== 255 || px[k + 1] !== 255 || px[k + 2] !== 255) return false; return true }
  check(PARTICLE_SHAPES.every(white), 'every shape is white')
  // CONTROL: they are five different pictures.
  const pics = PARTICLE_SHAPES.map((sh) => particleShape(sh, n))
  let distinct = true
  for (let i = 0; i < pics.length; i++) for (let j = i + 1; j < pics.length; j++) if (same(pics[i], pics[j])) distinct = false
  check(distinct, 'CONTROL: the five shapes are five pictures')

  // THE VIEW: a THREE with textures, and a loader whose files arrive when told.
  const T = {
    ...THREE,
    DataTexture: class { constructor (data, w, h) { this.image = { data, width: w, height: h } } },
    RGBAFormat: 1023, LinearFilter: 1006, NearestFilter: 1003, NoColorSpace: ''
  }
  const files = new Set(['/textures/ember.png', 'blob:studio/ember'])
  const asked = []
  const pending = []
  const loader = {
    load (url, onLoad, _p, onError) {
      asked.push(url)
      pending.push(() => (files.has(url) ? onLoad({ url }) : onError(new Error('404'))))
    }
  }
  const flush = () => { while (pending.length) pending.shift()() }
  const warned = console.warn
  let warns = 0
  console.warn = () => { warns++ }
  const scene = { add () {}, remove () {} }
  const view = new VfxView({ THREE: T, scene, catalogue: { kinds }, poolSize: 1, loader })
  const from = { x: 0, y: 1, z: 0 }
  const burst = (kind) => { view.spawn(kind, from, from, 0); return view.parts[view.parts.length - 1].points.material }
  // A kind that says nothing wears the soft dot -- the very texture it always did.
  check(burst(33).map === view.disc, 'a line that says nothing draws through the soft dot, as before')
  check(burst(30).map.image.data[3] === particleShape('star', 32)[3] &&
        same(burst(30).map.image.data, particleShape('star', 32)), 'shape star draws through the star')
  // The sprites the catalogue names were asked for at once, /sprites/ first.
  check(asked[0] === '/sprites/fx/spark.png' || asked[0] === '/sprites/ember.png',
        `the pictures are asked for when the view is made, /sprites/ first (${asked.join(', ')})`)
  // Before it has arrived: its shape. Hexagon for 32.
  const early = burst(32)
  check(same(early.map.image.data, particleShape('hexagon', 32)), 'before its picture arrives, a burst draws its shape')
  flush()   // /sprites/ember.png is not there; /textures/ember.png is asked for
  flush()
  check(asked.includes('/textures/ember.png'), 'not a sprite, so a texture of that name, as the HUD looks')
  check(early.map.url === '/textures/ember.png' && early.needsUpdate === true,
        'and the burst already in the air wears it from then')
  check(burst(32).map.url === '/textures/ember.png', 'the sprite wins over the shape once it is there')
  // A picture that is nowhere: its shape, said once.
  check(same(burst(31).map.image.data, particleShape('soft', 32)) && warns === 1,
        `a picture that is nowhere draws its shape, and is said once (${warns})`)
  console.warn = warned

  // THE STUDIO's pictures are blob URLs: resolveImage says where.
  const v2 = new VfxView({ THREE: T, scene, catalogue: { kinds }, poolSize: 1, loader,
                           resolveImage: (name) => (name === 'ember' ? 'blob:studio/ember' : null) })
  flush()
  v2.spawn(32, from, from, 0)
  check(v2.parts[0].points.material.map.url === 'blob:studio/ember', 'resolveImage hands the view its picture')
  check(defaultImageUrls('fx/spark').join() === '/sprites/fx/spark.png,/textures/fx/spark.png' &&
        defaultImageUrls('a.webp')[0] === '/sprites/a.webp', 'the default lookup, and a name with its extension')

  // A TRACER is unchanged by any of it.
  const tv = new VfxView({ THREE: T, scene, catalogue: { kinds }, poolSize: 1, loader })
  tv.spawn(35, { x: 0, y: 0, z: 0 }, { x: 2, y: 0, z: 0 }, 0)
  check(tv.live.length === 1 && tv.parts.length === 0, 'a tracer that named a shape is still a line')
}

console.log(`\nvfx: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
