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
const BUF32_LEN = 20

export const IDX = {
  POS: 0, VEL: 3, HASH: 6, DASH_T: 7, DASH_CD: 8, HOVER_F: 9, FLIGHT_F: 10,
  GRAP_ANCH: 11, GRAP_DIST: 14, DASH_DIR: 15, CLIMB_N: 17,

  FACING: 0, FLAGS: 1, JUMPS: 2, JUMP_HELD: 3, ACTIVE: 4, MODE: 5, DASHES: 6,
  SECTOR: 7, DASH_HELD: 8, GRAP_HELD: 9, CLIMBING: 10, GRAPPLING: 11,
  GRAP_PULL: 12, GROUNDED: 13, ANCHOR: 14, ANCHORED: 15, INTR_HELD: 16, ATK_HELD: 17, ATK_LEFT: 18, ATK_CD: 19
}

// A camera path's header, mirroring PATH32_* in client/wasm/bindings.cpp. Its
// own four slots and its own scratch buffer: a path's points are up to 3x256
// i64, which the agent buffer above is nowhere near, and widening that one
// would put a rarely-read field beside what the predictor touches every tick.
const PATH32_LEN = 4
export const PATH32 = { FIRST: 0, COUNT: 1, TAG: 2, FLAGS: 3 }

// map.hpp's path_flag::LOOP.
export const PATH_LOOP = 1

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
    // The same hazard, the same check, for the camera path header -- four slots
    // that cannot over-read, but CAN be read in the wrong order if one side
    // swaps tag and flags. That presents as a camera addressing the wrong path.
    const wantPath32 = M._dnp_path32_len()
    if (wantPath32 !== PATH32_LEN) {
      throw new Error(`[core] path buffer layout mismatch: wasm wants ` +
                      `${wantPath32}x32, JS has ${PATH32_LEN}x32`)
    }
    this.buf64 = M._malloc(BUF64_LEN * 8)
    this.buf32 = M._malloc(BUF32_LEN * 4)
    this.argbuf = M._malloc(8)
    // Lazily, because most sessions never read a path and this is 6 KiB.
    this.pathbuf64 = 0
    this.pathbuf32 = 0
    this._buffer = null
    this._views = null
  }

  /** Scratch big enough for any one path's points. Allocated on first use. */
  ensurePathBuffers () {
    if (this.pathbuf64) return
    const n = this.M._dnp_map_max_path_points()
    this.pathbuf64 = this.M._malloc(n * 3 * 8)
    this.pathbuf32 = this.M._malloc(Math.max(n, PATH32_LEN) * 4)
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

  /**
   * An EMPTY store, to be filled with chunks: a region's world on the client
   * (geom::MapStorage::add_chunk). Every level held is added at its origin.
   */
  createStore () { return new WasmMap(this, this.M._dnp_map_create()) }

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

  /** Copy `src` in as chunk `key` at raw-fx origin {x, y}. False when it would not fit. */
  addChunk (key, src, origin) {
    const { i64 } = this.core.views
    const b = this.core.buf64 / 8
    i64[b] = BigInt(origin.x); i64[b + 1] = BigInt(origin.y)
    return this.core.M._dnp_map_add_chunk(this.ptr, src.ptr, key, this.core.buf64) !== 0xFFFFFFFF
  }

  /** The chunk's { sectorFirst, sectorCount, sideFirst, sideCount }, or null. */
  chunkRange (key) {
    if (!this.core.M._dnp_map_chunk_range(this.ptr, key, this.core.buf32)) return null
    const u32 = this.core.views.u32
    const b = this.core.buf32 / 4
    return { sectorFirst: u32[b], sectorCount: u32[b + 1], sideFirst: u32[b + 2], sideCount: u32[b + 3] }
  }

  removeChunk (key) { return this.core.M._dnp_map_remove_chunk(this.ptr, key) !== 0 }

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

  /**
   * Texture pixels per world unit, as a number.
   *
   * From the MAP, not from a flag. A level converted at 1/48 from a game whose
   * textures are one pixel per map unit wants 48; the renderer's own default is
   * 32, and drawing a converted map at 32 makes every texture half again too
   * large -- which reads as textures being placed wrong rather than as a scale
   * error, and is why it went unnoticed.
   */
  get textureScale () {
    this.core.M._dnp_map_texture_scale(this.ptr, this.core.buf64)
    return Number(this.core.views.i64[this.core.buf64 / 8]) / 4294967296
  }

  // --- camera paths (design §4.1) -------------------------------------------
  //
  // Structure and live point positions. Nothing that EVALUATES a path: the
  // camera rides a curve through these points and client/src/spline.js owns
  // that, which is also what lets the curve be tested with no wasm at all.

  get pathCount () { return this.core.M._dnp_map_path_count(this.ptr) }

  /** Header for one path, or null if the index is out of range. */
  pathInfo (i) {
    const c = this.core
    c.ensurePathBuffers()
    if (!c.M._dnp_map_path_info(this.ptr, i, c.pathbuf32, c.pathbuf64)) return null
    const u = c.views.u32
    const b = c.pathbuf32 / 4
    const flags = u[b + PATH32.FLAGS]
    return {
      index: i,
      first: u[b + PATH32.FIRST],
      count: u[b + PATH32.COUNT],
      tag: u[b + PATH32.TAG],
      flags,
      loop: (flags & PATH_LOOP) !== 0,
      // Arc length of the CURVE, baked by tools/mapc. What a script divides a
      // speed into to time a flythrough, since the engine has no completion
      // event -- so it is the client's job to ride at the speed that makes the
      // number true, not to recompute it.
      curveLength: Number(c.views.i64[c.pathbuf64 / 8]) / 4294967296
    }
  }

  /**
   * LIVE positions of one path's points, as a flat Float64Array of x,y,z in
   * ENGINE space.
   *
   * Live, not authored: sector motion rewrites these every tick, which is the
   * whole reason a camera path can be bolted to a lift. Read once at load and
   * the path stops moving -- see refreshPathPositions in mapdata.js, which is
   * the caller that gets this right.
   *
   * `into` is reused across frames when given, since the common case is
   * rewriting the same path's points 60 times a second.
   */
  pathPoints (i, into = null) {
    const c = this.core
    c.ensurePathBuffers()
    const n = c.M._dnp_map_path_points(this.ptr, i, c.pathbuf64, c.M._dnp_map_max_path_points())
    const out = (into && into.length === n * 3) ? into : new Float64Array(n * 3)
    const v = c.views.i64
    const b = c.pathbuf64 / 8
    for (let k = 0; k < n * 3; k++) out[k] = Number(v[b + k]) / 4294967296
    return out
  }

  /** Anchor sector per point, or -1. For the debug overlay; the camera never asks. */
  pathAnchors (i) {
    const c = this.core
    c.ensurePathBuffers()
    const n = c.M._dnp_map_path_anchors(this.ptr, i, c.pathbuf32, c.M._dnp_map_max_path_points())
    const out = new Int32Array(n)
    const u = c.views.u32
    const b = c.pathbuf32 / 4
    for (let k = 0; k < n; k++) out[k] = u[b + k] | 0
    return out
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
    // name -> wire id, resolved once. See paramId.
    this.paramIds = new Map()
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

  /**
   * A sector's index in THIS world's store, from the name the wire carries
   * (geom::MapStorage::sector_name). -1 for a sector this client does not hold.
   * For a one-map store the name is the index.
   */
  sectorByName (name) {
    if (name === undefined || name === null || name < 0) return -1
    return this.core.M._dnp_world_sector_by_name(this.ptr, name | 0)
  }

  /**
   * Attach a (changed) map: after a region's store gained or lost a chunk,
   * the world must read the new view of it.
   */
  setMap (map) {
    this.core.M._dnp_world_set_map(this.ptr, map.ptr)
    this.map = map
  }

  /** Forget the motions of sectors [first, first + count): their chunk is gone. */
  forgetSectors (first, count) { this.core.M._dnp_world_forget_sectors(this.ptr, first, count) }

  /** Register a sector as movable and start a parametric motion on it. */
  applyMotionSet (msg) {
    if (!this.map) return false
    const M = this.core.M
    const sector = this.sectorByName(msg.sector)
    if (sector < 0) return false
    if (M._dnp_world_add_motion(this.ptr, this.map.ptr, sector) >= M._dnp_max_moving_sectors()) return false
    if (msg.motionType !== 1) return true   // discrete/static carry no path

    const { i64 } = this.core.views
    const b = this.core.buf64 / 8
    i64[b] = msg.from.x; i64[b + 1] = msg.from.y; i64[b + 2] = msg.from.z
    i64[b + 3] = msg.to.x; i64[b + 4] = msg.to.y; i64[b + 5] = msg.to.z
    // The turn: {pivot.x, pivot.y, spinFrom, spinTo}, written even when it is
    // all zeros. Passing the pointer unconditionally rather than branching is
    // what keeps "a motion that only travels" from being a second code path
    // that agrees with the first until it does not.
    i64[b + 6] = msg.pivot?.x ?? 0n; i64[b + 7] = msg.pivot?.y ?? 0n
    i64[b + 8] = msg.spinFrom ?? 0n; i64[b + 9] = msg.spinTo ?? 0n
    return M._dnp_world_start_parametric(
      this.ptr, sector, this.core.buf64, this.core.buf64 + 24,
      msg.startTick, msg.durationTicks, msg.loopMode, msg.motionTarget ?? 0,
      this.core.buf64 + 48) !== 0
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
    // MEMOISED, AND THAT IS NOT THE MISTAKE THE COMMENT ABOVE WARNS ABOUT.
    //
    // The warning is against a client HARDCODING these numbers, because the
    // table then exists in two programs and appending a MoveParams field shifts
    // every id after it. This asks the engine exactly once per name and keeps
    // the answer, which cannot go stale: the id is a property of the compiled
    // binary, and the binary does not change while the page is loaded. A new
    // wasm module means a new Core, which means a new cache.
    //
    // Worth doing because the callers are per-frame: readSwingShape resolves
    // seven names every frame, and each miss was a TextEncoder, a _malloc, a
    // heap write and a _free. Note this caches the ID only -- the VALUES behind
    // these ids are read fresh every frame on purpose (see swing.js), because
    // that is what makes the debug panel's sliders do anything.
    let id = this.paramIds.get(name)
    if (id !== undefined) return id
    const bytes = new TextEncoder().encode(name)
    const ptr = this.core.M._malloc(bytes.length + 1)
    this.core.M.HEAPU8.set(bytes, ptr)
    this.core.M.HEAPU8[ptr + bytes.length] = 0
    id = this.core.M._dnp_param_id_by_name(ptr)
    this.core.M._free(ptr)
    this.paramIds.set(name, id)
    return id
  }

  /** Read one parameter back, so a test can assert what was actually applied. */
  getParamRaw (idx, fieldId) {
    this.core.M._dnp_world_get_param_raw(this.ptr, idx, fieldId, this.core.buf64)
    return this.core.views.i64[this.core.buf64 / 8]
  }

  /**
   * Mirror a replicated prop into this world, at the entity index the wire used.
   *
   * A solid prop is physics, and physics has to be predictable: without this
   * the client walks through a barrel the server stops it against and
   * mispredicts every tick it is touching. The size and flags come from the
   * prop catalogue, which the client already has -- the engine does not know
   * them.
   */
  placeProp (entity, kind, pos, { radius, height, flags }) {
    const { i64 } = this.core.views
    const b = this.core.buf64 / 8
    i64[b] = pos.x; i64[b + 1] = pos.y; i64[b + 2] = pos.z
    i64[b + 3] = BigInt(Math.round(radius * 4294967296))
    i64[b + 4] = BigInt(Math.round(height * 4294967296))
    return this.core.M._dnp_world_place_prop(this.ptr, entity, kind, this.core.buf64, flags) !== 0
  }

  removeProp (entity) { this.core.M._dnp_world_remove_prop(this.ptr, entity) }

  /** Apply a scripted-discrete offset the server streamed. */
  setDiscreteOffset (name, offset, target = 0) {
    // By name, like every sector the wire names (see sectorByName).
    const sector = this.sectorByName(name)
    if (sector < 0) return false
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

  setInput (idx, { buttons = 0, moveX = 0, moveY = 0, aim = 0, pitch = 0 }) {
    this.core.M._dnp_world_set_input(this.ptr, idx, buttons, moveX, moveY, aim, pitch)
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
      attackCooldownLeft: u32[b32 + IDX.ATK_CD],
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
    u32[b32 + IDX.ATK_CD] = s.attackCooldownLeft ?? 0
    u32[b32 + IDX.CLIMBING] = s.climbing ? 1 : 0
    u32[b32 + IDX.GRAPPLING] = s.grappling ? 1 : 0
    u32[b32 + IDX.GRAP_PULL] = s.grapplePulling ? 1 : 0
    u32[b32 + IDX.GROUNDED] = s.grounded ? 1 : 0
    u32[b32 + IDX.ANCHOR] = s.anchor ?? 0
    u32[b32 + IDX.ANCHORED] = s.anchored ? 1 : 0
    this.core.M._dnp_world_write_agent(this.ptr, idx, this.core.buf64, this.core.buf32)
  }
}
