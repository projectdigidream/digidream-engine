//
// The INPUT_BUNDLE the client writes (core encode_input_bundle's layout):
// 15 bytes a frame, the last the module's own keys (docs/KEYS.md). With the
// full sixteen frames, because the buffer was once sized for fourteen-byte
// frames and overran only past six -- a test with a few frames passed.
//
import { encodeInputBundle } from '../src/proto.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

console.log('proto_input: what the client sends each tick')
for (const n of [1, 6, 7, 16]) {
  const frames = Array.from({ length: n }, (_, i) => ({ buttons: 1, moveX: 0, moveY: 0, aim: 0, actions: (i % 8) + 1 }))
  let bytes = null
  try { bytes = encodeInputBundle({ clientTick: 9, frames }) } catch (e) { bytes = null }
  check(bytes !== null && bytes.length === 10 + 15 * n, `${n} frame(s): ${bytes?.length} bytes, 15 a frame`)
  // The actions byte is each frame's last: frame 0's at 10 + 14.
  check(bytes !== null && bytes[10 + 14] === 1 && bytes[10 + 15 * (n - 1) + 14] === ((n - 1) % 8) + 1,
        `${n} frame(s): each frame's last byte is its keys`)
}

console.log(`\nproto_input: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
