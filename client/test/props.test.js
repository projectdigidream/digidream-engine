//
// The prop catalogue, as the renderer reads it.
//
// The claim this file exists to protect: adding a prop is editing a text file,
// and NOTHING about which kinds exist is compiled into the client. So the tests
// are about the parser being tolerant in the right places and strict in the
// right places, and about the animation clock being the client's own.
//
import { parseProps, frameAt } from '../src/render/props.js'
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

console.log(`\nprops: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
