//
// The desktop game's log file (filelog.js): stamped lines, a fresh file per
// launch with the one before kept, and no throw where it cannot be written.
//
import { mkdtempSync, readFileSync, existsSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { openGameLog } from '../filelog.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

console.log('filelog: the game\'s log file')
const dir = join(mkdtempSync(join(tmpdir(), 'dnp-filelog-')), 'logs')
try {
  const a = openGameLog(dir, 'doom2', 'first launch')
  check(a && a.where === join(dir, 'doom2-log.txt') && existsSync(a.where), `opened, in a folder it made (${a?.where})`)
  a.line('[srv] listening\n[page] [log] synth ready')
  a.line('')
  const first = readFileSync(a.where, 'utf8').trim().split('\n')
  check(first.length === 3, `the header and two lines, one per line given, empty ones dropped (${first.length})`)
  check(first.every((l) => /^\d\d:\d\d:\d\d\.\d{3} \[/.test(l)), 'every line stamped with the time')
  check(/\[shell\] first launch$/.test(first[0]) && /\[page\] \[log\] synth ready$/.test(first[2]), 'in order, as given')
  const b = openGameLog(dir, 'doom2', 'second launch')
  check(readFileSync(join(dir, 'doom2-log-previous.txt'), 'utf8').includes('first launch'), 'the next launch keeps the last as -previous')
  check(!readFileSync(b.where, 'utf8').includes('first launch'), 'and starts its own afresh')
  openGameLog(dir, 'doom2', 'third launch')
  check(readFileSync(join(dir, 'doom2-log-previous.txt'), 'utf8').includes('second launch'), 'only one previous is kept')
  const blocker = join(dir, 'not-a-dir')
  writeFileSync(blocker, 'x')
  check(openGameLog(join(blocker, 'logs'), 'doom2', 'x') === null, 'CONTROL: where it cannot write, null rather than a throw')
} finally {
  rmSync(join(dir, '..'), { recursive: true, force: true })
}
console.log(`\nfilelog: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
