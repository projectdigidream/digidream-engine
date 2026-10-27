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

export const MSG = {
  UI_OPEN: 0x20,
  UI_EVENT: 0x21,
  UI_CLOSE: 0x22,
  UI_SET: 0x23,
  CAMERA_SET: 0x24,
  SIDE_TEXTURE: 0x25,
  MUSIC: 0x26,
  SCREEN_SET: 0x27,
  // A level next door, and where it sits relative to this one. Sent just
  // before that level's own MAP_DATA stream, which is what identifies the
  // stream on a channel that is reliable and ordered.
  NEIGHBOUR_MAP: 0x28,
  NEIGHBOUR_DROP: 0x29,
  // Same level, new offset, no geometry: what a crossing does to most of them.
  NEIGHBOUR_MOVE: 0x2A,
  // The body walked into another level of the same world (docs/protocol.md 8.7).
  HOME: 0x2B,
  // Single-player save and load (docs/SAVING.md).
  GAME_SAVE: 0x2C,
  GAME_LOAD: 0x2D,
  GAME_RESULT: 0x2E,
  GAME_NEW: 0x2F,
  SOUND_PREDICT: 0x33,
  HUD_SET: 0x34,
  // The weapon table this client predicts its own weapon with
  // (docs/WEAPON_PREDICTION.md): the core's encoding, handed to the wasm core
  // as it came.
  WEAPON_DEFS: 0x35,
  // The sky the player's level is drawn under (docs/SKY.md).
  SKY_SET: 0x36,
  // A sector's look changes: floor or ceiling picture, light, an effect.
  SECTOR_SET: 0x37,
  // The module's own keys (docs/KEYS.md): name, keyboard code, touch label.
  KEYS: 0x38,
  // MARKERS (docs/MARKERS.md): a small UI hung on an entity or a place, and
  // its removal by id.
  MARK_SET: 0x39,
  MARK_CLEAR: 0x3A,
  // The module's movement states, as the text of its lines (docs/MOVEMENT.md).
  MOVE_DEFS: 0x3B,
  HELLO: 0x01,
  WELCOME: 0x02,
  REJECT: 0x03,
  MAP_DATA: 0x10,
  SECTOR_MOTION_SET: 0x12,
  PARAM_UPDATE: 0x14,
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

// docs/protocol.md §11. Reliable control messages, never datagrams.
// docs/protocol.md §11.1's camera field table. Named here so the client applies
// by NAME rather than by a number repeated in two files.
export const CAMERA_FIELD = [
  'distance', 'pitch', 'smoothing', 'maxAccel',
  'lead', 'leadSmoothing', 'deadzoneX', 'deadzoneY',
  'yaw', 'lockYaw', 'lockZoom',
  // Camera paths and shake (design §4.1, ids 11..17). IN ID ORDER, and appended
  // only: this array's index is the wire id, mirrored by hand from
  // CAMERA_FIELD_NAMES in server/src/lua/world_binding.cpp with no shared
  // header between them. client/test/proto_camera.test.js compares the two.
  'mode', 'path', 'pathSpeed', 'pathAttach', 'pathBlend',
  'shake', 'shakeTime',
  // 18: the aim point's lead grows with forward speed (camera.js leadVelocity).
  'leadVelocity',
  // 19..22: the automatic camera -- swings behind the player unless they turn
  // it, and walls pull it in (camera.js autoTurn, autoDelay, autoRate, collide).
  'autoTurn', 'autoDelay', 'autoRate', 'collide',
  // 23, 24: where the camera sits and looks, by height over the player
  // (camera.js cameraHeight, aimHeight).
  'cameraHeight', 'aimHeight'
]

// The cap on fields in ONE CameraSet, mirroring proto/messages.hpp. Not the
// number of field ids there can be -- an id is a u8 and an unknown one is
// skipped, which is how a newer server talks to this client. Over-cap is a
// different thing and is refused outright: the count is a length, and a length
// this reader trusted would be a length an attacker picks.
export const CAMERA_MAX_FIELDS = 32

// docs/protocol.md §11.1's screen field table: the curtain and the letterbox
// bars. Same record shape as CAMERA_SET and applied the same way, by NAME.
// `quit` is a request, not a setting: close the app (main.js quitGame).
export const SCREEN_FIELD = ['fade', 'fadeMs', 'letterbox', 'letterboxMs', 'quit']
export const SCREEN_MAX_FIELDS = 16

export const UI_KIND = {
  LABEL: 0, BUTTON: 1, SLIDER: 2, SELECT: 3, TOGGLE: 4
}
export const UI_MAX_TEXT = 128
export const UI_MAX_WIDGETS = 32
export const UI_MAX_OPTIONS = 32
// A panel template's name (docs/UI_LAYOUT.md 5): [a-z0-9_], at most this.
export const UI_MAX_TEMPLATE = 24

export const ENT_FIELD = {
  POS: 1 << 0,
  FACING: 1 << 1,
  ANIMATION_STATE: 1 << 2,
  VISUAL_PARAMS: 1 << 3,
  ANCHOR: 1 << 4,
  CONTROLLER_MODE: 1 << 5,
  DISPLAY_ID: 1 << 6,
  DESTROYED: 1 << 7,
  // Whether other bodies collide with this one -- a corpse stops being solid.
  SOLID: 1 << 8,
  // Which avatar draws this player. Its own field, not display_id's: that one
  // carries the KIND, and kind zero has to keep meaning "a player".
  AVATAR: 1 << 9,
  // Ticks left in another body's swing (u8; 0 not swinging).
  ATTACK: 1 << 10,
  // The position is a jump (a teleport), not a move: drawn there, not slid there.
  TELEPORTED: 1 << 11,
  // Which weapon step that swing is (u8 weapon, phase, step; weapon 255 none):
  // a player's `swing` step's own shape, looked up in the weapon table.
  ATTACK_STEP: 1 << 12,
  // Which particle kind follows it (u16; 0 none): a missile's trail, an aura.
  TRAIL: 1 << 13,
  // What else a player wears and which states it plays instead (decodeLook).
  LOOK: 1 << 14
}

// A PLAYER'S LOOK (messages.hpp proto::Look): { layers: [8 kinds, 0 none],
// remap: Map(from state -> to state) }. u8 mask of the layers present, a u16
// per one, then u8 n and n x (u16 from, u16 to); the first 8 pairs are kept.
export const LOOK_LAYERS = 8
export function decodeLook (r) {
  const present = r.u8()
  const layers = []
  for (let i = 0; i < LOOK_LAYERS; i++) layers.push(present & (1 << i) ? r.u16() : 0)
  const n = r.u8()
  const remap = new Map()
  for (let i = 0; i < n; i++) { const from = r.u16(); const to = r.u16(); if (i < 8) remap.set(from, to) }
  return { layers, remap }
}

// Snapshot header flags (docs/protocol.md §5).
export const SNAP_FLAG = {
  RESYNC_REQUIRED: 1 << 0,
  LUA_APPLIED: 1 << 1
}

export const STATE_FLAG = {
  GROUNDED: 1 << 0,
  CLIMBING: 1 << 1,
  HOVERING: 1 << 2,
  FLYING: 1 << 3,
  GRAPPLING: 1 << 4,
  ANCHORED: 1 << 5,
  // The body holds weapons: the self block carries the weapon machine.
  ARMED: 1 << 6,
  // Climbing on a ledge rather than a wall: hanging.
  HANGING: 1 << 7,
  // Off a ledge by JUMP: its hop, or the pull-up.
  HOPPING: 1 << 8
}

// No weapon asked for, in an input frame's `weapon` (sim::NO_WEAPON).
export const NO_WEAPON = 255

// The tick this client is drawing the others at, in an input frame's one
// `view` byte (lag compensation, docs/WEAPON_PREDICTION.md): its residue mod
// 255 plus one, so 0 can mean "none". messages.hpp view_byte, which the server
// undoes against its own clock.
export const viewByte = (tick) => (tick >>> 0) % 255 + 1

export const PROTOCOL_VERSION = 0
export const TICK_RATE_HZ = 60

/**
 * `proto::MAX_ENTITIES`, and A COPY — named as one, and checked.
 *
 * The decoder needs an upper bound on a length it read off the network, and the
 * authority for that bound is `core/include/dnp/proto/messages.hpp`, which
 * JavaScript cannot include. So it is copied, and `client/test/snapshots.test.js`
 * reads the header and fails if the two stop agreeing — the same trade
 * `editor/src/entities.js` makes for the Doom scale against the Lua that
 * defines it.
 *
 * The number that was here said 768 while calling itself `proto::MAX_ENTITIES`,
 * which was 1152. It had simply gone stale, and nothing could have noticed:
 * a 1200-byte datagram holds about 46 entities, so the cap was never
 * approached. A copy with no check is a copy that is already wrong.
 */
export const MAX_ENTITIES = 8192
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

// Uint8Array and DataView, not node's Buffer.
//
// This file is shared client code and the BROWSER is its primary target, so a
// `node:buffer` import made it unloadable in the one place it matters most --
// which went unnoticed for as long as the only client was a test harness. Node
// keeps working either way: a Buffer IS a Uint8Array, so a decoder handed one
// reads it unchanged.
//
// Little-endian everywhere, stated once here rather than at each call, because
// it is the wire format's choice and not this class's (docs/protocol.md §2).
const LE = true

class Writer {
  constructor (size = 2048) {
    this.buf = new Uint8Array(size)
    this.view = new DataView(this.buf.buffer)
    this.pos = 0
  }

  u8 (v) { this.view.setUint8(this.pos, v & 0xff); this.pos += 1; return this }
  u16 (v) { this.view.setUint16(this.pos, v & 0xffff, LE); this.pos += 2; return this }
  i16 (v) { this.view.setInt16(this.pos, v, LE); this.pos += 2; return this }
  u32 (v) { this.view.setUint32(this.pos, v >>> 0, LE); this.pos += 4; return this }
  i32 (v) { this.view.setInt32(this.pos, v | 0, LE); this.pos += 4; return this }
  u64 (v) { this.view.setBigUint64(this.pos, BigInt(v), LE); this.pos += 8; return this }
  bytes (b) { this.buf.set(b, this.pos); this.pos += b.length; return this }
  done () { return this.buf.subarray(0, this.pos) }
}

class Reader {
  constructor (buf) {
    // Accepts any view of bytes, including a node Buffer -- which is one.
    this.buf = buf instanceof Uint8Array ? buf : new Uint8Array(buf)
    this.view = new DataView(this.buf.buffer, this.buf.byteOffset, this.buf.byteLength)
    this.pos = 0
    this.ok = true
  }

  need (n) {
    if (!this.ok || this.pos + n > this.buf.length) { this.ok = false; return false }
    return true
  }

  u8 () { if (!this.need(1)) return 0; return this.view.getUint8(this.pos++) }
  i8 () { if (!this.need(1)) return 0; return this.view.getInt8(this.pos++) }
  u16 () { if (!this.need(2)) return 0; const v = this.view.getUint16(this.pos, LE); this.pos += 2; return v }
  i16 () { if (!this.need(2)) return 0; const v = this.view.getInt16(this.pos, LE); this.pos += 2; return v }
  u32 () { if (!this.need(4)) return 0; const v = this.view.getUint32(this.pos, LE); this.pos += 4; return v }
  i32 () { if (!this.need(4)) return 0; const v = this.view.getInt32(this.pos, LE); this.pos += 4; return v }
  // Returns null rather than a short view when the buffer does not hold n
  // bytes. A caller that got a short string back would decode the rest of the
  // message from the wrong offset and never know.
  bytes (n) {
    if (!this.need(n)) return null
    const v = new Uint8Array(this.view.buffer, this.view.byteOffset + this.pos, n)
    this.pos += n
    return v
  }
  i64 () { if (!this.need(8)) return 0n; const v = this.view.getBigInt64(this.pos, LE); this.pos += 8; return v }
  u64 () { if (!this.need(8)) return 0n; const v = this.view.getBigUint64(this.pos, LE); this.pos += 8; return v }

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
export function encodeControlMessage (type, body = new Uint8Array(0)) {
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

// A level's name, `u8 length, bytes`, if the message has one left -- it was
// appended, so a message from an older server simply ends before it.
function readLevelName (r) {
  if (!r.ok || r.pos >= r.buf.length) return null
  const n = r.u8()
  const b = r.bytes(n)
  return r.ok && b ? new TextDecoder().decode(b) : null
}

export function decodeControlMessage (buf) {
  const r = new Reader(buf)
  const len = r.u32()
  const type = r.u8()
  if (!r.ok) return null

  // A panel (docs/protocol.md §11.1). Every length in this message is a number
  // the sender chose, so each one is checked against the protocol's cap rather
  // than trusted -- a decoder that believed them would read past the buffer.
  if (type === MSG.UI_OPEN) {
    const readText = () => {
      const n = r.u16()
      if (!r.ok || n > UI_MAX_TEXT) return null
      const b = r.bytes(n)
      return b === null ? null : new TextDecoder().decode(b)
    }
    const uiId = r.u32()
    const title = readText()
    if (title === null) return null
    const count = r.u16()
    if (!r.ok || count > UI_MAX_WIDGETS) return null
    const widgets = []
    for (let i = 0; i < count; i++) {
      const kind = r.u8()
      // An unknown kind cannot be skipped: its fields are not self-describing,
      // so there is no way to step over one and stay aligned.
      if (!r.ok || kind > UI_KIND.TOGGLE) return null
      const id = r.u16()
      const label = readText()
      if (label === null) return null
      const value = r.i32()
      const lo = r.i32()
      const hi = r.i32()
      const optCount = r.u16()
      if (!r.ok || optCount > UI_MAX_OPTIONS) return null
      const options = []
      for (let k = 0; k < optCount; k++) {
        const o = readText()
        if (o === null) return null
        options.push(o)
      }
      widgets.push({ kind, id, label, value, lo, hi, options })
    }
    if (!r.ok) return null
    // The TEMPLATE, trailing and optional: present when the message goes on.
    // Anything there that is not a whole valid name is a message cut short or
    // corrupted, and refused like any other.
    let template = null
    let flags = 0
    const end = Math.min(r.buf.length, 4 + len)
    if (r.pos < end) {
      const tn = r.u16()
      if (!r.ok || tn > UI_MAX_TEMPLATE) return null
      if (tn > 0) {
        const b = r.bytes(tn)
        if (b === null) return null
        template = new TextDecoder().decode(b)
        if (!/^[a-z0-9_]+$/.test(template)) return null
      }
      // Flags after it (UI_FLAG_PINNED = 1: no ×). An empty template with
      // nothing after it is a message cut short.
      if (r.pos < end) flags = r.u8()
      else if (tn === 0) return null
      if (!r.ok) return null
    }
    return { type, uiId, title, widgets, template, flags }
  }

  if (type === MSG.UI_CLOSE) {
    const uiId = r.u32()
    return r.ok ? { type, uiId } : null
  }

  // Camera settings for this client (docs/protocol.md §11.1). Values arrive in
  // thousandths of their unit; converted here so nothing downstream has to
  // remember the scale.
  // The curtain and the bars (design §4.1). Thousandths, like the camera's.
  if (type === MSG.SCREEN_SET) {
    const count = r.u16()
    if (!r.ok || count > SCREEN_MAX_FIELDS) return null
    const fields = []
    for (let i = 0; i < count; i++) {
      const id = r.u8()
      const milli = r.i32()
      fields.push({ id, value: milli / 1000 })
    }
    return r.ok ? { type, fields } : null
  }

  // The sky (docs/SKY.md): a kind, its pictures by texture name, and
  // {id, milli} fields as CAMERA_SET's. An unknown kind refuses the message.
  // A sector's look (core encode_sector_set): what 0 floor, 1 ceiling (a
  // texture name), 2 light (u8, fade ms), 3 an effect (kind, on, settings).
  if (type === MSG.KEYS) {
    const n = r.u8()
    if (!r.ok || n > 8) return null
    const text = () => {
      const len = r.u16()
      if (!r.ok || len > 64) return null
      const b = r.bytes(len)
      return b === null ? null : new TextDecoder().decode(b)
    }
    const keys = []
    for (let i = 0; i < n; i++) {
      const name = text(); const code = text(); const label = text()
      if (name === null || code === null || label === null) return null
      keys.push({ name, code, label })
    }
    return r.ok ? { type, keys } : null
  }

  if (type === MSG.SECTOR_SET) {
    const sector = r.u32()
    const what = r.u8()
    if (!r.ok || what > 3) return null
    const m = { type, sector, what: ['floor', 'ceiling', 'light', 'effect'][what] }
    if (what <= 1) {
      const n = r.u16()
      if (!r.ok || n > UI_MAX_TEXT) return null
      const b = r.bytes(n)
      if (b === null) return null
      m.texture = new TextDecoder().decode(b)
    } else if (what === 2) {
      m.light = r.u8()
      m.fadeMs = r.u16()
    } else {
      m.kind = r.u8()
      m.on = r.u8() !== 0
      const n = r.u8()
      if (!r.ok || m.kind < 1 || m.kind > 5 || n > 32) return null
      m.params = []
      for (let i = 0; i < n; i++) m.params.push({ key: r.u8(), raw: r.i64() })
    }
    return r.ok ? m : null
  }

  if (type === MSG.SKY_SET) {
    const kind = r.u8()
    const n = r.u8()
    if (!r.ok || kind > 3 || n > 6) return null
    const images = []
    for (let i = 0; i < n; i++) {
      const len = r.u16()
      if (!r.ok || len > UI_MAX_TEXT) return null
      const b = r.bytes(len)
      if (b === null) return null
      images.push(new TextDecoder().decode(b))
    }
    const count = r.u16()
    if (!r.ok || count > 8) return null
    const fields = []
    for (let i = 0; i < count; i++) fields.push({ id: r.u8(), milli: r.i32() })
    return r.ok ? { type, kind, images, fields } : null
  }

  if (type === MSG.CAMERA_SET) {
    const count = r.u16()
    if (!r.ok || count > CAMERA_MAX_FIELDS) return null
    const fields = []
    for (let i = 0; i < count; i++) {
      const id = r.u8()
      const milli = r.i32()
      fields.push({ id, value: milli / 1000 })
    }
    return r.ok ? { type, fields } : null
  }

  // A side's picture changes -- a switch thrown (docs/protocol.md §11.1).
  if (type === MSG.SIDE_TEXTURE) {
    // The side's NAME: chunk key in the high half, the index within its own
    // map in the low (geom::MapStorage::side_name). For a one-map world the
    // name is the index.
    const side = r.u32()
    const field = r.u8()
    if (!r.ok || field > 2) return null
    const n = r.u16()
    if (!r.ok || n > UI_MAX_TEXT) return null
    const b = r.bytes(n)
    if (b === null) return null
    return { type, side, field, name: new TextDecoder().decode(b) }
  }

  // The map's music changes cue (docs/protocol.md §11.1). A name: the client
  // owns the manifest that maps it to a track.
  if (type === MSG.MUSIC) {
    const n = r.u16()
    if (!r.ok || n > UI_MAX_TEXT) return null
    const b = r.bytes(n)
    if (b === null) return null
    const fadeMs = r.u16()
    const at = r.u8()
    if (!r.ok || at > 2) return null
    return { type, cue: new TextDecoder().decode(b), fadeMs, at: ['nextBar', 'loopEnd', 'immediate'][at] }
  }

  // One widget's value, in a panel already on screen (docs/protocol.md §11.1).
  if (type === MSG.UI_SET) {
    const uiId = r.u32()
    const widgetId = r.u16()
    const value = r.i32()
    if (!r.ok) return null
    // WHAT is set, appended (docs/protocol.md 11.1): absent or 0 the value, 1
    // whether widget `widgetId` is shown (value 0 or 1), 2 whether the decor
    // element named after it is shown.
    const m = { type, uiId, widgetId, value, what: 'value' }
    if (r.pos < r.buf.length) {
      const what = r.u8()
      if (!r.ok || what > 2) return null
      m.what = ['value', 'shown', 'decor'][what]
      if (what === 2) {
        const n = r.u8()
        if (!r.ok || n === 0 || n > 32) return null
        const b = r.bytes(n)
        if (b === null) return null
        m.decor = new TextDecoder().decode(b)
      }
    }
    return m
  }

  if (type === MSG.WELCOME) {
    const msg = {
      type,
      version: r.u16(),
      serverId: r.u64(),
      tickRate: r.u16(),
      snapshotRate: r.u16(),
      serverTick: r.u32(),
      selfHandle: r.u32(),
      mapHash: r.u64(),
      // Which level this is, and whether its map follows. False means the client
      // already holds it -- it was streamed as a neighbour, or this is the level
      // just left and never dropped -- and must not wait for it
      // (docs/protocol.md §8.5).
      homeLevel: r.u16(),
      mapFollows: r.u8() !== 0,
      // The body kept its place across a seam (true) or was put at a spawn.
      exactArrival: r.u8() !== 0,
      // Where the server put the body, raw fx, in the world's coordinates.
      arrival: { x: r.i64(), y: r.i64(), z: r.i64() },
      // Where this level's own coordinates begin in the world's, raw fx. Zero
      // for a level simulated on its own; its cell's origin for a level in a
      // region. The client draws and simulates everything in world coordinates.
      origin: { x: r.i64(), y: r.i64() },
      // Whether this level shares its world with its neighbours (a region):
      // then crossing into one is a HOME message, not another WELCOME.
      region: r.u8() !== 0
    }
    // The level's name, appended (docs/protocol.md §3.2): what its music is
    // found by. Absent from an older server, which is not an error.
    msg.levelName = readLevelName(r)
    // The module's control scheme, appended after the name: 0 platformer, 1
    // fps (client/src/controls.js). Absent from an older server: a platformer.
    msg.controls = r.ok && r.pos < r.buf.length ? r.u8() : 0
    // Whether to show the debug overlay (module.txt `debug off`), after it.
    // Absent from an older server: shown, as it always was.
    msg.debug = r.ok && r.pos < r.buf.length ? r.u8() : 1
    // The time scale, in thousandths, after it: how fast the game runs in wall
    // time (the server's --time-scale). Absent from an older server: 1.
    msg.timeScale = r.ok && r.pos + 2 <= r.buf.length ? r.u16() / 1000 : 1
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
      to: r.vec3raw(),
      // The turn. `spinFrom`/`spinTo` are SIGNED and UNWRAPPED brad in 64 bits,
      // because a u32 can say neither "twice round" nor "the other way", and
      // both are things a rotating platform does.
      pivot: { x: r.i64(), y: r.i64() },
      spinFrom: r.i64(),
      spinTo: r.i64()
    }
    return r.ok ? msg : null
  }
  if (type === MSG.PARAM_UPDATE) {
    // docs/protocol.md §8.4. These are the RULES the client predicts with, not
    // state that reconciliation can correct toward, which is why they ride the
    // reliable channel.
    const msg = {
      type,
      handle: r.u32(),
      epoch: r.u8(),
      effectiveTick: r.u32(),
      flags: r.u8(),
      fields: []
    }
    const count = r.u16()
    if (!r.ok || count > 64) return null   // bounded before it is used as a loop count
    for (let i = 0; i < count; i++) {
      const id = r.u16()
      const raw = r.i64()
      msg.fields.push({ id, raw })
    }
    // Nothing is returned unless every byte was good: a parameter set that is
    // half old and half new is worse than one that is entirely stale, because
    // the stale one is at least self-consistent and the epoch names it.
    return r.ok ? msg : null
  }
  if (type === MSG.NEIGHBOUR_MAP) {
    // The offset is fx raw, not thousandths: a cell origin is an exact multiple
    // of the pitch and the seam depends on the two sides agreeing to the bit.
    // Rounding it for the renderer's convenience would put a visible crack
    // along every boundary.
    const level = r.u16(), dxRaw = r.i64(), dyRaw = r.i64()
    // BOTH FORMS. The number positions the mesh; the raw fx repositions the
    // BODY when this level is the one being entered, and a seam that rounded
    // that would put the player a fraction of a unit from where the server
    // says they are -- on every crossing, as a misprediction.
    const msg = { type, level, dxRaw, dyRaw,
                  dx: fxToNumber(dxRaw), dy: fxToNumber(dyRaw) }
    return r.ok ? msg : null
  }
  if (type === MSG.NEIGHBOUR_MOVE) {
    const level = r.u16(), dxRaw = r.i64(), dyRaw = r.i64()
    const msg = { type, level, dxRaw, dyRaw,
                  dx: fxToNumber(dxRaw), dy: fxToNumber(dyRaw) }
    return r.ok ? msg : null
  }
  if (type === MSG.GAME_RESULT) {
    const op = r.u8(), slot = r.u8(), ok = r.u8() !== 0
    const n = r.u16()
    const b = r.bytes(n)
    if (!r.ok || b === null) return null
    return { type, op: ['save', 'load', 'new'][op] ?? 'unknown', slot, ok, text: new TextDecoder().decode(b) }
  }
  // The movement states' text, for the wasm core to read with the server's own
  // parser (docs/MOVEMENT.md); no second reader of it here either.
  if (type === MSG.MOVE_DEFS) {
    if (len < 1 || buf.length < 4 + len) return null
    return { type, text: new TextDecoder().decode(buf.subarray(5, 4 + len)) }
  }
  // The weapon table, as bytes: the wasm core decodes it with the server's
  // own decoder, so there is no second reader of it here.
  if (type === MSG.WEAPON_DEFS) {
    if (len < 1 || buf.length < 4 + len) return null
    // A copy: the caller's receive buffer is reused for the next message.
    return { type, bytes: new Uint8Array(buf.subarray(5, 4 + len)) }
  }
  // HUD values (docs/UI_LAYOUT.md 4): {name, kind, value}; kind 0 i32, 1 str,
  // 2 clear. A kind this reader does not know refuses the message, as MUSIC's
  // reader refuses an unknown `at`.
  if (type === MSG.HUD_SET) {
    const n = r.u16()
    if (!r.ok || n > HUD_MAX_FIELDS) return null
    const fields = []
    const text = (max) => {
      const len = r.u16()
      if (!r.ok || len > max) return null
      const b = r.bytes(len)
      return b ? new TextDecoder().decode(b) : null
    }
    for (let i = 0; i < n; i++) {
      const name = text(HUD_NAME_MAX)
      if (name === null) return null
      const kind = r.u8()
      if (kind === 0) fields.push({ name, kind, value: r.i32() })
      else if (kind === 1) {
        const v = text(HUD_STR_MAX)
        if (v === null) return null
        fields.push({ name, kind, value: v })
      } else if (kind === 2) fields.push({ name, kind, value: undefined })
      else return null
    }
    return r.ok ? { type, fields } : null
  }
  // A MARKER (docs/MARKERS.md): what it hangs on, its template, and the values
  // the template binds. Refused whole on what it does not know, as the core's
  // decoder is: an anchor, a value kind, a name or a template it cannot read.
  if (type === MSG.MARK_SET) {
    const id = r.u32()
    const anchor = r.u8()
    if (!r.ok || anchor > 1) return null
    const m = { type, id, anchor, handle: 0, pos: null }
    if (anchor === 1) m.pos = r.vec3raw()
    else m.handle = r.u32()
    const text = (max, re) => {
      const len = r.u16()
      if (!r.ok || len > max) return null
      const b = r.bytes(len)
      if (!b) return null
      const v = new TextDecoder().decode(b)
      return re && !re.test(v) ? null : v
    }
    m.template = text(UI_MAX_TEMPLATE, /^[a-z0-9_]+$/)
    if (m.template === null) return null
    m.flags = r.u8()
    m.height = r.fxraw()
    m.range = r.fxraw()
    const n = r.u8()
    if (!r.ok || n > MARK_MAX_VALUES) return null
    m.values = {}
    for (let i = 0; i < n; i++) {
      const name = text(HUD_NAME_MAX, /^[a-z0-9_]+$/)
      if (name === null) return null
      const kind = r.u8()
      if (kind === 0) m.values[name] = r.i32()
      else if (kind === 1) {
        const v = text(HUD_STR_MAX)
        if (v === null) return null
        m.values[name] = v
      } else return null
    }
    return r.ok ? m : null
  }
  if (type === MSG.MARK_CLEAR) {
    const id = r.u32()
    return r.ok ? { type, id } : null
  }
  // What to play the moment this client predicts its own action (§5.7).
  if (type === MSG.SOUND_PREDICT) {
    const group = r.u32()
    const n = r.u8()
    if (!r.ok || n > 16) return null
    const entries = []
    for (let i = 0; i < n; i++) {
      entries.push({ trigger: r.u8(), sound: r.u32(), param: r.u16(), shots: r.i16() })
    }
    return r.ok ? { type, group, entries } : null
  }
  if (type === MSG.HOME) {
    const msg = { type, level: r.u16() }
    msg.levelName = readLevelName(r)
    return r.ok ? msg : null
  }
  if (type === MSG.NEIGHBOUR_DROP) {
    const msg = { type, level: r.u16() }
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

/** What the player did with a panel (docs/protocol.md §11.1). */
/** Ask the server to save the game to `slot` (or load it from there). */
export function encodeGameSave (slot) {
  const w = new Writer(8)
  w.u32(1 + 1).u8(MSG.GAME_SAVE).u8(slot & 0xFF)
  return w.done()
}
/** Ask for a new game: the world from scratch (docs/SAVING.md). */
export function encodeGameNew () {
  const w = new Writer(8)
  w.u32(1).u8(MSG.GAME_NEW)
  return w.done()
}
export function encodeGameLoad (slot) {
  const w = new Writer(8)
  w.u32(1 + 1).u8(MSG.GAME_LOAD).u8(slot & 0xFF)
  return w.done()
}

export function encodeUiEvent (uiId, widgetId, value) {
  const w = new Writer(16)
  w.u32(1 + 10).u8(MSG.UI_EVENT).u32(uiId).u16(widgetId).i32(value | 0)
  return w.done()
}

export function encodeInputBundle ({ clientTick, ackSnapshotTick = 0, frames }) {
  const n = Math.min(frames.length, 16)
  // 15 bytes a frame (the last is the module's own keys, docs/KEYS.md).
  const w = new Writer(16 + n * 15)
  w.u8(MSG.INPUT_BUNDLE).u32(clientTick).u32(ackSnapshotTick).u8(n)
  for (let i = 0; i < n; i++) {
    const f = frames[i]
    w.u16((f.buttons ?? 0) & ~0xf800)   // reserved bits stripped, as in C++
    w.i16(f.moveX ?? 0)
    w.i16(f.moveY ?? 0)
    w.u32(f.aim ?? 0)
    // Vertical aim, 1/65536 of a turn (docs/protocol.md 4).
    w.i16(f.pitch ?? 0)
    // The weapon wanted this tick, NO_WEAPON for none (docs/WEAPON_PREDICTION.md).
    w.u8(f.weapon ?? NO_WEAPON)
    // The tick it was drawing the others at, 0 for none.
    w.u8(f.view ?? 0)
    // The module's own keys held this tick (docs/KEYS.md): bit i, action i.
    w.u8(f.actions ?? 0)
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
    vfx: [],
    sounds: []
  }

  snap.self.pos = r.vec3raw()
  snap.self.vel = r.vec3raw()
  snap.self.facing = r.u32()
  snap.self.controllerMode = r.u8()
  snap.self.stateFlags = r.u16()
  snap.self.anchor = r.u32()
  snap.self.jumpsUsed = r.u8()
  // IN THE ENCODER'S ORDER (core/src/proto/messages.cpp encode_self): the two
  // swing counters sit between the jumps and the dashes. This read them as
  // dashes, swing, cooldown -- so reconciliation adopted the swing countdown
  // as the dash count and the dash count as the cooldown, which only ever
  // agreed when all three were zero.
  snap.self.attackLeft = r.u8()
  snap.self.attackCooldownLeft = r.u8()
  snap.self.dashesUsed = r.u8()
  snap.self.sector = r.i32()
  snap.self.dashTimer = r.fxraw()
  snap.self.dashCooldown = r.fxraw()
  snap.self.hoverFuel = r.fxraw()
  snap.self.flightFuel = r.fxraw()
  snap.self.coyote = r.fxraw()
  // The movement state and its ticks in it (docs/MOVEMENT.md).
  snap.self.moveState = r.u8()
  snap.self.stateTicks = r.u16()
  // The meters, up to the last that is not 0; the rest are 0. Raw fx, as BigInt.
  snap.self.meters = Array(8).fill(0n)
  for (let k = 0, n = r.u8(); k < n; k++) { const v = r.fxraw(); if (k < 8) snap.self.meters[k] = v }
  if (snap.self.stateFlags & STATE_FLAG.GRAPPLING) {
    snap.self.grappleAnchor = r.vec3raw()
    snap.self.grappleDist = r.fxraw()
  }
  // The weapon machine, when the body holds weapons (docs/WEAPON_PREDICTION.md).
  if (snap.self.stateFlags & STATE_FLAG.ARMED) {
    const k = {}
    k.have = r.u32()
    k.current = r.u8(); k.phase = r.u8(); k.armed = r.u8(); k.swingPhase = r.u8()
    k.prevButtons = r.u8()
    k.entered = r.u32(); k.done = r.u32()
    k.heldSince = [r.u32(), r.u32()]
    k.vars = [r.i32(), r.i32(), r.i32(), r.i32()]
    k.ammo = []
    for (let i = 0; i < 8; i++) k.ammo.push(r.u16())
    // Cooldown deadlines, up to the last one set (MAX_WEAPON_COOLDOWNS = 8).
    k.cool = [0, 0, 0, 0, 0, 0, 0, 0]
    const nCool = r.u8()
    for (let i = 0; i < nCool; i++) { const v = r.i32(); if (i < 8) k.cool[i] = v }
    // The step whose swing is its latest (weapon 255: a swing no step started).
    k.swungWeapon = r.u8(); k.swungPhase = r.u8(); k.swungStep = r.u8()
    snap.self.weapon = k
  }
  // Which generation of the movement parameters the server computed this state
  // with (docs/protocol.md 8.4). Compared against the client's applied epoch to
  // tell "I am one parameter generation behind" from "the two builds disagree"
  // -- see 10.1.2. Those look identical in the hash and have opposite fixes.
  // The observer's own animation state (design §4.1). The local agent never
  // appears in its own entity stream, so this is the only way a script's choice
  // of animation for the player reaches the player's own screen.
  snap.self.avatar = r.u16()
  snap.self.look = decodeLook(r)
  snap.self.animationState = r.u16()
  snap.self.paramEpoch = r.u8()
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
  // proto::MAX_ENTITIES. A cap rather than a trusted length: this is the size
  // the decoder will loop to on data from the network.
  //
  // IT SAID 768 AND CALLED ITSELF proto::MAX_ENTITIES, which was 1152 -- a
  // hand-copied constant that had gone stale, and the third copy of the
  // "two programs, one number" failure this repository keeps paying for. It was
  // harmless only because a 1200-byte datagram holds about 46 entities, so the
  // cap was never approached; it would have become a truncated snapshot the
  // moment anything raised the budget.
  if (!r.ok || entityCount > MAX_ENTITIES) return null
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
    if (e.mask & ENT_FIELD.AVATAR) e.avatar = r.u16()
    if (e.mask & ENT_FIELD.SOLID) e.solid = r.u8()
    if (e.mask & ENT_FIELD.ATTACK) e.attackLeft = r.u8()
    if (e.mask & ENT_FIELD.ATTACK_STEP) {
      e.attackWeapon = r.u8(); e.attackPhase = r.u8(); e.attackStep = r.u8()
    }
    if (e.mask & ENT_FIELD.TRAIL) e.trail = r.u16()
    if (e.mask & ENT_FIELD.LOOK) e.look = decodeLook(r)
    if (!r.ok) return null
    snap.entities.push(e)
  }

  const vfxCount = r.u16()
  if (!r.ok || vfxCount > 64) return null
  for (let i = 0; i < vfxCount; i++) {
    snap.vfx.push({ kind: r.u16(), pos: r.vec3q(), dir: r.u32(), param: r.u32() })
  }

  // Sounds, only when the snapshot has any (docs/protocol.md §5.7). `sound` is
  // the FNV-1a hash of the sound's name -- see soundHash.
  if (r.ok && r.pos < r.buf.length) {
    const n = r.u16()
    if (!r.ok || n > MAX_SOUNDS) return null
    for (let i = 0; i < n; i++) {
      snap.sounds.push({ sound: r.u32(), pos: r.vec3q(), source: r.u32(), flags: r.u8() })
    }
  }

  return r.ok ? snap : null
}

export const MAX_SOUNDS = 32
// HUD_SET's limits, as core/include/dnp/proto/messages.hpp sets them.
export const HUD_MAX_FIELDS = 32
export const HUD_NAME_MAX = 24
export const HUD_STR_MAX = 1024
export const MARK_MAX_VALUES = 16
// MARK_SET flags (docs/MARKERS.md).
export const MARK_FLAG = { DEBUG: 1, THROUGH: 2 }
// SOUND_PREDICT triggers, as core/include/dnp/proto/messages.hpp numbers them.
export const PREDICT = {
  JUMP: 1, LAND: 2, PRIMARY: 3, PRIMARY_HELD: 4, SECONDARY: 5, SECONDARY_HELD: 6, SWING: 7
}
export const SOUND_FOLLOW = 1      // moves with its source
export const SOUND_PREDICTED = 2   // its source's own client already played it

// The name hash the server puts on the wire: 32-bit FNV-1a over the UTF-8
// bytes, exactly proto::sound_hash in core/include/dnp/proto/messages.hpp.
export function soundHash (name) {
  let h = 2166136261
  for (const b of new TextEncoder().encode(name)) {
    h ^= b
    h = Math.imul(h, 16777619) >>> 0
  }
  return h >>> 0
}

export const handleIndex = (h) => h & 0x00ffffff
export const handleGeneration = (h) => (h >>> 24) & 0xff
