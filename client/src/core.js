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
const BUF32_LEN = 19

export const IDX = {
  POS: 0, VEL: 3, HASH: 6, DASH_T: 7, DASH_CD: 8, HOVER_F: 9, FLIGHT_F: 10,
  GRAP_ANCH: 11, GRAP_DIST: 14, DASH_DIR: 15, CLIMB_N: 17,

  FACING: 0, FLAGS: 1, JUMPS: 2, JUMP_HELD: 3, ACTIVE: 4, MODE: 5, DASHES: 6,
  SECTOR: 7, DASH_HELD: 8, GRAP_HELD: 9, CLIMBING: 10, GRAPPLING: 11,
  GRAP_PULL: 12, GROUNDED: 13, ANCHOR: 14, ANCHORED: 15, INTR_HELD: 16, ATK_HELD: 17, ATK_LEFT: 18
}

export class Core {
  /**
   * The engine's interact reach, in world units.
   *
   * From the engine, not a copy: the debug arrow is drawn this long, and an
   * arrow that disagreed with the reach would show you standing in range of a
   * door that will not open.
   */
  /**
   * How many movement parameters the engine has.
   *
   * From the engine, so a test can say "the full set" rather than a literal --
   * which was correct until MoveParams grew five fields and then silently
   * described a different claim.
   */
  get paramCount () { return this.M._dnp_param_count() }

  get interactReach () {
    this.M._dnp_interact_reach(this.buf64)
    return Number(this.views.i64[this.buf64 / 8]) / 4294967296
  }

  static async load () {
    const M = await createModule()
    return new Core(M)
  }

  constructor (M) {
    this.M = M
    // The two halves of this layout are maintained by hand -- the constants
    // above and BUF32_LEN/BUF64_LEN in client/wasm/bindings.cpp -- so they are
    // checked against each other rather than kept in step by discipline. A
    // field appended on the C++ side without one here is a one-word heap
    // overflow per read, and what it presents as is 212 mispredictions in a
    // netcode test with nothing wrong with the netcode.
    const want32 = M._dnp_buf32_len()
    const want64 = M._dnp_buf64_len()
    if (want32 !== BUF32_LEN || want64 !== BUF64_LEN) {
      throw new Error(`[core] agent buffer layout mismatch: wasm wants ` +
                      `${want64}x64/${want32}x32, JS has ${BUF64_LEN}x64/${BUF32_LEN}x32`)
    }
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

  /**
   * The body proportions this map declares, as raw fx.
   *
   * A property of the map, not of the session: a Doom map converted at 1/48 has
   * 56-unit doorways, and the engine's default 2.0 body is 96 of those units.
   * Reading it here means solo mode is walkable with no server at all, and a
   * live client is never predicting with the wrong body while it waits for the
   * first PARAM_UPDATE.
   */
  get body () {
    this.core.M._dnp_map_body(this.ptr, this.core.buf64)
    const b = this.core.buf64 / 8
    return { radius: this.core.views.i64[b], height: this.core.views.i64[b + 1] }
  }

  /** Content hash, as BigInt. Must equal WELCOME.map_hash. */
  get hash () {
    this.core.M._dnp_map_hash(this.ptr, this.core.buf64)
    return BigInt.asUintN(64, this.core.views.i64[this.core.buf64 / 8])
  }

  /**
   * Sector containing a world point, or -1.
   *
   * Takes doubles and converts, because every caller is on the RENDER side of
   * the boundary -- a shadow deciding whether it lands on a platform. The
   * predicate itself is the exact one the simulation uses; only the arguments
   * have been through a double.
   */
  findSector (x, y) {
    const { i64 } = this.core.views
    const b = this.core.buf64 / 8
    i64[b] = BigInt(Math.round(x * 4294967296))
    i64[b + 1] = BigInt(Math.round(y * 4294967296))
    return this.core.M._dnp_map_find_sector(this.ptr, this.core.buf64)
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
  set tick (t) { this.core.M._dnp_world_set_tick(this.ptr, t >>> 0) }

  spawn ({ x = 0n, y = 0n, z = 0n }) {
    const { i64 } = this.core.views
    const base = this.core.buf64 / 8
    i64[base] = x; i64[base + 1] = y; i64[base + 2] = z
    const idx = this.core.M._dnp_world_spawn(this.ptr, this.core.buf64)
    // The map's body, applied at spawn rather than waited for. The server does
    // exactly this for the agent it spawns, so the two agree from tick zero
    // instead of after the first PARAM_UPDATE lands.
    if (idx >= 0 && this.map) {
      const b = this.map.body
      this.setParamRaw(idx, 0, b.radius)
      this.setParamRaw(idx, 1, b.height)
    }
    return idx
  }

  setParamRaw (idx, fieldId, raw) {
    this.core.views.i64[this.core.buf64 / 8] = raw
    return this.core.M._dnp_world_set_param_raw(this.ptr, idx, fieldId, this.core.buf64) !== 0
  }

  /** Register a sector as movable and start a parametric motion on it. */
  applyMotionSet (msg) {
    if (!this.map) return false
    const M = this.core.M
    if (M._dnp_world_add_motion(this.ptr, this.map.ptr, msg.sector) >= 64) return false
    if (msg.motionType !== 1) return true   // discrete/static carry no path

    const { i64 } = this.core.views
    const b = this.core.buf64 / 8
    i64[b] = msg.from.x; i64[b + 1] = msg.from.y; i64[b + 2] = msg.from.z
    i64[b + 3] = msg.to.x; i64[b + 4] = msg.to.y; i64[b + 5] = msg.to.z
    return M._dnp_world_start_parametric(
      this.ptr, msg.sector, this.core.buf64, this.core.buf64 + 24,
      msg.startTick, msg.durationTicks, msg.loopMode, msg.motionTarget ?? 0) !== 0
  }

  /**
   * Apply a replicated movement parameter set (docs/protocol.md §8.4).
   *
   * Returns the number of fields applied. A field the server names but this
   * build does not have is skipped rather than failing the whole update: that
   * is version skew, and dropping one unknown field beats running with rules
   * one generation stale.
   */
  applyParamUpdate (idx, msg) {
    const { i64 } = this.core.views
    const b = this.core.buf64 / 8
    let applied = 0
    for (const f of msg.fields) {
      i64[b] = f.raw
      if (this.core.M._dnp_world_set_param_raw(this.ptr, idx, f.id, this.core.buf64)) applied++
    }
    return applied
  }

  /**
   * Set one movement parameter by NAME.
   *
   * For solo mode, where there is no server and no Lua to send a PARAM_UPDATE.
   * The name lookup is the engine's own, so a name that does not exist is
   * reported rather than silently writing nothing.
   */
  setParamByName (idx, name, value) {
    const bytes = new TextEncoder().encode(name)
    const ptr = this.core.M._malloc(bytes.length + 1)
    this.core.M.HEAPU8.set(bytes, ptr)
    this.core.M.HEAPU8[ptr + bytes.length] = 0
    const id = this.core.M._dnp_param_id_by_name(ptr)
    this.core.M._free(ptr)
    if (id < 0) return false
    const { i64 } = this.core.views
    i64[this.core.buf64 / 8] = BigInt(Math.round(value * 4294967296))
    return this.core.M._dnp_world_set_param_raw(this.ptr, idx, id, this.core.buf64) !== 0
  }

  /**
   * The wire id for a parameter name, or -1.
   *
   * The engine's own lookup. A client that hardcoded these numbers would be
   * keeping the same table in two programs, and appending one field to
   * MoveParams would silently shift every id after it -- which is exactly what
   * happened the first time the swing collider was drawn, using ids five short.
   */
  paramId (name) {
    const bytes = new TextEncoder().encode(name)
    const ptr = this.core.M._malloc(bytes.length + 1)
    this.core.M.HEAPU8.set(bytes, ptr)
    this.core.M.HEAPU8[ptr + bytes.length] = 0
    const id = this.core.M._dnp_param_id_by_name(ptr)
    this.core.M._free(ptr)
    return id
  }

  /** Read one parameter back, so a test can assert what was actually applied. */
  getParamRaw (idx, fieldId) {
    this.core.M._dnp_world_get_param_raw(this.ptr, idx, fieldId, this.core.buf64)
    return this.core.views.i64[this.core.buf64 / 8]
  }

  /** Apply a scripted-discrete offset the server streamed. */
  setDiscreteOffset (sector, offset, target = 0) {
    const { i64 } = this.core.views
    const b = this.core.buf64 / 8
    i64[b] = offset.x; i64[b + 1] = offset.y; i64[b + 2] = offset.z
    return this.core.M._dnp_world_set_discrete(this.ptr, sector, this.core.buf64, target) !== 0
  }

  /**
   * Crush and pin events from the last step (design §3.1).
   *
   * The client cannot act on these -- the consequence is server Lua's, and Lua
   * is not predicted. They are here for diagnosis: a client whose crush set
   * differs from the server's has a determinism bug, and seeing that directly
   * beats inferring it from a position that drifts for no visible reason.
   */
  crushes () {
    const M = this.core.M
    const n = M._dnp_world_crush_count(this.ptr)
    const out = []
    for (let i = 0; i < n; i++) {
      M._dnp_world_crush_at(this.ptr, i, this.core.buf32, this.core.buf64)
      const { i64, u32 } = this.core.views
      const b32 = this.core.buf32 / 4
      out.push({
        entity: u32[b32],
        sector: u32[b32 + 1],
        kind: u32[b32 + 2],       // 1 vertical, 2 horizontal
        depth: i64[this.core.buf64 / 8]
      })
    }
    return out
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
      interactHeld: u32[b32 + IDX.INTR_HELD] !== 0,
      attackHeld: u32[b32 + IDX.ATK_HELD] !== 0,
      attackLeft: u32[b32 + IDX.ATK_LEFT],
      climbing: u32[b32 + IDX.CLIMBING] !== 0,
      grappling: u32[b32 + IDX.GRAPPLING] !== 0,
      grapplePulling: u32[b32 + IDX.GRAP_PULL] !== 0,
      grounded: u32[b32 + IDX.GROUNDED] !== 0,
      // BIASED: sector + 1, with 0 meaning none. That convention comes from
      // the wire (SelfBlock.anchor is a Handle, and handle 0 means "no
      // entity"), and it is carried unchanged all the way to writeAgent so
      // reconciliation can round-trip a snapshot's value without translating
      // it. Left in its wire domain deliberately -- but that is exactly how it
      // ends up printed as "riding 1" for sector 0, so the decoded form is
      // offered beside it and anything human-facing should use that.
      anchor: u32[b32 + IDX.ANCHOR],
      anchorSector: u32[b32 + IDX.ANCHOR] === 0 ? -1 : u32[b32 + IDX.ANCHOR] - 1,
      anchored: u32[b32 + IDX.ANCHORED] !== 0
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
    u32[b32 + IDX.INTR_HELD] = s.interactHeld ? 1 : 0
    u32[b32 + IDX.ATK_HELD] = s.attackHeld ? 1 : 0
    u32[b32 + IDX.ATK_LEFT] = s.attackLeft ?? 0
    u32[b32 + IDX.CLIMBING] = s.climbing ? 1 : 0
    u32[b32 + IDX.GRAPPLING] = s.grappling ? 1 : 0
    u32[b32 + IDX.GRAP_PULL] = s.grapplePulling ? 1 : 0
    u32[b32 + IDX.GROUNDED] = s.grounded ? 1 : 0
    u32[b32 + IDX.ANCHOR] = s.anchor ?? 0
    u32[b32 + IDX.ANCHORED] = s.anchored ? 1 : 0
    this.core.M._dnp_world_write_agent(this.ptr, idx, this.core.buf64, this.core.buf32)
  }
}
