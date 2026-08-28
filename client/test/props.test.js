//
// The prop catalogue, as the renderer reads it.
//
// The claim this file exists to protect: adding a prop is editing a text file,
// and NOTHING about which kinds exist is compiled into the client. So the tests
// are about the parser being tolerant in the right places and strict in the
// right places, and about the animation clock being the client's own.
//
import { parseProps, frameAt, rotationFor, frameName, PropRegistry } from '../src/render/props.js'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

console.log('--- the shipped catalogue ---')
{
  // The real file, not a fixture: a parser that only works on the examples in
  // its own test is a parser nobody can trust with the file that matters.
  const text = readFileSync(join(ROOT, 'assets', 'props.txt'), 'utf8')
  const { kinds, warnings } = parseProps(text)
  check(warnings.length === 0, `parses clean (${warnings.length} warnings)`)
  check(kinds.size >= 6, `${kinds.size} kinds`)

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
  const cat = parseProps(readFileSync(join(ROOT, 'assets', 'props.txt'), 'utf8'))
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
  const cat = parseProps(readFileSync(join(ROOT, 'assets', 'props.txt'), 'utf8'))
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

console.log(`\nprops: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
