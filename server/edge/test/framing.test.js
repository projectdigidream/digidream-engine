//
// Unit tests for the length-prefixed stream readers.
//
// These matter more than they look. A stream reader that assumes one read ==
// one message passes every casual test on loopback (where writes usually do
// arrive whole) and then fails in production under fragmentation or coalescing.
// So the tests here deliberately feed the pathological chunkings: one byte at a
// time, several messages in one chunk, and splits landing inside the length
// prefix itself.
//
import { Buffer } from 'node:buffer'
import {
  encodeControlMessage, encodeLinkFrame, createControlReader, createLinkReader,
  decodeControlMessage, decodeLinkFrame, LINK
} from '../src/framing.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  if (!ok) { fails++; console.log(`  FAIL ${label}`) }
}

// --- control messages -------------------------------------------------------

{
  const msgs = [
    encodeControlMessage(0x01, Buffer.from('hello')),
    encodeControlMessage(0x02, Buffer.alloc(0)),
    encodeControlMessage(0x10, Buffer.from('x'.repeat(300)))
  ]
  const all = Buffer.concat(msgs)

  // Everything in one chunk.
  {
    const r = createControlReader()
    const got = r.push(all)
    check(got.length === 3, 'control: 3 messages from one chunk')
    check(decodeControlMessage(got[0]).type === 0x01, 'control: type preserved')
    check(decodeControlMessage(got[0]).body.toString() === 'hello', 'control: body preserved')
    check(decodeControlMessage(got[1]).body.length === 0, 'control: empty body')
    check(decodeControlMessage(got[2]).body.length === 300, 'control: 300-byte body')
  }

  // One byte at a time -- the worst case, and the one that catches a reader
  // that peeks at the length prefix before it has all four bytes of it.
  {
    const r = createControlReader()
    const got = []
    for (const byte of all) got.push(...r.push(Buffer.from([byte])))
    check(got.length === 3, 'control: 3 messages from single-byte chunks')
    check(decodeControlMessage(got[2]).body.length === 300, 'control: reassembled large body')
  }

  // Splits at every possible offset.
  {
    let ok = true
    for (let split = 1; split < all.length; split++) {
      const r = createControlReader()
      const got = [...r.push(all.subarray(0, split)), ...r.push(all.subarray(split))]
      if (got.length !== 3) { ok = false; break }
    }
    check(ok, 'control: correct at every split offset')
  }
}

// --- link frames ------------------------------------------------------------

{
  const frames = [
    encodeLinkFrame(LINK.SESSION_OPEN, 7, Buffer.from('{"path":"/dnp"}')),
    encodeLinkFrame(LINK.DATAGRAM, 7, Buffer.from([1, 2, 3, 4])),
    encodeLinkFrame(LINK.SESSION_CLOSE, 7, Buffer.from([0, 0]))
  ]
  const all = Buffer.concat(frames)

  const r = createLinkReader()
  const got = r.push(all)
  check(got.length === 3, 'link: 3 frames from one chunk')

  const open = decodeLinkFrame(got[0])
  check(open.type === LINK.SESSION_OPEN, 'link: type preserved')
  check(open.sessionId === 7, 'link: session id preserved')
  check(JSON.parse(open.payload.toString()).path === '/dnp', 'link: payload preserved')

  const dg = decodeLinkFrame(got[1])
  check(Buffer.compare(dg.payload, Buffer.from([1, 2, 3, 4])) === 0, 'link: datagram payload intact')

  // Byte-at-a-time across the whole stream.
  {
    const r2 = createLinkReader()
    const got2 = []
    for (const byte of all) got2.push(...r2.push(Buffer.from([byte])))
    check(got2.length === 3, 'link: 3 frames from single-byte chunks')
  }
}

// --- guard rail -------------------------------------------------------------

{
  // A corrupt or hostile length prefix must be rejected, not honoured. Without
  // this the process would try to allocate whatever the peer asked for.
  const evil = Buffer.alloc(9)
  evil.writeUInt8(LINK.DATAGRAM, 0)
  evil.writeUInt32LE(1, 1)
  evil.writeUInt32LE(0xffffffff, 5)
  const r = createLinkReader()
  let threw = false
  try { r.push(evil) } catch { threw = true }
  check(threw, 'link: oversized length prefix is rejected')
}

console.log(`framing: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
