//
// CLIMBING ON A REAL SERVER (docs/PLAYER.md): modules/climbtest's course.
//
// A body pushed into the vine ledge climbs its face and mantles onto the
// ledge -- up 3 and standing on it, not climbing the air above. CONTROL: the
// same push into the bare stone ledge beside it never grabs. The rules
// themselves are tested in core/tests/test_abilities.cpp; this is that they
// reach a player through a module's `player climb_auto on` and the wire.
//
import { spawn } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Buffer } from 'node:buffer'
import { WebTransport } from '@fails-components/webtransport'
import { Edge } from '../src/edge.js'
import { encodeHello, decodeControlMessage, encodeInputBundle, decodeSnapshot, axisFromNumber, fxToNumber, MSG } from '../../../client/src/proto.js'
import { serverBin } from './serverbin.js'
const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..', '..')
const MOD = join(ROOT, '..', 'modules', 'climbtest')
const CERTS = join(HERE, '..', 'certs')
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const waitFor = async (fn, ms) => { const end = Date.now() + ms; while (Date.now() < end) { if (fn()) return; await sleep(20) } throw new Error('timeout') }
const auto = true
const LINK = Number(process.env.DNP_LINK_PORT ?? 34560)
const WT = Number(process.env.DNP_WT_PORT ?? 34485)
let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
let log = ''
const server = spawn(serverBin, ['--port', String(LINK), '--map', join(MOD, 'maps', 'climb.dnpm'), '--module', MOD], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] })
server.stderr.on('data', (d) => { log += d }); server.stdout.on('data', (d) => { log += d })
await waitFor(() => log.includes('listening for edge'), 8000)
const certHash = Buffer.from(JSON.parse(readFileSync(join(CERTS, 'cert-hash.json'), 'utf8')).sha256_base64, 'base64')
const edge = new Edge({ port: WT, host: '127.0.0.1', path: '/dnp', upstreamPort: LINK, cert: readFileSync(join(CERTS, 'cert.pem')), privKey: readFileSync(join(CERTS, 'key.pem')), log: () => {} })
await edge.start(); await waitFor(() => log.includes('edge connected'), 8000)
const wt = new WebTransport(`https://127.0.0.1:${WT}/dnp`, { serverCertificateHashes: [{ algorithm: 'sha-256', value: certHash }] })
await wt.ready
const control = await wt.createBidirectionalStream(); const cw = control.writable.getWriter(); const dw = wt.datagrams.createWritable().getWriter()
let welcome = null; const snaps = []
;(async () => { const r = control.readable.getReader(); let buf = Buffer.alloc(0); try { for (;;) { const { value, done } = await r.read(); if (done) break; buf = Buffer.concat([buf, Buffer.from(value)]); while (buf.length >= 4) { const len = buf.readUInt32LE(0); if (buf.length < 4 + len) break; const m = decodeControlMessage(buf.subarray(0, 4 + len)); buf = buf.subarray(4 + len); if (m?.type === MSG.WELCOME) welcome = m } } } catch {} })()
;(async () => { const r = wt.datagrams.readable.getReader(); try { for (;;) { const { value, done } = await r.read(); if (done) break; const s = decodeSnapshot(Buffer.from(value)); if (s?.self) snaps.push(s) } } catch {} })()
await cw.write(encodeHello()); await waitFor(() => welcome && snaps.length > 5, 8000)
let tick = 0; let hist = []
const me = () => { const s = snaps.at(-1).self; return { x: fxToNumber(s.pos.x), y: fxToNumber(s.pos.y), z: fxToNumber(s.pos.z), climbing: (s.stateFlags & 2) !== 0 } }
async function drive (mx, my, buttons, ms) {
  const end = Date.now() + ms
  while (Date.now() < end) {
    tick = Math.max(tick + 1, (snaps.at(-1)?.serverTick ?? 0) + 6)
    hist = [{ buttons, moveX: axisFromNumber(mx), moveY: axisFromNumber(my), aim: 0 }, ...hist].slice(0, 12)
    await dw.write(encodeInputBundle({ clientTick: tick, ackSnapshotTick: snaps.at(-1)?.serverTick ?? 0, frames: hist }))
    await sleep(16)
  }
}
const CLIMB = 0x0004
const trace = (label) => { const p = me(); console.log(`${label.padEnd(28)} x ${p.x.toFixed(2)} y ${p.y.toFixed(2)} z ${p.z.toFixed(2)} climbing ${p.climbing}`); return p }
trace('start')
// to x 7 (under the vine ledge, bay 4..10)
while (me().x < 6.8) await drive(1, 0, 0, 50)
await drive(0, 0, 0, 300)
trace('below the vine ledge')
let maxZ = 0; let sawClimb = false
const end = Date.now() + 2500
while (Date.now() < end) { await drive(0, 1, auto ? 0 : CLIMB, 50); const p = me(); maxZ = Math.max(maxZ, p.z); sawClimb ||= p.climbing }
const top = trace('after pushing into it')
console.log(`  climbed: ${sawClimb}, highest z ${maxZ.toFixed(2)}`)
// back down to the walkway and along to the bare ledge (12..18)
while (me().y > 1.5) await drive(0, -1, 0, 50)
while (me().x < 14.8) await drive(1, 0, 0, 50)
await drive(0, 0, 0, 300)
trace('below the bare ledge')
let bareClimb = false; let bareZ = 0
const end2 = Date.now() + 1500
while (Date.now() < end2) { await drive(0, 1, auto ? 0 : CLIMB, 50); const p = me(); bareClimb ||= p.climbing; bareZ = Math.max(bareZ, p.z) }
trace('after pushing into the bare one')
console.log('climb: the course, on a real server')
check(sawClimb, 'pushing into the vine ledge grabs it (climb_auto, from the module)')
check(top.z > 2.9 && top.z < 3.1 && top.y > 3.2, `and climbs over onto it: standing on the ledge (z ${top.z.toFixed(2)}, y ${top.y.toFixed(2)})`)
check(maxZ < 4, `never climbing the air above it (highest z ${maxZ.toFixed(2)})`)
check(!bareClimb && bareZ < 0.5, 'CONTROL: the bare stone ledge beside it never grabs')
wt.close(); await edge.stop(); server.kill('SIGKILL')
console.log(`\nclimb: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
