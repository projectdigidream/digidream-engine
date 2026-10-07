//
// THE SAME ORIGIN EVERY LAUNCH (shells/app/static.js `port`): the static server
// takes the port the last run used, so the studio's storage -- kept per
// origin, port included -- survives a relaunch. CONTROL: a port that is taken
// falls back to one the OS picks, so two copies still run side by side.
//
// House style: standalone, prints ok/FAIL, ends with a count.
//
import { mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { startStatic } from '../static.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

const root = mkdtempSync(join(tmpdir(), 'dnp-port-'))
const first = await startStatic({ root })
const p = first.port
first.server.close()
await new Promise((r) => setTimeout(r, 100))
const again = await startStatic({ root, port: p })
check(again.port === p, `a launch asking for the last port gets it (${p})`)
const taken = await startStatic({ root, port: p })
check(taken.port !== p && taken.port > 0, `CONTROL: while it is taken, another (${taken.port})`)
const r = await fetch(`http://127.0.0.1:${taken.port}/nothing`)
check(r.status === 404, 'and that one serves')
again.server.close(); taken.server.close()
console.log(`\nsame_port: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
