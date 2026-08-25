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

const BUF64_LEN = 7   // pos.xyz, vel.xyz, hash
const BUF32_LEN = 5   // facing, state_flags, jumps_used, jump_held, active

export const IDX = {
  POS: 0, VEL: 3, HASH: 6,
  FACING: 0, FLAGS: 1, JUMPS: 2, JUMP_HELD: 3, ACTIVE: 4
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

  createWorld () { return new WasmWorld(this, this.M._dnp_world_create()) }

  /** Quantise an fx-raw BigInt with the server's own function (protocol R1). */
  axisFromFxRaw (raw) {
    this.views.i64[this.argbuf / 8] = raw
    return this.M._dnp_axis_from_buf(this.argbuf)
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
      facing: u32[b32 + IDX.FACING],
      stateFlags: u32[b32 + IDX.FLAGS],
      jumpsUsed: u32[b32 + IDX.JUMPS],
      jumpHeld: u32[b32 + IDX.JUMP_HELD] !== 0,
      active: u32[b32 + IDX.ACTIVE] !== 0
    }
  }

  writeAgent (idx, s) {
    const { i64, u32 } = this.core.views
    const b64 = this.core.buf64 / 8
    const b32 = this.core.buf32 / 4
    i64[b64 + IDX.POS] = s.pos.x; i64[b64 + IDX.POS + 1] = s.pos.y; i64[b64 + IDX.POS + 2] = s.pos.z
    i64[b64 + IDX.VEL] = s.vel.x; i64[b64 + IDX.VEL + 1] = s.vel.y; i64[b64 + IDX.VEL + 2] = s.vel.z
    u32[b32 + IDX.FACING] = s.facing >>> 0
    u32[b32 + IDX.FLAGS] = s.stateFlags >>> 0
    u32[b32 + IDX.JUMPS] = s.jumpsUsed >>> 0
    u32[b32 + IDX.JUMP_HELD] = s.jumpHeld ? 1 : 0
    u32[b32 + IDX.ACTIVE] = s.active === false ? 0 : 1
    this.core.M._dnp_world_write_agent(this.ptr, idx, this.core.buf64, this.core.buf32)
  }
}
