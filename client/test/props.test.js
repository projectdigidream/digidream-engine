//
// The prop catalogue, as the renderer reads it.
//
// The claim this file exists to protect: adding a prop is editing a text file,
// and NOTHING about which kinds exist is compiled into the client. So the tests
// are about the parser being tolerant in the right places and strict in the
// right places, and about the animation clock being the client's own.
//
import { parseProps, frameAt, rotationFor, frameName, propScale, PropRegistry } from '../src/render/props.js'
import { parseAtlas } from '../src/render/atlas.js'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')

const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// WHERE THE SHIPPED CATALOGUE LIVES NOW.
//
// The engine's own assets/props.txt is deliberately almost empty: the keys, the
// barrel, the lever and every monster are DOOM II art, and they moved to the
// module that owns that art (docs/MODULES.md). So the file with entries in it
// is the module's, and that is the one a parser is worth testing against.
//
// The engine's own file is still parsed below -- it must stay well-formed, and
// an empty catalogue is exactly the case a parser is least likely to have been
// tried on.
const ENGINE_CATALOGUE = join(ROOT, 'assets', 'props.txt')
const CATALOGUE = join(ROOT, 'modules', 'doom2', 'assets', 'props.txt')

console.log('--- the shipped catalogue ---')
{
  // The real file, not a fixture: a parser that only works on the examples in
  // its own test is a parser nobody can trust with the file that matters.
  const text = readFileSync(CATALOGUE, 'utf8')
  const { kinds, warnings } = parseProps(text)
  check(warnings.length === 0, `parses clean (${warnings.length} warnings)`)
  check(kinds.size >= 6, `${kinds.size} kinds`)

  // The engine's own catalogue: no props at all, and it must still parse
  // without complaint. A catalogue with nothing in it is what a new project
  // starts from, so "the parser copes with empty" is not a corner case here --
  // it is the first thing anybody will hit.
  const eng = parseProps(readFileSync(ENGINE_CATALOGUE, 'utf8'))
  check(eng.warnings.length === 0,
        `the engine's own catalogue parses clean (${eng.warnings.length} warnings)`)
  check(eng.kinds.size === 0, `and carries no Doom art (${eng.kinds.size} kinds)`)

  const blue = kinds.get(1)
  check(blue?.type === 'sprite', 'kind 1 is a sprite')
  check(blue?.billboard === true, 'and billboards')
  check(blue?.states.get(0)?.frames.length === 2, 'with a two-frame idle state')
}

console.log('\n--- the server half is not the renderer half ---')
{
  // Two readers, one file. This one must take the `sprite` lines and ignore the
  // `prop` lines entirely -- if it tried to understand them, a change to a
  // radius would need the assets rebuilt.
  const { kinds, warnings } = parseProps(`
prop 9 widget radius 0.5 height 1.0
sprite 9 size 2 frames A,B,C fps 12
`)
  check(kinds.size === 1, 'a prop line contributes no visual on its own')
  check(warnings.length === 0, 'and is skipped in silence, not warned about')
  check(kinds.get(9).size === 2, 'the sprite line carries the size')
}

console.log('\n--- tolerant where it should be, loud where it should be ---')
{
  const { kinds, warnings } = parseProps(`
sprite 1 size 1 frames A
whatever 2 something
sprite 3 size 1 wobble 4 frames B
sprite  size 1 frames C
sprite 5 size 1
`)
  // An unknown RECORD is somebody else's business, silently.
  check(kinds.has(1), 'a good line is kept')
  check(!kinds.has(2), 'an unknown record type is skipped')
  // An unknown ATTRIBUTE on a record we own is a typo in our half, so it is
  // reported -- and the rest of the line still works, because refusing it
  // outright would lose the frames over a stray word.
  check(kinds.has(3) && kinds.get(3).states.get(0).frames[0] === 'B',
        'an unknown attribute is reported but the line still parses')
  check(warnings.some((w) => w.includes('wobble')), 'and names the attribute')
  check(warnings.some((w) => w.includes('not a prop id')), 'a missing id is named')
  check(warnings.some((w) => w.includes('no frames')), 'a sprite with no frames is named')
  check(!kinds.get(5)?.states.size, 'and contributes no state')
}

console.log('\n--- animation is the client\'s own clock ---')
{
  // The server declares a STATE; how fast it plays and which frame is showing
  // is decided here (design §4). A pure function of seconds, so it is the same
  // for every entity in that state -- which is what makes a row of keys bob
  // together instead of shimmering.
  const three = { frames: ['A', 'B', 'C'], fps: 10 }
  check(frameAt(three, 0) === 0, 'starts on the first frame')
  check(frameAt(three, 0.15) === 1, 'advances with time')
  check(frameAt(three, 0.25) === 2, 'and again')
  check(frameAt(three, 0.35) === 0, 'and wraps')
  // A whole number of cycles lands back at the start, so an animation cannot
  // drift out of phase over a long session.
  check(frameAt(three, 300) === frameAt(three, 0), 'no drift after five minutes')

  // fps 0 is a held frame, expressed as a rate rather than as a special case in
  // the caller: a still image is an animation that does not advance.
  check(frameAt({ frames: ['A', 'B'], fps: 0 }, 99) === 0, 'fps 0 holds one frame')
  check(frameAt({ frames: ['A'], fps: 30 }, 99) === 0, 'a one-frame state is still')

  // CONTROL: a state with several frames and a real rate must NOT be still.
  // Without this, every assertion above is satisfied by a function returning 0.
  const moved = new Set([0, 0.05, 0.1, 0.15, 0.2].map((t) => frameAt(three, t)))
  check(moved.size > 1, `a real animation changes frame (${moved.size} distinct)`)
}

console.log('\n--- a declared state actually plays ---')
{
  // The bug this replaces: the entity view recorded the server's animation
  // state and the registry played animations, and nothing connected them. A
  // barrel switched to its explosion state and went on showing idle frames
  // until it vanished, with both halves working correctly in isolation.
  //
  // A stub THREE, because none of this needs a GPU: the registry only ever
  // touches geometry, a basic material and a mesh.
  const made = []
  const THREE = {
    PlaneGeometry: class { constructor (w, h) { this.w = w; this.h = h } },
    MeshBasicMaterial: class {
      constructor (o) { Object.assign(this, o); this.color = { setHex () {} } }
    },
    Mesh: class {
      constructor (geometry, material) {
        this.geometry = geometry
        this.material = material
        this.userData = {}
        this.parent = {}          // pretend it is in the scene
        made.push(this)
      }
    },
    DoubleSide: 2
  }

  const catalogue = parseProps(`
prop 10 barrel radius 0.2 height 0.9 solid
sprite 10 size 0.9 billboard state 0 fps 0 frames IDLE
sprite 10 state 1 fps 10 frames BOOM1,BOOM2,BOOM3
`)
  const textures = new Map([['IDLE', 'tex:idle'], ['BOOM1', 'tex:b1'],
                            ['BOOM2', 'tex:b2'], ['BOOM3', 'tex:b3']])
  const reg = new PropRegistry({ THREE, textures, catalogue })

  const mesh = reg.make(10)
  check(mesh !== null, 'the registry built a sprite for the kind')

  reg.update(0)
  check(mesh.material.map === 'tex:idle', 'and shows its idle frame')

  // What the server declaring state 1 has to do.
  reg.setState(mesh, 1)
  reg.update(10)
  check(mesh.material.map === 'tex:b1', 'switching state starts the new one from its FIRST frame')
  reg.update(10.15)
  check(mesh.material.map === 'tex:b2', 'and it advances')

  // CONTROL: without the call, the state never changes. This is the whole bug
  // -- the registry was correct and nothing told it anything.
  const other = reg.make(10)
  reg.update(20)
  check(other.material.map === 'tex:idle', 'CONTROL: an untold sprite stays on its idle state')

  // Restarting matters: an explosion that begins a third of the way through is
  // not an explosion. A shared clock would have put it wherever it happened to
  // be.
  const third = reg.make(10)
  reg.setState(third, 1)
  reg.update(99.7)
  check(third.material.map === 'tex:b1', 'a state entered late still starts at its first frame')
}


// --- eight viewing angles -------------------------------------------------
//
// Doom stores a monster as eight sprites and picks by the angle between where
// it faces and where you stand. Both previous angle conventions in this
// renderer shipped mirrored -- the billboard and the swing collider -- so this
// is checked at the cardinal points rather than reasoned about.

{
  const cat = parseProps(readFileSync(CATALOGUE, 'utf8'))
  const poss = cat.kinds.get(20)
  check(poss?.rot8 === true, 'the zombieman declares eight rotations')
  check(poss?.states.get(1)?.frames?.[0] === 'POSSA',
        `and its frames drop the rotation digit (${poss?.states.get(1)?.frames?.[0]})`)
  check(frameName(poss, poss.states.get(1), 0, 5) === 'POSSA5',
        'which the renderer puts back')
  // A prop with no rot8 is untouched, or every key and barrel would ask for a
  // file that does not exist.
  const key = cat.kinds.get(1)
  check(frameName(key, key.states.get(0), 0, 3) === key.states.get(0).frames[0],
        'a sprite without rot8 keeps its frame name exactly')
}

// Rotation 1 is the FRONT: the thing facing the camera. Facing east with the
// camera to its east means it is looking straight at you.
check(rotationFor(0, 0) === 1, 'facing the camera shows the front view (1)')
// Directly away: half a turn round, which is view 5.
check(rotationFor(0, 0.5) === 5, 'facing away shows the back view (5)')
// And the two sides are distinct and opposite each other.
const left = rotationFor(0, 0.25)
const right = rotationFor(0, 0.75)
check(left !== right && left !== 1 && right !== 1,
      `the two profiles differ and are not the front (${left} vs ${right})`)
check(((left - 1) + (right - 1)) % 8 === 0,
      `and are mirror images about the front (${left}, ${right})`)

// Every view is reachable, and the wrap is clean: sweeping a full turn must
// visit all eight and come back to where it started. An off-by-one in the
// rounding shows up here as a view that never appears.
{
  const seen = new Set()
  for (let i = 0; i < 64; i++) seen.add(rotationFor(0, i / 64))
  check(seen.size === 8, `a full sweep visits all eight views (${seen.size})`)
  check(rotationFor(0, 0) === rotationFor(0, 1),
        'and a whole turn is the same view it started on')
  check(rotationFor(0.3, 0.3) === 1, 'the angle is RELATIVE, not either one alone')
}


// --- play once, and hold -----------------------------------------------------
//
// A death or an explosion is an EVENT, not a loop. Without this the only
// options are to cycle it forever or to remove the entity the moment it ends --
// and a corpse is neither. The first version of this edit did not apply at all
// and nothing noticed, because a looping death animation looks like an
// animation.
{
  const cat = parseProps(readFileSync(CATALOGUE, 'utf8'))
  const die = cat.kinds.get(20).states.get(4)
  const walk = cat.kinds.get(20).states.get(1)
  check(die.once === true, 'the death state is declared play-once')
  check(!walk.once, 'and the walk cycle is not')

  const last = die.frames.length - 1
  check(frameAt(die, 0) === 0, 'a death starts at its first frame')
  check(frameAt(die, last / die.fps) === last, 'reaches its last')
  check(frameAt(die, 100) === last, 'and STAYS there rather than wrapping to the start')
  // The control: the walk cycle at the same times does wrap, so the assertion
  // above is `once` working rather than frameAt having stopped advancing.
  check(frameAt(walk, 100) !== undefined && frameAt(walk, 0) === 0, 'a loop still loops')
  const seen = new Set()
  for (let t = 0; t < 4; t += 0.05) seen.add(frameAt(walk, t))
  check(seen.size === walk.frames.length,
        `and visits every one of its frames (${seen.size}/${walk.frames.length})`)
}

// --- the registry forgets what has left the scene ---------------------------
//
// `prune` was written with the registry and then never called from anywhere, so
// `animated` grew for the whole session: every barrel that had ever exploded
// still in the map, still being stepped over sixty times a second to be
// skipped, and its mesh and material never collectable. The frame loop calls
// this once a second now.
{
  const THREE = {
    PlaneGeometry: class { constructor (w, h) { this.w = w; this.h = h } },
    MeshBasicMaterial: class {
      constructor (o) { Object.assign(this, o); this.color = { setHex () {} } }
    },
    Mesh: class {
      constructor (geometry, material) {
        this.geometry = geometry
        this.material = material
        this.userData = {}
        this.parent = {}          // pretend it is in the scene
      }
    },
    DoubleSide: 2
  }
  const catalogue = parseProps(`
prop 10 barrel radius 0.2 height 0.9 solid
sprite 10 size 0.9 billboard state 0 fps 0 frames IDLE
`)
  const textures = new Map([['IDLE', 'tex:idle']])

  const spawnAndKill = (reg, n) => {
    const live = []
    for (let i = 0; i < n; i++) {
      const mesh = reg.make(10)
      // What EntityView.#remove leaves behind: out of the scene, parent null.
      if (i % 2 === 0) mesh.parent = null
      else live.push(mesh)
    }
    return live
  }

  const reg = new PropRegistry({ THREE, textures, catalogue })
  const live = spawnAndKill(reg, 500)
  const before = reg.animated.size
  reg.prune()
  check(before === 500 && reg.animated.size === live.length,
        `500 spawned, 250 destroyed: ${before} tracked, ${reg.animated.size} after prune`)

  // CONTROL: the leak itself. Without the call the registry keeps all 500 --
  // which is what it did in every session until the frame loop started calling
  // it. Without this control the assertion above passes for a registry that
  // never tracked anything in the first place.
  const leak = new PropRegistry({ THREE, textures, catalogue })
  spawnAndKill(leak, 500)
  check(leak.animated.size === 500,
        `CONTROL: never pruned, the registry still holds all ${leak.animated.size}`)
}

// --- `scale` makes the picture bigger and the body exactly the same ----------
//
// One number per kind, multiplying whatever its art would otherwise be drawn
// at. Both drawing paths have to honour it -- the per-PNG plane, whose size is
// baked into the geometry when the mesh is made, and the atlas, whose quad is
// rewritten on every frame from the sheet's declared height.
//
// The control is the body. `radius` and `height` are replicated and predicted;
// a scale that moved them would draw one size and collide at another, which is
// a misprediction on every touch and presents as a netcode fault rather than
// as an art setting.
console.log('\n--- a kind is drawn at its own scale ---')
{
  const THREE = {
    PlaneGeometry: class { constructor (w, h) { this.w = w; this.h = h } },
    MeshBasicMaterial: class {
      constructor (o) { Object.assign(this, o); this.color = { setHex () {} } }
    },
    Mesh: class {
      constructor (geometry, material) {
        this.geometry = geometry; this.material = material
        this.userData = {}; this.parent = {}
      }
    },
    DoubleSide: 2
  }
  const catalogue = parseProps(`
prop 30 runt   radius 0.2 height 0.9 scale 0.5 solid
sprite 30 size 1 billboard state 0 fps 0 frames IDLE
prop 31 giant  radius 0.2 height 0.9 scale 3 solid
sprite 31 size 1 billboard state 0 fps 0 frames IDLE
prop 32 plain  radius 0.2 height 0.9 solid
sprite 32 size 1 billboard state 0 fps 0 frames IDLE
`)
  const textures = new Map([['IDLE', 'tex:idle']])
  const reg = new PropRegistry({ THREE, textures, catalogue })

  const runt = reg.make(30)
  const giant = reg.make(31)
  const plain = reg.make(32)
  check(runt.geometry.w === 0.5, 'a half-scale kind gets a half-size quad')
  check(giant.geometry.w === 3, 'and a triple-scale one a triple-size quad')
  check(plain.geometry.w === 1, 'while a kind with no scale is unchanged')

  // Feet stay on the ground: the simulation's position is the BOTTOM of the
  // thing, so the offset that lifts the quad has to scale with it. Getting
  // this wrong sinks a big sprite into the floor rather than making it big.
  check(runt.userData.yOffset === 0.25 && giant.userData.yOffset === 1.5,
        'and each sits with its feet on the ground')

  // THE CONTROL. Same three kinds, read back from the catalogue: the body is
  // identical whatever the picture does.
  for (const id of [30, 31, 32]) {
    const d = catalogue.kinds.get(id)
    check(d.radius === 0.2 && d.height === 0.9,
          `CONTROL: kind ${id}'s body is untouched by its scale`)
  }
  check(propScale(catalogue.kinds.get(32)) === 1, 'and absent means 1, not 0')
}

// --- and the atlas path, which sizes its quad from the SHEET ------------------
//
// The two drawing paths arrive at a size completely differently. A per-PNG
// sprite bakes it into the geometry once, when the mesh is made; an atlas
// rewrites the quad on every frame change, from the height the sheet declares,
// because each frame carries its own trim. So scale has to be applied in two
// places and a test of one says nothing about the other.
console.log('\n--- the atlas path scales too ---')
{
  const CANVAS = 64
  const atlas = parseAtlas({
    format: 'threesprite-atlas',
    version: 1,
    sprite: 'SPRT',
    canvas: { w: CANVAS, h: CANVAS },
    worldHeight: 2,
    image: 'SPRT.png',
    size: { w: 128, h: 128 },
    views: { count: 1, mirror: false, list: [] },
    frames: {
      f0: {
        rect: { x: 0, y: 0, w: CANVAS, h: CANVAS },
        source: { x: 0, y: 0, w: CANVAS, h: CANVAS },
        pivot: { x: 0.5, y: 1 }
      }
    },
    animations: [{ name: 'idle', fps: 8, loop: true, frames: [{ index: 0, views: { 1: 'f0' } }] }]
  }, 'SPRT')

  // The quad the geometry is given, captured. A full-canvas frame is
  // worldHeight square, so the numbers below are the claim stated directly.
  const written = []
  const THREE = {
    // The attribute API writeAtlasFrame actually uses -- setXYZ/setXY, not a
    // raw array. A double that took the shape this test wanted rather than the
    // shape the code calls would test nothing.
    PlaneGeometry: class {
      constructor (w, h) {
        this.w = w; this.h = h
        const buf = (n) => ({
          v: [],
          setXYZ (i, x, y, z) { this.v[i] = { x, y, z } },
          setXY (i, x, y) { this.v[i] = { x, y } },
          needsUpdate: false,
          n
        })
        this.attributes = { position: buf(3), uv: buf(2) }
      }
    },
    MeshBasicMaterial: class { constructor (o) { Object.assign(this, o); this.color = { setHex () {} } } },
    Mesh: class {
      constructor (geometry, material) {
        this.geometry = geometry; this.material = material
        this.userData = {}; this.parent = {}
      }
    },
    DoubleSide: 2
  }
  const catalogue = parseProps(`
prop 40 small radius 0.2 height 0.9 scale 0.5 solid
atlas 40 sheet SPRT anim idle
prop 41 big   radius 0.2 height 0.9 scale 4 solid
atlas 41 sheet SPRT anim idle
prop 42 plain radius 0.2 height 0.9 solid
atlas 42 sheet SPRT anim idle
`)
  const atlases = new Map([['SPRT', { atlas, material: {} }]])
  const reg = new PropRegistry({ THREE, textures: new Map(), catalogue, atlases })

  // Corner 0 is top-left and corner 2 is bottom-left, as writeAtlasFrame lays
  // a quad out, so their y difference is how tall it was drawn.
  const heightOf = (mesh) => {
    const v = mesh.geometry.attributes.position.v
    return Math.abs(v[0].y - v[2].y)
  }

  const small = reg.make(40)
  const big = reg.make(41)
  const plain = reg.make(42)
  reg.update(0)
  written.push(heightOf(small), heightOf(big), heightOf(plain))
  check(near(heightOf(plain), 2),
        `an unscaled kind is drawn at the sheet's worldHeight (${heightOf(plain)})`)
  check(near(heightOf(small), 1), `a half-scale one at half of it (${heightOf(small)})`)
  check(near(heightOf(big), 8), `and a four-times one at four (${heightOf(big)})`)

  // CONTROL: the sheet itself did not move. Scaling one kind must not resize
  // every other kind drawn from the same sheet -- which is exactly what
  // writing the scale into the atlas rather than passing it would do.
  check(atlas.worldHeight === 2, "CONTROL: the sheet's own worldHeight is unchanged")
}

console.log(`\nprops: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
