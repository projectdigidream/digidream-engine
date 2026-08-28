//
// The browser client.
//
// Two modes, and the difference between them is the design's central claim
// (§7.1, §9.3) made visible:
//
//   solo  the wasm core runs the simulation locally. No server, no network, no
//         Lua. What you see is the physics core on its own.
//   live  the same wasm core PREDICTS while a real server is authoritative.
//
// The RENDERER does not know which it is in. It is handed a position each frame
// and draws it, because prediction's whole purpose is that the two are
// indistinguishable -- and if they ever stop being indistinguishable, the
// renderer is not where the fault is.
//
import * as THREE from 'three'
import { Core } from './core.js'
import { readMap, fxToFloat } from './mapdata.js'
import { toThreeX, toThreeY, toThreeZ } from './mapgeom.js'
import {
  MapView, makeCamera, resizeCamera, distanceFor, makeAgentMesh, PROJECTIONS,
  makeFacingArrow, aimFacingArrow
} from './render/scene.js'
import {
  Camera, cameraOffset, screenRelativeMove, facingDirection, aimFor, billboardYaw,
  DEFAULT_DISTANCE
} from './camera.js'
import { EntityView } from './render/entities.js'
import { ShadowCaster } from './render/shadows.js'
import { Connection } from './net.js'
import { Predictor } from './predictor.js'
import { parseProps, PropRegistry } from './render/props.js'
import { sweepOffset, makeSwingMesh, swingYaw } from './render/swing.js'
import { parseVfx, VfxView, tracerEnds } from './render/vfx.js'
// The ONE button table. main.js used to keep its own, where CROUCH was 1<<6 --
// which is proto's INTERACT bit, so pressing C sent "use".
import { BUTTON } from './proto.js'
import { MapAccumulator } from './maploader.js'
import { MSG } from './proto.js'

const TICK_HZ = 60
const DT = 1 / TICK_HZ

const params = new URLSearchParams(location.search)

// Live if a server is named; solo otherwise.
const SERVER = params.get('server')
const LIVE = SERVER !== null
const SERVER_URL = SERVER || 'https://127.0.0.1:34433/dnp'

const MAP_URL = params.get('map') ?? '/maps/test.dnpm'
const SPAWN = params.get('spawn') ?? 'start'
// View controls as URL parameters, so a particular framing can be reproduced --
// by a person comparing two builds, or by a headless render in a script.
const START_ZOOM = Number(params.get('zoom') ?? 44)
const START_YAW = Number(params.get('yaw') ?? 45) * Math.PI / 180
const PAUSED = params.get('paused') === '1'
// Perspective by default. ?proj=orthographic gives the parallel projection of
// design §4 -- the two are framed from the same viewHeight, so switching
// between them changes the projection and nothing else.
const PROJECTION = PROJECTIONS.includes(params.get('proj')) ? params.get('proj') : 'perspective'
const FOV = Number(params.get('fov') ?? 50)

const hud = document.getElementById('hud')
const status = document.getElementById('status')
const say = (m) => { status.textContent = m }

const LOAD_STATUS = ['Ok', 'BadMagic', 'BadVersion', 'Truncated', 'TooLarge',
                     'BadIndex', 'HashMismatch', 'TriangleMismatch']

say('loading simulation core...')
const core = await Core.load()

// --- getting a map ---------------------------------------------------------
//
// Solo fetches one over HTTP; live receives one over the control channel. Both
// end up parsing the SAME bytes with the same loader, which is the property
// that makes client and server agree about geometry -- not a shared compiled-in
// copy, and not two parsers that are believed to match.

let mapBytes
let conn = null
let welcome = null
// Why the transport closed, if it has. Read by the handshake loop, which must
// stop waiting for a reply that a closed connection can never deliver.
let closedWhy = null
// A map change in flight: the new WELCOME, and the map arriving behind it.
let pendingWelcome = null
let movingMap = null
const motionSets = []
const pendingSnapshots = []
// Declared BEFORE the connection opens. Messages start arriving during the
// await inside open(), so a handler closing over a `const` declared further
// down would hit the temporal dead zone on the first PARAM_UPDATE -- which is
// to say, on a real server, immediately.
const paramUpdates = []

if (LIVE) {
  say(`connecting to ${SERVER_URL}...`)
  // A development edge presents a self-signed certificate. Its hash is public
  // -- the server sends the certificate itself -- so serving the hash is not a
  // secret being leaked; the private key is never within reach of the dev
  // server's mounts.
  let certHash = null
  try {
    const j = await (await fetch('/certhash')).json()
    certHash = Uint8Array.from(atob(j.sha256_base64), (c) => c.charCodeAt(0))
  } catch {
    // No hash available: fall through with none and let WebTransport apply the
    // ordinary trust rules, which is right for a real deployment.
  }

  const acc = new MapAccumulator()
  conn = new Connection({
    url: SERVER_URL,
    certHash,
    onControl (m) {
      if (m.type === MSG.WELCOME) {
        // A SECOND welcome is the server saying "you are in a different map
        // now". The join sequence is re-sent in full -- welcome, map, motions,
        // parameters -- because that is exactly what arriving somewhere needs,
        // and one code path on each side beats two that drift.
        if (welcome) { pendingWelcome = m; movingMap = new MapAccumulator() }
        else welcome = m
      }
      else if (m.type === MSG.MAP_DATA) (movingMap ?? acc).push(m)
      else if (m.type === MSG.SECTOR_MOTION_SET) motionSets.push(m)
      else if (m.type === MSG.PARAM_UPDATE) paramUpdates.push(m)
      else if (m.type === MSG.REJECT) say(`server rejected the connection (reason ${m.reason})`)
    },
    onSnapshot (s) { pendingSnapshots.push(s) },
    // A WebTransport close carries { closeCode, reason }, not an Error, so
    // `e.message` is undefined and the object stringifies to [object Object].
    // The reason is the whole point of the close -- it is how the edge says
    // "the game server is not running" -- so it is read explicitly.
    onClose (e) {
      closedWhy = e?.reason || e?.message || (e ? String(e) : 'connection closed')
      say(`disconnected: ${closedWhy}`)
    }
  })
  await conn.open()
  say('connected; waiting for WELCOME...')

  // Progress reported at each step of the handshake, because "connecting..."
  // that never changes says nothing about WHICH step is stuck -- and the three
  // are entirely different failures: no transport, no reply, or a map that
  // never finishes arriving.
  const deadline = Date.now() + 10000
  while ((!welcome || !acc.complete) && Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 20))
    if (acc.failed) throw new Error(`map transfer failed: ${acc.failed}`)
    // A close during the handshake is the ANSWER, not something to keep
    // waiting through. The edge closes with a reason when it has no game
    // server behind it, and this loop was overwriting that reason with its own
    // progress message twenty times a second and then reporting a timeout --
    // so the one line that said what was actually wrong never survived long
    // enough to be read.
    if (closedWhy) throw new Error(closedWhy)
    say(welcome
      ? `receiving map... ${acc.received}/${acc.total} bytes`
      : 'connected; waiting for WELCOME...')
  }
  if (closedWhy) throw new Error(closedWhy)
  // Named separately from "the server said no". Reaching here means the
  // transport came up and nothing answered -- which most often means the edge
  // is running and the game server behind it is not, so say that rather than
  // leaving the reader to guess which of the two processes to look at.
  if (!welcome) {
    throw new Error('no WELCOME: the edge accepted the connection but the game ' +
                    'server behind it never replied -- check that dnp-server is running')
  }
  if (!acc.complete) throw new Error('map transfer did not complete')
  mapBytes = acc.bytes()
} else {
  say(`fetching ${MAP_URL}...`)
  mapBytes = new Uint8Array(await (await fetch(MAP_URL)).arrayBuffer())
}

let { ok, status: loadStatus, map } = core.loadMap(mapBytes)
if (!ok) {
  // Named, not numbered. A client that reports "status 5" sends its user to
  // read the source; the loader already knows which of its checks refused.
  say(`map rejected: ${LOAD_STATUS[loadStatus] ?? loadStatus}`)
  throw new Error(`map rejected: ${LOAD_STATUS[loadStatus] ?? loadStatus}`)
}

let mapData = readMap(map)
let world = core.createWorld({ map })

if (LIVE && welcome && map.hash !== welcome.mapHash) {
  // Same bytes, different parse: that is a build mismatch, and it is worth
  // refusing rather than playing a game whose geometry disagrees with the
  // server's. Every collision would differ, and it would look like a physics
  // bug for as long as anyone was willing to look.
  say(`map hash mismatch: client ${map.hash.toString(16)} vs server ${welcome.mapHash.toString(16)}`)
  throw new Error('map hash mismatch')
}

// --- what moves ------------------------------------------------------------

let movers = []

// A sector that is now known to move.
//
// Called for the motions that arrived with the map AND for the ones that arrive
// later, when a switch opens a door mid-match. Adding it to `movers` is what
// puts it in the renderer's dynamic set -- without that the door opens in
// physics and stays shut on screen, which is the most confusing possible way
// for this to be half-right.
// Returns true if this is a sector that was not moving before, which is the
// only case that needs the renderer's static/dynamic split redone. A door
// re-triggered every few seconds re-declares a motion it already had, and
// rebuilding the whole map each time would be a visible stutter for nothing.
function takeMotion (m) {
  if (!world.applyMotionSet(m)) return false
  if (movers.includes(m.sector)) return false
  movers.push(m.sector)
  return true
}

if (LIVE) {
  // Replicated. Parametric movers cost nothing after this: the client computes
  // their position for any tick from the parameters it now holds (§3.1.2).
  for (const m of motionSets) takeMotion(m)
} else {
  // Solo mode has NO movers unless you name them.
  //
  // It used to read a tag convention -- "tag 100+N is a lift that rises N" --
  // which was a Phase 3 bridge from before Lua drove sector motion, and it was
  // wrong twice over. Tags are a gameplay handle a map may use for anything:
  // converted DOOM2 maps carry Doom's own sector tags, and MAP15 arrived with
  // thirty-two of its sectors silently bobbing up and down. And guessing at
  // motion is not solo mode's job at all -- on a server this is a Lua decision
  // (§3.1.2), and solo mode has no Lua.
  //
  //   ?movers=12:2,40:1.5      sector 12 rises 2 units, sector 40 rises 1.5
  //
  // Sector INDICES, not tags: an index is what the map actually addresses.
  for (const spec of (params.get('movers') ?? '').split(',').filter(Boolean)) {
    const [idxRaw, riseRaw] = spec.split(':')
    const i = Number(idxRaw)
    const rise = Number(riseRaw ?? 1)
    if (!Number.isInteger(i) || i < 0 || i >= mapData.sectors.length) {
      console.warn(`[solo] no sector ${idxRaw}`)
      continue
    }
    if (world.applyMotionSet({
      sector: i, motionType: 1, motionTarget: 0,
      from: { x: 0n, y: 0n, z: 0n },
      to: { x: 0n, y: 0n, z: BigInt(Math.round(rise * 4294967296)) },
      startTick: 0, durationTicks: 240, loopMode: 2
    })) movers.push(i)
  }
}

let spawn = map.spawnForTag(SPAWN)
let agent = world.spawn(spawn)

// Our own entity index, as the SERVER numbers it -- not this world's slot
// number, which is always 0 because this world has one agent and the server's
// has many. The body-collision rule is decided by index, so predicting it needs
// ours, and the mirroring below needs it to avoid mirroring us into our own
// path.
//
// Declared HERE rather than beside the renderer state further down. It was
// down there, assigned from up here, which is a temporal dead zone: `let` does
// not hoist an initialisation, so this line threw before the declaration it
// referred to had run. The fix is the ordinary one -- declare a binding where
// it is first needed.
let myAgentIndex = (LIVE && welcome) ? (welcome.selfHandle & 0x00ffffff) - 1 : null

// Where mirrored bodies live in this client's own world: the band of prop slots
// the engine reserves for exactly this. spawn_prop never allocates there, so a
// mirror can never land on a real prop.
//
// Asked of the engine rather than written down here. The last constant this
// client kept its own copy of was the agent buffer length, and the copy going
// stale cost a day and looked like a determinism bug.
const MIRROR_BASE = core.M._dnp_mirror_base()

// Body proportions, for maps built at a scale other than this engine's.
//
// A converted Doom map is at 1/48, chosen so Doom's 24-unit step lands on our
// 0.5 threshold -- which makes the default 2-unit body 96 Doom units tall
// against Doom's 56, and it jams in every doorway. radius and height are
// per-entity MoveParams, so this is what they are for; on a server it would be
// a PARAM_UPDATE from Lua rather than a URL.
for (const [name, key] of [['radius', 'radius'], ['height', 'height'],
                           ['move_speed', 'speed'], ['jump_impulse', 'jump']]) {
  const v = params.get(key)
  if (v === null) continue
  if (!world.setParamByName(agent, name, Number(v))) {
    console.warn(`[solo] no movement parameter named ${name}`)
  }
}
let predictor = LIVE ? new Predictor(world, agent) : null
if (LIVE) {
  // Parameter updates that arrived during the handshake, applied before the
  // first tick: a client predicting with the wrong movement rules mispredicts
  // every tick and never recovers (docs/protocol.md §8.4).
  for (const m of paramUpdates) {
    world.applyParamUpdate(agent, m)
    predictor.paramEpoch = m.epoch
  }
  paramUpdates.length = 0
  world.tick = welcome.serverTick
}

// How far you may fall before this demo puts you back. The ENGINE does not do
// this: an entity that leaves the map falls forever, deliberately, because "you
// died" is a gameplay decision and gameplay is server Lua -- the same shape
// §3.1 gives crushing. A kill plane in the core would be a rule no server could
// opt out of. In live mode the server owns the answer, so this is solo-only.
let lowestFloor = Math.min(...mapData.sectors.map((s) => s.floorZ - (s.thickness ?? 0)))
let VOID_FLOOR = lowestFloor - 40
let falls = 0

// --- scene -----------------------------------------------------------------

const renderer = new THREE.WebGLRenderer({ antialias: true })
// Pixel ratio, capped and overridable.
//
// The fragment shader is where this renderer's time goes -- lighting, occlusion
// and a dither, per pixel -- so the device pixel ratio multiplies the whole
// frame cost. On a 2x display that is four times the fragments for a sharper
// picture, which is a trade worth being able to make rather than one baked in.
renderer.setPixelRatio(Number(params.get('px') ?? Math.min(2, devicePixelRatio)))
renderer.setSize(innerWidth, innerHeight)
renderer.setClearColor(0x0b0d12)
document.body.appendChild(renderer.domElement)

const scene = new THREE.Scene()
let view = new MapView(mapData)
scene.add(view.root)
// Only these sectors are rebuilt per frame; everything else is built once.
if (movers.length) view.setDynamicSectors(movers)

// Real texture images, if this client has them. Not awaited: the map is already
// on screen with generated placeholders, and each real texture replaces one as
// it arrives.
view.loadTextures({ pxPerUnit: Number(params.get('texpx') ?? 32) })
  .then(({ loaded, total }) => {
    if (loaded) console.log(`[render] ${loaded}/${total} textures loaded from /textures`)
  })

// Drawn at the size the agent ACTUALLY is, read back from the world rather
// than assumed. The map declares its own body (a converted Doom map is a third
// of the engine's default width and a bit over half its height), so a hardcoded
// capsule would be a body you can see standing in walls its real one clears --
// which is the exact opposite of what a debug view is for.
const RADIUS_ID = 0
const HEIGHT_ID = 1
let bodyRadius = Number(world.getParamRaw(agent, RADIUS_ID)) / 4294967296
let bodyHeight = Number(world.getParamRaw(agent, HEIGHT_ID)) / 4294967296

let body = makeAgentMesh(0x66ccff, bodyRadius, bodyHeight)
scene.add(body)

// The swing collider, drawn while a swing is running.
//
// Rebuilt from the parameter table rather than replicated: the client already
// has these numbers because it predicts with them, so this cannot disagree with
// the simulation except by getting the arithmetic wrong -- which is what
// sweepOffset's test is for. ?swing=0 hides it.
// Resolved by NAME through the engine's own table. Hardcoding the numbers put
// this five ids short the moment MoveParams grew a field, which is the same
// two-programs-one-number trap the prop catalogue exists to avoid.
const par = (name) => Number(world.getParamRaw(agent, world.paramId(name))) / 4294967296
const swingShape = {
  reach: par('attack_reach'),
  halfArc: par('attack_half_arc'),
  from: par('attack_sweep_from'),
  to: par('attack_sweep_to'),
  // A U8 parameter is stored as an integer, not a fixed-point fraction.
  ticks: Number(world.getParamRaw(agent, world.paramId('attack_ticks')))
}
const SHOW_SWING = params.get('swing') !== '0'
// The whole region the swing travels through: from the start of the sweep less
// its half-arc, to the end plus it. Fixed to the body, so the moving collider
// can be seen crossing it.
const swingSpan = Math.abs(swingShape.to - swingShape.from) / 2 + swingShape.halfArc
const swingMid = (swingShape.from + swingShape.to) / 2
const swingWhole = SHOW_SWING
  ? makeSwingMesh(THREE, { reachUnits: swingShape.reach, halfArcTurns: swingSpan, segments: 24, faint: true })
  : null
const swing = SHOW_SWING
  ? makeSwingMesh(THREE, { reachUnits: swingShape.reach, halfArcTurns: swingShape.halfArc })
  : null
if (swingWhole) scene.add(swingWhole)
if (swing) scene.add(swing)

// Which way the agent is looking, and how far it can reach to work a door.
// On by default because it is the only way to see either; ?arrow=0 hides it.
const SHOW_ARROW = params.get('arrow') !== '0'
const arrow = SHOW_ARROW ? makeFacingArrow(core.interactReach) : null
if (arrow) scene.add(arrow)

// bodyHeight matches the agent mesh, so the trapezium is the projection of
// the body actually drawn rather than of an assumed one.
const shadows = new ShadowCaster({ THREE, scene, radius: bodyRadius, bodyHeight })

// The surface a shadow lands on. The sector's floor, taken LIVE so a shadow on
// a lift rides with it, and null out over the void -- where there is nothing
// beneath and drawing a disc at some default height would put it in mid-air.
function groundUnder (x, z) {
  // three-space back to engine: x stays, three z is the negated engine y.
  const s = map.findSector(x, -z)
  if (s < 0) return null
  return toThreeY(liveSector(s).floorZ)
}
const onGround = (x, z) => map.findSector(x, -z) >= 0

// The prop catalogue, and the sprite frames it names.
//
// Fetched rather than compiled in: adding a prop is editing assets/props.txt
// and naming it from a map's Lua, and neither this file nor the server has a
// table of kinds in it. A missing catalogue is not fatal -- a map that spawns
// nothing does not need one -- so a failure here leaves props drawn as bodies
// and says so once.
let props = null
let vfx = null
// Tracer accounting, for the HUD. See the counting site in drainSnapshots.
let vfxSeen = 0
let vfxNoView = 0
try {
  const text = await (await fetch('/props.txt')).text()
  const catalogue = parseProps(text)
  for (const w of catalogue.warnings) console.warn(`[props] ${w}`)

  // Every frame every kind names, loaded once. A sprite frame is a texture with
  // holes in it, so it is indexed and loaded exactly like a wall texture.
  const wanted = new Set()
  for (const def of catalogue.kinds.values()) {
    for (const st of def.states.values()) for (const f of st.frames ?? []) wanted.add(f)
  }
  const textures = new Map()
  if (wanted.size) {
    const loader = new THREE.TextureLoader()
    const load = (name) => new Promise((resolve) => {
      loader.load(
        `/sprites/${name}.png`,
        (tex) => {
          // Nearest, and no mipmaps. A Doom sprite is 20-odd pixels tall and
          // filtering one is how it turns into a smudge -- the same reason the
          // wall textures are not filtered either.
          tex.magFilter = THREE.NearestFilter
          tex.minFilter = THREE.NearestFilter
          tex.generateMipmaps = false
          tex.colorSpace = THREE.SRGBColorSpace
          textures.set(name, tex)
          resolve()
        },
        undefined,
        () => { console.warn(`[props] missing sprite ${name}`); resolve() }
      )
    })
    await Promise.all([...wanted].map(load))
  }
  props = new PropRegistry({ THREE, textures, catalogue })
  // Transients read the same file. A tracer's colour, width and lifetime are
  // catalogue lines, so a shot can be retuned without touching either binary.
  const vfxCat = parseVfx(text)
  for (const w of vfxCat.warnings) console.warn(`[vfx] ${w}`)
  vfx = new VfxView({ THREE, scene, catalogue: vfxCat })
  console.log(`[props] ${catalogue.kinds.size} kinds, ${textures.size} sprite frames, ` +
              `${vfxCat.kinds.size} effect kinds`)
} catch (e) {
  console.warn(`[props] no catalogue (${e.message}); props will draw as bodies`)
}

// Other players. Interpolated, never predicted -- predicting one means guessing
// what another person will press (see render/entities.js).
const others = new EntityView({
  // Other players are the same size we are: the body is the MAP's, not this
  // session's, so drawing them at the engine default would put every remote
  // player a head taller than the doorways they walk through.
  // Kind 0 is a player; anything else is a prop the catalogue describes. A
  // kind the catalogue does not cover falls back to the body mesh rather than
  // to nothing, because an entity you cannot see is indistinguishable from one
  // the server never sent.
  makeMesh: (kind) => (kind ? props?.make(kind) : null) ??
                      makeAgentMesh(0xffa066, bodyRadius, bodyHeight),
  scene,
  // A prop is not only something to draw. A solid one is PHYSICS -- the mover
  // pushes bodies out of it -- so the client mirrors it into its own world at
  // the entity index the wire used, or it walks through what the server stops
  // it against and mispredicts every tick it is touching.
  //
  // The size and the flags come from the catalogue both sides read. Nothing
  // about a prop's collision travels on the wire beyond its position.
  onProp: (index, kind, pos) => {
    const def = props?.kinds.get(kind)
    if (!def) return
    world.placeProp(index, kind, pos, def)
  },
  // Other BODIES -- players and monsters -- mirrored as colliders.
  //
  // ALL of them, not a filtered subset. The engine's rule is that every body is
  // pushed out of every other and none of them is pushed BY one
  // (World::resolve_agent_collisions), so what this client has to reproduce is
  // its own displacement out of everyone -- and the positions it needs for that
  // are the start-of-tick ones a snapshot already carries. That is the whole
  // reason the rule reads a start-of-tick snapshot rather than live positions.
  //
  // Placed in prop slots rather than agent slots: what this world needs from
  // them is a cylinder to stop at, and it must NOT simulate them -- remote
  // bodies are interpolated, never predicted.
  onBody: (index, kind, pos) => {
    if (myAgentIndex !== null && index === myAgentIndex) return
    // A monster's size is its catalogue entry; a player's is the map's declared
    // body. Kind 0 is a player.
    const def = kind ? props?.kinds.get(kind) : { radius: bodyRadius, height: bodyHeight }
    if (!def) return
    world.placeProp(MIRROR_BASE + index, 0, pos,
                    { radius: def.radius, height: def.height, flags: 1 /* solid */ })
  },
  onBodyGone: (index) => world.removeProp(MIRROR_BASE + index),
  onPropGone: (index) => world.removeProp(index)
})

/**
 * Take up residence in a new map, mid-session.
 *
 * The server hosts one simulation per map and moves a session between them, so
 * a second WELCOME means "you are somewhere else now". Everything downstream of
 * the geometry is rebuilt: the world, the agent, the predictor, the meshes, the
 * shadow's body size, and the set of sectors known to move.
 *
 * Everything NOT downstream of the geometry is deliberately kept -- the
 * renderer, the scene, the camera and its yaw, the held keys. Rebuilding those
 * would flash the canvas and snap the view for no reason: the player is the
 * same player, looking the same way, and only the world under them changed.
 */
function adoptMap (bytes, w) {
  const loaded = core.loadMap(bytes)
  if (!loaded.ok) {
    say(`new map rejected: ${LOAD_STATUS[loaded.status] ?? loaded.status}`)
    return false
  }
  map = loaded.map
  mapData = readMap(map)

  world = core.createWorld({ map })
  spawn = map.spawnForTag(SPAWN)
  agent = world.spawn(spawn)
  // A new map is a new entity index -- change_map re-seats the client, and
  // keeping the old one would mirror a body on top of us and mirror us onto
  // somebody else.
  myAgentIndex = (w.selfHandle & 0x00ffffff) - 1
  world.tick = w.serverTick
  predictor = new Predictor(world, agent)

  bodyRadius = Number(world.getParamRaw(agent, RADIUS_ID)) / 4294967296
  bodyHeight = Number(world.getParamRaw(agent, HEIGHT_ID)) / 4294967296

  lowestFloor = Math.min(...mapData.sectors.map((s) => s.floorZ - (s.thickness ?? 0)))
  VOID_FLOOR = lowestFloor - 40

  // The old geometry leaves the scene explicitly. Dropping the reference would
  // leak every buffer in it -- a map change is not a page load, and nothing
  // else is going to collect a GPU allocation.
  for (const m of view.meshes) { view.root.remove(m); m.geometry.dispose() }
  scene.remove(view.root)
  view = new MapView(mapData)
  scene.add(view.root)
  view.loadTextures({ pxPerUnit: Number(params.get('texpx') ?? 32) })

  movers = []

  scene.remove(body)
  body = makeAgentMesh(0x66ccff, bodyRadius, bodyHeight)
  scene.add(body)
  shadows.setBody(bodyRadius, bodyHeight)

  // Other players in the map just left are not other players here: the indices
  // in the next snapshot address a different set of people.
  others.clear()

  // The snapshot stream is about the new world from the next one onward;
  // anything still queued describes the old one.
  pendingSnapshots.length = 0
  motionSets.length = 0
  inputHistory = []
  newestServerTick = w.serverTick

  camera.snapTo({
    x: toThreeX(fxToFloat(spawn.x)), y: toThreeY(fxToFloat(spawn.z)), z: toThreeZ(fxToFloat(spawn.y))
  })
  // No banner. A map change is the game doing what you asked it to; the
  // geometry in front of you says it happened, and a line of text over the
  // view says it twice.
  return true
}

let viewHeight = START_ZOOM
const cam3 = makeCamera(PROJECTION, innerWidth / innerHeight, viewHeight, FOV)
const camera = new Camera()

// Under perspective, zooming MOVES the camera; under orthographic it reshapes
// the frustum and the position is irrelevant to framing. One number drives
// both.
const camDistance = () => distanceFor(cam3, viewHeight, DEFAULT_DISTANCE)

addEventListener('resize', () => {
  renderer.setSize(innerWidth, innerHeight)
  resizeCamera(cam3, innerWidth / innerHeight, viewHeight)
})
addEventListener('wheel', (e) => {
  viewHeight = Math.min(140, Math.max(8, viewHeight * (e.deltaY > 0 ? 1.1 : 1 / 1.1)))
  resizeCamera(cam3, innerWidth / innerHeight, viewHeight)
}, { passive: true })

// --- input -----------------------------------------------------------------

const held = new Set()
addEventListener('keydown', (e) => { held.add(e.code); if (e.code === 'Space') e.preventDefault() })
addEventListener('keyup', (e) => held.delete(e.code))
addEventListener('blur', () => held.clear())

let yaw = START_YAW

// The last direction the agent was actually pointed. Brad 0 is due east, not
// "unset", so this starts there and every idle tick repeats whatever came last.
let lastAim = 0

// Quantised with the SAME function the server decodes with (protocol R1).
// Reimplementing it in JS would be one more place for the two sides to
// disagree, and the disagreement would be one ulp wide and present every tick.
function axis (v) {
  return core.axisFromFxRaw(BigInt(Math.round(Math.max(-1, Math.min(1, v)) * 4294967296)))
}

function currentInput () {
  let mx = 0
  let my = 0
  if (held.has('KeyW') || held.has('ArrowUp')) my += 1
  if (held.has('KeyS') || held.has('ArrowDown')) my -= 1
  if (held.has('KeyD') || held.has('ArrowRight')) mx += 1
  if (held.has('KeyA') || held.has('ArrowLeft')) mx -= 1

  const len = Math.hypot(mx, my)
  if (len > 1) { mx /= len; my /= len }

  // Screen-relative, from the same yaw the camera uses. See camera.js: the two
  // are halves of one fact and deriving them separately is how they invert.
  const { x: wx, y: wy } = screenRelativeMove(mx, my, yaw)

  let buttons = 0
  if (held.has('Space')) buttons |= BUTTON.JUMP
  if (held.has('ShiftLeft') || held.has('ShiftRight')) buttons |= BUTTON.DASH
  // E is USE: work a door, throw a switch. Climb is deliberately UNBOUND --
  // it is about to change shape, and leaving a key wired to the old behaviour
  // would make the change look like a regression in whatever it lands on.
  if (held.has('KeyE')) buttons |= BUTTON.INTERACT
  if (held.has('KeyC')) buttons |= BUTTON.CROUCH
  // Left mouse would be the obvious binding and is not wired yet: the canvas
  // has no pointer capture, so a click would leave the page rather than swing.
  if (held.has('KeyF')) buttons |= BUTTON.ATTACK_PRIMARY

  // Held across a stop, so letting go of the keys leaves the agent facing where
  // it was walking rather than snapping east. See aimFor.
  lastAim = aimFor(wx, wy, lastAim)
  return { buttons, moveX: axis(wx), moveY: axis(wy), aim: lastAim }
}

// --- loop ------------------------------------------------------------------
//
// A fixed-timestep accumulator. The simulation MUST advance in whole 60 Hz
// ticks -- it is the same core the server runs, and stepping it by a frame time
// would be a different simulation (docs/physics-contract.md). Rendering runs at
// the display's rate and reads whatever the last tick produced.

let acc = 0
let last = performance.now()
let ticks = 0
let frames = 0
let fpsAt = last
let fps = 0
let inputHistory = []
let newestServerTick = 0

// Where the frame's time actually goes, so this is measurable on real hardware
// rather than argued about. Smoothed, because a single frame's numbers are
// noise and the question is always "what is this costing on average".
const cost = { sim: 0, mesh: 0, draw: 0 }
const blend = (k, v) => { cost[k] += (v - cost[k]) * 0.1 }

function drainSnapshots () {
  while (pendingSnapshots.length) {
    const s = pendingSnapshots.shift()
    newestServerTick = Math.max(newestServerTick, s.serverTick)

    // Transient effects, drawn once and forgotten. The event carries where the
    // shot STARTED, which way it went and how long it was -- so the tracer ends
    // where the shot ended rather than running to infinity through whatever it
    // hit.
    for (const v of s.vfx ?? []) {
      // Counted BEFORE anything can reject it, so the HUD can tell three
      // different failures apart. "Still invisible" is not a diagnosis, and it
      // has now been reported four times: seen==0 means nothing arrived and the
      // problem is the server or the wire; seen>0 with live==0 means the
      // renderer threw them away (an unknown kind, a zero length); both non-zero
      // means they are on screen and something else is wrong.
      vfxSeen++
      if (!vfx) { vfxNoView++; continue }
      const { from, to } = tracerEnds(v, fxToFloat)
      vfx.spawn(v.kind,
                { x: toThreeX(from.x), y: toThreeY(from.z), z: toThreeZ(from.y) },
                { x: toThreeX(to.x), y: toThreeY(to.z), z: toThreeZ(to.y) },
                performance.now() / 1000)
    }
    for (const so of s.sectorOffsets ?? []) world.setDiscreteOffset(so.sector, so.offset)
    others.ingest(s, { fxToFloat, toThree: (x, y, z) => ({ x: toThreeX(x), y: toThreeY(z), z: toThreeZ(y) }) })
    predictor.reconcile(s)
  }
  for (const m of paramUpdates) {
    world.applyParamUpdate(agent, m)
    predictor.paramEpoch = m.epoch
  }
  paramUpdates.length = 0
}

function frame (now) {
  requestAnimationFrame(frame)
  const dt = Math.min(0.25, (now - last) / 1000)
  last = now

  // A map change lands FIRST, before any input is sampled or any tick is
  // simulated. Stepping the old world for even one frame after the server has
  // moved us would send inputs addressed to a room we are no longer in.
  //
  // And only once the whole map has arrived: a MapAccumulator that is still
  // filling is not a map, and adopting a partial one would fail the load and
  // leave the session with no geometry at all.
  if (pendingWelcome && movingMap?.complete) {
    const w = pendingWelcome
    const bytes = movingMap.bytes()
    pendingWelcome = null
    movingMap = null
    if (adoptMap(bytes, w)) welcome = w
  } else if (movingMap?.failed) {
    say(`map change failed: ${movingMap.failed}`)
    pendingWelcome = null
    movingMap = null
  }

  if (held.has('KeyQ')) yaw -= 1.5 * dt
  if (held.has('KeyR')) yaw += 1.5 * dt

  acc += dt
  // Bounded, so a backgrounded tab does not return and run ten thousand ticks
  // in one frame. Dropping the missed time is the same choice the server makes
  // after a stall, and for the same reason.
  const tSim = performance.now()
  let budget = PAUSED ? 0 : 8
  while (acc >= DT && budget-- > 0) {
    const f = currentInput()
    if (LIVE) {
      predictor.step(f)
      inputHistory = [f, ...inputHistory].slice(0, 8)   // redundancy, protocol §4.1
      conn.send(predictor.tick, predictor.lastSnapshot?.serverTick ?? 0, inputHistory)
    } else {
      world.setInput(agent, f)
      world.step()
    }
    acc -= DT
    ticks++
  }
  if (acc > DT) acc = 0
  blend('sim', performance.now() - tSim)

  if (LIVE) {
    // Motions declared since the last frame -- a door someone just opened.
    let fresh = false
    while (motionSets.length) fresh = takeMotion(motionSets.shift()) || fresh
    if (fresh) view.setDynamicSectors(movers)
    drainSnapshots()
  }

  // Moving sectors changed the geometry, so the mesh follows -- for the movers
  // and their neighbours only. See MapView.setDynamicSectors.
  const tMesh = performance.now()
  if (movers.length) {
    for (const i of movers) {
      const live = liveSector(i)
      mapData.sectors[i].floorZ = live.floorZ
      mapData.sectors[i].ceilZ = live.ceilZ
    }
    view.rebuildDynamic()
  }
  blend('mesh', performance.now() - tMesh)

  let a = world.readAgent(agent)
  if (!LIVE && fxToFloat(a.pos.z) < VOID_FLOOR) {
    falls++
    world.writeAgent(agent, { ...a, pos: spawn, vel: { x: 0n, y: 0n, z: 0n }, grounded: false })
    camera.snapTo({ x: toThreeX(fxToFloat(spawn.x)), y: toThreeY(fxToFloat(spawn.z)), z: toThreeZ(fxToFloat(spawn.y)) })
    a = world.readAgent(agent)
  }
  const px = fxToFloat(a.pos.x)
  const py = fxToFloat(a.pos.y)
  const pz = fxToFloat(a.pos.z)
  const subject = { x: toThreeX(px), y: toThreeY(pz), z: toThreeZ(py) }

  body.position.set(subject.x, subject.y + body.userData.yOffset, subject.z)
  if (swing) {
    // The engine's own numbers, run through the engine's own formula. See
    // render/swing.js: the offset is a turn fraction from the aim.
    const off = sweepOffset({ ...swingShape, left: a.attackLeft })
    const facingTurns = (a.facing >>> 0) / 4294967296
    // Drawn at the middle of the body rather than at its feet: a collider
    // hugging the floor reads as unrelated to the thing it is about to hit.
    const at = subject.y + bodyHeight / 2
    swing.visible = off !== null
    swingWhole.visible = off !== null
    if (off !== null) {
      swing.position.set(subject.x, at, subject.z)
      swing.rotation.y = swingYaw(facingTurns, off)
      swingWhole.position.set(subject.x, at - 0.02, subject.z)
      swingWhole.rotation.y = swingYaw(facingTurns, swingMid)
    }
  }
  if (arrow) {
    // From the body's middle, which is roughly where a reach comes from and is
    // where you look when you are judging whether a door is in range.
    aimFacingArrow(arrow,
                   { x: subject.x, y: subject.y + body.userData.yOffset, z: subject.z },
                   facingDirection(a.facing))
  }
  // Aimed at the body's middle rather than its feet: a window centred on the
  // feet leaves the head behind the wall.
  view.setOcclusionTarget({ x: subject.x, y: subject.y + body.userData.yOffset, z: subject.z })
  shadows.update(subject, groundUnder(subject.x, subject.z), view.lights, onGround)
  if (LIVE) {
    others.update(newestServerTick, acc / DT)
    if (props) {
      // The animation's own clock, in seconds, shared by every prop of a kind.
      // Independent phases would make a row of keys shimmer in a way nobody
      // could name.
      // Hand the server's declared animation state to the renderer. The entity
      // view records it and the registry plays it, and until this line existed
      // the two were never introduced -- a barrel switched to its explosion
      // state and went on showing idle frames until it vanished.
      for (const ent of others.entities.values()) {
        if (ent.kind) props.setState(ent.mesh, ent.animationState ?? 0)
      }
      props.update(now / 1000)
      // performance.now(), NOT the frame's `now`. They are the same clock but
      // not the same instant: `now` is when the frame began, and the tracers
      // being faded here were spawned later in this same frame, from
      // drainSnapshots. Fading them against the earlier stamp gave them a
      // negative age, which used to mean "retire immediately".
      if (vfx) vfx.update(performance.now() / 1000)
      // Billboarding: yaw only, and derived rather than guessed. See
      // billboardYaw -- the first version had the sign of yaw backwards.
      const bb = billboardYaw(yaw)
      for (const ent of others.entities.values()) {
        if (ent.kind) ent.mesh.rotation.y = bb
      }
    }
  }

  // The camera follows the PREDICTED position -- here, whichever of the two
  // modes produced it. Never a snapshot: see camera.js.
  camera.cfg.offset = cameraOffset(yaw, camDistance())
  const eye = camera.update(subject, dt)
  cam3.position.set(eye.x, eye.y, eye.z)
  cam3.lookAt(eye.lookAt.x, eye.lookAt.y, eye.lookAt.z)

  const tDraw = performance.now()
  renderer.render(scene, cam3)
  blend('draw', performance.now() - tDraw)

  frames++
  if (now - fpsAt > 500) { fps = Math.round(frames * 1000 / (now - fpsAt)); frames = 0; fpsAt = now }

  // What the frame actually costs, from the renderer rather than from a guess.
  const ri = renderer.info.render

  const st = predictor?.stats
  hud.textContent =
    `${LIVE ? 'LIVE' : 'solo'}  ${PROJECTION}  tick ${ticks}   ${fps} fps\n` +
    `pos  ${px.toFixed(2)} ${py.toFixed(2)} ${pz.toFixed(2)}\n` +
    `sector ${a.sector < 0 ? 'VOID' : a.sector}   ${a.grounded ? 'grounded' : 'airborne'}` +
    `${a.anchored ? `   riding sector ${a.anchorSector}` : ''}\n` +
    `jumps ${a.jumpsUsed}   movers ${movers.length}   textures ${mapData.textures.length}\n` +
    `draws ${ri.calls}   tris ${ri.triangles}   px ${renderer.getPixelRatio()}   ` +
    `meshes ${view.meshes.length}\n` +
    `sim ${cost.sim.toFixed(2)}ms   mesh ${cost.mesh.toFixed(2)}ms   ` +
    `draw ${cost.draw.toFixed(2)}ms\n` +
    `vfx  seen ${vfxSeen}   live ${vfx ? vfx.live.length : 0}   ` +
    `pool ${vfx ? vfx.pool.length : 'NO VIEW'}` +
    (vfxNoView ? `   dropped-no-view ${vfxNoView}` : '') +
    (falls ? `   falls ${falls}` : '') +
    (LIVE
      ? `\nothers ${others.count}   exact ${st.exactMatches}   misp ${st.mispredictions}` +
        `   starved ${st.starvedDivergences}   resync ${st.hardResyncs}`
      : '')
}

// The live floor/ceiling of a sector, read from the wasm store that sector
// motion writes into. Recomputing it in JS would be a second implementation of
// parametric_offset, and the two would disagree the first time either changed.
function liveSector (i) {
  const M = core.M
  M._dnp_map_sector(map.ptr, i, core.buf64, core.buf32)
  const { i64 } = core.views
  const b = core.buf64 / 8
  return { floorZ: fxToFloat(i64[b]), ceilZ: fxToFloat(i64[b + 1]) }
}

say('')
document.getElementById('help').style.display = 'block'
requestAnimationFrame(frame)
