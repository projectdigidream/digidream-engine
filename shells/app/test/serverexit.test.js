//
// A server the shell stopped is not a crash, however late it says goodbye
// (shells/app/serverexit.js). The case that shipped: a play-test restart
// stopped the old server, started the new one, and the old one's 'close'
// arrived after -- read as the new one failing, so the app showed an error
// box quoting the old server's log, or quit outright.
//
import { exitVerdict } from '../serverexit.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

const base = { intended: false, current: true, quitting: false, playTest: false, studio: true }
// The restart: the old process, stopped on purpose, closing after the new one
// is running -- no longer current, and marked.
check(exitVerdict({ ...base, intended: true, current: false }) === 'expected',
      'the old server of a restart, closing after the new one started: expected')
check(exitVerdict({ ...base, intended: false, current: false }) === 'expected',
      'any server that is not the one running now: expected')
check(exitVerdict({ ...base, intended: true, current: true }) === 'expected',
      'one the shell stopped, still the one it holds: expected')
check(exitVerdict({ ...base, quitting: true }) === 'expected', 'on quit: expected')
// CONTROLS: a real failure is still one.
check(exitVerdict({ ...base, playTest: true }) === 'back-to-studio',
      'CONTROL: the running play-test server failing: back to the studio')
check(exitVerdict({ ...base }) === 'fatal', 'CONTROL: the running game server failing: fatal')
check(exitVerdict({ ...base, playTest: true, studio: false }) === 'fatal',
      'CONTROL: a packaged game (no studio) failing: fatal')

console.log(`\nserverexit: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
