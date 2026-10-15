//
// EVERY FILE THE DESKTOP SHELL IMPORTS IS PACKED: scripts/build_app.sh copies
// the shell's files by name, so a new module main.js imports (links.js was
// one) and nobody adds there is an app that dies at launch. Read from the
// sources: main.js's and preload.cjs's local imports, against the cp line.
//
import { readFileSync } from 'node:fs'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const src = (p) => readFileSync(new URL(p, import.meta.url), 'utf8')

console.log('shell_files: what the desktop shell imports, build_app.sh packs')
const build = src('../../../scripts/build_app.sh')
// Every cp into the app folder (some files come from server/edge/src), with its continuation lines.
const cps = build.replace(/\\\n/g, ' ').split('\n').filter((l) => /^\s*cp\b/.test(l) && l.includes('$OUT/app/'))
const packed = new Set(cps.flatMap((l) => [...l.matchAll(/([\w.-]+\.c?js)\b/g)].map((m) => m[1])))
const local = new Set()
for (const f of ['main.js', 'preload.cjs', 'static.js', 'relay.js', 'studio_files.js', 'filelog.js']) {
  for (const m of src(`../${f}`).matchAll(/(?:from\s+|require\()\s*['"]\.\/([\w.-]+)['"]/g)) local.add(m[1])
}
check(local.size > 3, `the shell's own imports are found (${[...local].join(', ')})`)
for (const f of local) check(packed.has(f), `${f} is packed by build_app.sh`)
check(!packed.has('nonexistent.js'), 'CONTROL: a file nobody packs is not seen as packed')

console.log(`\nshell_files: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
