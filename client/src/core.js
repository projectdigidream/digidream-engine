//
// JS wrapper around the wasm simulation core.
//
// Its whole job is to move state across the boundary WITHOUT losing precision.
// Positions and velocities are raw Q32.32 i64 values, read and written as
// BigInt through a BigInt64Array view. Anything that turned them into a double
// on the way past would break exact-match reconciliation (docs/protocol.md R3)
// at the one place it cannot tolerate being broken -- and it would look like a
// physics bug, not a marshalling bug.
//
import createModule from '../wasm/dist/dnp_core.mjs'

// Must mirror client/wasm/bindings.cpp exactly. The layout carries every field
// the controller keeps between ticks, because reconciliation overwrites local
// state and replays from it -- anything missing here survives the overwrite and
// diverges silently (docs/protocol.md 5.1).
const BUF64_LEN = 19
const BUF32_LEN = 14

export const IDX = {
  POS: 0, VEL: 3, HASH: 6, DASH_T: 7, DASH_CD: 8, HOVER_F: 9, FLIGHT_F: 10,
  GRAP_ANCH: 11, GRAP_DIST: 14, DASH_DIR: 15, CLIMB_N: 17,

  FACING: 0, FLAGS: 1, JUMPS: 2, JUMP_HELD: 3, ACTIVE: 4, MODE: 5, DASHES: 6,
  SECTOR: 7, DASH_HELD: 8, GRAP_HELD: 9, CLIMBING: 10, GRAPPLING: 11,
  GRAP_PULL: 12, GROUNDED: 13
}

export class Core {
  static async load () {
    const M = await createModule()
    return new Core(M)
  }

  constructor (M) {
    this.M = M
    this.buf64 = M._malloc(BUF64_LEN * 8)
    this.buf32 = M._malloc(BUF32_LEN * 4)
    this.argbuf = M._malloc(8)
    this._buffer = null
    this._views = null
  }

  // Heap views are invalidated whenever wasm memory grows -- the underlying
  // ArrayBuffer is REPLACED, and a cached view silently points at detached
  // memory. Rebuilding on buffer identity change is cheap and removes an entire
  // class of heisenbug that only appears once allocation crosses a page.
  get views () {
    const buffer = this.M.HEAPU8.buffer
    if (this._buffer !== buffer) {
      this._buffer = buffer
      this._views = {
        i64: new BigInt64Array(buffer),
        u32: new Uint32Array(buffer)
      }
    }
    return this._views
  }

  createWorld ({ map = null } = {}) {
    const w = new WasmWorld(this, this.M._dnp_world_create())
    if (map) {
      this.M._dnp_world_set_map(w.ptr, map.ptr)
      w.map = map
    } else {
      // Fallback for tests. The real path is a map parsed from the bytes the
      // server sent, so the two sides agree because they parsed the same file.
      this.M._dnp_world_use_test_map(w.ptr)
    }
    return w
  }

  /** Parse a baked .dnpm buffer. Returns { ok, status, map } . */
  loadMap (bytes) {
    const ptr = this.M._malloc(bytes.length)
    this.M.HEAPU8.set(bytes, ptr)
    const store = this.M._dnp_map_create()
    const status = this.M._dnp_map_load(store, ptr, bytes.length)
    this.M._free(ptr)
    if (status !== 0) {
      this.M._dnp_map_destroy(store)
      return { ok: false, status, map: null }
    }
    return { ok: true, status: 0, map: new WasmMap(this, store) }
  }

  /** Quantise an fx-raw BigInt with the server's own function (protocol R1). */
  axisFromFxRaw (raw) {
    this.views.i64[this.argbuf / 8] = raw
    return this.M._dnp_axis_from_buf(this.argbuf)
  }
}

export class WasmMap {
  constructor (core, ptr) {
    this.core = core
    this.ptr = ptr
  }

  destroy () { this.core.M._dnp_map_destroy(this.ptr); this.ptr = 0 }
  get sectorCount () { return this.core.M._dnp_map_sector_count(this.ptr) }

  /** Content hash, as BigInt. Must equal WELCOME.map_hash. */
  get hash () {
    this.core.M._dnp_map_hash(this.ptr, this.core.buf64)
    return BigInt.asUintN(64, this.core.views.i64[this.core.buf64 / 8])
  }

  spawnForTag (tag) {
    const bytes = new TextEncoder().encode(tag)
    const ptr = this.core.M._malloc(Math.max(1, bytes.length))
    this.core.M.HEAPU8.set(bytes, ptr)
    this.core.M._dnp_map_spawn_for_tag(this.ptr, ptr, bytes.length, this.core.buf64)
    this.core.M._free(ptr)
    const i64 = this.core.views.i64
    const b = this.core.buf64 / 8
    return { x: i64[b], y: i64[b + 1], z: i64[b + 2] }
  }
}

export class WasmWorld {
  constructor (core, ptr) {
    this.core = core
    this.ptr = ptr
  }

  destroy () { this.core.M._dnp_world_destroy(this.ptr); this.ptr = 0 }
  reset () { this.core.M._dnp_world_reset(this.ptr) }
  get tick () { return this.core.M._dnp_world_tick(this.ptr) }

  spawn ({ x = 0n, y = 0n, z = 0n }) {
    const { i64 } = this.core.views
    const base = this.core.buf64 / 8
    i64[base] = x; i64[base + 1] = y; i64[base + 2] = z
    return this.core.M._dnp_world_spawn(this.ptr, this.core.buf64)
  }

  setInput (idx, { buttons = 0, moveX = 0, moveY = 0, aim = 0 }) {
    this.core.M._dnp_world_set_input(this.ptr, idx, buttons, moveX, moveY, aim)
  }

  step () { this.core.M._dnp_world_step(this.ptr) }

  readAgent (idx) {
    this.core.M._dnp_world_read_agent(this.ptr, idx, this.core.buf64, this.core.buf32)
    const { i64, u32 } = this.core.views
    const b64 = this.core.buf64 / 8
    const b32 = this.core.buf32 / 4
    return {
      pos: { x: i64[b64 + IDX.POS], y: i64[b64 + IDX.POS + 1], z: i64[b64 + IDX.POS + 2] },
      vel: { x: i64[b64 + IDX.VEL], y: i64[b64 + IDX.VEL + 1], z: i64[b64 + IDX.VEL + 2] },
      // agent_hash is a u64; it crosses as i64 and is reinterpreted here.
      hash: BigInt.asUintN(64, i64[b64 + IDX.HASH]),
      dashTimer: i64[b64 + IDX.DASH_T],
      dashCooldown: i64[b64 + IDX.DASH_CD],
      hoverFuel: i64[b64 + IDX.HOVER_F],
      flightFuel: i64[b64 + IDX.FLIGHT_F],
      grappleAnchor: {
        x: i64[b64 + IDX.GRAP_ANCH], y: i64[b64 + IDX.GRAP_ANCH + 1], z: i64[b64 + IDX.GRAP_ANCH + 2]
      },
      grappleDist: i64[b64 + IDX.GRAP_DIST],
      dashDir: { x: i64[b64 + IDX.DASH_DIR], y: i64[b64 + IDX.DASH_DIR + 1] },
      climbNormal: { x: i64[b64 + IDX.CLIMB_N], y: i64[b64 + IDX.CLIMB_N + 1] },
      facing: u32[b32 + IDX.FACING],
      stateFlags: u32[b32 + IDX.FLAGS],
      jumpsUsed: u32[b32 + IDX.JUMPS],
      jumpHeld: u32[b32 + IDX.JUMP_HELD] !== 0,
      active: u32[b32 + IDX.ACTIVE] !== 0,
      mode: u32[b32 + IDX.MODE],
      dashesUsed: u32[b32 + IDX.DASHES],
      sector: u32[b32 + IDX.SECTOR] | 0,
      dashHeld: u32[b32 + IDX.DASH_HELD] !== 0,
      grappleHeld: u32[b32 + IDX.GRAP_HELD] !== 0,
      climbing: u32[b32 + IDX.CLIMBING] !== 0,
      grappling: u32[b32 + IDX.GRAPPLING] !== 0,
      grapplePulling: u32[b32 + IDX.GRAP_PULL] !== 0,
      grounded: u32[b32 + IDX.GROUNDED] !== 0
    }
  }

  writeAgent (idx, s) {
    const { i64, u32 } = this.core.views
    const b64 = this.core.buf64 / 8
    const b32 = this.core.buf32 / 4
    const z = 0n
    i64[b64 + IDX.POS] = s.pos.x; i64[b64 + IDX.POS + 1] = s.pos.y; i64[b64 + IDX.POS + 2] = s.pos.z
    i64[b64 + IDX.VEL] = s.vel.x; i64[b64 + IDX.VEL + 1] = s.vel.y; i64[b64 + IDX.VEL + 2] = s.vel.z
    i64[b64 + IDX.DASH_T] = s.dashTimer ?? z
    i64[b64 + IDX.DASH_CD] = s.dashCooldown ?? z
    i64[b64 + IDX.HOVER_F] = s.hoverFuel ?? z
    i64[b64 + IDX.FLIGHT_F] = s.flightFuel ?? z
    i64[b64 + IDX.GRAP_ANCH] = s.grappleAnchor?.x ?? z
    i64[b64 + IDX.GRAP_ANCH + 1] = s.grappleAnchor?.y ?? z
    i64[b64 + IDX.GRAP_ANCH + 2] = s.grappleAnchor?.z ?? z
    i64[b64 + IDX.GRAP_DIST] = s.grappleDist ?? z
    i64[b64 + IDX.DASH_DIR] = s.dashDir?.x ?? z
    i64[b64 + IDX.DASH_DIR + 1] = s.dashDir?.y ?? z
    i64[b64 + IDX.CLIMB_N] = s.climbNormal?.x ?? z
    i64[b64 + IDX.CLIMB_N + 1] = s.climbNormal?.y ?? z

    u32[b32 + IDX.FACING] = s.facing >>> 0
    u32[b32 + IDX.FLAGS] = (s.stateFlags ?? 0) >>> 0
    u32[b32 + IDX.JUMPS] = (s.jumpsUsed ?? 0) >>> 0
    u32[b32 + IDX.JUMP_HELD] = s.jumpHeld ? 1 : 0
    u32[b32 + IDX.ACTIVE] = s.active === false ? 0 : 1
    u32[b32 + IDX.MODE] = (s.mode ?? 0) >>> 0
    u32[b32 + IDX.DASHES] = (s.dashesUsed ?? 0) >>> 0
    u32[b32 + IDX.SECTOR] = (s.sector ?? -1) >>> 0
    u32[b32 + IDX.DASH_HELD] = s.dashHeld ? 1 : 0
    u32[b32 + IDX.GRAP_HELD] = s.grappleHeld ? 1 : 0
    u32[b32 + IDX.CLIMBING] = s.climbing ? 1 : 0
    u32[b32 + IDX.GRAPPLING] = s.grappling ? 1 : 0
    u32[b32 + IDX.GRAP_PULL] = s.grapplePulling ? 1 : 0
    u32[b32 + IDX.GROUNDED] = s.grounded ? 1 : 0
    this.core.M._dnp_world_write_agent(this.ptr, idx, this.core.buf64, this.core.buf32)
  }
}
