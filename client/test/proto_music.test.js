//
// The MUSIC control message (docs/protocol.md §11.1), as the server writes it
// and this client reads it: a cue name, a crossfade, and when it lands.
//
import { encodeControlMessage, decodeControlMessage, MSG, UI_MAX_TEXT } from '../src/proto.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  if (!ok) { fails++; console.log(`  FAIL ${label}`) }
}

// The body the server's encode_music_cue writes: u16 length + bytes, u16 fade, u8 at.
const body = (cue, fadeMs, at) => {
  const b = new TextEncoder().encode(cue)
  const out = new Uint8Array(2 + b.length + 3)
  out[0] = b.length & 255; out[1] = b.length >> 8
  out.set(b, 2)
  out[2 + b.length] = fadeMs & 255; out[3 + b.length] = fadeMs >> 8
  out[4 + b.length] = at
  return out
}

const m = decodeControlMessage(encodeControlMessage(MSG.MUSIC, body('fight', 1200, 1)))
check(m?.type === MSG.MUSIC && m.cue === 'fight' && m.fadeMs === 1200 && m.at === 'loopEnd', `a cue change decodes with its fade and moment (${JSON.stringify(m)})`)
check(decodeControlMessage(encodeControlMessage(MSG.MUSIC, body('default', 0, 0)))?.at === 'nextBar', 'moment 0 is the next bar')
check(decodeControlMessage(encodeControlMessage(MSG.MUSIC, body('calm', 65535, 2)))?.at === 'immediate', 'moment 2 is at once, and the fade fills a u16')
check(decodeControlMessage(encodeControlMessage(MSG.MUSIC, body('fight', 800, 3))) === null, 'CONTROL: a moment the protocol does not know is refused')
check(decodeControlMessage(encodeControlMessage(MSG.MUSIC, body('x'.repeat(UI_MAX_TEXT + 1), 800, 0))) === null, 'CONTROL: a name past the cap is refused, not clamped')
check(decodeControlMessage(encodeControlMessage(MSG.MUSIC, body('fight', 800, 0).subarray(0, 8))) === null, 'CONTROL: a truncated body is refused')

console.log(`proto_music: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
