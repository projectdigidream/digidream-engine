//
// Other bodies' parameters (remoteparams.js): kept apart from the client's
// own, by entity and generation, with the engine's defaults for what was not
// sent. The bug it ends: every PARAM_UPDATE went into the client's own body.
//
import { RemoteParams, isOwnParams } from '../src/remoteparams.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

console.log('remoteparams: other bodies\' parameters')
const ids = { attack_reach: 5, attack_ticks: 9 }
const rp = new RemoteParams({ defaultRaw: (id) => BigInt(1000 + id), paramId: (n) => ids[n] })
const handle = (index, gen) => ((gen << 24) | (index + 1)) >>> 0
const SELF = 3
check(isOwnParams({ handle: handle(SELF, 0) }, SELF), 'an update about our own body is ours')
check(!isOwnParams({ handle: handle(7, 0) }, SELF), 'CONTROL: one about entity 7 is not')
rp.take({ handle: handle(7, 2), flags: 0, fields: [{ id: 5, raw: 8589934592n }] })
const v = rp.view(7)
check(v.getParamRaw(7, v.paramId('attack_reach')) === 8589934592n, 'what was sent is what is read')
check(v.getParamRaw(7, v.paramId('attack_ticks')) === 1009n, 'and what was not is the engine\'s default')
check(rp.view(8).getParamRaw(8, 5) === 1005n, 'a body nobody spoke of is all defaults')
rp.take({ handle: handle(7, 3), flags: 0, fields: [{ id: 9, raw: 16n }] })
check(rp.view(7).getParamRaw(7, 5) === 1005n && rp.view(7).getParamRaw(7, 9) === 16n,
      'a new body in the same slot (another generation) does not inherit the last one\'s')
rp.take({ handle: handle(7, 3), flags: 1, fields: [{ id: 5, raw: 1n }] })
check(rp.view(7).getParamRaw(7, 9) === 1009n && rp.view(7).getParamRaw(7, 5) === 1n, 'a FULL update replaces, not merges')
console.log(`\nremoteparams: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
