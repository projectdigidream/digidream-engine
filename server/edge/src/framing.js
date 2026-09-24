//
// Two byte-stream framings, both length-prefixed, both incremental.
//
//   * CONTROL messages on the WebTransport bidirectional stream
//     (docs/protocol.md section 8): u32 length | u8 type | body
//   * LINK frames on the edge <-> C++ server socket: u8 type | u32 session |
//     u32 length | payload
//
// Both are parsed by the same incremental reader, because both are streams and
// both have the same failure mode if you get it wrong: a read boundary is NOT a
// message boundary. One TCP read may carry half a message or six of them. Code
// that assumes otherwise works perfectly on loopback and falls apart the first
// time it meets a real network, which is the worst possible time to find out.
//
// PLAIN Uint8Array AND DataView, not Buffer, and that is deliberate.
//
// This is the byte-level contract between the edge, the desktop shell's relay
// and now an Android one, and the third of those runs in a WebView where
// `node:buffer` does not exist. The alternative was a second implementation of
// the framing for the browser -- the exact "two programs, one number" failure
// this repository keeps paying for, and the one thing a wire format must never
// have.
//
// Nothing was lost by dropping Buffer: it IS a Uint8Array subclass, so every
// caller that handed a frame to `socket.write` or to a WebTransport writer
// still can, and `server/edge/test/framing.test.js` is the proof that the bytes
// did not move. What a Buffer bought was `readUInt32LE` as a method, which is a
// DataView here and is the only thing that reads differently.
//

export const LINK = {
  SESSION_OPEN: 0x01,
  SESSION_CLOSE: 0x02,
  CONTROL: 0x03,
  DATAGRAM: 0x04
}

export const LINK_HEADER_BYTES = 9

/**
 * A DataView over exactly the bytes of `u8`, however it was sliced.
 *
 * The byteOffset is the part that bites: a subarray of a larger buffer shares
 * the underlying ArrayBuffer and starts partway into it, so a DataView made
 * without the offset reads the frame BEFORE this one. Every frame this file
 * hands out is a subarray, so every read goes through here.
 */
const view = (u8) => new DataView(u8.buffer, u8.byteOffset, u8.byteLength)

const asBytes = (x) =>
  x instanceof Uint8Array ? x : new Uint8Array(x ?? 0)

/** Encode one link frame: u8 type | u32 session_id | u32 length | payload */
export function encodeLinkFrame (type, sessionId, payload = new Uint8Array(0)) {
  const body = asBytes(payload)
  const out = new Uint8Array(LINK_HEADER_BYTES + body.length)
  const dv = view(out)
  dv.setUint8(0, type)
  dv.setUint32(1, sessionId, true)
  dv.setUint32(5, body.length, true)
  out.set(body, LINK_HEADER_BYTES)
  return out
}

/** Encode one control message: u32 length | u8 type | body */
export function encodeControlMessage (type, body = new Uint8Array(0)) {
  const b = asBytes(body)
  const out = new Uint8Array(4 + 1 + b.length)
  const dv = view(out)
  dv.setUint32(0, 1 + b.length, true)
  dv.setUint8(4, type)
  out.set(b, 5)
  return out
}

/**
 * Incremental length-prefixed reader.
 *
 * `maxMessageBytes` is a guard, not a tuning knob: without it, a corrupt or
 * hostile length prefix asks the process to allocate up to 4 GiB, which turns a
 * malformed packet into an out-of-memory kill.
 */
export class FrameReader {
  constructor ({ headerBytes, readLength, maxMessageBytes = 1 << 20 }) {
    this.headerBytes = headerBytes
    this.readLength = readLength
    this.maxMessageBytes = maxMessageBytes
    this.buf = new Uint8Array(0)
  }

  /** Feed bytes; returns an array of complete frames (possibly empty). */
  push (chunk) {
    const add = asBytes(chunk)
    if (this.buf.length === 0) {
      // Copied rather than kept by reference. The caller's chunk is a socket
      // read buffer that may be reused under us, and holding a slice of it
      // across a turn of the event loop reads whatever arrived next.
      this.buf = add.slice()
    } else {
      const joined = new Uint8Array(this.buf.length + add.length)
      joined.set(this.buf, 0)
      joined.set(add, this.buf.length)
      this.buf = joined
    }

    const out = []
    for (;;) {
      if (this.buf.length < this.headerBytes) break
      const len = this.readLength(this.buf)
      if (len > this.maxMessageBytes) {
        throw new Error(`frame length ${len} exceeds max ${this.maxMessageBytes}`)
      }
      const total = this.headerBytes + len
      if (this.buf.length < total) break
      out.push(this.buf.subarray(0, total))
      this.buf = this.buf.subarray(total)
    }
    return out
  }
}

/** Reader for link frames (u8 type | u32 session | u32 length | payload). */
export function createLinkReader () {
  return new FrameReader({
    headerBytes: LINK_HEADER_BYTES,
    readLength: (b) => view(b).getUint32(5, true)
  })
}

/** Reader for control messages (u32 length | u8 type | body). */
export function createControlReader () {
  return new FrameReader({
    headerBytes: 4,
    readLength: (b) => view(b).getUint32(0, true)
  })
}

export function decodeLinkFrame (frame) {
  const dv = view(frame)
  return {
    type: dv.getUint8(0),
    sessionId: dv.getUint32(1, true),
    payload: frame.subarray(LINK_HEADER_BYTES)
  }
}

export function decodeControlMessage (frame) {
  return {
    type: view(frame).getUint8(4),
    body: frame.subarray(5)
  }
}
