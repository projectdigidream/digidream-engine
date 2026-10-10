//
// The desktop shell's error dialog quotes what the server said (said.js).
//
import { serverSaid } from '../said.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

console.log('said: what a dying server said, for the dialog')
const lines = ['[srv] controls: platformer', ...Array.from({ length: 30 }, (_, i) => `[srv] line ${i}`),
  '[srv] game script maps/broken.lua failed: 1: \'=\' expected near \'is\'', '[srv] refusing to start without the game\'s scripts']
const said = serverSaid(lines)
check(said.startsWith('[srv] game script maps/broken.lua failed'), 'the reason comes first')
check(said.includes('refusing to start'), 'with the refusal')
check(!said.includes('line 0\n') && said.includes('line 29'), 'then only the end of the rest')
check(serverSaid(['[srv] a', '[srv] b']) === '[srv] a\n[srv] b', 'CONTROL: with no reason, just the end')
check(serverSaid([]) === '', 'and nothing from nothing')
console.log(`\nsaid: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
