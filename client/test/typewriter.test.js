//
// A typed subtitle (render/typewriter.js): letters out at a speed, a pack at a
// time, and a sound for every step that shows a letter.
//
import { Typewriter } from '../src/render/typewriter.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

console.log('typewriter: a subtitle typed out')
{
  // 10 letters a second, 2 at a time: a step every 0.2 s.
  const t = new Typewriter({ text: 'Hello there', speed: 10, pack: 2, startedAt: 100 })
  check(t.shownAt(100) === 'He', `the first step at once (${JSON.stringify(t.shownAt(100))})`)
  check(t.shownAt(100.19) === 'He' && t.shownAt(100.2) === 'Hell', 'the next after pack / speed seconds')
  check(t.shownAt(100.6) === 'Hello th', `and on, by the clock (${JSON.stringify(t.shownAt(100.6))})`)
  check(t.shownAt(200) === 'Hello there' && t.doneAt(200) && !t.doneAt(100.5), 'the whole line in the end, and no further')
  check(t.shownAt(99) === 'He', 'a clock before the start shows the first step, not nothing')
}
{
  // Sounds: one per step that shows a letter; a step of spaces is silent.
  const t = new Typewriter({ text: 'ab  cd', speed: 2, pack: 2, startedAt: 0 })
  check(t.takeSounds(0).join() === '0', 'the first step sounds')
  check(t.takeSounds(0.5).length === 0, 'CONTROL: the same step is not sounded twice')
  check(t.takeSounds(1).length === 0, 'a step of only spaces is silent')
  check(t.takeSounds(2).join() === '2', 'the next letters sound')
  check(t.takeSounds(9).length === 0, 'and nothing after the end')
  const late = new Typewriter({ text: 'abcdef', speed: 2, pack: 2, startedAt: 0 })
  check(late.takeSounds(5).join() === '0,1,2', 'a frame that missed several steps is told them all, once')
}
{
  const e = new Typewriter({ text: 'a😀b', speed: 100, pack: 1, startedAt: 0 })
  check(e.shownAt(0.015) === 'a😀', 'an emoji is one letter, not half of one')
  check(new Typewriter({ text: '', speed: 5, pack: 3, startedAt: 0 }).shownAt(1) === '', 'an empty line types nothing')
  check(new Typewriter({ text: 'abc', speed: 0, pack: 0, startedAt: 0 }).shownAt(0) === 'a', 'nonsense speed and pack are held to one')
}
console.log(`\ntypewriter: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
