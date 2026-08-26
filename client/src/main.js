//
// The browser client.
//
// Two modes, and the difference between them is exactly the design's central
// claim (§7.1, §9.3) made visible:
//
//   solo  the wasm core runs the simulation locally. No server, no network, no
//         Lua. What you see is the physics core alone.
//   live  the same wasm core PREDICTS while a real server is authoritative.
//
// Nothing in the renderer knows which mode it is in. It is handed a position
// each frame and draws it, because prediction's whole purpose is that the two
// look identical -- and if they ever stopped looking identical, the renderer is
// not where the fault would be.
//
import * as THREE from 'three'
import { Core } from './core.js'
import { readMap, fxToFloat } from './mapdata.js'
import { toThreeX, toThreeY, toThreeZ } from './mapgeom.js'
import { MapView, makeIsoCamera, resizeIsoCamera, makeAgentMesh } from './render/scene.js'
import { Camera, cameraOffset, screenRelativeMove } from './camera.js'

const TICK_HZ = 60
const DT = 1 / TICK_HZ

const params = new URLSearchParams(location.search)
const MAP_URL = params.get('map') ?? '/maps/test.dnpm'
const SPAWN = params.get('spawn') ?? 'start'
// View controls as URL parameters, so a particular framing can be reproduced --
// by a person comparing two builds, or by a headless render in a script.
const START_ZOOM = Number(params.get('zoom') ?? 44)
const START_YAW = Number(params.get('yaw') ?? 45) * Math.PI / 180
const PAUSED = params.get('paused') === '1'

const hud = document.getElementById('hud')
const status = document.getElementById('status')
const say = (m) => { status.textContent = m }

// --- boot ------------------------------------------------------------------

say('loading simulation core...')
const core = await Core.load()

say(`fetching ${MAP_URL}...`)
const bytes = new Uint8Array(await (await fetch(MAP_URL)).arrayBuffer())
const { ok, status: loadStatus, map } = core.loadMap(bytes)
if (!ok) {
  // Named, not numbered. A client that says "status 5" sends its user to read
  // the source; the loader already knows which of its checks refused the file.
  const names = ['Ok', 'BadMagic', 'BadVersion', 'Truncated', 'TooLarge', 'BadIndex',
                 'HashMismatch', 'TriangleMismatch']
  say(`map rejected: ${names[loadStatus] ?? loadStatus}`)
  throw new Error(`map rejected: ${names[loadStatus] ?? loadStatus}`)
}

const mapData = readMap(map)
const world = core.createWorld({ map })

// Sector motion, from the authored tag convention. On a real server this is
// Lua's business (§3.1.2) and arrives as SECTOR_MOTION_SET; solo mode has no
// Lua, so it reads the same tags the server's fallback used to, purely so a
// lift is moving while you look at it.
const movers = []
for (let i = 0; i < mapData.sectors.length; i++) {
  const tag = mapData.sectors[i].tag
  if (tag < 100) continue
  const rise = tag - 100
  if (world.applyMotionSet({
    sector: i, motionType: 1, motionTarget: 0,
    from: { x: 0n, y: 0n, z: 0n },
    to: { x: 0n, y: 0n, z: BigInt(Math.round(rise * 4294967296)) },
    startTick: 0, durationTicks: 240, loopMode: 2
  })) movers.push(i)
}

const spawn = map.spawnForTag(SPAWN)
const agent = world.spawn(spawn)

// --- scene -----------------------------------------------------------------

const renderer = new THREE.WebGLRenderer({ antialias: true })
renderer.setPixelRatio(Math.min(2, devicePixelRatio))
renderer.setSize(innerWidth, innerHeight)
renderer.setClearColor(0x0b0d12)
document.body.appendChild(renderer.domElement)

const scene = new THREE.Scene()
const view = new MapView(mapData)
scene.add(view.root)

const body = makeAgentMesh()
scene.add(body)

let viewHeight = START_ZOOM
const cam3 = makeIsoCamera(innerWidth / innerHeight, viewHeight)
const camera = new Camera()

addEventListener('resize', () => {
  renderer.setSize(innerWidth, innerHeight)
  resizeIsoCamera(cam3, innerWidth / innerHeight, viewHeight)
})
addEventListener('wheel', (e) => {
  viewHeight = Math.min(140, Math.max(8, viewHeight * (e.deltaY > 0 ? 1.1 : 1 / 1.1)))
  resizeIsoCamera(cam3, innerWidth / innerHeight, viewHeight)
}, { passive: true })

// --- input -----------------------------------------------------------------

const BUTTON = { JUMP: 1 << 0, DASH: 1 << 1, CLIMB: 1 << 2, HOVER: 1 << 3, FLIGHT: 1 << 4, GRAPPLE: 1 << 5, CROUCH: 1 << 6 }
const held = new Set()
addEventListener('keydown', (e) => { held.add(e.code); if (e.code === 'Space') e.preventDefault() })
addEventListener('keyup', (e) => held.delete(e.code))
addEventListener('blur', () => held.clear())

// Camera yaw, so the map can be looked at from any side. Movement is rotated to
// match: in an isometric view "up" means away from the camera, and a control
// scheme where it means +y regardless would make half the rotations unplayable.
let yaw = START_YAW

// The engine's move axes are quantised to Q1.15 by the SAME function the server
// decodes with (protocol R1). Even here, where there is no server to agree
// with, going through it keeps solo and live modes producing identical input
// from identical keys -- so a movement difference between them is never the
// input path.
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
  if (held.has('KeyE')) buttons |= BUTTON.CLIMB
  if (held.has('KeyC')) buttons |= BUTTON.CROUCH

  const aim = wx === 0 && wy === 0 ? 0 : Math.round((Math.atan2(wy, wx) / (Math.PI * 2)) * 4294967296) >>> 0
  return { buttons, moveX: axis(wx), moveY: axis(wy), aim }
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

function frame (now) {
  requestAnimationFrame(frame)
  const dt = Math.min(0.25, (now - last) / 1000)
  last = now

  if (held.has('KeyQ')) yaw -= 1.5 * dt
  if (held.has('KeyR')) yaw += 1.5 * dt

  acc += dt
  // Bounded, so a backgrounded tab does not return and run ten thousand ticks
  // in one frame. Dropping the missed time is the same choice the server makes
  // after a stall, and for the same reason.
  let budget = PAUSED ? 0 : 8
  while (acc >= DT && budget-- > 0) {
    world.setInput(agent, currentInput())
    world.step()
    acc -= DT
    ticks++
  }
  if (acc > DT) acc = 0

  // Moving sectors changed the geometry, so the mesh follows. Only the movers
  // and what looks at them are dirty, but the whole map is re-merged -- see
  // MapView.rebuildSectors for why that is the cheaper of the two shapes.
  if (movers.length) {
    for (const i of movers) {
      const s = mapData.sectors[i]
      const live = liveSector(i)
      s.floorZ = live.floorZ
      s.ceilZ = live.ceilZ
    }
    view.rebuildSectors(movers)
  }

  const a = world.readAgent(agent)
  const px = fxToFloat(a.pos.x)
  const py = fxToFloat(a.pos.y)
  const pz = fxToFloat(a.pos.z)
  const subject = { x: toThreeX(px), y: toThreeY(pz), z: toThreeZ(py) }

  body.position.set(subject.x, subject.y + body.userData.yOffset, subject.z)

  // The camera follows the PREDICTED position -- here, the locally simulated
  // one. Never a snapshot: see camera.js.
  camera.cfg.offset = cameraOffset(yaw)
  const eye = camera.update(subject, dt)
  cam3.position.set(eye.x, eye.y, eye.z)
  cam3.lookAt(eye.lookAt.x, eye.lookAt.y, eye.lookAt.z)

  renderer.render(scene, cam3)

  frames++
  if (now - fpsAt > 500) { fps = Math.round(frames * 1000 / (now - fpsAt)); frames = 0; fpsAt = now }
  hud.textContent =
    `tick ${ticks}   ${fps} fps\n` +
    `pos  ${px.toFixed(2)} ${py.toFixed(2)} ${pz.toFixed(2)}\n` +
    `sector ${a.sector}   ${a.grounded ? 'grounded' : 'airborne'}` +
    `${a.anchored ? `   riding sector ${a.anchorSector}` : ''}\n` +
    `jumps ${a.jumpsUsed}   movers ${movers.length}   textures ${mapData.textures.length}`
}

// The live floor/ceiling of a sector, which sector motion has written back into
// the wasm store. Read from there rather than recomputed in JS: recomputing
// would be a second implementation of parametric_offset, and the two would
// disagree the first time either changed.
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
