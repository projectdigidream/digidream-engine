// EVERY FILE THE SHELL IMPORTS IS IN THE APP (scripts/build_app.sh copies the
// shell by an explicit list). A module main.js imports but the list leaves
// out is an app that cannot start at all: engine 0.2.3.4's desktop kits
// shipped main.js importing ./selfupdate.js without it.
//
//   node shells/app/test/copied.test.js

import { readFileSync, existsSync } from 'node:fs'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

const here = (p) => new URL(p, import.meta.url)
const script = readFileSync(here('../../../scripts/build_app.sh'), 'utf8')
const copied = new Set([...script.matchAll(/shells\/app\/([\w.-]+\.(?:js|cjs))/g)].map((m) => m[1]))
// the shell's own modules, followed from main.js and preload.cjs through each other
const seen = new Set()
const walk = (name) => {
  if (seen.has(name)) return
  seen.add(name)
  const text = readFileSync(here(`../${name}`), 'utf8')
  for (const m of text.matchAll(/(?:from\s+|import\(\s*|require\(\s*)['"]\.\/([\w.-]+\.(?:js|cjs))['"]/g)) if (existsSync(here(`../${m[1]}`))) walk(m[1])
}
walk('main.js'); walk('preload.cjs')
for (const f of [...seen].sort()) check(copied.has(f), `${f} is copied into the app`)
// CONTROL: the list is really read (a name it does not hold is not "copied")
check(!copied.has('nothing-here.js') && copied.has('main.js'), 'CONTROL: the copy list is read from build_app.sh')

console.log(`\ncopied: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
