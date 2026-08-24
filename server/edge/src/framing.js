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
import { Buffer } from 'node:buffer'

export const LINK = {
  SESSION_OPEN: 0x01,
  SESSION_CLOSE: 0x02,
  CONTROL: 0x03,
  DATAGRAM: 0x04
}

export const LINK_HEADER_BYTES = 9

/** Encode one link frame: u8 type | u32 session_id | u32 length | payload */
export function encodeLinkFrame (type, sessionId, payload = Buffer.alloc(0)) {
  const body = Buffer.isBuffer(payload) ? payload : Buffer.from(payload)
  const out = Buffer.allocUnsafe(LINK_HEADER_BYTES + body.length)
  out.writeUInt8(type, 0)
  out.writeUInt32LE(sessionId, 1)
  out.writeUInt32LE(body.length, 5)
  body.copy(out, LINK_HEADER_BYTES)
  return out
}

/** Encode one control message: u32 length | u8 type | body */
export function encodeControlMessage (type, body = Buffer.alloc(0)) {
  const b = Buffer.isBuffer(body) ? body : Buffer.from(body)
  const out = Buffer.allocUnsafe(4 + 1 + b.length)
  out.writeUInt32LE(1 + b.length, 0)
  out.writeUInt8(type, 4)
  b.copy(out, 5)
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
    this.buf = Buffer.alloc(0)
  }

  /** Feed bytes; returns an array of complete frames (possibly empty). */
  push (chunk) {
    this.buf = this.buf.length === 0 ? Buffer.from(chunk) : Buffer.concat([this.buf, chunk])
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
    readLength: (b) => b.readUInt32LE(5)
  })
}

/** Reader for control messages (u32 length | u8 type | body). */
export function createControlReader () {
  return new FrameReader({
    headerBytes: 4,
    readLength: (b) => b.readUInt32LE(0)
  })
}

export function decodeLinkFrame (frame) {
  return {
    type: frame.readUInt8(0),
    sessionId: frame.readUInt32LE(1),
    payload: frame.subarray(LINK_HEADER_BYTES)
  }
}

export function decodeControlMessage (frame) {
  return {
    type: frame.readUInt8(4),
    body: frame.subarray(5)
  }
}
