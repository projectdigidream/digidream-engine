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
import { followMovers } from './mapmovers.js'
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
import { LocalConnection, hasLocalTransport } from './net_local.js'
import { Predictor } from './predictor.js'
import { parseProps, PropRegistry } from './render/props.js'
import { parseAtlas, pivotDisagreement } from './render/atlas.js'
import { sweepOffset, makeSwingMesh, swingYaw, readSwingShape, sameSwingShape,
         sweptSpan, SwingTrail } from './render/swing.js'
import { parseVfx, VfxView, tracerEnds } from './render/vfx.js'
import { UiView } from './render/ui.js'
import { parseAnimations, TextureAnimator } from './render/animtex.js'
// The ONE button table. main.js used to keep its own, where CROUCH was 1<<6 --
// which is proto's INTERACT bit, so pressing C sent "use".
import { BUTTON } from './proto.js'
import { MapAccumulator } from './maploader.js'
import { Music } from './audio/music.js'
import { MSG, encodeUiEvent, CAMERA_FIELD } from './proto.js'

const TICK_HZ = 60
const DT = 1 / TICK_HZ

const params = new URLSearchParams(location.search)

// Live if a server is named; solo otherwise.
//
// THE DESKTOP SHELL IS A THIRD WAY IN, and it is live without a URL. When the
// packaged app is what loaded this page it exposes a bridge to a server it
// started itself, so there is nothing to name -- and no ?server= to type. The
// transport differs; nothing after this line does (see net_local.js).
const DESKTOP = hasLocalTransport()
const SERVER = params.get('server')
const LIVE = SERVER !== null || DESKTOP
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

// --- the loading bar --------------------------------------------------------
//
// WHY A BAR AND NOT JUST THE STATUS LINE. Loading a Doom map fetches the
// simulation core, a map over the control channel, several hundred wall
// textures and a few thousand sprite frames. The status line said which step it
// was on and nothing about how far through it was, so a slow texture pass and a
// hung one looked identical -- and the wait is long enough that the difference
// matters to whoever is staring at it.
//
// WEIGHTED, not one bar per step. The phases take wildly different times, so a
// bar that gave each an equal share would crawl through textures and then jump.
// The weights are rough measurements, not guesses at importance.
//
// MONOTONIC by construction: each phase reports its own 0..1 and the total is
// the weighted sum, so nothing can go backwards even if a later phase reports
// before an earlier one finishes.
const loadingBox = document.getElementById('loading')
const loadingFill = loadingBox?.querySelector('.fill')
const loadingDetail = loadingBox?.querySelector('.detail')

const PHASES = [
  ['core', 1],       // the wasm module
  ['connect', 1],    // transport up, WELCOME received
  ['map', 2],        // the map, over the control channel
  ['geometry', 2],   // triangulation into meshes
  ['textures', 6],   // walls and flats -- the long one
  ['sprites', 3]     // prop and monster frames
]
const phaseDone = new Map(PHASES.map(([k]) => [k, 0]))
const phaseTotal = PHASES.reduce((n, [, w]) => n + w, 0)

/** Report a phase's completion, 0..1, and optionally what it is doing. */
function stage (key, frac, detail = null) {
  if (!loadingBox) return
  const clamped = Math.max(0, Math.min(1, frac))
  // Never backwards: a phase that reports 0.4 after 0.6 is a rounding artefact
  // of counting requests, not the load actually undoing itself.
  phaseDone.set(key, Math.max(phaseDone.get(key) ?? 0, clamped))

  // NOT `acc`: there is a module-scope `acc` (the frame accumulator) declared
  // hundreds of lines below, and shadowing it here reads as a temporal-dead-zone
  // bug to anyone scanning the file -- and to client/test/tdz.test.js, which
  // reported it.
  let weighted = 0
  for (const [k, w] of PHASES) weighted += (phaseDone.get(k) ?? 0) * w
  const pct = Math.round(100 * weighted / phaseTotal)
  loadingBox.style.display = 'block'
  loadingFill.style.width = `${pct}%`
  if (detail !== null) loadingDetail.textContent = detail
}

/**
 * Start a fresh load -- a map change is one.
 *
 * Without this the phases are still at 1 from the last map, so the bar opens at
 * 100% and never moves. `stage()` is deliberately monotonic, which is right
 * within one load and wrong across two.
 */
function stageReset (keys) {
  for (const k of keys) phaseDone.set(k, 0)
}

// --- the health bar ---------------------------------------------------------
//
// The snapshot has carried `hp` since Phase 4 (proto.js reads it into
// snap.hp) and nothing on the client had ever looked at it: the only way to
// see your own health was the debug panel's slider, which is a development
// tool and shows the value it last SET rather than the value the server has.
//
// LIVE ONLY. Solo runs the core with no server and no Lua, so there is nothing
// keeping hp -- a bar sitting at whatever the field defaults to would be a
// number that means nothing.
const healthBox = document.getElementById('health')
const healthNum = healthBox?.querySelector('.num')
const healthFill = healthBox?.querySelector('.fill')
// The most health seen this session, which is what the bar is drawn against.
// The server does not send a maximum -- hp is one integer -- and a script can
// raise it, so the alternative is hardcoding 100 here and having the bar sit
// at a third full for anything that grants more.
let healthMax = 0

function showHealth (hp) {
  if (!healthBox || hp === undefined || hp === null) return
  if (hp > healthMax) healthMax = hp
  if (healthMax <= 0) return
  healthBox.style.display = 'block'
  healthNum.textContent = String(Math.max(0, hp))
  const frac = Math.max(0, Math.min(1, hp / healthMax))
  healthFill.style.width = `${(frac * 100).toFixed(1)}%`
  // Green, amber, red. Thresholds rather than a gradient: the point is to be
  // readable at a glance while something is shooting at you, and a continuous
  // hue ramp reads as "some colour" at every value.
  healthFill.style.background = frac > 0.6 ? '#5fbf6a' : frac > 0.25 ? '#d6a13a' : '#d65b5b'
}

/** Everything is in; put the bar away. */
function stageDone () {
  if (!loadingBox) return
  for (const [k] of PHASES) phaseDone.set(k, 1)
  loadingFill.style.width = '100%'
  loadingBox.style.display = 'none'
}

const LOAD_STATUS = ['Ok', 'BadMagic', 'BadVersion', 'Truncated', 'TooLarge',
                     'BadIndex', 'HashMismatch', 'TriangleMismatch']

say('loading simulation core...')
stage('core', 0, 'simulation core')
const core = await Core.load()
stage('core', 1)

// --- getting a map ---------------------------------------------------------
//
// Solo fetches one over HTTP; live receives one over the control channel. Both
// end up parsing the SAME bytes with the same loader, which is the property
// that makes client and server agree about geometry -- not a shared compiled-in
// copy, and not two parsers that are believed to match.

let mapBytes
let conn = null
let welcome = null
// Panels the server has opened on us. Not simulation state: never hashed, never
// predicted, and it survives nothing -- a reconnect starts with none.
let ui = null
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
  say(DESKTOP ? 'starting...' : `connecting to ${SERVER_URL}...`)
  // A development edge presents a self-signed certificate. Its hash is public
  // -- the server sends the certificate itself -- so serving the hash is not a
  // secret being leaked; the private key is never within reach of the dev
  // server's mounts.
  let certHash = null
  // The desktop shell serves no /certhash and needs none: its transport is an
  // IPC bridge with no TLS anywhere in it.
  try {
    if (DESKTOP) throw new Error('desktop: no certificate involved')
    const j = await (await fetch('/certhash')).json()
    certHash = Uint8Array.from(atob(j.sha256_base64), (c) => c.charCodeAt(0))
  } catch {
    // No hash available: fall through with none and let WebTransport apply the
    // ordinary trust rules, which is right for a real deployment.
  }

  const acc = new MapAccumulator()
  // One of two transports, chosen here and nowhere else. Both present the
  // same four methods and three callbacks, so everything below is written
  // once. LocalConnection ignores `url` and `certHash`.
  const Transport = DESKTOP ? LocalConnection : Connection
  conn = new Transport({
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
      // Panels (docs/protocol.md §11). Reliable channel, like everything that
      // must not be missed.
      else if (m.type === MSG.UI_OPEN) ui.open(m)
      else if (m.type === MSG.UI_CLOSE) ui.close(m.uiId)
      // A switch was thrown, here or before we arrived. Purely visual: what the
      // switch DID travels as sector motion, exactly as it did before.
      else if (m.type === MSG.SIDE_TEXTURE) {
        const sector = view.setSideTexture(m.side, m.field, m.name.toUpperCase(),
                                           { pxPerUnit: params.has('texpx') ? Number(params.get('texpx')) : (map.textureScale ?? 32) })
        if (sector >= 0) {
          mapData.sides[m.side] = view.map.sides[m.side]
          view.setDynamicSectors([...new Set([...movers, sector])])
          view.rebuildDynamic()
        }
      }
      else if (m.type === MSG.UI_SET) ui.set(m.uiId, m.widgetId, m.value)
      // How the camera should follow (design §4.1). The server never says where
      // the camera IS -- only how it behaves -- so this is applied to the
      // client's own camera and nothing about it is predicted or replicated
      // back.
      else if (m.type === MSG.CAMERA_SET) {
        for (const f of m.fields) {
          const name = CAMERA_FIELD[f.id]
          if (!name) continue          // a setting this client does not know
          if (name === 'distance') camOverrideDistance = f.value
          else if (name === 'pitch') camOverridePitch = f.value
          else camera.cfg[name] = f.value
        }
      }
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
  // Panels, and the way back. A UI event is a reliable control message: a
  // click that arrived late still bought the thing, and one that arrived never
  // is a player pressing a button that does nothing.
  ui = new UiView({
    onEvent: (uiId, widgetId, value) => conn.sendControl(encodeUiEvent(uiId, widgetId, value))
  })

  await conn.open()
  stage('connect', 0.5, 'connected; waiting for the server')
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
    if (welcome) {
      stage('connect', 1)
      // Byte-accurate, because the accumulator knows both numbers. This is the
      // one phase whose progress is a real measurement rather than a count of
      // requests.
      stage('map', acc.total ? acc.received / acc.total : 0,
            `map ${acc.received} / ${acc.total} bytes`)
    }
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
stage('geometry', 0, 'building geometry')
let view = new MapView(mapData)
stage('geometry', 1)
scene.add(view.root)
// Only these sectors are rebuilt per frame; everything else is built once.
if (movers.length) view.setDynamicSectors(movers)

// Real texture images, if this client has them. Not awaited: the map is already
// on screen with generated placeholders, and each real texture replaces one as
// it arrives.
// Every frame of every sequence, so the animator has something to swap
// between: the frames after the first appear in no map's texture list.
// Animated textures -- nukage, water, lava (render/animtex.js). Client only:
// which picture is on a wall changes nothing the simulation can observe.
//
// DECLARED AND FETCHED HERE, above the texture load that reads it. It sat two
// hundred lines below, which is a temporal dead zone by way of a function call:
// loadMapTextures() is invoked at module scope and reads animCatalogue, so the
// `let` had not been initialised and the call threw before it ever reached
// view.loadTextures. The whole map rendered untextured, and the only sign was
// one unhandled rejection in the console.
let animCatalogue = { groups: [], warnings: [] }
let animator = null
try {
  animCatalogue = parseAnimations(await (await fetch('/animations.txt')).text())
  for (const w of animCatalogue.warnings) console.warn(`[anim] ${w}`)
} catch (e) {
  // Not fatal, and the failure is the right one: everything draws its first
  // frame and nothing moves.
  console.warn(`[anim] no animation catalogue (${e.message}); textures will be still`)
}

async function loadMapTextures () {
  const extra = animCatalogue.groups.flatMap((g) => g.frames)
  // The map's own scale unless a flag overrides it. ?texpx= stays for
  // experimenting; what it no longer does is decide the answer.
  const pxPerUnit = params.has('texpx') ? Number(params.get('texpx')) : (map.textureScale ?? 32)
  const res = await view.loadTextures({
    pxPerUnit,
    extra,
    onProgress: (done, total) => stage('textures', total ? done / total : 1,
                                       `textures ${done} / ${total}`)
  })
  animator = new TextureAnimator({
    groups: animCatalogue.groups,
    names: mapData.textures,
    materials: view.materials,
    frameTexture: (n) => view.frameTextures?.get(n) ?? null
  })
  if (animator.count > 0) console.log(`[anim] ${animator.count} animated textures`)
  return res
}
// KEPT, and awaited before play begins.
//
// This used to be fired and forgotten, deliberately: the map was already on
// screen with generated placeholders and each real texture replaced one as it
// arrived. That is the right trade when there is no loading screen -- something
// to look at beats a blank page -- and the wrong one when there is, because
// "loaded" then means "the pictures are actually here" rather than "they are on
// their way".
const texturesReady = loadMapTextures()
  .then((r) => {
    if (r?.loaded) console.log(`[render] ${r.loaded}/${r.total} textures loaded from /textures`)
    return r
  })
  // Reported, not swallowed. This threw for two hundred lines' worth of
  // declaration order and the map simply had no textures on it; an unhandled
  // rejection in a console nobody was reading is not a report.
  .catch((e) => { say(`textures failed to load: ${e.message}`); return null })

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
const SHOW_SWING = params.get('swing') !== '0'

// The two wedges, REBUILT whenever the shape changes.
//
// Built once at startup they showed the weapon the session began with, so every
// attack parameter looked inert -- which is what a debug panel full of sliders
// that appear to do nothing feels like. These are replicated parameters and a
// script may change any of them at any moment, so the shape is re-read each
// frame and the geometry replaced when it actually differs.
let swingShape = null
let swingSpan = 0
let swingMid = 0
let swingWhole = null
let swing = null
// The offset most recently drawn, for the HUD. Null when nothing is swinging.
let swingLastOff = null

function rebuildSwing (shape) {
  if (!SHOW_SWING) return
  if (swingWhole) { scene.remove(swingWhole); swingWhole.geometry.dispose() }
  if (swing) { scene.remove(swing); swing.geometry.dispose() }
  // The region the collider ACTUALLY sweeps, not the one the parameters
  // describe. See sweptSpan: the declared endpoints are angles the swing never
  // occupies, so a region drawn from them is a wedge the red can never fill.
  const swept = sweptSpan(shape)
  swingSpan = swept.half
  swingMid = swept.mid
  swingWhole = makeSwingMesh(THREE, {
    reachUnits: shape.reach, innerUnits: shape.inner,
    halfArcTurns: swingSpan, segments: 24, faint: true
  })
  swing = makeSwingMesh(THREE, {
    reachUnits: shape.reach, innerUnits: shape.inner, halfArcTurns: shape.halfArc
  })
  scene.add(swingWhole)
  scene.add(swing)
  swingShape = shape
}
rebuildSwing(readSwingShape(world, agent))
// The path the swing swept, one segment per tick. See SwingTrail: the moving
// collider alone crosses and is gone inside a sixth of a second, so where it
// has BEEN is the part you can actually look at.
const swingTrail = SHOW_SWING ? new SwingTrail({ THREE, scene }) : null

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
    for (const st of def.states.values()) {
      for (const f of st.frames ?? []) {
        // A rot8 frame names eight files, not one -- the catalogue gives the
        // base and the renderer appends the view. Loading only the base would
        // leave every sprite magenta, which is what a missing texture looks
        // like and not at all what a missing ROTATION looks like.
        if (def.rot8) { for (let r = 1; r <= 8; r++) wanted.add(`${f}${r}`) }
        else wanted.add(f)
      }
    }
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
    // Counted, so the bar keeps moving through the longest phase on a Doom
    // map: a few thousand sprite frames, each its own request.
    let spritesDone = 0
    const spritesTotal = wanted.size ?? [...wanted].length
    stage('sprites', 0, `sprites 0 / ${spritesTotal}`)
    await Promise.all([...wanted].map((n) => load(n).then(() => {
      stage('sprites', spritesTotal ? ++spritesDone / spritesTotal : 1,
            `sprites ${spritesDone} / ${spritesTotal}`)
    })))
  }
  // ThreeSprite sheets. A kind with an `atlas` line draws from one packed image
  // and a manifest instead of a PNG per frame per angle, so each sheet is one
  // request for the JSON, one for the image, and ONE material shared by every
  // entity that uses it -- where the per-frame path gives each entity its own.
  //
  // A sheet that fails to load is not fatal: the registry says so once and the
  // entity falls back to the default body, which is visibly wrong rather than
  // invisible. A monster that draws nothing becomes a bug report about
  // collision; one that draws a capsule becomes a bug report about art.
  const atlases = new Map()
  const sheets = new Set()
  for (const def of catalogue.kinds.values()) if (def.type === 'atlas' && def.sheet) sheets.add(def.sheet)
  if (sheets.size) {
    const sheetLoader = new THREE.TextureLoader()
    await Promise.all([...sheets].map(async (name) => {
      try {
        const res = await fetch(`/sprites/${name}.json`)
        if (!res.ok) throw new Error(`${res.status} fetching the manifest`)
        const atlas = parseAtlas(await res.json(), `${name}.json`)
        // The manifest states the feet twice. If the two disagree the sheet was
        // written by something that is not the exporter, and every sprite on it
        // stands at the wrong height -- which reads as a content mistake.
        const drift = pivotDisagreement(atlas)
        if (drift > 1e-6) console.warn(`[props] sheet ${name}: its two statements of the feet differ by ${drift}`)
        const texture = await new Promise((resolve, reject) => {
          sheetLoader.load(`/sprites/${atlas.image}`, resolve, undefined,
                           () => reject(new Error(`cannot load ${atlas.image}`)))
        })
        texture.magFilter = THREE.NearestFilter
        texture.minFilter = THREE.NearestFilter
        texture.generateMipmaps = false
        texture.colorSpace = THREE.SRGBColorSpace
        atlases.set(name, {
          atlas,
          texture,
          material: new THREE.MeshBasicMaterial({
            map: texture, transparent: true, alphaTest: 0.5, side: THREE.DoubleSide
          })
        })
      } catch (e) {
        console.warn(`[props] sheet ${name} not loaded (${e.message}); kinds using it draw the fallback body`)
      }
    }))
    console.log(`[props] ${atlases.size} of ${sheets.size} sprite sheet(s) loaded`)
  }

  props = new PropRegistry({ THREE, textures, catalogue, atlases })
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
  onBody: (index, kind, pos, solid) => {
    if (myAgentIndex !== null && index === myAgentIndex) return
    // A corpse is drawn and not walked into. Without this the client keeps
    // colliding with something the server walks straight through, every tick
    // it is standing there -- which is a misprediction that looks exactly like
    // an invisible wall where a monster died.
    if (!solid) { world.removeProp(MIRROR_BASE + index); return }
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
  // A MAP CHANGE IS A LOAD, and it has to end the way the first one does.
  //
  // stageDone() ran once at startup and nothing closed the bar afterwards, so
  // walking through an exit reopened it -- loadMapTextures calls stage() -- and
  // left it on screen for the rest of the session. Reported exactly that way.
  //
  // Only the phases a map change actually redoes are reset: the core and the
  // connection are not reloaded, and resetting them would show the bar
  // rewinding through work nobody is waiting for.
  // A NEW MAP IS A NEW TRACK, crossfaded rather than cut: an exit should not
  // punch a hole in the soundtrack, and the synth can hold both songs long
  // enough to blend them. music.play() is a no-op when the track is the same,
  // so two maps sharing one song do not restart it at the door.
  playMapMusic(map.hash, 1200)

  stageReset(['map', 'geometry', 'textures'])
  stage('map', 1)
  stage('geometry', 1)
  loadMapTextures()
    .catch((e) => say(`textures failed to load: ${e.message}`))
    .finally(() => stageDone())

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
// Set by CAMERA_SET, or null for "whatever the zoom implies". Kept apart from
// camera.cfg because distance and pitch describe where the camera sits rather
// than how it follows, and the offset is recomputed from them every frame as
// yaw and zoom change.
let camOverrideDistance = null
let camOverridePitch = null
const camDistance = () =>
  camOverrideDistance ?? distanceFor(cam3, viewHeight, DEFAULT_DISTANCE)

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
// Geometry rebuilds per second, sampled over the same window as fps.
//
// THE NUMBER THAT MAKES THE GUARD VISIBLE. A rebuild is the expensive thing the
// renderer does -- derive surfaces, merge by texture, allocate attribute buffers,
// upload them -- and the only honest way to show it is not being done needlessly
// is a counter that reads 0 while nothing moves. Standing still in a level whose
// doors have all been opened reads 0; remove the guard in the frame loop and it
// reads whatever the frame rate is.
let buildsAt = 0
let rebuildRate = 0
// `?perf=1` -- one parseable line a second, so a run on real hardware can be
// pasted into a commit message. There is no headless GPU here and a software
// rasteriser measures a differently-shaped frame, so a recorded human run is the
// honest baseline rather than a harness that would name culprits this machine
// does not have.
const PERF_LOG = params.get('perf') === '1'
let perfAt = last
let perfLogged = false
let pruneAt = last
let inputHistory = []
let newestServerTick = 0

// Where the frame's time actually goes, so this is measurable on real hardware
// rather than argued about. Smoothed, because a single frame's numbers are
// noise and the question is always "what is this costing on average".
//
// `follow` IS SEPARATE FROM `mesh` ON PURPOSE. They used to be one figure
// covering the wasm height reads and the geometry rebuild together. Now that
// the rebuild is skipped on frames where nothing moved, a combined figure would
// sit near zero and "costs nothing" would be indistinguishable from "did not
// happen" -- and a measurement that names the mesh builder has to be measuring
// the mesh builder. `follow` is the reads, which happen every frame regardless.
const cost = { frame: 0, sim: 0, follow: 0, mesh: 0, draw: 0 }
const blend = (k, v) => { cost[k] += (v - cost[k]) * 0.1 }

// The newest health the server has told us. Kept here rather than read from a
// retained snapshot object, because drainSnapshots consumes them.
let latestHp = null

function drainSnapshots () {
  while (pendingSnapshots.length) {
    const s = pendingSnapshots.shift()
    if (s.hp !== undefined) latestHp = s.hp
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
  const tFrame = performance.now()
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

  let fresh = false
  if (LIVE) {
    // Motions declared since the last frame -- a door someone just opened.
    while (motionSets.length) fresh = takeMotion(motionSets.shift()) || fresh
    drainSnapshots()
  }

  // Moving sectors changed the geometry, so the mesh follows -- for the movers
  // and their neighbours only. See MapView.setDynamicSectors.
  //
  // THE HEIGHTS ARE COPIED BEFORE THE SPLIT IS RE-DECLARED, not after, and the
  // order matters for one frame in the life of every door: a sector that
  // becomes a mover is already in motion when we hear about it, so building it
  // from the baked heights draws it shut for a frame before the next frame
  // corrects it.
  const tFollow = performance.now()
  const moved = movers.length > 0 && followMovers(movers, liveSector, mapData.sectors)
  blend('follow', performance.now() - tFollow)

  // AND THE REBUILD IS GUARDED ON MOVEMENT, not on there being movers at all.
  // `movers` never shrinks (takeMotion), so the unguarded form rebuilt every
  // frame for the rest of the map once any door had opened once -- see
  // mapmovers.js, which is where that reasoning lives.
  const tMesh = performance.now()
  if (fresh) view.setDynamicSectors(movers)   // rebuilds both halves itself
  else if (moved) view.rebuildDynamic()
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
  if (SHOW_SWING) {
    // The shape as it is NOW, and a rebuild BEFORE anything is drawn with it.
    // A parameter change arrives as a PARAM_UPDATE and is applied to the
    // client's own world, so this is where the debug panel's sliders become
    // visible -- and doing it after setting `visible` would leave the freshly
    // built mesh hidden for a frame.
    const nowShape = readSwingShape(world, agent)
    if (!sameSwingShape(nowShape, swingShape)) rebuildSwing(nowShape)

    // The engine's own numbers, run through the engine's own formula. See
    // render/swing.js: the offset is a turn fraction from the aim.
    const off = sweepOffset({ ...swingShape, left: a.attackLeft })
    swingLastOff = off
    const facingTurns = (a.facing >>> 0) / 4294967296
    // Drawn at the middle of the body rather than at its feet: a collider
    // hugging the floor reads as unrelated to the thing it is about to hit.
    const at = subject.y + bodyHeight / 2

    // The faint region is drawn ALWAYS, not only mid-swing.
    //
    // It is the shape of the weapon, and the shape is what the parameters
    // change -- so showing it only during the 16 ticks of an actual swing means
    // dragging a reach slider does nothing observable unless you happen to be
    // attacking at that moment. Which is what "very hard to test for a human"
    // is. The bright collider still appears only while the swing is live,
    // because that one is about WHEN rather than about shape.
    swingWhole.visible = true
    swingWhole.position.set(subject.x, at - 0.02, subject.z)
    swingWhole.rotation.y = swingYaw(facingTurns, swingMid)

    swing.visible = off !== null
    if (off !== null) {
      swing.position.set(subject.x, at, subject.z)
      swing.rotation.y = swingYaw(facingTurns, off)
    }

    // One segment per TICK of the swing, at the angle the collider held then.
    // Offered every frame and added only when the countdown moves, so the trail
    // has as many segments as the swing had ticks whatever the display rate is.
    swingTrail.offer({
      shape: swingShape, left: a.attackLeft, offsetTurns: off, facingTurns,
      x: subject.x, y: at - 0.01, z: subject.z, now: performance.now() / 1000
    })
    swingTrail.update(performance.now() / 1000)
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

  // ANIMATED SURFACES ARE NEITHER A LIVE FEATURE NOR A PROP FEATURE, and used
  // to be treated as both: this sat inside `if (props)` inside `if (LIVE)`, so
  // nukage and lava never moved in solo mode at all, and stopped moving in a
  // live game if the prop CATALOGUE failed to fetch -- wall textures freezing
  // because a list of barrels did not arrive. Which picture is on a wall is
  // nothing the simulation can observe, which is why it is out here.
  //
  // One clock for every animated surface: two nukage pools out of phase read as
  // a bug nobody can name.
  if (animator) animator.update(now / 1000)

  if (LIVE) {
    others.update(newestServerTick, acc / DT)
    // performance.now(), NOT the frame's `now`. They are the same clock but not
    // the same instant: `now` is when the frame began, and the tracers being
    // faded here were spawned later in this same frame, from drainSnapshots.
    // Fading them against the earlier stamp gave them a negative age, which
    // used to mean "retire immediately". Out of `if (props)` for the same
    // reason as the animator: a tracer is not a prop.
    if (vfx) vfx.update(performance.now() / 1000)
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
      // The camera's position, so a sprite can choose which of its eight views
      // to show: the view depends on the angle between the thing's facing and
      // the direction to the viewer, and only the caller knows the second half.
      props.update(now / 1000, cam3.position)
      // Billboarding: yaw only, and derived rather than guessed. See
      // billboardYaw -- the first version had the sign of yaw backwards.
      const bb = billboardYaw(yaw)
      for (const ent of others.entities.values()) {
        if (!ent.kind) continue
        ent.mesh.rotation.y = bb
        // Which way the thing is POINTING, kept on the mesh for the eight-view
        // sprites to read. It cannot be recovered from the mesh's own rotation,
        // because that has just been overwritten to face the camera -- which is
        // what billboarding is.
        ent.mesh.userData.facingTurns = ((ent.facing ?? 0) >>> 0) / 4294967296
      }
    }
  }

  // The camera follows the PREDICTED position -- here, whichever of the two
  // modes produced it. Never a snapshot: see camera.js.
  // Pitch arrives in TURNS, which is the engine's angle unit everywhere else;
  // cameraOffset wants radians.
  camera.cfg.offset = cameraOffset(
    yaw, camDistance(),
    camOverridePitch === null ? undefined : camOverridePitch * Math.PI * 2)
  // The agent's facing, so the camera can aim AHEAD of it rather than at it.
  // facingDirection already returns three-space -- it is the same vector the
  // debug arrow is drawn along -- so converting again would negate z twice.
  const eye = camera.update(subject, dt, facingDirection(a.facing))
  cam3.position.set(eye.x, eye.y, eye.z)
  cam3.lookAt(eye.lookAt.x, eye.lookAt.y, eye.lookAt.z)

  const tDraw = performance.now()
  renderer.render(scene, cam3)
  blend('draw', performance.now() - tDraw)

  frames++
  if (now - fpsAt > 500) {
    const span = now - fpsAt
    fps = Math.round(frames * 1000 / span)
    rebuildRate = Math.round((view.builds - buildsAt) * 1000 / span)
    frames = 0; fpsAt = now; buildsAt = view.builds
  }

  // Forget the meshes of things that have died. `prune` existed from the start
  // and nothing ever called it, so `animated` grew for the life of the session
  // and the per-frame scan grew with it -- every corpse in the level still
  // being stepped over, sixty times a second, to be skipped.
  //
  // Once a second rather than per removal, because per removal would mean the
  // entity view knowing about the prop registry, and the two are deliberately
  // strangers. The list is small and the scan is cheap; the leak was the growth.
  if (props && now - pruneAt > 1000) { pruneAt = now; props.prune() }

  // What the frame actually costs, from the renderer rather than from a guess.
  const ri = renderer.info.render

  // Health, from the newest snapshot the server sent. Not predicted: damage is
  // a Lua decision and the client has no way to run it, so this is the server's
  // number and lags by exactly one snapshot -- which is the honest thing to
  // show.
  if (LIVE) showHealth(latestHp)

  // The synth's own numbers, when it has any. A background noise and a synth
  // that is struggling sound alike; steals, drops and queue depth are what
  // separate them, and the sample rate is there because the engine renders at a
  // fixed 48 kHz and does not resample.
  const musicLine = LIVE ? music.health() : null

  const st = predictor?.stats
  hud.textContent =
    `${LIVE ? 'LIVE' : 'solo'}  ${PROJECTION}  tick ${ticks}   ${fps} fps\n` +
    `pos  ${px.toFixed(2)} ${py.toFixed(2)} ${pz.toFixed(2)}\n` +
    `sector ${a.sector < 0 ? 'VOID' : a.sector}   ${a.grounded ? 'grounded' : 'airborne'}` +
    `${a.anchored ? `   riding sector ${a.anchorSector}` : ''}\n` +
    `jumps ${a.jumpsUsed}   movers ${movers.length}   textures ${mapData.textures.length}\n` +
    `draws ${ri.calls}   tris ${ri.triangles}   px ${renderer.getPixelRatio()}   ` +
    // Which half is which, and no allocation to read it: `view.meshes` builds a
    // combined array, which is fine once on a map change and wasteful sixty
    // times a second for a length.
    `meshes ${view.staticMeshes.length}+${view.dynamicMeshes.length}\n` +
    // Rebuilds per second, and the two counters that only ever go up if
    // something is leaking. renderer.info.memory is the renderer's own count of
    // what it is holding, not a guess from this side.
    `rebuilds/s ${rebuildRate}   geo ${renderer.info.memory.geometries}   ` +
    `tex ${renderer.info.memory.textures}` +
    `${props ? `   props ${props.animated.size}` : ''}\n` +
    // "of 16.7" is the budget the frame is being spent against, not a second
    // measurement -- it is the display's interval, derived from fps. Blank
    // until fps has a sample, because "of 1000.0" in the first half second
    // reads as a number rather than as a missing one.
    `frame ${cost.frame.toFixed(2)}ms${fps > 0 ? ` of ${(1000 / fps).toFixed(1)}` : ''}   ` +
    `sim ${cost.sim.toFixed(2)}   follow ${cost.follow.toFixed(2)}   ` +
    `mesh ${cost.mesh.toFixed(2)}   draw ${cost.draw.toFixed(2)}\n` +
    `vfx  seen ${vfxSeen}   live ${vfx ? vfx.live.length : 0}   ` +
    `pool ${vfx ? vfx.pool.length : 'NO VIEW'}\n` +
    // The swing as the renderer sees it. Three numbers rather than a
    // description, because "the arc does not move much" and "the arc is drawn
    // once and never updated" look identical from a chair and are told apart
    // by whether `left` counts down and `off` changes with it.
    `swing left ${a.attackLeft}   off ${swingLastOff === null ? '-' : swingLastOff.toFixed(3)}` +
    `   trail ${swingTrail ? swingTrail.count : '-'}\n` +
    // The weapon, in numbers. The wedge shows the shape; these show what it is
    // made of, which is what you read while dragging a slider to see whether
    // the value you set is the value the engine took.
    (swingShape
      ? `atk  reach ${swingShape.reach.toFixed(2)}` +
        (swingShape.inner > 0 ? `-${swingShape.inner.toFixed(2)}` : '') +
        `   arc ${(swingShape.halfArc * 2).toFixed(3)}t` +
        `   sweep ${swingShape.from.toFixed(3)}..${swingShape.to.toFixed(3)}` +
        `   ${swingShape.ticks}t/${swingShape.cooldown}cd`
      : '') +
    (vfxNoView ? `   dropped-no-view ${vfxNoView}` : '') +
    (falls ? `   falls ${falls}` : '') +
    (LIVE
      ? `\nothers ${others.count}   exact ${st.exactMatches}   misp ${st.mispredictions}` +
        `   starved ${st.starvedDivergences}   resync ${st.hardResyncs}`
      : '')
  // Inside frame(), where musicLine is. This sat after the closing brace,
  // which put it at module scope reading a const declared in the function --
  // "musicLine is not defined", on the first frame.
  if (musicLine) hud.textContent += `\nmusic ${musicLine}`

  // The capture. One line a second, comma-separated, headed once -- a minute of
  // this from a real GPU is a baseline somebody can paste into a commit message
  // and compare against, which "it felt slow" is not.
  if (PERF_LOG && now - perfAt >= 1000) {
    perfAt = now
    if (!perfLogged) {
      perfLogged = true
      console.log('[perf] t,fps,frame,sim,follow,mesh,draw,rebuilds/s,draws,tris,geo,tex,props,ents')
    }
    console.log(`[perf] ${(now / 1000).toFixed(0)},${fps},${cost.frame.toFixed(2)},` +
      `${cost.sim.toFixed(2)},${cost.follow.toFixed(2)},${cost.mesh.toFixed(2)},` +
      `${cost.draw.toFixed(2)},${rebuildRate},${ri.calls},${ri.triangles},` +
      `${renderer.info.memory.geometries},${renderer.info.memory.textures},` +
      `${props ? props.animated.size : 0},${LIVE ? others.count : 0}`)
  }

  // THE WHOLE FRAME, which is the number the others are fractions of.
  //
  // sim + follow + mesh + draw do not add up to it and are not meant to: entity
  // interpolation, billboarding, shadows, the camera and this HUD are all real
  // frame cost and none of them is timed separately. The gap between this and
  // the sum is where any unaccounted work is hiding, which is the point of
  // showing a total rather than only the parts somebody thought to measure.
  //
  // Measured to the END of the frame, so the HUD is displaying the PREVIOUS
  // frame's total -- the current one is not over yet. Against 16.7 ms at 60 Hz
  // this says how much headroom there is: a frame doing 4 ms of work at 60 fps
  // is waiting for the display, not struggling.
  blend('frame', performance.now() - tFrame)
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

// EVERYTHING IN, THEN PLAY.
//
// The last thing waited on is the textures, which is the only asset load that
// is not already awaited above. A failure here is not fatal -- the map renders
// with placeholders and said so -- so this waits for the promise to settle
// rather than for it to succeed.
await texturesReady
stageDone()

// --- music -------------------------------------------------------------------
//
// Armed, not started. A browser refuses to open an AudioContext until the
// player has interacted with the page, so this waits for the first key or
// click; the track chosen below is remembered until then rather than dropped.
//
// Deliberately after the loading bar has gone: the synth's first act is to
// fetch a megabyte of baked track, and doing that while the textures are still
// arriving would slow down the thing the player is actually waiting for.
const music = new Music({ log: (m) => console.log(m) })
// Exposed for the page smoke test, which has no other way to ask whether the
// synth came up -- the same reason the editor's page test reaches into its app.
window.__music = music
music.arm()

// WHICH TRACK BELONGS TO THIS MAP, by content hash.
//
// The only identity the client has for a map is its hash: WELCOME carries one
// and this client computes the same number and compares them a few hundred
// lines up. There is no level NAME on the wire, and adding one would be a
// protocol change to answer a question already answerable.
//
// The manifest is generated by tools/wad/music_all.js beside the tracks, so a
// module ships its soundtrack and its map-to-track table together. A REBAKE
// CHANGES EVERY HASH, so a map with no entry is the normal way this goes stale
// -- it plays nothing and says which hash was missing, rather than failing.
let musicIndex = null
async function musicFor (hash) {
  if (musicIndex === null) {
    musicIndex = await fetch('/music/index.json').then((r) => r.json()).catch(() => ({}))
  }
  const key = hash.toString(16)
  const cues = musicIndex[key]
  if (!cues) {
    console.log(`[music] no track for map ${key} (re-run tools/wad/music_all.js after a rebake)`)
    return null
  }
  // A map's entry is a TABLE OF NAMED CUES, and `default` is where it starts.
  // Every DOOM II map has exactly one; a map with a quiet loop and a fight loop
  // has several and switches between them while you are in it.
  const out = {}
  for (const [name, file] of Object.entries(cues)) out[name] = `/music/${file}`
  return out
}

async function playMapMusic (hash, fadeMs = 0) {
  try {
    const cues = await musicFor(hash)
    if (!cues) return
    music.setCues(cues)
    // Only the starting cue is chosen here. Anything that changes it later is
    // the map's decision, which reaches the client over the wire -- and when
    // that message exists its handler is one call to music.cue().
    // `immediate`: a map change is not a musical transition. The new map's
    // track is a different piece with its own instruments, so it swaps rather
    // than waiting for a bar that belongs to the song being left.
    await music.cue('default', { fadeMs, at: 'immediate' })
  } catch (e) {
    // Not fatal, and said once. A game with no music is a game; a game that
    // fails to start because a track is missing is not.
    console.warn(`[music] ${e.message}`)
  }
}
playMapMusic(map.hash)

say('')
document.getElementById('help').style.display = 'block'
requestAnimationFrame(frame)
