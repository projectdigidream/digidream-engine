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
import { parseVfx, tracerEnds, fade, VfxView } from '../src/render/vfx.js'

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
  }
}

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

// Due east: a quarter of the way round is north, so zero must be +x and the
// line must be exactly as long as the event said. A sign error here is the
// single most likely reason a tracer is invisible: it would point away from
// everything and out of the room.
{
  const e = { kind: 1, pos: at(10, 20, 1), dir: BRAD(0), param: 4 * 256 }
  const { from, to, length } = tracerEnds(e, fxToFloat)
  check(Math.abs(from.x - 10) < 1e-6 && Math.abs(from.y - 20) < 1e-6,
        `it starts where the shot started (${from.x.toFixed(2)}, ${from.y.toFixed(2)})`)
  check(Math.abs(to.x - 14) < 1e-6 && Math.abs(to.y - 20) < 1e-6,
        `zero brad points east (${to.x.toFixed(2)}, ${to.y.toFixed(2)})`)
  check(Math.abs(length - 4) < 1e-6, `and it is ${length} units long, as param said`)
  check(Math.abs(to.z - from.z) < 1e-6, 'flat: the engine casts a 2D ray, so the line is one too')
}

// A quarter turn. Checked as well as east, because a convention that is right
// at zero and mirrored everywhere else is exactly what shipped twice before in
// this renderer -- the sprite billboard and the swing collider.
{
  const e = { kind: 1, pos: at(0, 0, 0), dir: BRAD(0.25), param: 2 * 256 }
  const { to } = tracerEnds(e, fxToFloat)
  check(Math.abs(to.x) < 1e-6 && Math.abs(to.y - 2) < 1e-6,
        `a quarter turn points north (${to.x.toFixed(3)}, ${to.y.toFixed(3)})`)
}

// --- the lifetime ----------------------------------------------------------

check(fade(0, 0.1) === 1, 'a new tracer is fully visible')
check(fade(0.05, 0.1) > 0 && fade(0.05, 0.1) < 1, 'half way through it is half faded')
check(fade(0.1, 0.1) === 0, 'and at its life it is gone')
check(fade(0.2, 0.1) === 0, 'and stays gone')

// --- the whole path --------------------------------------------------------

{
  const scene = { added: [], add (o) { this.added.push(o) } }
  const view = new VfxView({ THREE, scene, catalogue: CAT, poolSize: 4 })
  const e = { kind: 1, pos: at(10, 20, 1), dir: BRAD(0), param: 4 * 256 }
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

  // The control: an event naming a kind the catalogue does not describe must
  // draw NOTHING rather than a default-sized line at the origin. Without it, a
  // typo in a map script would put a stray line through the middle of the level
  // and nothing would say why.
  view.spawn(9999, from, to, 200)
  check(view.live.length === 0, 'CONTROL: an unknown kind draws nothing')
}

console.log(`\nvfx: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
