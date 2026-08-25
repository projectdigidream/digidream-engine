//
// JavaScript implementation of the wire protocol (docs/protocol.md).
//
// This is a SECOND, independent implementation of the format that core/proto
// implements in C++. That redundancy is the point at this stage: a codec tested
// only against itself passes even when both halves are wrong in the same way,
// whereas an independent decoder disagreeing with the C++ encoder is a real
// signal. It also lets the harness drive the server without a wasm build.
//
// The shipping client will decode through the wasm core instead, because it must
// share the server's exact arithmetic (protocol R2/R3). This file stays useful
// for tests, tooling and the network simulator.
//
import { Buffer } from 'node:buffer'

export const MSG = {
  HELLO: 0x01,
  WELCOME: 0x02,
  REJECT: 0x03,
  MAP_DATA: 0x10,
  SECTOR_MOTION_SET: 0x12,
  INPUT_BUNDLE: 0x80,
  SNAPSHOT: 0x81
}

export const BUTTON = {
  JUMP: 0x0001,
  DASH: 0x0002,
  CLIMB: 0x0004,
  HOVER: 0x0008,
  FLIGHT: 0x0010,
  GRAPPLE: 0x0020,
  INTERACT: 0x0040,
  ATTACK_PRIMARY: 0x0080,
  ATTACK_SECONDARY: 0x0100,
  USE: 0x0200,
  CROUCH: 0x0400
}

export const ENT_FIELD = {
  POS: 1 << 0,
  FACING: 1 << 1,
  ANIMATION_STATE: 1 << 2,
  VISUAL_PARAMS: 1 << 3,
  ANCHOR: 1 << 4,
  CONTROLLER_MODE: 1 << 5,
  DISPLAY_ID: 1 << 6,
  DESTROYED: 1 << 7
}

export const STATE_FLAG = {
  GROUNDED: 1 << 0,
  CLIMBING: 1 << 1,
  HOVERING: 1 << 2,
  FLYING: 1 << 3,
  GRAPPLING: 1 << 4,
  ANCHORED: 1 << 5
}

export const PROTOCOL_VERSION = 0
export const TICK_RATE_HZ = 60
export const FX_ONE = 4294967296n // 2^32
const FXQ_SHIFT = 20n

/** fx raw (BigInt, Q32.32) -> JS number. Display and assertions only. */
export const fxToNumber = (raw) => Number(raw) / 4294967296

/** JS number -> fx raw. Only for building test inputs, never for simulation. */
export const numberToFx = (n) => BigInt(Math.round(n * 4294967296))

// Q1.15 input axis. Note the SYMMETRIC clamp: Q1.15 could represent -32768, but
// using it would make full-left stronger than full-right and break the mirror
// symmetry the physics contract preserves. Must match axis_from_fx in C++.
export function axisFromNumber (v) {
  const c = Math.max(-1, Math.min(1, v))
  const t = Math.trunc(c * 32768)
  return Math.max(-32767, Math.min(32767, t))
}

class Writer {
  constructor (size = 2048) {
    this.buf = Buffer.alloc(size)
    this.pos = 0
  }

  u8 (v) { this.buf.writeUInt8(v & 0xff, this.pos); this.pos += 1; return this }
  u16 (v) { this.buf.writeUInt16LE(v & 0xffff, this.pos); this.pos += 2; return this }
  i16 (v) { this.buf.writeInt16LE(v, this.pos); this.pos += 2; return this }
  u32 (v) { this.buf.writeUInt32LE(v >>> 0, this.pos); this.pos += 4; return this }
  u64 (v) { this.buf.writeBigUInt64LE(BigInt(v), this.pos); this.pos += 8; return this }
  bytes (b) { b.copy(this.buf, this.pos); this.pos += b.length; return this }
  done () { return this.buf.subarray(0, this.pos) }
}

class Reader {
  constructor (buf) {
    this.buf = Buffer.isBuffer(buf) ? buf : Buffer.from(buf)
    this.pos = 0
    this.ok = true
  }

  need (n) {
    if (!this.ok || this.pos + n > this.buf.length) { this.ok = false; return false }
    return true
  }

  u8 () { if (!this.need(1)) return 0; return this.buf.readUInt8(this.pos++) }
  i8 () { if (!this.need(1)) return 0; return this.buf.readInt8(this.pos++) }
  u16 () { if (!this.need(2)) return 0; const v = this.buf.readUInt16LE(this.pos); this.pos += 2; return v }
  u32 () { if (!this.need(4)) return 0; const v = this.buf.readUInt32LE(this.pos); this.pos += 4; return v }
  i32 () { if (!this.need(4)) return 0; const v = this.buf.readInt32LE(this.pos); this.pos += 4; return v }
  i64 () { if (!this.need(8)) return 0n; const v = this.buf.readBigInt64LE(this.pos); this.pos += 8; return v }
  u64 () { if (!this.need(8)) return 0n; const v = this.buf.readBigUInt64LE(this.pos); this.pos += 8; return v }

  varint () {
    let result = 0n
    let shift = 0n
    for (let i = 0; i < 10; i++) {
      const b = this.u8()
      if (!this.ok) return 0
      result |= BigInt(b & 0x7f) << shift
      if ((b & 0x80) === 0) return Number(result)
      shift += 7n
    }
    this.ok = false
    return 0
  }

  fxraw () { return this.i64() }
  fxq () { return BigInt(this.i32()) << FXQ_SHIFT }
  vec3raw () { const x = this.fxraw(); const y = this.fxraw(); const z = this.fxraw(); return { x, y, z } }
  vec3q () { const x = this.fxq(); const y = this.fxq(); const z = this.fxq(); return { x, y, z } }
  atEnd () { return this.pos >= this.buf.length }
}

// ------------------------------------------------------------- control channel

/** u32 length | u8 type | body  (docs/protocol.md section 8) */
export function encodeControlMessage (type, body = Buffer.alloc(0)) {
  const w = new Writer(body.length + 8)
  w.u32(1 + body.length).u8(type).bytes(body)
  return w.done()
}

export function encodeHello ({ version = PROTOCOL_VERSION } = {}) {
  // Only the version is sent for now: the server parses nothing further until
  // identity and inventory land in Phase 7. The remaining HELLO fields
  // (client_id, entry_spawn_tag, inventory) are specified but not yet wired.
  const w = new Writer(8)
  w.u16(version)
  return encodeControlMessage(MSG.HELLO, w.done())
}

export function decodeControlMessage (buf) {
  const r = new Reader(buf)
  const len = r.u32()
  const type = r.u8()
  if (!r.ok) return null

  if (type === MSG.WELCOME) {
    const msg = {
      type,
      version: r.u16(),
      serverId: r.u64(),
      tickRate: r.u16(),
      snapshotRate: r.u16(),
      serverTick: r.u32(),
      selfHandle: r.u32(),
      mapHash: r.u64()
    }
    return r.ok ? msg : null
  }
  if (type === MSG.SECTOR_MOTION_SET) {
    const msg = {
      type,
      sector: r.u32(),
      motionType: r.u8(),
      // Which surfaces the vertical path moves: 0 both, 1 floor, 2 ceiling.
      // A client that ignored it would put a crusher's floor where its ceiling
      // belongs (docs/protocol.md §8.2).
      motionTarget: r.u8(),
      startTick: r.u32(),
      durationTicks: r.u32(),
      loopMode: r.u8(),
      from: r.vec3raw(),
      to: r.vec3raw()
    }
    return r.ok ? msg : null
  }
  if (type === MSG.MAP_DATA) {
    const totalBytes = r.u32()
    const offset = r.u32()
    if (!r.ok) return null
    // The remainder of the message body is the chunk. `len` counts the type
    // byte plus the body, so the chunk is what is left after the two headers.
    const chunkLen = len - 1 - 8
    if (chunkLen < 0 || r.pos + chunkLen > r.buf.length) return null
    const chunk = r.buf.subarray(r.pos, r.pos + chunkLen)
    return { type, totalBytes, offset, chunk }
  }
  if (type === MSG.REJECT) {
    const msg = { type, reason: r.u16() }
    return r.ok ? msg : null
  }
  return { type, length: len }
}

// --------------------------------------------------------------- state channel

export function encodeInputBundle ({ clientTick, ackSnapshotTick = 0, frames }) {
  const n = Math.min(frames.length, 16)
  const w = new Writer(16 + n * 10)
  w.u8(MSG.INPUT_BUNDLE).u32(clientTick).u32(ackSnapshotTick).u8(n)
  for (let i = 0; i < n; i++) {
    const f = frames[i]
    w.u16((f.buttons ?? 0) & ~0xf800)   // reserved bits stripped, as in C++
    w.i16(f.moveX ?? 0)
    w.i16(f.moveY ?? 0)
    w.u32(f.aim ?? 0)
  }
  return w.done()
}

export function decodeSnapshot (buf) {
  const r = new Reader(buf)
  if (r.u8() !== MSG.SNAPSHOT) return null

  const snap = {
    serverTick: r.u32(),
    baselineTick: r.u32(),
    lastInputTick: r.u32(),
    inputMargin: r.i8(),
    flags: r.u16(),
    self: {},
    entities: [],
    vfx: []
  }

  snap.self.pos = r.vec3raw()
  snap.self.vel = r.vec3raw()
  snap.self.facing = r.u32()
  snap.self.controllerMode = r.u8()
  snap.self.stateFlags = r.u16()
  snap.self.anchor = r.u32()
  snap.self.jumpsUsed = r.u8()
  snap.self.dashesUsed = r.u8()
  snap.self.sector = r.i32()
  snap.self.dashTimer = r.fxraw()
  snap.self.dashCooldown = r.fxraw()
  snap.self.hoverFuel = r.fxraw()
  snap.self.flightFuel = r.fxraw()
  if (snap.self.stateFlags & STATE_FLAG.GRAPPLING) {
    snap.self.grappleAnchor = r.vec3raw()
    snap.self.grappleDist = r.fxraw()
  }
  snap.self.stateHash = r.u64()
  snap.hp = r.i32()
  if (!r.ok) return null

  // Scripted-discrete sector offsets. Parametric movers are absent by
  // construction -- the client computes those from the parameters it holds.
  const offsetCount = r.u16()
  if (!r.ok || offsetCount > 32) return null
  snap.sectorOffsets = []
  for (let i = 0; i < offsetCount; i++) {
    snap.sectorOffsets.push({ sector: r.u32(), offset: r.vec3raw() })
  }
  if (!r.ok) return null

  const entityCount = r.u16()
  if (!r.ok || entityCount > 256) return null
  for (let i = 0; i < entityCount; i++) {
    const e = { handle: r.u32(), mask: r.varint() }
    if (!r.ok) return null
    if (e.mask & ENT_FIELD.POS) e.pos = r.vec3q()
    if (e.mask & ENT_FIELD.FACING) e.facing = r.u32()
    if (e.mask & ENT_FIELD.ANIMATION_STATE) e.animationState = r.u16()
    if (e.mask & ENT_FIELD.VISUAL_PARAMS) {
      const n = r.varint()
      if (!r.ok || n > 8) return null
      e.visual = []
      for (let v = 0; v < n; v++) e.visual.push({ key: r.u16(), value: r.i32() })
    }
    if (e.mask & ENT_FIELD.ANCHOR) e.anchor = r.u32()
    if (e.mask & ENT_FIELD.CONTROLLER_MODE) e.controllerMode = r.u8()
    if (e.mask & ENT_FIELD.DISPLAY_ID) e.displayId = r.u32()
    if (!r.ok) return null
    snap.entities.push(e)
  }

  const vfxCount = r.u16()
  if (!r.ok || vfxCount > 64) return null
  for (let i = 0; i < vfxCount; i++) {
    snap.vfx.push({ kind: r.u16(), pos: r.vec3q(), dir: r.u32(), param: r.u32() })
  }

  return r.ok ? snap : null
}

export const handleIndex = (h) => h & 0x00ffffff
export const handleGeneration = (h) => (h >>> 24) & 0xff
