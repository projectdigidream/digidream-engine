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
import { Core, WEAPON_ACTION } from './core.js'
import { RenderClock } from './render/renderclock.js'
import { GhostMissiles } from './render/ghostmissiles.js'
import { KnockLayer, shoveOf } from './render/knock.js'
import { NearPresent, presentWeight } from './render/nearnow.js'
import { readMap, fxToFloat, readPaths, refreshPathPositions, pathIndexForTag, effectFrom, SIDE_FLAG } from './mapdata.js'
import { followMovers } from './mapmovers.js'
import { toThreeX, toThreeY, toThreeZ } from './mapgeom.js'
import {
  MapView, makeCamera, resizeCamera, distanceFor, makeAgentMesh, PROJECTIONS,
  makeFacingArrow, aimFacingArrow
} from './render/scene.js'
import { Sky, skyFromMessage } from './render/sky.js'
import {
  Camera, CAM_MODE, cameraOffset, screenRelativeMove, facingDirection, aimFor, billboardYaw,
  viewYaw, between,
  DEFAULT_DISTANCE
} from './camera.js'
import { buildSpline } from './spline.js'
import { PathView } from './render/paths.js'
import { stateFor, resolveState, deriveRemote } from './render/animstate.js'
import { SCREEN_FIELD } from './proto.js'
import { EntityView } from './render/entities.js'
import { ShadowCaster } from './render/shadows.js'
import { Connection } from './net.js'
import { LocalConnection, hasLocalTransport } from './net_local.js'
import { Predictor } from './predictor.js'
import { parseProps, PropRegistry } from './render/props.js'
import { parseAtlas, pivotDisagreement } from './render/atlas.js'
import { sweepOffset, pitchOffset, makeSwingMesh, aimSwing, readSwingShape,
         sameSwingShape, sweptSpan, SwingTrail } from './render/swing.js'
import { parseVfx, VfxView, tracerEnds } from './render/vfx.js'
import { SectorFx } from './render/sectorfx.js'
import { UiView } from './render/ui.js'
import { wantsTouch } from './touch.js'
import { mountTouch } from './render/touchui.js'
import { parseAnimations, TextureAnimator } from './render/animtex.js'
// The ONE button table. main.js used to keep its own, where CROUCH was 1<<6 --
// which is proto's INTERACT bit, so pressing C sent "use".
import { BUTTON, NO_WEAPON, viewByte } from './proto.js'
import { MapAccumulator } from './maploader.js'
import { Neighbourhood } from './seam/neighbourhood.js'
import { buildCell } from './seam/buildcell.js'
import { report, CrossingProbe, BodyWatch } from './seam/diag.js'
import { Music } from './audio/music.js'
import { RemoteParams, isOwnParams } from './remoteparams.js'
import { SlashView, parseSwingStyles, styleFor } from './render/slash.js'
import { Typewriter } from './render/typewriter.js'
import { musicKeyFor } from './audio/musickey.js'
import { MSG, encodeUiEvent, encodeGameSave, encodeGameLoad, encodeGameNew, CAMERA_FIELD } from './proto.js'
import { SOUND_FOLLOW, SOUND_PREDICTED } from './proto.js'
import { Sfx } from './audio/sfx.js'
import { OwnSounds } from './audio/ownsounds.js'
import { SCHEME, FpsLook, fpsPush, TOUCH_SENSITIVITY } from './controls.js'
import { ViewModel } from './render/viewmodel.js'
import { HudView, parseLayout, layoutFileFor, DEFAULT_LAYOUT, UiAssets } from './render/layout.js'
import { soundHash } from './proto.js'

const TICK_HZ = 60
const DT = 1 / TICK_HZ

const params = new URLSearchParams(location.search)
const AUTOWALK = params.has('autowalk')
// +1 east, -1 west. Flipped by enterLevel under ?autowalk=shuttle.
let autowalkDir = 1

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

// --- the controls you can touch ----------------------------------------------
//
// MOUNTED HERE, ahead of every `await` below, and that position is the whole
// point. The wasm core, the map fetch and the connection all come after this
// line and any of them can be slow or fail -- on a phone, with a server that
// is not there, that used to be a black rectangle with no way to tell whether
// the game had started. The controls now appear immediately and do nothing
// until there is something to drive, which is the honest order.
/**
 * The thumbstick and its buttons, on a device whose pointer is a finger.
 *
 * MOUNTED, NOT BRANCHED ON. Everything below asks the touch state for a push
 * and a set of buttons exactly as it asks the keyboard, and adds the two --
 * so a tablet with a keyboard uses both without a mode, and a desktop with
 * `?touch=1` gets the controls over a game that still answers to WASD. There
 * is no "mobile version" of the input path, which is the only way to be sure
 * the two do not drift.
 *
 * Null when the device has no coarse pointer. See wantsTouch: it asks the
 * BROWSER whether the primary pointer is a finger rather than sniffing a name,
 * because a name is wrong for whatever shipped last month.
 */
const touch = wantsTouch(params) ? mountTouch() : null
if (touch) {
  document.documentElement.classList.add('dnp-touch-on')
  // No "drag to turn / pinch to zoom" tip any more (the user's call): the
  // controls speak for themselves, and a module can put its own words up.
}

/** Which protocol bit each touch button holds down. */
const TOUCH_BITS = {
  jump: BUTTON.JUMP,
  dash: BUTTON.DASH,
  use: BUTTON.INTERACT,
  attack: BUTTON.ATTACK_PRIMARY,
  // The second trigger: shown by an FPS layout (render/layout.js touch_fps).
  alt: BUTTON.ATTACK_SECONDARY,
  // Held onto a climbable wall, where the module's rules say walls are held.
  grab: BUTTON.CLIMB
}

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
// THE DEBUG OVERLAY: the numbers block above and the controls legend. The
// module decides whether a game launches with it (module.txt `debug off`, told
// in WELCOME.debug); ?debug=0 or ?debug=1 on the page overrides the module
// either way. Hidden, the block is not written at all, not merely not shown.
const DEBUG_PARAM = params.get('debug')
let debugShown = DEBUG_PARAM !== '0'
let helpShown = false
function showDebug (on) {
  debugShown = on
  hud.style.display = on ? '' : 'none'
  if (!on) hud.textContent = ''
  const help = document.getElementById('help')
  if (help) help.style.display = on && helpShown ? 'block' : 'none'
}
showDebug(debugShown)
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

/** Everything is in; put the bar away. */
function stageDone () {
  if (!loadingBox) return
  for (const [k] of PHASES) phaseDone.set(k, 1)
  loadingFill.style.width = '100%'
  loadingBox.style.display = 'none'
}

const LOAD_STATUS = ['Ok', 'BadMagic', 'BadVersion', 'Truncated', 'TooLarge',
                     'BadIndex', 'HashMismatch', 'TriangleMismatch']

// --- music -------------------------------------------------------------------
//
// STARTED FIRST, before the level loads, so the music plays as soon as the
// level is known rather than after the last texture: the worklet, the synth's
// wasm and the instruments come up while the core, the map and the textures
// load, and the level's track starts the moment its name is (playMapMusic,
// called where the map is parsed). It once waited for the loading bar to go,
// so as not to compete with the textures; the player waited in silence.
//
// Armed, not forced. A browser refuses to open an AudioContext until the
// player has interacted with the page (the shells allow it from the start:
// the Android WebView and the desktop app); where it refuses, this waits for
// the first key or click, and the track chosen is remembered until then.
const music = new Music({ log: (m) => console.log(m) })
// Exposed for the page smoke test, which has no other way to ask whether the
// synth came up -- the same reason the editor's page test reaches into its app.
window.__music = music

// SOUND EFFECTS (client/src/audio/sfx.js), in the music's context once it has
// one, ducking the music under whatever the bank says deserves it.
const sfx = new Sfx({ log: (m) => console.log(m), onDuck: (milli) => music.duck(milli, 60) })
window.__sfx = sfx
music.onContext = (ctx) => sfx.attach(ctx).catch((e) => console.warn(`[sfx] ${e.message}`))
sfx.setBank('/sounds/bank.sfxproj')
music.arm()

// WHICH TRACK BELONGS TO THIS MAP, by content hash.
//
// The only identity the client has for a map is its hash: WELCOME carries one
// and this client computes the same number and compares them a few hundred
// lines up. There is no level NAME on the wire, and adding one would be a
// protocol change to answer a question already answerable.
//
// The manifest is generated by tools/wad/music_all.js beside the tracks, so a
// module ships its soundtrack and its map-to-track table together.
//
// Keyed by the level's NAME, which the server sends in WELCOME and HOME: see
// audio/musickey.js for the rule and why it is not the map's hash any more.
let musicIndex = null
async function musicFor (hash, name) {
  if (musicIndex === null) {
    musicIndex = await fetch('/music/index.json').then((r) => r.json()).catch(() => ({}))
  }
  const key = musicKeyFor(musicIndex, hash, name)
  const cues = key ? musicIndex[key] : null
  if (!cues) {
    console.log(`[music] no track for map ${name ?? hash.toString(16)}`)
    return null
  }
  // A map's entry is a TABLE OF NAMED CUES, and `default` is where it starts.
  // Every DOOM II map has exactly one; a map with a quiet loop and a fight loop
  // has several and switches between them while you are in it.
  const out = {}
  for (const [name, file] of Object.entries(cues)) out[name] = `/music/${file}`
  return out
}

async function playMapMusic (hash, name, fadeMs = 0) {
  try {
    const cues = await musicFor(hash, name)
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

say('loading simulation core...')
stage('core', 0, 'simulation core')
// THE SYNTH BEFORE THE LEVEL. Nothing of the level is fetched, and nobody joins
// a server, until the music engine is up and proven working (Music.whenUp: the
// worklet ready, the bank and the kit loaded, the audio thread rendering) -- so
// the first level's track starts with the level, never after it. The core
// loads meanwhile; it is not the level.
//
// Where the page may not make sound until the player acts (a browser; the
// shells allow it), it asks for a key or a tap first -- and the player is not
// yet in any world while it asks. A synth that fails is said on screen, and the
// game goes on without music rather than not at all.
const corePromise = Core.load()
{
  const root = document.documentElement
  const prompt = () => {
    root.dataset.music = 'blocked'
    say(touch ? 'Tap to start' : 'Press a key or click to start')
  }
  music.onBlocked = prompt
  if (music.blocked) prompt()
  else say('starting the music...')
  try {
    await music.whenUp()
    // And the sound effects, which load into the same audio thread: their
    // bank and its pre-rendered cache, done now rather than over the level's
    // first seconds. Bounded -- an effects engine that never answers is said
    // and left, not waited on for ever.
    const banked = await Promise.race([sfx.whenBank().then(() => true),
                                       new Promise((r) => setTimeout(() => r(false), 15000))])
    if (!banked) console.warn('[sfx] the sound bank did not load in 15 s; going on')
    root.dataset.music = 'up'
    say('')
  } catch (e) {
    root.dataset.music = 'failed'
    console.warn(`[music] ${e.message}`)
    say(`music did not start: ${e.message} -- playing without it`)
    await new Promise((r) => setTimeout(r, 3000))
  }
  music.onBlocked = null
}
const core = await corePromise
// Held from here to the level's first frame: the level's track is loaded while
// the level loads, and starts with it (Music.hold).
music.hold()
// OUT OF SIGHT, OUT OF EARSHOT: an app sent to the background or a tab hidden
// stops its sound, and gets it back where it was. The page's own visibility,
// and the Android shell's lifecycle (window.__dnpAppHidden), which says so
// whether or not the WebView reports it.
const appHidden = (hidden) => {
  if (hidden) music.suspend(); else music.resume()
  document.documentElement.dataset.audio = hidden ? 'suspended' : 'running'
}
document.addEventListener('visibilitychange', () => appHidden(document.hidden))
window.__dnpAppHidden = appHidden
stage('core', 1)

// --- getting a map ---------------------------------------------------------
//
// Solo fetches one over HTTP; live receives one over the control channel. Both
// end up parsing the SAME bytes with the same loader, which is the property
// that makes client and server agree about geometry -- not a shared compiled-in
// copy, and not two parsers that are believed to match.

let mapBytes
// --- the levels this client holds (docs/protocol.md §8.5) --------------------
//
// Level changes and the cells next door, IN THE ORDER THE SERVER SENT THEM.
//
// One queue, not one per kind of message. The server's messages mean what they
// mean relative to each other -- a move after a welcome is an offset from the
// new home, the same move before it is an offset from the old one -- and
// separate queues drained in a fixed order threw exactly that away.
//
// A welcome whose map follows is queued when its map is complete, not when it
// arrives; everything after it on the channel is after it here too.
//
// Drained at the top of the frame, because that is where the scene exists: this
// callback runs during the join, long before.
const seamEvents = []
// The map stream in progress, if any: a welcome's or a neighbour's. The channel
// is ordered, so there is at most one, and MAP_DATA belongs to it.
let stream = null
// The bytes the level underfoot was built from. When it is left they become a
// held level like any other (client/src/seam/neighbourhood.js).
let liveMapBytes = null
// Switches thrown, waiting for a frame. See the SIDE_TEXTURE case in onControl:
// that callback runs during the join, before the renderer exists.
const sideTextures = []
// Music cues, for the same reason. See the MUSIC case in onControl.
const musicCues = []
// THE MODULE'S OWN KEYS (KEYS; docs/KEYS.md): [{ name, code, label }], bit i
// of every frame's `actions` being key i. Read at input time, so the frame
// loop needs no queue.
let moduleKeys = []
let keyBits = {}
// The sky (docs/SKY.md), for the same reason: only the latest one matters.
let skyMessage = null
// Counters for the page tests (client/test/leak.test.js): how many times the
// world was handed over, how many snapshots arrived, and -- once the renderer
// exists -- what three.js says is still alive on the GPU. Declared up here
// because the handlers that count are installed before the renderer is made.
const dnpStats = { welcomes: 0, snapshots: 0, gpu: () => null }
// THE HUD'S SERVER-KNOWN VALUES (HUD_SET), as `hud.<name>`: the held weapon,
// its ammunition. What the view model draws from now and the HUD layout will
// (docs/UI_LAYOUT.md). Kept across level changes -- the server re-sends it.
const hudValues = new Map()
// A TYPED SUBTITLE (render/typewriter.js; the graphs' subtitle node, mode
// `typed`): while one is coming out, `hud.subtitle` holds what is shown so far
// and the whole line is the typewriter's. Started when the line or how it is
// typed arrives; the frame loop types it.
let typer = null
let typerSound = null
let typerSounds = 0
function subtitleArrived () {
  const full = hudValues.get('hud.subtitle')
  const speed = Number(hudValues.get('hud.subtitle_typing'))
  if (typeof full !== 'string' || !(speed > 0)) { typer = null; return }
  const now = performance.now() / 1000
  typer = new Typewriter({ text: full, speed, pack: hudValues.get('hud.subtitle_pack'), startedAt: now })
  typerSound = hudValues.get('hud.subtitle_sound') || null
  hudValues.set('hud.subtitle', typer.shownAt(now))
}
dnpStats.subtitle = () => ({ shown: hudValues.get('hud.subtitle') ?? null, typing: !!typer, sounds: typerSounds })
// THE CONTROL SCHEME, the module's (WELCOME.controls): a platformer's screen-
// relative push and turning body, or a first-person view that IS the aim
// (client/src/controls.js). Up here because the input and camera code below
// both read it from the first frame.
let scheme = SCHEME.PLATFORMER
// ?pitch= (degrees, up positive) starts a first-person view looking up or
// down: what a headless check needs to see a ceiling, having no pointer lock.
const fpsLook = new FpsLook({ pitch: Number(params.get('pitch') ?? 0) * Math.PI / 180 })
const isFps = () => scheme === SCHEME.FPS
window.__dnpStats = dnpStats
dnpStats.hud = () => Object.fromEntries(hudValues)
// The wasm core's whole memory, which only ever grows: a world or a map never
// destroyed shows up here and nowhere in the JS heap.
dnpStats.wasmBytes = () => (core.M.HEAPU8 ?? core.M.HEAP8)?.buffer?.byteLength ?? 0
// What this client plays on its own predicted actions (audio/ownsounds.js).
// Declared up here because SOUND_PREDICT arrives with the WELCOME, long before
// the sound engine further down exists.
const ownSounds = new OwnSounds()
// THE FIRST-PERSON WEAPON (render/viewmodel.js), drawn only in an FPS scheme.
// Its frames are the module's (sprites/weapons.json) and its pictures are
// placed by the sprite index's offsets; both fetched once, and a module without
// them simply has no gun on screen.
const viewModel = new ViewModel({ host: document.body, log: (m) => console.warn(m) })
// Our body's animation state as the server last said (the self block), for a
// weapon this client does not predict.
let lastSelfAnim = null

// THE LAYOUT (docs/UI_LAYOUT.md): the module's HUD and touch controls as data,
// or the built-in default, which is what index.html and touchui.js drew before.
// The health box that used to be hard-coded is drawn by the HUD now.
const hudView = new HudView({ host: document.body, touch: wantsTouch(params) })
let uiLayout = DEFAULT_LAYOUT
let uiIndex = null
const uiAssets = new UiAssets()
let uiLayoutFile = null
async function loadLayout (levelName) {
  try {
    if (uiIndex === null) {
      const r = await fetch('/ui/index.json')
      uiIndex = r.ok ? await r.json() : {}
    }
    const file = layoutFileFor(uiIndex, levelName)
    if (file === uiLayoutFile) return
    uiLayoutFile = file
    let layout = DEFAULT_LAYOUT
    if (file) {
      const r = await fetch(`/ui/${file}`)
      const parsed = parseLayout(r.ok ? await r.text() : '{}')
      // A layout's errors are SAID, on screen -- a phone has no console. What
      // failed was dropped and replaced by the default (parseLayout's rule),
      // and the rest of it is used: one bad font or style does not take a
      // module's whole HUD away (docs/UI_STYLES.md 3).
      if (!parsed.ok) say(`ui layout ${file}: ${parsed.errors.slice(0, 2).join('; ')} (that part is dropped)`)
      for (const w of parsed.warnings) console.warn(`[ui] ${file}: ${w}`)
      layout = parsed.layout
    }
    uiLayout = layout
    hudView.setLayout(layout)
    ui?.setLayout(layout)
    // Its fonts and skins: drawn with fallbacks at once, and again when they
    // are in, so the first frame never keeps the fallback for good.
    uiAssets.load(layout).then(() => {
      if (uiLayout !== layout) return
      hudView.setLayout(layout)
      ui?.setLayout(layout)
    })
    touch?.setLayout(layout, isFps() ? 'fps' : 'platformer')
  } catch (e) {
    console.warn(`[ui] layout: ${e.message}`)
  }
}
const WEAPON_GROUP = soundHash('weapon')
Promise.all([
  fetch('/sprites/weapons.json').then((r) => (r.ok ? r.json() : null)).catch(() => null),
  fetch('/sprites/index.json').then((r) => (r.ok ? r.json() : null)).catch(() => null)
]).then(([defs, index]) => { if (defs) viewModel.setDefs(defs, index) })
// Where the listener was last placed, for the sounds that are the CLIENT's own
// (a panel opening, a widget clicked): heard at the ear, not from anywhere.
let lastEar = { x: 0, y: 0, z: 0 }
function uiSound (name) {
  // The engine is made further down; a panel can open before it exists, and
  // that one is simply silent.
  try { sfx.play(name, lastEar) } catch {}
}
// The last input sampled, for the body watch (client/src/seam/diag.js).
let lastSampled = null

// --- the region (docs/protocol.md §8.7) -------------------------------------
//
// When the level shares its world with its neighbours, this client SIMULATES
// them too: its store holds every level it holds, each at its cell's origin,
// joined where they meet (geom::MapStorage::add_chunk) -- the same world the
// server steps. Walking from one level into the next is then walking between
// two rooms: the server says HOME and nothing is re-placed, re-predicted or
// re-based. Everything, body and camera and every view, is in WORLD coordinates.
//
// Outside a region (no metamap pitch) the home map is the whole world, its
// origin is zero, and all of this is the identity.
let region = false
let homeLevel = -1
// The home level's NAME, which is what its music is found by (WELCOME, HOME).
let homeName = null
let homeOrigin = { x: 0, y: 0 }          // engine units, for drawing
let homeOriginRaw = { x: 0n, y: 0n }     // raw fx, for placing chunks exactly
let simStore = null                      // the world's store: the region's, or the home map
const chunkMaps = new Map()              // level -> its own parse, for every level held (region)
const motionNames = new Set()            // every sector name a motion has been declared for
const sideTextureLog = new Map()         // level -> switches thrown in it, for when it is home
// Sector lights fading (SECTOR_SET with a fade): local sector -> { from, to,
// start, ms }. Kept in the rebuilt-every-frame half of the mesh while they run.
const lightFades = new Map()

let conn = null
let welcome = null
// Panels the server has opened on us. Not simulation state: never hashed, never
// predicted, and it survives nothing -- a reconnect starts with none.
let ui = null
// A MODAL panel (a layout template's `modal`) has the screen: the touch
// controls step aside and a first-person player gets the mouse back.
let uiModal = false
function panelsChanged () {
  const now = ui?.modal ?? false
  if (now === uiModal) return
  uiModal = now
  touch?.setBlocked(now)
  if (now && document.pointerLockElement) document.exitPointerLock?.()
}
// Why the transport closed, if it has. Read by the handshake loop, which must
// stop waiting for a reply that a closed connection can never deliver.
let closedWhy = null
// How a remote body is MOVING, measured from the two samples the interpolation
// already found rather than sent.
//
// A remote player carries no velocity on the wire: EntityBlock has a position
// and a facing and nothing about motion. So its locomotion is measured, which is
// allowed to be approximately right -- a rendering guess, and anything that has
// to be exact is declared by the server instead.
//
// Samples are in THREE space and DT apart in ticks, so the gap is converted once
// here. Fewer than two samples is a body that has just arrived.
const remoteCondition = (ent) => {
  const s = ent.samples
  if (!s || s.length < 2) return 'idle'
  const a = s[s.length - 2]
  const b = s[s.length - 1]
  const ticks = b.tick - a.tick
  if (!(ticks > 0)) return 'idle'
  return deriveRemote(a, b, ticks * DT, {})
}

// THE GAME ASKED TO QUIT (SCREEN_SET `quit`, a main menu's "Quit"). A studio
// play-test goes back to the studio, as its ◀ Studio link does; a shell with a
// quit closes the app (desktop, Android, the packager's runner); a plain tab
// cannot close itself -- nor may an iOS app -- so the screen goes black and
// says so. Once: a second request while leaving changes nothing.
let quitting = false
function quitGame () {
  if (quitting) return
  quitting = true
  dnpStats.quit = (dnpStats.quit ?? 0) + 1
  const back = document.getElementById('studio-back')
  if (back && !back.hidden) { location.href = back.href; return }
  if (typeof window.dnp?.quit === 'function') {
    try { window.dnp.quit(); return } catch (e) { say(`quit: ${e?.message ?? e}`) }
  }
  curtainEl.style.transitionDuration = '600ms'
  curtainEl.style.opacity = '1'
  const note = document.createElement('div')
  note.id = 'quit-note'
  note.textContent = 'Thanks for playing \u2014 you can close this tab.'
  note.style.cssText = 'position:fixed;inset:0;display:flex;align-items:center;justify-content:center;z-index:100;' +
    'color:#cfe3ff;font:600 18px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;text-align:center;padding:16px;pointer-events:none'
  document.body.appendChild(note)
}

// The cutscene overlay's three elements. Looked up once: SCREEN_SET can arrive
// on join, and querying the DOM per message would be the same hazard the
// camera's own settings are staged for.
const curtainEl = document.getElementById('curtain')
const lbTopEl = document.getElementById('lbtop')
const lbBotEl = document.getElementById('lbbot')
// The camera state CAMERA_SET writes, declared HERE, above the connection: the local transport hands over the messages a join already
// produced the moment the handler is registered, which is before this module
// has reached the camera code below. A `let` further down is in its dead zone
// then, and one assignment threw away the whole message. The Camera object
// itself is made later, so its settings wait in camCfgPending until it is.
let camOverrideDistance = null   // set by CAMERA_SET, or null for "whatever the zoom implies"
let camOverridePitch = null
// A map may lock the camera's yaw or zoom (CAMERA_SET lock_yaw, lock_zoom): a
// level made for one angle is not a level to be looked at from another.
let camLockYaw = false
let camLockZoom = false
let yaw = START_YAW
// Camera paths (design 4.1). The TAG, not an index: inserting a path earlier in
// a .map file renumbers every index after it, and would silently repoint every
// camera in the level. Resolved to a spline inside the frame loop, where the
// map certainly exists -- a level's CAMERA_SET arrives on join, possibly before
// the map is parsed and certainly before `camera` is made.
// WHERE THE CAMERA IS ACTUALLY LOOKING, as of the last frame it drew.
//
// Equal to `yaw` for a free camera and not equal to it on a path, which is the
// whole reason it exists -- see viewYaw. Held from frame to frame because the
// input is built before the camera runs, so the stick is mapped through the
// view the player was looking at when they pushed it.
let camViewYaw = 0
// The camera's angle as this client holds it, in degrees: what a page test
// reads to see a CAMERA_SET (a graph's camera node) arrive.
dnpStats.debug = () => ({ shown: debugShown, hud: hud.style.display !== 'none', text: hud.textContent.length })
dnpStats.camera = () => ({ yaw: yaw * 180 / Math.PI, viewYaw: camViewYaw * 180 / Math.PI })
// The home level's sector effects (render/sectorfx.js), and which map they are of.
let sectorFx = null
let sectorFxFor = null
// The home level's sector effects: what was built from the map.
dnpStats.sectorFx = () => sectorFx
  ? { counts: sectorFx.counts, fogs: sectorFx.fogCount, meshes: sectorFx.meshes.length, fx: mapData?.fx }
  : { none: true, fx: mapData?.fx ?? null }
// One sector's look as this client holds it: what a page test reads to see a
// SECTOR_SET arrive (its light, its pictures, its effects).
dnpStats.sectorLook = (i) => {
  const s = mapData?.sectors?.[i]
  if (!s) return null
  return { light: s.light, floor: view.map.textures[s.floorTex], ceiling: view.map.textures[s.ceilTex],
           effects: (mapData.fx ?? []).filter((e) => e.sector === i).map((e) => ({ kind: e.kind, ...e.params })),
           fogs: sectorFx?.fogCount ?? 0 }
}
let camPathTag = -1
let camPaths = []
let camPathSpline = null
let camPathIndex = -1
const camCfgPending = {}
let cameraReady = false
const motionSets = []
const pendingSnapshots = []
// Declared BEFORE the connection opens. Messages start arriving during the
// await inside open(), so a handler closing over a `const` declared further
// down would hit the temporal dead zone on the first PARAM_UPDATE -- which is
// to say, on a real server, immediately.
const paramUpdates = []
// The time the others are drawn at, in server ticks: smooth, steered by the
// snapshots rather than stepped by them (render/renderclock.js). Up here with
// the other things the join's callbacks may reach.
const renderClock = new RenderClock()
// The weapon table (WEAPON_DEFS, docs/WEAPON_PREDICTION.md): the newest bytes,
// and the world they were last handed to -- a level change builds a new world,
// which has to be handed them again.
let weaponDefs = null
let weaponDefsWorld = null
// A number key: the n-th weapon held, asked for in the next tick's input.
let weaponSlotWanted = 0
// What our predicted weapon did on each new tick since the last frame: its
// sounds, its tracers and effects, and whether the gun kicked.
const ownWeapon = []

// What the music was when the level began to load (the page test reads it).
document.documentElement.dataset.levelStart = document.documentElement.dataset.music ?? 'none'
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
        dnpStats.welcomes++
        scheme = m.controls ?? SCHEME.PLATFORMER
        if (DEBUG_PARAM !== '0' && DEBUG_PARAM !== '1') showDebug(m.debug !== 0)
        // The touch controls follow the scheme, and the layout the level.
        touch?.setLayout(uiLayout, scheme === SCHEME.FPS ? 'fps' : 'platformer')
        loadLayout(m.levelName ?? null)
        // The legend says the controls this scheme has.
        if (scheme === SCHEME.FPS) {
          const help = document.getElementById('help')
          if (help) help.textContent = 'click to look   WASD move   mouse look   LMB/F fire   RMB alt   SPACE jump   E use   Esc release'
        }
        if (!welcome) {
          // The join. The handshake below waits for this map in `acc`.
          welcome = m
          stream = { welcome: m, acc }
        } else {
          // A SECOND welcome is the server saying "you are in a different level
          // now". Whatever is still queued for the renderer is about the level
          // being left; everything from here on is about the one entered.
          motionSets.length = 0
          sideTextures.length = 0
          if (m.mapFollows) stream = { welcome: m, acc: new MapAccumulator() }
          else seamEvents.push({ welcome: m, bytes: null })
        }
      }
      // THE CELL NEXT DOOR. A selector, then that level's own MAP_DATA stream.
      else if (m.type === MSG.NEIGHBOUR_MAP) stream = { neighbour: m, acc: new MapAccumulator(), chunks: 0 }
      else if (m.type === MSG.NEIGHBOUR_DROP) seamEvents.push({ drop: m.level })
      // Still held, somewhere else. No geometry follows.
      else if (m.type === MSG.NEIGHBOUR_MOVE) seamEvents.push({ move: m })
      // The body walked into another level of the same world. In order, like
      // everything else about levels: the moves that follow are from there.
      else if (m.type === MSG.HOME) seamEvents.push({ home: m.level, name: m.levelName })
      // What became of a save or load the player asked for. Said on screen:
      // "saving is not enabled for this module" is an answer, not an error.
      else if (m.type === MSG.GAME_RESULT) {
        const what = m.op === 'new' ? 'new game' : `${m.op} slot ${m.slot}`
        say(`${what}: ${m.ok ? '' : 'refused -- '}${m.text}`)
        report(`[save] ${what}: ${m.ok ? 'ok' : 'refused'} -- ${m.text}`)
      }
      else if (m.type === MSG.MAP_DATA) {
        if (!stream) {
          report('[seam] VIOLATION: map data with no map announced', 'error')
        } else {
          stream.acc.push(m)
          stream.chunks = (stream.chunks ?? 0) + 1
          if (stream.acc.failed) {
            report(`[seam] map transfer failed: ${stream.acc.failed}`, 'error')
            stream = null
          } else if (stream.acc.complete) {
            const bytes = stream.acc.bytes()
            if (stream.neighbour) {
              report(`[seam] level ${stream.neighbour.level}: ${bytes.length} bytes in ${stream.chunks} chunk(s)`)
              seamEvents.push({ map: stream.neighbour, bytes })
            }
            else if (stream.welcome !== welcome) seamEvents.push({ welcome: stream.welcome, bytes })
            stream = null
          }
        }
      }
      else if (m.type === MSG.SECTOR_MOTION_SET) motionSets.push(m)
      else if (m.type === MSG.PARAM_UPDATE) paramUpdates.push(m)
      else if (m.type === MSG.WEAPON_DEFS) { weaponDefs = m.bytes; weaponDefsWorld = null }
      // Panels (docs/protocol.md §11). Reliable channel, like everything that
      // must not be missed.
      else if (m.type === MSG.UI_OPEN) { ui.open(m); uiSound('ui.open') }
      else if (m.type === MSG.UI_CLOSE) { ui.close(m.uiId); uiSound('ui.close') }
      // A switch was thrown, here or before we arrived. Purely visual: what the
      // switch DID travels as sector motion, exactly as it did before.
      // The map's music changes cue. The name is resolved here, from the
      // manifest playMapMusic already loaded for this map (docs/protocol.md
      // §11.1); a cue the map did not ship is said by music.cue, not fatal.
      // QUEUED for the same reason SIDE_TEXTURE is: `const music` is two
      // thousand lines below this callback, and this callback runs during the
      // join. A cue arriving then would have thrown the same ReferenceError.
      else if (m.type === MSG.MUSIC) musicCues.push(m)
      else if (m.type === MSG.SKY_SET) skyMessage = m
      else if (m.type === MSG.KEYS) {
        moduleKeys = m.keys
        keyBits = Object.fromEntries(m.keys.map((k, i) => [`key:${k.name}`, 1 << i]))
        touch?.setKeys(m.keys)
      }
      // What to play the moment this client predicts its own jump or shot.
      else if (m.type === MSG.SOUND_PREDICT) ownSounds.set(m.group, m.entries)
      else if (m.type === MSG.HUD_SET) {
        for (const f of m.fields) {
          // A clear reaches the HUD too: the frame loop only ever passes the
          // names still set, so a value it was never told was dropped would
          // stay on screen for good.
          if (f.kind === 2) { hudValues.delete(`hud.${f.name}`); hudView.update({ [`hud.${f.name}`]: undefined }); ui?.update({ [`hud.${f.name}`]: undefined }) }
          else hudValues.set(`hud.${f.name}`, f.value)
        }
        if (m.fields.some((f) => f.name === 'subtitle' || f.name === 'subtitle_typing')) subtitleArrived()
      }
      // QUEUED, like every other message that needs the renderer.
      //
      // This used to reach `view` directly from here -- and this callback runs
      // during the JOIN, two hundred lines before `let view` exists. A level
      // with no switch ever thrown sends none of these and the bug never fired;
      // a level somebody has played sends one per switch, on join, and the page
      // died with "can't access lexical declaration 'view' before
      // initialization" before it drew anything.
      //
      // Every neighbour of it was already a queue drained in the frame loop --
      // motion sets, parameter updates, welcomes. This was the one that was not.
      else if (m.type === MSG.SIDE_TEXTURE) sideTextures.push(m)
      // Sector looks share the switches' queue, and so their region bookkeeping.
      else if (m.type === MSG.SECTOR_SET) sideTextures.push(m)
      else if (m.type === MSG.UI_SET) {
        if (m.what === 'shown') ui.show(m.uiId, m.widgetId, m.value !== 0)
        else if (m.what === 'decor') ui.showDecor(m.uiId, m.decor, m.value !== 0)
        else ui.set(m.uiId, m.widgetId, m.value)
      }
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
          // Where the camera stands and whether the player may move it: the
          // map's choice (docs/protocol.md §11.1, fields 8..10). Yaw arrives in
          // turns, like pitch.
          else if (name === 'yaw') yaw = f.value * Math.PI * 2
          else if (name === 'lockYaw') camLockYaw = f.value >= 0.5
          else if (name === 'lockZoom') camLockZoom = f.value >= 0.5
          // Which path, by TAG. Held rather than resolved: this message can
          // arrive on join, before the map is parsed and certainly before the
          // Camera exists, so the frame loop does the lookup where both do.
          else if (name === 'path') camPathTag = Math.round(f.value)
          else if (name === 'shake' || name === 'shakeTime') { /* below */ }
          else if (cameraReady) camera.cfg[name] = f.value
          else camCfgPending[name] = f.value
        }

        // SHAKE IS EDGE-TRIGGERED, and it is the only field in this message
        // that is. Every other one is a setting, and re-sending it must change
        // nothing; a shake is an event, and re-sending it must shake again.
        //
        // Fired after the whole record is applied rather than while walking it,
        // so the order of the two fields inside one message does not matter --
        // a sender need not remember to put the duration first.
        const shake = m.fields.find((f) => CAMERA_FIELD[f.id] === 'shake')
        if (shake && cameraReady) {
          const t = m.fields.find((f) => CAMERA_FIELD[f.id] === 'shakeTime')
          camera.screenShake(shake.value, (t ? t.value : 300) / 1000)
        }
      }
      // The curtain and the bars (design §4.1). Pure presentation: applied to
      // the DOM and nothing else, never predicted, never sent back. The
      // TRANSITION carries the timing, so a two-second fade costs no per-frame
      // work and keeps running even if the tab throttles rAF.
      else if (m.type === MSG.SCREEN_SET) {
        let fadeMs = null
        let lbMs = null
        let fade = null
        let lb = null
        let quit = false
        for (const f of m.fields) {
          const name = SCREEN_FIELD[f.id]
          if (name === 'fade') fade = f.value
          else if (name === 'fadeMs') fadeMs = f.value
          else if (name === 'letterbox') lb = f.value
          else if (name === 'letterboxMs') lbMs = f.value
          else if (name === 'quit' && f.value) quit = true
        }
        // Duration first, then the value, or the browser animates from the old
        // duration -- the one case where the order of two fields in one record
        // does matter, and it is handled here rather than asked of the sender.
        if (fade !== null) {
          curtainEl.style.transitionDuration = `${Math.max(0, fadeMs ?? 0)}ms`
          curtainEl.style.opacity = String(Math.max(0, Math.min(1, fade)))
        }
        if (lb !== null) {
          const pct = `${Math.max(0, Math.min(50, lb * 100))}%`
          for (const el of [lbTopEl, lbBotEl]) {
            el.style.transitionDuration = `${Math.max(0, lbMs ?? 0)}ms`
            el.style.height = pct
          }
        }
        if (quit) quitGame()
      }
      else if (m.type === MSG.REJECT) say(`server rejected the connection (reason ${m.reason})`)
    },
    onSnapshot (s) { dnpStats.snapshots++; pendingSnapshots.push(s) },
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
    layout: uiLayout,
    onChange: () => panelsChanged(),
    onEvent: (uiId, widgetId, value) => {
      uiSound('ui.select')
      conn.sendControl(encodeUiEvent(uiId, widgetId, value))
    }
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
  liveMapBytes = mapBytes
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
// Camera paths, read once: their STRUCTURE never changes, only their point
// positions, which refreshPathPositions re-reads per frame for the one path the
// camera is actually on.
camPaths = readPaths(map)
region = LIVE && !!welcome?.region
homeLevel = welcome?.homeLevel ?? 0
// Solo has no WELCOME: the name is the map file's own.
homeName = welcome?.levelName ?? (LIVE ? null : MAP_URL.split('/').pop().replace(/\.dnpm$/, ''))
// The level is known: its music starts now, while the rest of it loads.
const firstMusic = playMapMusic(map.hash, homeName)
if (region) {
  homeOriginRaw = { x: welcome.origin.x, y: welcome.origin.y }
  homeOrigin = { x: fxToFloat(homeOriginRaw.x), y: fxToFloat(homeOriginRaw.y) }
  shiftPaths(camPaths)
  simStore = core.createStore()
  if (!simStore.addChunk(homeLevel, map, homeOriginRaw)) throw new Error('the home level does not fit')
  chunkMaps.set(homeLevel, map)
} else {
  simStore = map
}
let world = core.createWorld({ map: simStore })

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
  motionNames.add(m.sector)
  // Drawn moving only in the home level's view; the rest are simulated.
  const local = homeLocal(m.sector)
  if (local < 0 || movers.includes(local)) return false
  movers.push(local)
  return true
}

// A sector or side NAME (docs/protocol.md §8.6) as an index into the home
// level's own map, or -1 when it belongs to another level. Outside a region
// the name is the index.
function homeLocal (name) {
  if (!region) return name
  return (name >>> 16) === homeLevel ? (name & 0xFFFF) : -1
}

// A path's live points, re-read. NOT in a region: the level's own parse is not
// the one simulated there (the region's store is, and it carries no paths), so
// its paths hold still -- a path on a moving platform in a region level does
// not follow the platform. Known, and the place to fix it is add_chunk.
function refreshPath (p) {
  return region ? false : refreshPathPositions(map, p)
}

// A level's camera paths are in its own coordinates; the camera is in the world's.
function shiftPaths (paths) {
  if (!region || (homeOrigin.x === 0 && homeOrigin.y === 0)) return
  for (const p of paths) {
    for (let k = 0; k + 2 < p.pos.length; k += 3) { p.pos[k] += homeOrigin.x; p.pos[k + 1] += homeOrigin.y }
  }
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

  // A camera path, in solo mode, for the same reason ?movers= exists: there is
  // no CAMERA_SET without a server, so without this the whole feature cannot be
  // tried by hand at all -- and "does it feel right" is not a question a test
  // answers.
  //
  //   ?cam=track:1           keep the point on path tag 1 nearest the player
  //   ?cam=flythrough:2@6    ride path tag 2 at 6 units a second
  //   ?cam=auto:1@r8         attach to path tag 1 within 8 units, else follow
  //
  // Tags, not indices, exactly as the wire field is.
  const camSpec = params.get('cam')
  if (camSpec) {
    const m = /^(follow|track|flythrough|auto):(-?\d+)(?:@(r?)([\d.]+))?$/.exec(camSpec.trim())
    if (!m) {
      console.warn(`[solo] bad ?cam= -- want track:<tag>, flythrough:<tag>@<speed>, auto:<tag>@r<radius>`)
    } else {
      const [, mode, tagRaw, isRadius, numRaw] = m
      camPathTag = Number(tagRaw)
      // Into camCfgPending, NOT onto camera.cfg: this block runs while the
      // module is still setting up and `camera` is made a few hundred lines
      // below, so touching it here is the temporal dead zone this file's
      // comment at the top already records losing a message to.
      camCfgPending.mode = { follow: CAM_MODE.FOLLOW, track: CAM_MODE.TRACK,
                             flythrough: CAM_MODE.FLYTHROUGH, auto: CAM_MODE.AUTO }[mode]
      const n = numRaw === undefined ? null : Number(numRaw)
      if (n !== null) {
        if (isRadius) camCfgPending.pathAttach = n
        else camCfgPending.pathSpeed = n
      }
      // A usable default, so `auto:1` without a radius attaches to something
      // rather than silently behaving as FOLLOW.
      if (camCfgPending.mode === CAM_MODE.AUTO && !(camCfgPending.pathAttach > 0)) {
        camCfgPending.pathAttach = 8
      }
      console.log(`[solo] camera ${mode} on path tag ${camPathTag}` +
                  (camCfgPending.pathSpeed ? ` at ${camCfgPending.pathSpeed} u/s` : '') +
                  (camCfgPending.pathAttach ? ` within ${camCfgPending.pathAttach}u` : ''))
    }
  }
}

// Where the server put the body, in the world's coordinates -- the same point as
// the map's spawn for a level on its own, and the only right one in a region.
let spawn = (LIVE && welcome?.arrival) ? welcome.arrival : map.spawnForTag(SPAWN)
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
// Everybody else's parameters (remoteparams.js): what their swings are drawn
// with. A PARAM_UPDATE about another body goes here, never into our own.
const remoteParams = new RemoteParams({ defaultRaw: (id) => core.paramDefaultRaw(id), paramId: (n) => world.paramId(n) })
// This client's own handle, as the wire writes it: what a PREDICTED sound's
// `source` is compared with (docs/protocol.md §5.7).
let mySelfHandle = (LIVE && welcome) ? welcome.selfHandle >>> 0 : 0

// Where mirrored bodies live in this client's own world: the band of prop slots
// the engine reserves for exactly this. spawn_prop never allocates there, so a
// mirror can never land on a real prop.
//
// Asked of the engine rather than written down here. The last constant this
// client kept its own copy of was the agent buffer length, and the copy going
// stale cost a day and looked like a determinism bug.
const MIRROR_BASE = core.M._dnp_mirror_base()
const EYE_FRACTION = core.eyeFraction

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
    if (!isOwnParams(m, myAgentIndex)) { remoteParams.take(m); continue }
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
dnpStats.gpu = () => ({ ...renderer.info.memory, programs: renderer.info.programs?.length ?? 0 })
// How many of the GPU's textures the sprite caches explain: frames and sheets
// loaded once, kept for the session, and uploaded the first time each is DRAWN
// -- so their count climbs whenever play reaches a frame not seen before (a
// pain frame, a death). A leak is what grows beyond them (client/test/leak.test.js).
dnpStats.cachedTextures = () => {
  if (!props) return 0
  const uploaded = (t) => t && renderer.properties.get(t).__webglTexture !== undefined
  let n = 0
  for (const t of props.textures.values()) if (uploaded(t)) n++
  for (const a of props.atlases.values()) if (uploaded(a.texture)) n++
  return n
}
// The leak test's CONTROL: a texture uploaded and never let go of, which no
// cache explains -- the shape of a texture set stranded per new game.
dnpStats.plantTexture = () => {
  const t = new THREE.DataTexture(new Uint8Array(4), 1, 1)
  t.needsUpdate = true
  renderer.initTexture(t)
  ;(dnpStats.planted ??= []).push(t)
}
// What is in the scene, by type and name: what a test compares across a new
// game to find the object that was added and never taken away.
dnpStats.scene = () => {
  const out = {}
  for (const o of scene.children) {
    const k = `${o.type}:${o.name || o.userData?.kind || ''}`
    out[k] = (out[k] ?? 0) + 1
  }
  return out
}
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
// THE SKY (render/sky.js), before any MapView: its sky flats wear its material.
const sky = new Sky(THREE)
MapView.sky = sky
scene.add(sky.background)
stage('geometry', 0, 'building geometry')
let view = new MapView(mapData)
stage('geometry', 1)
// Engine (x, y) -> Three (x, ., -y), at the level's origin in the world.
view.root.position.set(homeOrigin.x, 0, -homeOrigin.y)
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
let animCatalogue = { groups: [], sheets: new Map(), warnings: [] }
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
    sheets: animCatalogue.sheets,
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

// --- drawing the cells next door ---------------------------------------------
//
// A neighbour is built exactly the way this level was -- loadMap, readMap,
// MapView -- and then MOVED. That is the whole of it, and it is the reason the
// offset is carried on the wire instead of the cell coordinates: the client
// never learns where in the world it is standing, only how far away the thing
// next to it is.
//
// `core.loadMap` allocates its own MapStorage (client/wasm/bindings.cpp), so
// several maps coexist in one core with no second module and no second world.
// There is exactly one simulation and it is this level's.
// ITS OWN SCALE, not this map's. A level converted at 1/48 wants 48 and the
// engine's default is 32, and a neighbour drawn at its observer's scale would
// have every texture half again the wrong size -- which reads as textures being
// placed wrong rather than as a scale error, which is why the map declares it.
// Every map in a composed world happens to agree today; nothing makes them.
function neighbourTextureOpts (scale) {
  return {
    pxPerUnit: params.has('texpx') ? Number(params.get('texpx')) : (scale ?? 32),
    extra: animCatalogue.groups.flatMap((g) => g.frames),
    sheets: animCatalogue.sheets
  }
}

// THE LEVELS HELD BESIDES THIS ONE (client/src/seam/neighbourhood.js), and the
// four things it asks of the scene.
//
// A neighbour is drawn and nothing else: not simulated, not collided with. The
// view is built from the worker's groups at the neighbour's own texture scale.
function makeNeighbourView (built) {
  const slim = { textures: built.textures, lights: built.lights, sectors: [],
                 textureScale: built.textureScale }
  const v = new MapView(slim, { prebuilt: built.groups })
  // Whether its pictures have arrived: a cell walked into soon after it
  // appears can be promoted with placeholders still on it.
  v.texturesLoaded = false
  v.loadTextures(neighbourTextureOpts(built.textureScale))
    .then(() => { v.texturesLoaded = true })
    .catch((e) => console.warn(`[seam] neighbour textures: ${e.message}`))
  return v
}

// THE WORKER THAT BUILDS THEM, started on the first neighbour and never if
// there is no metamap. Null if one could not be started; buildCell then runs
// here, which is the same code on the main thread -- slower, and correct.
let seamWorker
let seamWorkerTried = false
function getSeamWorker () {
  if (seamWorkerTried) return seamWorker
  seamWorkerTried = true
  try {
    seamWorker = new Worker(new URL('./seamworker.js', import.meta.url), { type: 'module' })
    seamWorker.onmessage = (e) => { neighbourBuilt.push(e.data) }
    seamWorker.onerror = (e) => {
      console.warn(`[seam] worker failed (${e.message}); building on the main thread`)
      seamWorker = null
    }
  } catch (e) {
    console.warn(`[seam] no worker (${e.message}); building on the main thread`)
    seamWorker = null
  }
  return seamWorker
}

/** Builds finished, waiting for a frame to upload them: one per frame. */
const neighbourBuilt = []

const hood = new Neighbourhood({
  build (level, token, bytes) {
    const w = getSeamWorker()
    // A COPY, not a transfer: the bytes are kept here, because walking into
    // this level builds the world from them.
    if (w) { w.postMessage({ level, token, bytes: bytes.buffer.slice(0) }); return }
    const r = buildCell(core, bytes)
    neighbourBuilt.push(r.error ? { level, token, error: r.error } : { level, token, ...r.built })
  },
  make: makeNeighbourView,
  // Engine z-up (x, y) -> Three y-up (x, ., -y), as every vertex in mapgeom.js.
  show (v, dx, dy) {
    v.root.position.set(homeOrigin.x + dx, 0, -(homeOrigin.y + dy))
    if (!v.root.parent) scene.add(v.root)
  },
  hide (v) { scene.remove(v.root) },
  // The geometry, NOT the materials or textures: those are shared
  // (render/textures.js), and disposing them would take the level underfoot
  // with it.
  dispose (v) {
    scene.remove(v.root)
    v.root.traverse((o) => o.geometry?.dispose?.())
  },
  warn: (line) => report(line.replace('[seam] ', '[seam] VIOLATION: '), 'error')
})
if (LIVE && welcome) hood.enter(welcome.homeLevel, null)

/** Everything the server said about levels, in the order it said it. */
function drainSeam () {
  while (seamEvents.length) {
    const e = seamEvents.shift()
    if (e.welcome) {
      if (enterLevel(e.welcome, e.bytes)) welcome = e.welcome
    } else if (e.map) {
      const m = e.map
      if (region) addChunk(m.level, e.bytes, m.dxRaw, m.dyRaw)
      hood.map(m.level, m.dx, m.dy, m.dxRaw, m.dyRaw, e.bytes)
    } else if (e.move) {
      const m = e.move
      hood.move(m.level, m.dx, m.dy, m.dxRaw, m.dyRaw)
    } else if (e.drop !== undefined) {
      if (region) dropChunk(e.drop)
      hood.drop(e.drop)
    } else if (e.home !== undefined) {
      walkInto(e.home, e.name)
    }
  }
  // ONE UPLOAD PER FRAME. A cell you cannot reach yet appearing over the next
  // eight frames is invisible; eight in one frame is a stutter you feel.
  if (neighbourBuilt.length) {
    const b = neighbourBuilt.shift()
    if (b.error) console.warn(`[seam] neighbour ${b.level}: ${b.error}`)
    else hood.built(b.level, b.token, b)
  }
}

// A HELD LEVEL IS SIMULATED, in a region: it becomes a chunk of this client's
// world at its origin, joined to what it meets, so the body walks into it the
// way the server's does. The parse is kept -- it is what the level's view and
// its hash, spawns and texture scale come from when it becomes home.
function addChunk (level, bytes, dxRaw, dyRaw) {
  if (chunkMaps.has(level)) dropChunk(level)
  const r = core.loadMap(bytes)
  if (!r.ok) { report(`[seam] level ${level} would not load (status ${r.status})`, 'error'); return }
  const at = { x: homeOriginRaw.x + dxRaw, y: homeOriginRaw.y + dyRaw }
  if (!simStore.addChunk(level, r.map, at)) {
    report(`[seam] level ${level} does not fit in this client's world`, 'error')
    r.map.destroy()
    return
  }
  chunkMaps.set(level, r.map)
  world.setMap(simStore)
}

function dropChunk (level) {
  const range = simStore.chunkRange(level)
  if (range) {
    world.forgetSectors(range.sectorFirst, range.sectorCount)
    simStore.removeChunk(level)
    world.setMap(simStore)
  }
  chunkMaps.get(level)?.destroy()
  chunkMaps.delete(level)
  sideTextureLog.delete(level)
}

/**
 * THE BODY WALKED INTO ANOTHER LEVEL OF THE REGION (MSG_HOME).
 *
 * Nothing about the simulation changes: same world, same body, same predictor,
 * same camera -- which is the whole point, and why this is short. What does
 * change is which level's view is the full, animated one; whose music plays;
 * whose paths the camera may follow; and what every held level's offset is
 * measured from.
 */
function walkInto (level, name = null) {
  const entry = hood.held.get(level)
  const parse = chunkMaps.get(level)
  if (!entry || !entry.placed || !parse) {
    report(`[seam] VIOLATION: walked into level ${level}, which is not held here`, 'error')
    return
  }
  // The new home's origin, before the neighbourhood re-places its views from it.
  homeOriginRaw = { x: homeOriginRaw.x + entry.dxRaw, y: homeOriginRaw.y + entry.dyRaw }
  homeOrigin = { x: fxToFloat(homeOriginRaw.x), y: fxToFloat(homeOriginRaw.y) }
  const got = hood.enter(level, { bytes: liveMapBytes, view })
  homeLevel = level
  homeName = name
  map = parse
  mapData = readMap(map)
  liveMapBytes = got.bytes
  camPaths = readPaths(map)
  shiftPaths(camPaths)
  camPathIndex = -1
  camPathSpline = null
  lowestFloor = Math.min(...mapData.sectors.map((s) => s.floorZ - (s.thickness ?? 0)))
  VOID_FLOOR = lowestFloor - 40

  // THE VIEW: the one drawn next door is promoted, as a WELCOME's is, and given
  // the full map so its movers can be split out and animated.
  const promoted = got.view
  if (promoted) {
    view = promoted
    view.map = mapData
    view.root.position.set(homeOrigin.x, 0, -homeOrigin.y)
    if (!view.root.parent) scene.add(view.root)
    animator = new TextureAnimator({
      groups: animCatalogue.groups,
      names: mapData.textures,
      materials: view.materials,
      frameTexture: (n) => view.frameTextures?.get(n) ?? null
    })
  } else {
    view = new MapView(mapData)
    view.root.position.set(homeOrigin.x, 0, -homeOrigin.y)
    scene.add(view.root)
    loadMapTextures().catch((e) => say(`textures failed to load: ${e.message}`))
  }
  movers = [...motionNames].map(homeLocal).filter((i) => i >= 0)
  // A fade in the level left is over: its sector indices mean nothing here.
  lightFades.clear()
  if (movers.length) view.setDynamicSectors(movers)
  // Switches thrown in it while it was next door, now that it can show them.
  for (const m of sideTextureLog.get(level) ?? []) sideTextures.push(m)
  sideTextureLog.delete(level)

  playMapMusic(map.hash, homeName, 1200)
  crossing.mark(0)
  if (params.get('autowalk') === 'shuttle') autowalkDir = -autowalkDir
  const here = world.readAgent(agent)
  report(`[seam] walked into level ${level}, view ${promoted ? 'promoted' : 'built'}` +
         ` (${fxToFloat(here.pos.x).toFixed(2)},${fxToFloat(here.pos.y).toFixed(2)} sector ${here.sector})` +
         `; holding ${hood.describe()}`)
}

/**
 * Has this body walked out of the cell its own map occupies? Only then is a
 * step into no sector a seam rather than a pit. A hair outside the boundary,
 * so a body exactly on the line is not held back from a step it is allowed.
 */
function leftTheCell (a) {
  const pitch = hood.pitch()
  if (!Number.isFinite(pitch)) return false
  const x = fxToFloat(a.pos.x), y = fxToFloat(a.pos.y)
  const slack = 0.01
  return x < -slack || x > pitch + slack || y < -slack || y > pitch + slack
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
const CLIMB_ENABLED_ID = world.paramId('climb_enabled')
const CLIMB_AUTO_ID = world.paramId('climb_auto')
// THE CONTROLS ARE TAKEN (a cinematic: input_scale 0, predicted like every
// rule): the player neither moves nor turns the view, and on a touch screen
// the joystick is not drawn. The camera is still the graph's to turn.
const INPUT_SCALE_ID = world.paramId('input_scale')
const controlsTaken = () => world.getParamRaw(agent, INPUT_SCALE_ID) === 0n
// Whether the home level has a climbable wall at all, and of which map that was.
let climbableHere = false
let climbableFor = null
const HEIGHT_ID = 1
let bodyRadius = Number(world.getParamRaw(agent, RADIUS_ID)) / 4294967296
let bodyHeight = Number(world.getParamRaw(agent, HEIGHT_ID)) / 4294967296

// THE LOCAL PLAYER'S BODY, and the one function that makes it.
//
// A capsule until an avatar is known, then the avatar's own mesh. Two things
// make that a rebuild rather than a choice made once: the catalogue is fetched
// asynchronously, so at startup there is nothing to build from; and the avatar
// NUMBER arrives in a snapshot, so which one to build is not known until the
// first one lands.
//
// Blue for a capsule so self and remote are still distinguishable when a module
// ships no avatar -- which is the state every catalogue in the repo was in until
// one declared one.
// The capsule is built HERE and unconditionally, because neither the catalogue
// nor the avatar number exists yet at this point in the module -- and buildBody
// below reads both. The TDZ guard in client/test/tdz.test.js caught exactly that
// when this was one call instead of two, which is what that test is for.
let body = makeAgentMesh(0x66ccff, bodyRadius, bodyHeight)
let bodyAvatar = -1
scene.add(body)

// Swap the body for the avatar's own mesh once one is known, or back again.
//
// A rebuild rather than a choice made once, for two reasons: the catalogue is
// fetched asynchronously, so at startup there is nothing to build from, and the
// avatar NUMBER arrives in a snapshot, so which one is not known until the first
// one lands. Called from the frame loop, where it returns immediately unless the
// answer has actually changed.
//
// Blue for a capsule so self and remote stay distinguishable when a module ships
// no avatar -- the state every catalogue in this repo was in until one did.
function buildBody () {
  const want = props?.avatarFor(selfAvatar) ? selfAvatar : -1
  if (want === bodyAvatar) return
  const next = want >= 0 ? props.makeAvatar(want) : null
  if (!next && want >= 0) return          // the catalogue could not build it
  scene.remove(body)
  // THE OLD CAPSULE IS FREED, when it was ours. Every level change resets the
  // avatar and comes through here, so the capsule it replaced stayed on the
  // GPU: one geometry per new world, for ever (client/test/leak.test.js). An
  // avatar from the catalogue is the catalogue's -- its geometry and material
  // may be shared with every other body drawn from it -- so only our own goes.
  if (bodyAvatar < 0) {
    body.geometry?.dispose?.()
    body.material?.dispose?.()
  }
  bodyAvatar = want
  body = next ?? makeAgentMesh(0x66ccff, bodyRadius, bodyHeight)
  scene.add(body)
}

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
let swingPitchMid = 0
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
  swingPitchMid = swept.pitchMid
  swingWhole = makeSwingMesh(THREE, {
    reachUnits: shape.reach, innerUnits: shape.inner,
    halfArcTurns: swingSpan, halfPitchTurns: swept.pitchHalf,
    segments: 24, faint: true
  })
  swing = makeSwingMesh(THREE, {
    reachUnits: shape.reach, innerUnits: shape.inner,
    halfArcTurns: shape.halfArc, halfPitchTurns: shape.halfPitch
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

// THE SWING AS THE GAME SHOWS IT, debug or not: a slash streak along each
// body's real sweep, in the module's look (render/slash.js, docs/SWINGS.md;
// assets/sprites/swings.json). The drawings above are the collider, for
// somebody tuning a weapon; this is the effect.
const slash = new SlashView({ THREE, scene })
let swingStyles = parseSwingStyles({}).styles
fetch('/sprites/swings.json').then((r) => (r.ok ? r.text() : null)).then((text) => {
  if (text === null) return
  const parsed = parseSwingStyles(text)
  // Said, not fatal: what is wrong is left out and the rest is used.
  for (const e of parsed.errors) console.warn(`[swings] ${e}`)
  swingStyles = parsed.styles
}).catch(() => {})
dnpStats.slash = () => ({ live: slash.count, swingers: [...slash.swingers.keys()] })

// Camera paths, drawn. OPT-IN, unlike the swing view beside it: a map may hold
// sixteen paths and each is a spline sampled every frame it moves. That is
// nothing beside a level's geometry and it is not nothing beside zero, and the
// only person who wants it is one authoring a shot.
const SHOW_PATHS = params.get('paths') === '1'
const pathView = SHOW_PATHS ? new PathView({ THREE, scene }) : null
if (pathView) {
  pathView.setPaths(camPaths, (q) => ({ x: toThreeX(q.x), y: toThreeY(q.z), z: toThreeZ(q.y) }))
  // Which sectors move in the mode a camera path must not be bolted to. Built
  // from the motion type the client already has to look at: a discrete mover's
  // offset is STREAMED every snapshot, so a path riding one judders at the
  // snapshot rate rather than at the frame rate.
  pathView.setDiscrete(motionSets.filter((m) => m.motionType !== 1).map((m) => m.sector))
}

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
  const s = simStore.findSector(x, -z)
  if (s < 0) return null
  return toThreeY(liveStoreSector(s).floorZ)
}
const onGround = (x, z) => simStore.findSector(x, -z) >= 0

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
          // ONE MATERIAL PER SHEET, shared by every body drawn from it -- which
          // is what lets the renderer batch them, and what makes disposing it
          // from any one of them a bug. Said on the object rather than left to
          // the reader, because the disposal path is three files away.
          material: Object.assign(new THREE.MeshBasicMaterial({
            map: texture, transparent: true, alphaTest: 0.5, side: THREE.DoubleSide
          }), { userData: { shared: true } })
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
  // A player is kind 0 and kind 0 has no catalogue record by construction, so a
  // player's art is addressed through the `avatar` line instead -- see
  // PropRegistry.makeAvatar. Falling through to the body mesh when no avatar is
  // declared keeps every catalogue that has never heard of one working unchanged.
  makeMesh: (kind, avatarNo = 0) => (kind ? props?.make(kind) : props?.makeAvatar(avatarNo)) ??
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

// OUR OWN MISSILES, flown here the server's way from the tick they are fired
// (render/ghostmissiles.js), so a rocket leaves the barrel and bursts when it
// does rather than a snapshot later. The server's copies are hidden below.
// Our blast's shove of the bodies around it, drawn when it happens rather
// than a snapshot later (render/knock.js).
const knock = new KnockLayer()
// The bodies near us, drawn at the present rather than a beat behind
// (render/nearnow.js).
const near = new NearPresent()
// The server tick our next step lands on: input ticks and server ticks differ
// by a near-constant the newest snapshot states.
const serverTickOf = (inputTick) => {
  const s = predictor?.lastSnapshot
  return s ? inputTick + (s.serverTick - s.lastInputTick) : null
}

const ghosts = new GhostMissiles({
  world,
  agent,
  // EVERY BODY IN REACH, shoved by the server's rule (sim/blast.hpp) at its
  // last known position, with its mass from the weapon table -- in the step
  // the burst lands in, as the server's damage_area does it.
  onBlast: (impact, blast) => {
    const S = serverTickOf(predictor.tick + 1)
    if (S === null) return
    for (const [index, ent] of others.entities) {
      if (!ent.fxPos) continue
      const mass = ent.kind ? ghosts.world.mobMass(ent.kind) : 100
      if (!mass) continue
      // Its size and speed: the catalogue's for a monster, the map's body for
      // a player.
      const def = ent.kind ? props?.kinds.get(ent.kind) : { radius: bodyRadius, height: bodyHeight }
      if (!def) continue
      const sh = shoveOf(ghosts.world, impact, ent.fxPos, blast, mass, def)
      if (sh) knock.add(`${index}/${ent.generation}`, S, sh.v, sh.path)
    }
  },
  // OUR OWN ROCKET AT OUR OWN FEET: the shove its blast gives us, by the
  // server's rule (sim/blast.hpp), in the step about to run -- and replayed
  // with it, so a correction does not take the jump back.
  selfBlast: (impact, blast) => {
    const v = ghosts.world.blastShove(impact, ghosts.world.readAgent(ghosts.agent).pos, blast)
    if (v) predictor.addImpulse(predictor.tick + 1, v)
  },
  draw: {
    make: (kind) => {
      const m = props?.make(kind) ?? null
      if (m) scene.add(m)
      return m
    },
    place: (m, p, turns) => {
      if (!m) return
      m.position.set(toThreeX(p.x), toThreeY(p.z) + (m.userData.yOffset ?? 0), toThreeZ(p.y))
      m.userData.facingTurns = turns
    },
    burst: (m, state) => { if (m) props?.setState(m, state, performance.now() / 1000) },
    // As EntityView removes a prop: never a shared material, never the texture.
    remove: (m) => {
      if (!m) return
      scene.remove(m)
      m.geometry?.dispose?.()
      if (m.material && !m.material.userData?.shared) m.material.dispose?.()
    },
    blast: (v) => drawVfx(v)
  }
})

/**
 * Take up residence in a new level, mid-session.
 *
 * The server hosts one simulation per level and moves a session between them,
 * so a second WELCOME means "you are somewhere else now". Everything
 * downstream of the geometry is rebuilt: the world, the agent, the predictor,
 * the body, the set of sectors known to move.
 *
 * Everything NOT downstream of the geometry is kept -- the renderer, the scene,
 * the camera and its yaw, the held keys. The player is the same player,
 * looking the same way, and only the world under them changed.
 *
 * `streamed` is the map that followed the welcome, or null when it did not:
 * then the client holds it (docs/protocol.md §8.5) and the neighbourhood hands
 * it back, with its view if one was built.
 */
const crossing = new CrossingProbe()
const bodyWatch = new BodyWatch()

function enterLevel (w, streamed) {
  const t0 = performance.now()
  // The level left becomes held, with the bytes and view it was using; the
  // level entered stops being held, and every other cell is re-stated relative
  // to it. From here `view` belongs to the neighbourhood unless it comes back.
  // THE FRAME FIRST: every held view is re-placed as the neighbourhood re-homes,
  // and it is placed from the new home's origin.
  const wasRegion = region
  region = !!w.region
  homeOriginRaw = region ? { x: w.origin.x, y: w.origin.y } : { x: 0n, y: 0n }
  homeOrigin = { x: fxToFloat(homeOriginRaw.x), y: fxToFloat(homeOriginRaw.y) }
  const got = hood.enter(w.homeLevel, { bytes: liveMapBytes, view })
  if (streamed && got && got.view !== view) {
    report(`[seam] VIOLATION: the map of level ${w.homeLevel} was sent, and it was held`, 'error')
  }
  const bytes = streamed ?? got?.bytes
  if (!bytes) {
    // The server said this client holds a level it does not. By the rule
    // nothing can make that happen, so it is a bug on one side or the other,
    // and nothing is coming that could repair it.
    report(`[seam] VIOLATION: level ${w.homeLevel} was said to be held here and is not`, 'error')
    say(`map change failed: level ${w.homeLevel} is not held`)
    return false
  }
  const loaded = core.loadMap(bytes)
  if (!loaded.ok) {
    say(`new map rejected: ${LOAD_STATUS[loaded.status] ?? loaded.status}`)
    return false
  }
  // Held bytes are identified by level, and the hash is the proof. A mismatch
  // is the wrong thing held, not a cache miss.
  if (loaded.map.hash !== w.mapHash) {
    report(`[seam] VIOLATION: level ${w.homeLevel} holds map ${loaded.map.hash.toString(16)}` +
           `, the server says ${w.mapHash.toString(16)}`, 'error')
    say('map hash mismatch after a level change')
    return false
  }
  // What this replaces, retired once the new ones exist (see below).
  const oldWorld = world
  const oldMap = map
  const oldStore = simStore
  map = loaded.map
  mapData = readMap(map)
  // A new map's paths are different paths, and the tag the camera holds may not
  // exist in it. Dropping the resolved index forces the lookup to run again.
  camPaths = readPaths(map)
  camPathIndex = -1
  camPathSpline = null
  homeLevel = w.homeLevel
  homeName = w.levelName ?? null

  // THE WORLD. Outside a region, the level's own map. In one, a store of every
  // level held: the one entered at its origin, the rest at theirs, joined where
  // they meet -- rebuilt here because an exit is the one time the body really
  // does go somewhere else (walking between levels never comes through here).
  if (!wasRegion) { for (const [, m] of chunkMaps) if (m !== map) m.destroy(); chunkMaps.clear() }
  if (region) {
    const old = chunkMaps.get(w.homeLevel)
    if (old && old !== map) old.destroy()
    chunkMaps.set(w.homeLevel, map)
    for (const [li, m] of [...chunkMaps]) {
      if (li !== w.homeLevel && !hood.has(li)) { m.destroy(); chunkMaps.delete(li) }
    }
    shiftPaths(camPaths)
    simStore = core.createStore()
    simStore.addChunk(w.homeLevel, map, homeOriginRaw)
    for (const [li, n] of hood.held) {
      if (!n.placed) continue
      let m = chunkMaps.get(li)
      if (!m) {
        const r = core.loadMap(n.bytes)
        if (!r.ok) continue
        m = r.map
        chunkMaps.set(li, m)
      }
      simStore.addChunk(li, m, { x: homeOriginRaw.x + n.dxRaw, y: homeOriginRaw.y + n.dyRaw })
    }
  } else {
    simStore = map
  }
  world = core.createWorld({ map: simStore })
  // THE OLD WORLD AND ITS MAP GO, in the wasm heap. They were replaced and never
  // freed: a world is two thousand agent slots and their state, and every level
  // change and every new game stranded one -- 28 MB of wasm memory becoming
  // 154 MB over ten new games, which on a phone's fixed ceiling is a crash after
  // a handful (client/test/leak.test.js). Not a map still held as a neighbour.
  const held = new Set(chunkMaps.values())
  if (oldWorld && oldWorld !== world) oldWorld.destroy()
  if (oldStore && oldStore !== simStore && oldStore !== oldMap && !held.has(oldStore)) oldStore.destroy()
  if (oldMap && oldMap !== map && oldMap !== simStore && !held.has(oldMap)) oldMap.destroy()
  // Where the server put the body, in the world's coordinates.
  spawn = w.arrival
  agent = world.spawn(spawn)
  const atSeam = false

  // A new level is a new entity index: keeping the old one would mirror a body
  // on top of us and us onto somebody else.
  myAgentIndex = (w.selfHandle & 0x00ffffff) - 1
  mySelfHandle = w.selfHandle >>> 0
  sfx.stopAll()
  // A new body: the game says again what it sounds like (on_player_enter).
  ownSounds.clear()
  world.tick = w.serverTick
  predictor = new Predictor(world, agent)

  bodyRadius = Number(world.getParamRaw(agent, RADIUS_ID)) / 4294967296
  bodyHeight = Number(world.getParamRaw(agent, HEIGHT_ID)) / 4294967296

  lowestFloor = Math.min(...mapData.sectors.map((s) => s.floorZ - (s.thickness ?? 0)))
  VOID_FLOOR = lowestFloor - 40

  // THE VIEW. A level already drawn next door is promoted: its textures are
  // loaded and its materials compiled, and building a second MapView over the
  // same geometry is what put the loading bar back at every doorway. Its
  // texture set came from the same map's list, in the same order, so every
  // material index still means what it meant; the full map replaces the slim
  // one the worker sent, so setDynamicSectors works as for a view built here.
  const promoted = got?.view ?? null
  if (promoted) {
    view = promoted
    view.map = mapData
    view.root.position.set(homeOrigin.x, 0, -homeOrigin.y)
    if (!view.root.parent) scene.add(view.root)
  } else {
    view = new MapView(mapData)
    view.root.position.set(homeOrigin.x, 0, -homeOrigin.y)
    scene.add(view.root)
  }

  camera.snapTo({
    x: toThreeX(fxToFloat(spawn.x)), y: toThreeY(fxToFloat(spawn.z)), z: toThreeZ(fxToFloat(spawn.y))
  })
  if (params.get('autowalk') === 'shuttle') autowalkDir = -autowalkDir

  // A NEW MAP IS SEEN. A curtain or bars left from the map before -- a map
  // change faded out to black, a cutscene cut short by an exit -- are lifted,
  // the curtain fading in. The rule the screen already has for a join ("not
  // remembered": a player arrives at a screen they can see out of), for an
  // arrival. A cutscene the new map starts on arrival comes after this, since
  // the welcome is what runs it.
  curtainEl.style.transitionDuration = '600ms'
  curtainEl.style.opacity = '0'
  for (const el of [lbTopEl, lbBotEl]) { el.style.transitionDuration = '300ms'; el.style.height = '0%' }
  // A NEW MAP IS A NEW TRACK, crossfaded rather than cut. music.play() is a
  // no-op when the track is the same, so two maps sharing a song do not
  // restart it at the door.
  playMapMusic(map.hash, homeName, 1200)

  if (promoted) {
    // Nothing to load: the pictures are on the GPU. Only the animator is
    // rebuilt, because it holds the materials it swaps.
    animator = new TextureAnimator({
      groups: animCatalogue.groups,
      names: mapData.textures,
      materials: view.materials,
      frameTexture: (n) => view.frameTextures?.get(n) ?? null
    })
  } else {
    // A MAP CHANGE IS A LOAD, and it ends the way the first one does. Only the
    // phases a map change redoes are reset.
    stageReset(['map', 'geometry', 'textures'])
    stage('map', 1)
    stage('geometry', 1)
    loadMapTextures()
      .catch((e) => say(`textures failed to load: ${e.message}`))
      .finally(() => stageDone())
  }

  movers = []
  motionNames.clear()
  sideTextureLog.clear()
  // A new map may ship a different avatar, so the body is rebuilt.
  bodyAvatar = -1
  buildBody()
  shadows.setBody(bodyRadius, bodyHeight)

  // Other players in the level just left are not other players here, and the
  // snapshots still queued describe the old world.
  others.clear()
  ghosts.clear()
  knock.clear()
  near.clear()
  ghosts.world = world
  ghosts.agent = agent
  pendingSnapshots.length = 0
  inputHistory = []
  newestServerTick = w.serverTick
  renderClock.reset(w.serverTick)
  liveMapBytes = bytes

  const here = world.readAgent(agent)
  report(`[seam] entered level ${w.homeLevel}: ${streamed ? 'streamed' : 'held'}` +
         `, ${promoted ? 'view promoted' : 'view built'}` +
         `, arrived ${atSeam ? 'at the seam' : 'at the spawn'}` +
         ` (${fxToFloat(here.pos.x).toFixed(2)},${fxToFloat(here.pos.y).toFixed(2)} sector ${here.sector})` +
         `, took ${(performance.now() - t0).toFixed(1)}ms; holding ${hood.describe()}`)
  return true
}

let viewHeight = START_ZOOM
const cam3 = makeCamera(PROJECTION, innerWidth / innerHeight, viewHeight, FOV)
const camera = new Camera()
// Settings that arrived before the camera existed (see camCfgPending).
Object.assign(camera.cfg, camCfgPending)
cameraReady = true

// Under perspective, zooming MOVES the camera; under orthographic it reshapes
// the frustum and the position is irrelevant to framing. One number drives
// both.
// camOverrideDistance and camOverridePitch (declared above the connection, see
// there) are kept apart from camera.cfg because distance and pitch describe
// where the camera sits rather than how it follows, and the offset is
// recomputed from them every frame as yaw and zoom change.
const camDistance = () =>
  camOverrideDistance ?? distanceFor(cam3, viewHeight, DEFAULT_DISTANCE)

addEventListener('resize', () => {
  renderer.setSize(innerWidth, innerHeight)
  resizeCamera(cam3, innerWidth / innerHeight, viewHeight)
  ui?.relayout()
})
/**
 * Zoom by a factor, clamped.
 *
 * One function because there are two ways in now -- a wheel and a pinch -- and
 * they must land on the same number with the same limits. Two copies of a
 * clamp is how one of them ends up able to zoom into the floor.
 */
function zoomBy (factor) {
  if (camLockZoom) return
  viewHeight = Math.min(140, Math.max(8, viewHeight * factor))
  resizeCamera(cam3, innerWidth / innerHeight, viewHeight)
}

addEventListener('wheel', (e) => {
  zoomBy(e.deltaY > 0 ? 1.1 : 1 / 1.1)
}, { passive: true })

// --- input -----------------------------------------------------------------

const held = new Set()
addEventListener('keydown', (e) => { held.add(e.code); if (e.code === 'Space') e.preventDefault() })
let newGameArmedAt = 0
// QUICK SAVE AND LOAD, as Doom had them: F5 saves to slot 0, F9 loads it. The
// server decides whether this game may (docs/SAVING.md) and says why not.
addEventListener('keydown', (e) => {
  if (!LIVE || !conn || e.repeat) return
  // 1-9: the n-th weapon held, in the weapon table's order.
  const digit = /^Digit([1-9])$/.exec(e.code)
  if (digit) weaponSlotWanted = Number(digit[1])
  if (e.code === 'F5') { e.preventDefault(); conn.sendControl(encodeGameSave(0)); say('saving...') }
  if (e.code === 'F9') { e.preventDefault(); conn.sendControl(encodeGameLoad(0)); say('loading...') }
  // A NEW GAME throws the current one away, so it asks twice: F2, then F2
  // again within three seconds.
  if (e.code === 'F2') {
    e.preventDefault()
    const now = performance.now()
    if (now - newGameArmedAt < 3000) {
      newGameArmedAt = 0
      conn.sendControl(encodeGameNew())
      say('starting a new game...')
    } else {
      newGameArmedAt = now
      say('press F2 again to start a new game (the current one is lost unless saved)')
    }
  }
})
addEventListener('keyup', (e) => held.delete(e.code))
addEventListener('blur', () => held.clear())

// FIRST PERSON: the mouse is the view. A click on the game takes the pointer
// (the browser shows the player how to give it back: Escape), and while it is
// held every movement turns the view and the mouse buttons are the triggers.
const mouseHeld = new Set()
addEventListener('mousedown', (e) => {
  if (!isFps()) return
  // A click on a script's panel is the panel's: it must not take the pointer
  // (which would hide the very cursor clicking it), and while a modal panel is
  // open the game has no pointer to take.
  if (document.pointerLockElement !== renderer.domElement &&
      (uiModal || e.target?.closest?.('.dnp-ui'))) return
  if (document.pointerLockElement !== renderer.domElement) {
    renderer.domElement.requestPointerLock?.()
    return
  }
  mouseHeld.add(e.button)
})
addEventListener('mouseup', (e) => mouseHeld.delete(e.button))
// The right button is the second trigger, not the browser's menu.
addEventListener('contextmenu', (e) => { if (isFps()) e.preventDefault() })
addEventListener('mousemove', (e) => {
  if (isFps() && document.pointerLockElement === renderer.domElement && !controlsTaken()) fpsLook.move(e.movementX, e.movementY)
})
document.addEventListener('pointerlockchange', () => {
  if (document.pointerLockElement !== renderer.domElement) mouseHeld.clear()
})

// The last direction the agent was actually pointed. Brad 0 is due east, not
// "unset", so this starts there and every idle tick repeats whatever came last.
let lastAim = 0

// Quantised with the SAME function the server decodes with (protocol R1).
// Reimplementing it in JS would be one more place for the two sides to
// disagree, and the disagreement would be one ulp wide and present every tick.
function axis (v) {
  return core.axisFromFxRaw(BigInt(Math.round(Math.max(-1, Math.min(1, v)) * 4294967296)))
}

// The FIRST-PERSON input frame: a push relative to the view, the view as the
// aim, and its pitch. Nothing about the camera turns it -- the simulation turns
// the push by the body's facing, which in this scheme is the view.
function fpsInput () {
  if (touch) {
    const d = touch.state.takeDrag()
    if ((d.dx || d.dy) && !controlsTaken()) fpsLook.move(d.dx, d.dy, TOUCH_SENSITIVITY)
  }
  const stick = touch ? touch.state.move2d() : null
  const push = fpsPush({
    forward: held.has('KeyW') || held.has('ArrowUp'), back: held.has('KeyS') || held.has('ArrowDown'),
    right: held.has('KeyD'), left: held.has('KeyA'), stick
  })
  // Arrow keys turn, for a player with no mouse.
  if (held.has('ArrowLeft')) fpsLook.move(-6, 0)
  if (held.has('ArrowRight')) fpsLook.move(6, 0)
  let buttons = 0
  if (held.has('Space')) buttons |= BUTTON.JUMP
  if (held.has('ShiftLeft') || held.has('ShiftRight')) buttons |= BUTTON.DASH
  if (held.has('KeyE')) buttons |= BUTTON.INTERACT
  if (held.has('KeyC')) buttons |= BUTTON.CROUCH
  // G holds onto a climbable wall (climb_auto off; docs/PLAYER.md).
  if (held.has('KeyG')) buttons |= BUTTON.CLIMB
  if (held.has('KeyF') || mouseHeld.has(0)) buttons |= BUTTON.ATTACK_PRIMARY
  if (mouseHeld.has(2)) buttons |= BUTTON.ATTACK_SECONDARY
  if (touch) buttons |= touch.state.buttons(TOUCH_BITS)
  lastAim = fpsLook.aimBrad()
  lastSampled = { buttons, moveX: axis(push.x), moveY: axis(push.y), aim: lastAim, pitch: fpsLook.pitchWire(),
                  weapon: takeWeaponWanted(), view: drawnTick(), actions: heldActions() }
  return lastSampled
}

function currentInput () {
  if (isFps()) return fpsInput()
  let mx = 0
  let my = 0
  if (held.has('KeyW') || held.has('ArrowUp')) my += 1
  if (held.has('KeyS') || held.has('ArrowDown')) my -= 1
  if (held.has('KeyD') || held.has('ArrowRight')) mx += 1
  if (held.has('KeyA') || held.has('ArrowLeft')) mx -= 1

  // THE STICK IS ADDED, not substituted. A key and a thumb pushing the same
  // way must not double the speed, which is what the clamp below is for --
  // and it is the clamp the keyboard's own diagonal already needed, so there
  // is one of it rather than one per input.
  if (touch) {
    const push = touch.state.move2d()
    mx += push.x
    my += push.y
  }

  const len = Math.hypot(mx, my)
  if (len > 1) { mx /= len; my /= len }

  // Screen-relative, from the yaw the camera is actually LOOKING along rather
  // than from the orbit angle. See camera.js: the two are halves of one fact
  // and deriving them separately is how they invert -- and on a camera path
  // they are not the same number at all, so a stick mapped through the orbit
  // angle pushes the player somewhere unrelated to what is on screen.
  let { x: wx, y: wy } = screenRelativeMove(mx, my, camViewYaw)
  // DEV ONLY: ?autowalk=east walks due east in WORLD terms, whatever the camera
  // is doing. A headless browser has no hands, and every measurement of a seam
  // crossing so far has needed a person to walk through one. ?autowalk=shuttle
  // turns round at every level change: east, back west, east again.
  if (AUTOWALK) { wx = autowalkDir; wy = 0 }

  let buttons = 0
  if (held.has('Space')) buttons |= BUTTON.JUMP
  if (held.has('ShiftLeft') || held.has('ShiftRight')) buttons |= BUTTON.DASH
  // E is USE: work a door, throw a switch. Climb is deliberately UNBOUND --
  // it is about to change shape, and leaving a key wired to the old behaviour
  // would make the change look like a regression in whatever it lands on.
  if (held.has('KeyE')) buttons |= BUTTON.INTERACT
  if (held.has('KeyC')) buttons |= BUTTON.CROUCH
  // G holds onto a climbable wall (climb_auto off; docs/PLAYER.md).
  if (held.has('KeyG')) buttons |= BUTTON.CLIMB
  // Left mouse would be the obvious binding and is not wired yet: the canvas
  // has no pointer capture, so a click would leave the page rather than swing.
  if (held.has('KeyF')) buttons |= BUTTON.ATTACK_PRIMARY
  if (touch) buttons |= touch.state.buttons(TOUCH_BITS)

  // Held across a stop, so letting go of the keys leaves the agent facing where
  // it was walking rather than snapping east. See aimFor.
  lastAim = aimFor(wx, wy, lastAim)
  lastSampled = { buttons, moveX: axis(wx), moveY: axis(wy), aim: lastAim, weapon: takeWeaponWanted(), view: drawnTick(),
                  actions: heldActions() }
  return lastSampled
}

// --- loop ------------------------------------------------------------------
//
// A fixed-timestep accumulator. The simulation MUST advance in whole 60 Hz
// ticks -- it is the same core the server runs, and stepping it by a frame time
// would be a different simulation (docs/physics-contract.md). Rendering runs at
// the display's rate and reads whatever the last tick produced.

let acc = 0
// WHERE THE LAST TICK STARTED, for drawing between ticks (see `subject`).
let stepFrom = null
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
// The server's DECLARED animation state for this player, from SelfBlock.
//
// Kept here rather than read off readAgent, because it is not simulation state
// the client predicts: it is a script's decision, deliberately excluded from
// World::agent_hash so that setting it cannot mispredict. Zero means "nothing
// declared", which is why it starts there.
let selfAnimation = 0
// Which avatar draws THIS player, from SelfBlock. Server state like hp, not
// simulation state: a costume is not physics.
let selfAvatar = 0

function drainSnapshots () {
  while (pendingSnapshots.length) {
    const s = pendingSnapshots.shift()
    if (s.hp !== undefined) latestHp = s.hp
    if (s.self?.animationState !== undefined) selfAnimation = s.self.animationState
    if (s.self?.avatar !== undefined) selfAvatar = s.self.avatar
    newestServerTick = Math.max(newestServerTick, s.serverTick)
    renderClock.heard(s.serverTick)
    // The newest input the SERVER says it has applied, AS IT LAST SAID IT --
    // not the highest it has ever said.
    //
    // A maximum was the wrong instrument and produced a wrong answer: the
    // server's counter is reset on a level change and on its starve-recovery
    // path, so a value that resets and climbs again reads as frozen at its old
    // peak. That is what "applied up to 1823" was, and it is a fact about this
    // line rather than about the server.
    bodyWatch.server(s)

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
      // The blast of a rocket of ours that already burst here.
      if (ghosts.swallow(v)) continue
      drawVfx(v)
    }
    // SOUNDS (docs/protocol.md §5.7). One this client PREDICTED -- its own
    // jump, its own shot -- already played when it happened here, so the
    // server's copy, a round trip later, is dropped rather than heard twice.
    // The seed is a function of what every client receives, so two clients
    // hear the same variation (sound/docs/EMBEDDING.md, 3).
    for (const so of s.sounds ?? []) {
      if ((so.flags & SOUND_PREDICTED) && (so.source >>> 0) === mySelfHandle) continue
      const seed = Math.imul(s.serverTick ^ so.sound, 2654435761) ^ so.source
      sfx.play(so.sound, { x: fxToFloat(so.pos.x), y: fxToFloat(so.pos.y), z: fxToFloat(so.pos.z) },
               { seed, key: (so.flags & SOUND_FOLLOW) ? so.source >>> 0 : 0 })
    }
    for (const so of s.sectorOffsets ?? []) world.setDiscreteOffset(so.sector, so.offset)
    others.ingest(s, { fxToFloat, toThree: (x, y, z) => ({ x: toThreeX(x), y: toThreeY(z), z: toThreeZ(y) }) })
    if (s.self) lastSelfAnim = s.self.animationState ?? 0
    predictor.reconcile(s)
  }
  let ownParams = false
  for (const m of paramUpdates) {
    if (!isOwnParams(m, myAgentIndex)) { remoteParams.take(m); continue }
    world.applyParamUpdate(agent, m)
    predictor.paramEpoch = m.epoch
    ownParams = true
  }
  if (ownParams) refreshBody()
  paramUpdates.length = 0
  applyWeaponDefs()
}

// THE BODY'S SIZE, as the server's parameters say it now. Read once on entering
// a level it was whatever the world was spawned with -- in a region, a store
// of chunks that carries no body of its own, so the engine's default 2-unit
// one -- and the first-person eye sat 0.7 units too high until the next level.
function refreshBody () {
  const r = Number(world.getParamRaw(agent, RADIUS_ID)) / 4294967296
  const h = Number(world.getParamRaw(agent, HEIGHT_ID)) / 4294967296
  if (r === bodyRadius && h === bodyHeight) return
  bodyRadius = r
  bodyHeight = h
  shadows.setBody(bodyRadius, bodyHeight)
}

// One effect, as a snapshot carries it -- the server's, or one our own predicted
// weapon produced (World::present_weapon), which arrive in identical terms.
function drawVfx (v, muzzle = null) {
  if (!vfx) { vfxNoView++; return }
  // AN ATTACHED EMITTER READS `param` AS AN ENTITY, not as a length.
  //
  // Branching here rather than inside spawn because tracerEnds would turn
  // an entity index into a distance and hand the renderer a point several
  // hundred units away -- the two meanings of `param` have to be told apart
  // before anything does arithmetic on it.
  const vdef = vfx.kindOf(v.kind)
  if (vdef?.attach) {
    vfx.attach(v.kind, v.param, (v.dir >>> 0) / 4294967296, performance.now() / 1000)
    return
  }
  const { from, to } = tracerEnds(v, fxToFloat)
  vfx.spawn(v.kind,
            muzzle ?? { x: toThreeX(from.x), y: toThreeY(from.z), z: toThreeZ(from.y) },
            { x: toThreeX(to.x), y: toThreeY(to.z), z: toThreeZ(to.y) },
            performance.now() / 1000)
}

// WHERE OUR OWN SHOT IS DRAWN FROM in first person: the gun, a little below and
// right of the eye. The ray itself leaves from the eye (World::hitscan), which
// is right for where it lands and wrong to look at -- a line from the exact
// centre of the view along the view is a dot. Drawing only; the end is the
// core's.
const MUZZLE = new THREE.Vector3(0.09, -0.11, -0.3)
function ownMuzzle () {
  if (!isFps() || camPathSpline) return null
  const p = MUZZLE.clone().applyMatrix4(cam3.matrixWorld)
  return { x: p.x, y: p.y, z: p.z }
}

// Predict our own weapon with the table the server sent, drawing the spread
// from the stream of our body's index ON THE SERVER, which is not our local one.
function applyWeaponDefs () {
  if (!weaponDefs || weaponDefsWorld === world || myAgentIndex === null) return
  if (!world.setWeapons(weaponDefs, agent, myAgentIndex)) {
    report('[weapons] this build cannot read the server\'s weapon table; not predicting them', 'error')
  }
  weaponDefsWorld = world
}

// The tick the others are on screen at, for the server to judge a shot against
// (lag compensation): what the last frame drew, to the nearest tick. 0 before
// anything has been drawn.
function drawnTick () {
  const t = others?.renderTick
  if (!(LIVE && t !== undefined && t > 0)) return 0
  // A shot at a body drawn at the PRESENT (render/nearnow.js) is judged as
  // now -- which 0 asks for -- not rewound to where the interpolated picture
  // would have had it, where nothing was drawn.
  if (aimingAtPresent()) return 0
  return viewByte(Math.round(t))
}

// Is our aim on a body drawn mostly at the present? Within the auto-aim cone
// plus the body's own width, in plan, in front of us.
const PRESENT_AIM_CONE = 6 * Math.PI / 180
function aimingAtPresent () {
  if (!others) return false
  const me = world.readAgent(agent).pos
  const mx = fxToFloat(me.x)
  const my = fxToFloat(me.y)
  const a = (lastAim >>> 0) / 4294967296 * 2 * Math.PI
  for (const ent of others.entities.values()) {
    if (!(ent.presentWeight >= 0.5)) continue
    const dx = ent.mesh.position.x - mx
    const dy = -ent.mesh.position.z - my
    const d = Math.hypot(dx, dy)
    if (d < 1e-6) return true
    let off = Math.atan2(dy, dx) - a
    off = Math.atan2(Math.sin(off), Math.cos(off))
    const r = (ent.kind ? props?.kinds.get(ent.kind)?.radius : bodyRadius) ?? 0.5
    if (Math.abs(off) <= PRESENT_AIM_CONE + Math.atan2(r, d)) return true
  }
  return false
}

// The weapon wanted this tick, by number key, then forgotten: it is a key
// going down, and holding it would undo every switch the weapon makes itself.
function takeWeaponWanted () {
  if (!weaponSlotWanted) return NO_WEAPON
  const n = weaponSlotWanted
  weaponSlotWanted = 0
  return world.hasWeapons ? world.weaponSlot(agent, n) : NO_WEAPON
}

// The sectors whose mesh is rebuilt every frame it changes: the movers, and
// any sector whose light is fading.
function dynamicSectors (extra = []) {
  return [...new Set([...movers, ...lightFades.keys(), ...extra])]
}

// A sector's look changed (SECTOR_SET; the server's set_sector): a floor or
// ceiling picture, its light -- at once or fading -- or one of its effects.
function applySectorSet (local, m) {
  const s = mapData?.sectors?.[local]
  if (!s) return
  if (m.what === 'floor' || m.what === 'ceiling') {
    const changed = view.setSectorTexture(local, m.what, m.texture.toUpperCase(),
      { pxPerUnit: params.has('texpx') ? Number(params.get('texpx')) : (map.textureScale ?? 32), sheets: animCatalogue.sheets })
    if (changed >= 0) view.setDynamicSectors(dynamicSectors([changed]))
  } else if (m.what === 'light') {
    if (m.fadeMs > 0) {
      lightFades.set(local, { from: s.light, to: m.light, start: performance.now(), ms: m.fadeMs })
    } else {
      lightFades.delete(local)
      s.light = m.light
    }
    view.setDynamicSectors(dynamicSectors([local]))
  } else {
    // An effect: that kind's entry replaced, or taken away, and the effects
    // drawn again from the list (the frame loop rebuilds when it changes).
    const list = (mapData.fx ??= [])
    const at = list.findIndex((e) => e.sector === local && e.kind === m.kind)
    const e = m.on ? effectFrom(core, local, m.kind, m.params) : null
    if (at >= 0) list.splice(at, 1)
    if (e) list.push(e)
    sectorFxFor = null
  }
}

// The module's own keys held now: by keyboard code, and by touch button.
function heldActions () {
  let bits = 0
  moduleKeys.forEach((k, i) => { if (held.has(k.code)) bits |= 1 << i })
  if (touch) bits |= touch.state.buttons(keyBits)
  return bits
}

function frame (now) {
  requestAnimationFrame(frame)
  const tFrame = performance.now()
  const dt = Math.min(0.25, (now - last) / 1000)
  last = now

  // A level change lands FIRST, before any input is sampled or any tick is
  // simulated: stepping the old world for even one frame after the server has
  // moved us would send inputs addressed to a room we are no longer in. And
  // with it every other word about which levels are held, in the order said.
  if (LIVE) drainSeam()

  // Turning the view is the player's -- unless a cinematic took the controls.
  const turnable = !camLockYaw && !controlsTaken()
  if (turnable && held.has('KeyQ')) yaw -= 1.5 * dt
  if (turnable && held.has('KeyR')) yaw += 1.5 * dt
  if (touch) {
    touch.setStickHidden(controlsTaken())
    // TAKEN once per frame, which zeroes them. A drag accumulates between
    // frames and is applied whole; reading without clearing would turn by the
    // same drag every frame until the thumb moved again, which is a camera
    // that spins when you stop touching it. Taken even when locked, so the
    // drag does not pile up and land the moment the lock lifts.
    const dyaw = touch.state.takeYaw()
    if (turnable) yaw += dyaw
    const pinch = touch.state.takeZoom()
    // Pixels of pinch to a zoom factor. Exponential rather than linear so that
    // the same finger movement is the same proportional change however far in
    // you already are -- a linear step is imperceptible when zoomed out and a
    // lurch when zoomed in.
    if (pinch !== 0) zoomBy(Math.exp(pinch * 0.004))
  }

  acc += dt
  // STEER THE CLOCK: run a tick more, or one fewer, so the server holds the
  // few ticks of our input it needs and no more (Predictor._steerClock). Every
  // tick it holds is a sixtieth of a second between pressing something and its
  // effect -- and nothing ever read this: the lead was whatever it was at the
  // start plus every catch-up burst after a hitch, measured at 13 ticks (over
  // 200 ms) where 2 were wanted.
  if (LIVE && predictor && !PAUSED) {
    const adj = predictor.takeClockAdjust()
    if (adj > 0) acc += DT * adj
    else if (adj < 0) acc = Math.max(0, acc + DT * adj)
  }
  // Bounded, so a backgrounded tab does not return and run ten thousand ticks
  // in one frame. Dropping the missed time is the same choice the server makes
  // after a stall, and for the same reason.
  const tSim = performance.now()
  let budget = PAUSED ? 0 : 8
  while (acc >= DT && budget-- > 0) {
    const f = currentInput()
    if (LIVE) {
      // HOLD AT THE EDGE RATHER THAN STEP OFF IT.
      //
      // This client predicts ahead of the server, so at a seam it reaches the
      // boundary first and walks out of its own cell -- into no sector, because
      // the next cell is a different map and the simulation only knows this
      // one. A body with no sector is one the server has not crossed yet and
      // will not agree with for another few hundred milliseconds, and every
      // snapshot drags it back while the prediction pushes it out again.
      //
      // What that oscillation feels like is not a stutter, it is the controls
      // going dead: NOTHING responds, not even backing away from the edge.
      //
      // So a step that lands in no sector is refused, and the body keeps the
      // last position that was in one. The boundary becomes a wall for as long
      // as the server takes to say otherwise -- which it then does, and the
      // body carries on. Everything OTHER than walking further out still works,
      // which is the whole difference between "it hitched" and "it jammed".
      //
      // Client-side only, and deliberately not in the shared controller: the
      // server is right to let the body through, and this is a prediction
      // declining to guess at something only the server can know.
      // ONLY AT A CELL BOUNDARY. A pit inside the level is a fall and must
      // stay one; this is about leaving the map the simulation is holding.
      const before = world.readAgent(agent)
      stepFrom = before.pos
      // Our missiles fly BEFORE the step, where the server's on_tick flies its
      // own, and one that bursts at our feet shoves us in this very step.
      // This tick's launches are added after it -- the server's order again:
      // a missile spawned this tick first moves on the next.
      ghosts.tick()
      predictor.step(f)
      if (predictor.weapon) ownWeapon.push(predictor.weapon)
      for (const l of predictor.weapon?.launches ?? []) ghosts.launch(l)
      const after = world.readAgent(agent)
      if (!region && before.sector >= 0 && after.sector < 0 && leftTheCell(after)) {
        // AND STOPPED, not merely put back.
        //
        // Restoring the position and keeping the velocity that carried it out
        // means the body tries again next tick and is put back again: it runs
        // on the spot, animating, in the direction it was going when the
        // boundary arrived. Reported as exactly that -- "the character keeps
        // moving on himself in the direction before the map load" -- and it is
        // a worse jam than the one it replaced, because it looks like input
        // being ignored rather than like a wall.
        //
        // The vertical component survives: the ground is still under it and
        // gravity is not the thing being refused.
        world.writeAgent(agent, { ...before, vel: { x: 0n, y: 0n, z: before.vel.z } })
      }
      inputHistory = [f, ...inputHistory].slice(0, 8)   // redundancy, protocol §4.1
      conn.send(predictor.tick, predictor.lastSnapshot?.serverTick ?? 0, inputHistory)
    } else {
      stepFrom = world.readAgent(agent).pos
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
    // And switches thrown, here or before we arrived. Purely visual: what the
    // switch DID travels as sector motion, exactly as it did before.
    if (skyMessage) {
      sky.set(skyFromMessage(skyMessage))
      skyMessage = null
    }
    while (musicCues.length) {
      const m = musicCues.shift()
      music.cue(m.cue, { fadeMs: m.fadeMs, at: m.at })
        .catch((e) => console.warn(`[music] ${e.message}`))
    }
    while (sideTextures.length) {
      const m = sideTextures.shift()
      const name = m.type === MSG.SECTOR_SET ? m.sector : m.side
      // By name: a switch thrown in another level of the region waits until
      // that level is home, when its full view exists to show it.
      if (region && (name >>> 16) !== homeLevel) {
        const k = name >>> 16
        if (!sideTextureLog.has(k)) sideTextureLog.set(k, [])
        sideTextureLog.get(k).push(m)
        continue
      }
      if (region) {
        if (!sideTextureLog.has(homeLevel)) sideTextureLog.set(homeLevel, [])
        sideTextureLog.get(homeLevel).push(m)
      }
      if (m.type === MSG.SECTOR_SET) {
        applySectorSet(homeLocal(m.sector), m)
        continue
      }
      const side = homeLocal(m.side)
      const sector = view.setSideTexture(side, m.field, m.name.toUpperCase(),
        { pxPerUnit: params.has('texpx') ? Number(params.get('texpx')) : (map.textureScale ?? 32), sheets: animCatalogue.sheets })
      if (sector >= 0) {
        mapData.sides[side] = view.map.sides[side]
        view.setDynamicSectors([...new Set([...movers, sector])])
        view.rebuildDynamic()
      }
    }
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
  const moved = movers.length > 0 &&
                followMovers(movers, liveSector, mapData.sectors, mapData.points)
  blend('follow', performance.now() - tFollow)

  // AND THE REBUILD IS GUARDED ON MOVEMENT, not on there being movers at all.
  // `movers` never shrinks (takeMotion), so the unguarded form rebuilt every
  // frame for the rest of the map once any door had opened once -- see
  // mapmovers.js, which is where that reasoning lives.
  const tMesh = performance.now()
  if (fresh) view.setDynamicSectors(dynamicSectors())   // rebuilds both halves itself
  else if (moved) view.rebuildDynamic()
  // Sector lights fading: the light written, the fading sectors rebuilt; one
  // that has arrived leaves the per-frame half again.
  if (lightFades.size) {
    const t = performance.now()
    let ended = false
    for (const [i, f] of lightFades) {
      const k = Math.min(1, (t - f.start) / f.ms)
      const s = mapData.sectors[i]
      if (s) s.light = Math.round(f.from + (f.to - f.from) * k)
      if (k >= 1) { lightFades.delete(i); ended = true }
    }
    if (ended) view.setDynamicSectors(dynamicSectors())
    else view.rebuildDynamic()
  }
  blend('mesh', performance.now() - tMesh)

  let a = world.readAgent(agent)
  // Stuck, and which kind (client/src/seam/diag.js).
  if (LIVE) {
    bodyWatch.frame({ x: fxToFloat(a.pos.x), y: fxToFloat(a.pos.y), sector: a.sector },
                    !!lastSampled && (lastSampled.moveX !== 0 || lastSampled.moveY !== 0),
                    world.tick)
  }
  if (!LIVE && fxToFloat(a.pos.z) < VOID_FLOOR) {
    falls++
    world.writeAgent(agent, { ...a, pos: spawn, vel: { x: 0n, y: 0n, z: 0n }, grounded: false })
    camera.snapTo({ x: toThreeX(fxToFloat(spawn.x)), y: toThreeY(fxToFloat(spawn.z)), z: toThreeZ(fxToFloat(spawn.y)) })
    a = world.readAgent(agent)
  }
  const px = fxToFloat(a.pos.x)
  const py = fxToFloat(a.pos.y)
  const pz = fxToFloat(a.pos.z)
  // DRAWN BETWEEN TICKS. The simulation moves in whole 60 Hz ticks and the
  // display runs at its own rate, so a frame drawn at the last tick's position
  // shows the body standing still for a frame and then jumping -- at 120 Hz
  // every other frame, at 60 Hz whenever a frame gets two ticks or none. Fast,
  // that is the body and the camera jiggling. So the body is drawn where it was
  // `acc` of the way from the tick before to the latest one (a tick behind,
  // the price of every interpolated renderer). A jump of more than a few units
  // is a teleport, a respawn or a new frame of reference, and is drawn as one.
  // The simulation, the sounds and the HUD read `a`; only what is DRAWN follows.
  const drawn = between(stepFrom && { x: fxToFloat(stepFrom.x), y: fxToFloat(stepFrom.y), z: fxToFloat(stepFrom.z) },
                        { x: px, y: py, z: pz }, acc / DT)
  const subject = { x: toThreeX(drawn.x), y: toThreeY(drawn.z), z: toThreeZ(drawn.y) }

  // THIS CLIENT'S OWN SOUNDS, the moment it predicts the action: its jump, its
  // landing, its swing, its shot (audio/ownsounds.js). The server's copies are
  // marked PREDICTED and dropped when they arrive.
  if (LIVE) {
    // OUR WEAPON, AS THE CORE PREDICTED IT (docs/WEAPON_PREDICTION.md): what it
    // played, drew and kicked on the tick it fired, rather than a guess from the
    // buttons. The button guess below stays for jumps and landings, and for a
    // weapon the server did not send a table for.
    const predictsWeapon = world.hasWeapons
    if (predictsWeapon) ownSounds.mute.add(WEAPON_GROUP)
    else ownSounds.mute.delete(WEAPON_GROUP)
    for (const wf of ownWeapon.splice(0)) {
      let kick = false
      for (const e of wf.events) {
        if (e.action === WEAPON_ACTION.SOUND && e.predicted && e.sound) {
          sfx.play(e.sound, { x: px, y: py, z: pz }, { seed: Math.imul(world.tick ^ e.sound, 2654435761), key: mySelfHandle })
          kick = true
        }
        if (e.action === WEAPON_ACTION.HITSCAN || e.action === WEAPON_ACTION.LAUNCH ||
            e.action === WEAPON_ACTION.DAMAGE_AREA) kick = true
      }
      for (const v of wf.vfx) drawVfx(v, ownMuzzle())
      if (kick) viewModel.fire()
    }
    for (const h of ownSounds.step({
      grounded: !!a.grounded, velZ: fxToFloat(a.vel.z), jumps: a.jumpsUsed | 0, attackLeft: a.attackLeft | 0,
      buttons: lastSampled?.buttons ?? 0, tick: world.tick
    })) {
      sfx.play(h, { x: px, y: py, z: pz }, { seed: Math.imul(world.tick ^ h, 2654435761), key: mySelfHandle })
    }
    // The gun kicks on the same predicted shot.
    if (!predictsWeapon && ownSounds.fired.some((f) => f.group === WEAPON_GROUP)) viewModel.fire()
  }

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
    // The vertical half of the same sweep, from the same phase, so an overhead
    // chop tilts as it widens rather than the two being separately guessed.
    const pitch = pitchOffset({ ...swingShape, left: a.attackLeft }) ?? 0
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
    // Not in first person: a wedge on the floor drawn from the eyes is a slab
    // across the view, and the weapon on screen says what the swing does.
    // Debug drawings, so they go with the overlay (module.txt `debug off`).
    swingWhole.visible = !isFps() && debugShown
    swingWhole.position.set(subject.x, at - 0.02, subject.z)
    aimSwing(swingWhole, facingTurns, swingMid, swingPitchMid)

    swing.visible = off !== null && !isFps() && debugShown
    if (off !== null) {
      swing.position.set(subject.x, at, subject.z)
      aimSwing(swing, facingTurns, off, pitch)
    }

    // One segment per TICK of the swing, at the angle the collider held then.
    // Offered every frame and added only when the countdown moves, so the trail
    // has as many segments as the swing had ticks whatever the display rate is.
    if (debugShown) swingTrail.offer({
      shape: swingShape, left: a.attackLeft, offsetTurns: off,
      pitchTurns: pitch, facingTurns,
      x: subject.x, y: at - 0.01, z: subject.z, now: performance.now() / 1000
    })
    swingTrail.update(performance.now() / 1000)
  }
  // THE SLASHES: this body's, from its predicted swing (not in first person,
  // where the weapon on screen is the swing), and every other body's, from its
  // swing countdown (ATTACK) and the parameters the server sent for it.
  {
    const now = performance.now() / 1000
    if (!isFps()) {
      const shape = readSwingShape(world, agent)
      slash.offer('self', {
        shape, left: a.attackLeft,
        offsetTurns: sweepOffset({ ...shape, left: a.attackLeft }),
        pitchTurns: pitchOffset({ ...shape, left: a.attackLeft }) ?? 0,
        facingTurns: (a.facing >>> 0) / 4294967296,
        x: subject.x, y: subject.y + bodyHeight / 2, z: subject.z,
        style: styleFor(swingStyles, { weapon: hudValues.get('hud.weapon') || null }),
        now
      })
    }
    for (const [index, ent] of others.entities) {
      if (!ent.attackLeft && !slash.swingers.has(index)) continue
      const shape = readSwingShape(remoteParams.view(index), index)
      const rec = ent.kind ? props?.kinds.get(ent.kind) : null
      const h = rec?.height ?? bodyHeight
      const p = ent.mesh.position
      slash.offer(index, {
        shape, left: ent.attackLeft ?? 0,
        offsetTurns: sweepOffset({ ...shape, left: ent.attackLeft ?? 0 }),
        pitchTurns: pitchOffset({ ...shape, left: ent.attackLeft ?? 0 }) ?? 0,
        facingTurns: ((ent.facing ?? 0) >>> 0) / 4294967296,
        x: p.x, y: p.y - (ent.mesh.userData.yOffset ?? 0) + h / 2, z: p.z,
        style: styleFor(swingStyles, { monster: rec?.name ?? null }),
        now
      })
    }
    slash.update(now)
  }

  // The body's MIDDLE, from the body's declared height rather than from the
  // mesh's yOffset.
  //
  // They used to be the same number and are not any more. yOffset is the offset
  // from the simulation's position -- the FEET -- to the mesh's own origin, which
  // is height/2 for a capsule and ZERO for a sprite quad standing on the ground.
  // So the moment a player became a sprite, these two sites silently dropped to
  // the feet: an aiming arrow drawn along the floor, and an occlusion window
  // centred low enough to leave the head behind the wall. The position write
  // above is correct as-is and must keep using yOffset.
  // THE LOCAL PLAYER'S OWN ANIMATION, which nothing used to do at all.
  //
  // Its state is RESOLVED rather than read: the server declares one when a script
  // or a weapon says so, and zero means "animate from motion". The motion here is
  // the PREDICTED agent -- velocity, grounded, climbing, attackLeft, all of it
  // computed locally this tick -- so a jump animates on the tick the body leaves
  // the ground rather than a round trip later, and a swing animates the instant
  // the collider starts rather than when the server agrees it did.
  //
  // Outside `if (LIVE)`: a player animating is not a live feature.
  // The body may have to become the avatar, or a different one. Cheap: buildBody
  // returns immediately unless the answer has actually changed.
  buildBody()
  if (props && bodyAvatar >= 0) {
    props.setState(body, resolveState(selfAnimation, a, props.avatarFor(selfAvatar)), now / 1000)
    // A sprite quad has to face the camera, and keep its own heading somewhere
    // the billboard has not overwritten -- the same two lines every remote body
    // gets, which the local one never needed while it was a capsule.
    body.rotation.y = billboardYaw(camViewYaw)
    body.userData.facingTurns = ((a.facing >>> 0) / 4294967296)
  }

  const middle = { x: subject.x, y: subject.y + bodyHeight / 2, z: subject.z }
  // THE THIRD-PERSON HELPERS stay out of a first-person view: the facing arrow
  // is drawn from the eyes straight ahead (a disc in the middle of the screen),
  // and the occlusion window fades the walls between a camera and a body that,
  // here, are in the same place.
  if (arrow) {
    arrow.visible = !isFps() && debugShown
    // From the body's middle, which is roughly where a reach comes from and is
    // where you look when you are judging whether a door is in range.
    aimFacingArrow(arrow, middle, facingDirection(a.facing))
  }
  // Aimed at the body's middle rather than its feet: a window centred on the
  // feet leaves the head behind the wall.
  view.setOcclusionTarget(isFps() ? null : middle)
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
    // On the render clock, not `newest snapshot + this client's tick fraction`:
    // snapshots come every other tick, so that went forward, slid back and
    // jumped two, and every remote body stuttered at 30 Hz.
    const drawn = renderClock.advance(dt)
    others.update(Math.floor(drawn), drawn - Math.floor(drawn))
    // WHERE EACH REMOTE BODY IS DRAWN, past the plain interpolation:
    //   - a body our blast has shoved is ahead of the stream by the part of
    //     the shove the stream has not drawn yet (render/knock.js);
    //   - a body near us is drawn at the present rather than a beat behind,
    //     fading back to the interpolated picture with distance
    //     (render/nearnow.js).
    {
      const now = serverTickOf(predictor.tick)
      const nowF = now === null ? null : now + acc / DT
      const kept = new Set()
      const three = (o) => o && { x: toThreeX(o.x), y: toThreeY(o.z), z: toThreeZ(o.y) }
      for (const [index, ent] of others.entities) {
        const key = `${index}/${ent.generation}`
        const pos = ent.mesh.position
        const kR = now === null ? null : three(knock.offset(key, now, others.renderTick))
        if (kR) { pos.x += kR.x; pos.y += kR.y; pos.z += kR.z }
        const last = ent.samples.at(-1)
        const w = (nowF === null || !last) ? 0 : presentWeight(Math.hypot(last.x - subject.x, last.z - subject.z))
        ent.presentWeight = w
        if (w <= 0) continue
        kept.add(key)
        const p = near.present(key, ent.samples, nowF, dt, (from) => three(knock.offset(key, now, from)))
        if (!p) continue
        const yo = ent.mesh.userData.yOffset ?? 0
        pos.x += (p.x - pos.x) * w
        pos.y += (p.y + yo - pos.y) * w
        pos.z += (p.z - pos.z) * w
      }
      near.keep(kept)
      knock.prune(others.renderTick)
    }
    // The server's copy of a rocket we are already flying is not drawn twice.
    for (const [index, ent] of others.entities) {
      if (!ent.kind) continue
      ent.mesh.visible = !ghosts.hides(`${index}/${ent.generation}`, ent.kind,
                                       { x: ent.mesh.position.x, y: -ent.mesh.position.z })
    }
    // performance.now(), NOT the frame's `now`. They are the same clock but not
    // the same instant: `now` is when the frame began, and the tracers being
    // faded here were spawned later in this same frame, from drainSnapshots.
    // Fading them against the earlier stamp gave them a negative age, which
    // used to mean "retire immediately". Out of `if (props)` for the same
    // reason as the animator: a tracer is not a prop.
    // WHERE AN ATTACHED EMITTER'S BODY IS RIGHT NOW. The local player is not in
    // `others` -- an observer never receives its own entity block -- so the two
    // come from different places, and vfx.js is not told the difference.
    if (vfx) {
      vfx.update(performance.now() / 1000, (index) => {
        if (myAgentIndex !== null && index === myAgentIndex) {
          return { x: subject.x, y: subject.y + bodyHeight / 2, z: subject.z }
        }
        const ent = others.entities.get(index)
        return ent ? ent.mesh.position : null
      })
    }
    if (props) {
      // ORDER MATTERS HERE, and it used to be wrong.
      //
      // props.update() READS userData.facingTurns to pick which of eight views a
      // sprite shows. It used to run BEFORE the loop that writes it, so every
      // eight-view sprite in the game chose its view from LAST frame's facing --
      // invisible while standing still and a frame of lag on every turn.
      //
      // So: facing and billboard first, then the state, then the clock that
      // consumes both.
      //
      // Billboarding is yaw only, and derived rather than guessed. See
      // billboardYaw -- the first version had the sign of yaw backwards.
      const bb = billboardYaw(camViewYaw)
      for (const g of ghosts.flying) if (g.mesh) g.mesh.rotation.y = bb
      for (const ent of others.entities.values()) {
        ent.mesh.rotation.y = bb
        // Which way the thing is POINTING, kept on the mesh for the eight-view
        // sprites to read. It cannot be recovered from the mesh's own rotation,
        // because that has just been overwritten to face the camera -- which is
        // what billboarding is.
        //
        // For EVERY entity, players included. A remote player is kind 0 and used
        // to be skipped here, which was right while it was drawn as a capsule and
        // wrong the moment it became a sprite with eight views.
        ent.mesh.userData.facingTurns = ((ent.facing ?? 0) >>> 0) / 4294967296
      }
      // Hand the server's declared animation state to the renderer. The entity
      // view records it and the registry plays it, and until this line existed
      // the two were never introduced -- a barrel switched to its explosion
      // state and went on showing idle frames until it vanished.
      //
      // Players included, and for them the state is RESOLVED rather than passed
      // through: zero means "nothing declared", so a remote player animates from
      // the motion two samples apart imply. See render/animstate.js.
      for (const ent of others.entities.values()) {
        const declared = ent.animationState ?? 0
        if (ent.kind) { props.setState(ent.mesh, declared, now / 1000); continue }
        // Zero is "nothing declared", so a remote player animates from the
        // motion its two newest samples imply. Any non-zero value is the
        // server's, and wins.
        props.setState(ent.mesh,
                       declared || stateFor(props.avatarFor(ent.avatar ?? 0), remoteCondition(ent)),
                       now / 1000)
      }
    }
  }

  // The prop clock, OUT of `if (LIVE)`.
  //
  // The same argument the animated surfaces above make for themselves: the local
  // player's own animation is neither a live feature nor a prop feature, and
  // leaving this inside `if (LIVE)` would mean a player stood frozen in solo
  // mode while every barrel around them animated.
  //
  // The camera follows the PREDICTED position -- here, whichever of the two
  // modes produced it. Never a snapshot: see camera.js.
  // Pitch arrives in TURNS, which is the engine's angle unit everywhere else;
  // cameraOffset wants radians.
  camera.cfg.offset = cameraOffset(
    yaw, camDistance(),
    camOverridePitch === null ? undefined : camOverridePitch * Math.PI * 2)
  // The camera path this tag names, rebuilt only when its points have moved.
  //
  // Resolved HERE rather than where CAMERA_SET is applied, because a level's
  // camera settings arrive on join -- before the map is necessarily parsed.
  // Rebuilding is gated on refreshPathPositions reporting a change: a static
  // path is the overwhelming common case, and a spline's arc-length table is
  // the expensive part, not the reading.
  if (camPathTag < 0) {
    camPathSpline = null
    camPathIndex = -1
  } else {
    if (camPathIndex < 0 || camPaths[camPathIndex]?.tag !== camPathTag) {
      camPathIndex = pathIndexForTag(camPaths, camPathTag)
      camPathSpline = null
      if (camPathIndex < 0) console.warn(`[camera] no path tagged ${camPathTag}`)
    }
    if (camPathIndex >= 0) {
      const p = camPaths[camPathIndex]
      const moved = refreshPath(p)
      if (pathView && moved) pathView.update(new Set([p.index]))
      if (!camPathSpline || moved) {
        // THREE space, because that is the space `subject` is in and the space
        // the eye is written back into. Safe to convert first rather than last:
        // toThreeX/Y/Z is a signed permutation, so it is linear and an
        // isometry, and both the curve and its arc length commute with it --
        // asserted in spline.test.js, with a nonlinear map as the control.
        const pts = []
        for (let i = 0; i < p.pos.length; i += 3) {
          pts.push({
            x: toThreeX(p.pos[i]),
            y: toThreeY(p.pos[i + 2]),
            z: toThreeZ(p.pos[i + 1])
          })
        }
        camPathSpline = buildSpline(pts, { loop: p.loop })
        // By TAG, so the camera does not read a rebuilt path as a different
        // one and restart a flythrough on every frame of a lift's travel.
        camPathSpline.id = p.tag
      }
    }
  }

  // The overlay redraws any path whose points moved, not just the one the
  // camera is on -- an author wants to see a path riding a lift whether or not
  // the shot is currently using it. Only when it is switched on at all.
  if (pathView) {
    const moved = new Set()
    for (const p of camPaths) {
      if (p.index === camPathIndex) continue   // already refreshed above
      if (refreshPath(p)) moved.add(p.index)
    }
    if (moved.size) pathView.update(moved)
    pathView.setActive(camPathTag)
  }

  // The agent's facing, so the camera can aim AHEAD of it rather than at it.
  // facingDirection already returns three-space -- it is the same vector the
  // debug arrow is drawn along -- so converting again would negate z twice.
  // Its velocity too, in three-space, for a lead that grows with speed
  // (Camera leadVelocity). Units a second, like the engine's.
  const eye = camera.update(subject, dt, facingDirection(a.facing), camPathSpline,
                            { x: fxToFloat(a.vel.x), z: -fxToFloat(a.vel.y) })
  cam3.position.set(eye.x, eye.y, eye.z)
  cam3.lookAt(eye.lookAt.x, eye.lookAt.y, eye.lookAt.z)
  // FIRST PERSON: the camera is the player's eyes, looking where the view does,
  // and the player's own body is not drawn -- it would fill the screen. Over
  // the third-person camera rather than instead of it, so a cinematic path
  // still has a camera to blend from when one takes over.
  body.visible = !isFps()
  // The weapon in hand, bobbing with the walk.
  {
    const held = hudValues.get('hud.weapon')
    if (held) viewModel.setWeapon(held)
    // WHICH ANIMATION the weapon is in (docs/VIEWMODEL_PHASES.md): the state its
    // phase set, PREDICTED -- the tick it happens -- with the phase's entry
    // tick, so a second swing of the same phase restarts it. A weapon the
    // client does not predict (one on the server's Lua path) goes by the
    // snapshot's, a round trip late; with neither, ready and fire as before.
    if (LIVE && world.hasWeapons) {
      const st = world.animationState(agent)
      viewModel.setAnim(st, `${st}:${world.readWeapon(agent).entered}`)
    } else if (LIVE && lastSelfAnim !== null) {
      viewModel.setAnim(lastSelfAnim, lastSelfAnim)
    } else viewModel.setAnim(null)
    const sp = Math.hypot(fxToFloat(a.vel.x), fxToFloat(a.vel.y)) / 8
    viewModel.update(dt, a.grounded ? sp : 0, isFps() && !camPathSpline,
                     { width: innerWidth, height: innerHeight })
  }
  // THE NEAR PLANE. 0.5 suits a camera metres behind the body; at the eyes it
  // is further out than the body's own radius (DOOM's 16 units are 0.33 here),
  // so walking up to a wall put the wall inside it and it vanished. The plane's
  // CORNERS reach further than its centre -- about 1.7x at this field of view --
  // so it has to sit well inside the radius, not just under it. Depth
  // precision at 0.05 is still ~1 cm at 100 m.
  const wantNear = isFps() && !camPathSpline ? 0.05 : 0.5
  if (cam3.near !== wantNear) { cam3.near = wantNear; cam3.updateProjectionMatrix() }
  if (isFps() && !camPathSpline) {
    // The core's eye, where a first-person shot leaves from (EYE_FRACTION).
    const eyeH = (bodyHeight || 1.17) * EYE_FRACTION
    const f = fpsLook.forward()
    // Simulation (x, y, z-up) to three (x, z, -y), as mapgeom.js does.
    eye.x = subject.x; eye.y = subject.y + eyeH; eye.z = subject.z
    eye.lookAt = { x: eye.x + f.x, y: eye.y + f.z, z: eye.z - f.y }
    cam3.position.set(eye.x, eye.y, eye.z)
    cam3.lookAt(eye.lookAt.x, eye.lookAt.y, eye.lookAt.z)
  }
  // What the next frame's input and this frame's billboards are measured
  // against. Written here because only now is it known: the path crossfade is
  // resolved inside camera.update.
  // In FIRST PERSON the orbit angle is nobody's view -- the eye is the player's
  // and looks where the mouse says -- so the yaw is read off the eye itself, as
  // on a path. Without this every billboard faced a camera that was not there
  // and showed edge-on.
  camViewYaw = viewYaw(eye, yaw, isFps() && !camPathSpline ? 1 : camera.pathMix)

  // THE LISTENER, once a frame, now that the camera is placed: at the body, in
  // SIMULATION space, facing the way the camera looks -- so a sound on the
  // left of the screen is heard on the left. Converted from three-space here
  // rather than taken off the camera, which is Y-up with z = -y and would pan
  // everything backwards (sound/docs/EMBEDDING.md, 4).
  {
    const fx3 = eye.lookAt.x - eye.x
    const fz3 = eye.lookAt.z - eye.z
    const yawTurns = (Math.atan2(-fz3, fx3) / (2 * Math.PI) + 1) % 1
    // AT THE CAMERA while a camera path has it (a flythrough, a cutscene): the
    // camera is somewhere else then, and the scene is heard from where it is
    // seen. Blended with the path's own crossfade, so the ear travels with the
    // view rather than jumping.
    const m = camera.pathMix ?? 0
    const ex = eye.x, ey = -eye.z, ez = eye.y      // three-space back to simulation
    lastEar = m > 0
      ? { x: px + (ex - px) * m, y: py + (ey - py) * m, z: pz + (ez - pz) * m }
      : { x: px, y: py, z: pz }
    sfx.listener(lastEar, Math.floor(yawTurns * 4294967296) >>> 0)
    // And whatever a sound is following, where it is drawn now.
    const keys = sfx.followed()
    if (keys.length) {
      const at = []
      for (const k of keys) {
        const index = (k & 0x00ffffff) - 1
        if (index === myAgentIndex) { at.push([k, { x: px, y: py, z: pz }]); continue }
        const ent = others?.entities?.get(index)
        if (!ent) continue
        const m = ent.mesh.position
        at.push([k, { x: m.x, y: -m.z, z: m.y - (ent.mesh.userData.yOffset ?? 0) }])
      }
      sfx.follow(at)
    }
  }

  // Did the view move with the body across the last crossing?
  // client/test/seamcam.test.js reads this.
  crossing.frame(eye, subject)

  // The prop clock, OUT of `if (LIVE)`.
  //
  // The same argument the animated surfaces above make for themselves: the
  // local player's own animation is neither a live feature nor a prop feature,
  // and leaving this inside `if (LIVE)` would mean a player stood frozen in
  // solo mode while every barrel around them animated.
  //
  // The camera's position is handed in so a sprite can choose which of its
  // eight views to show: the view depends on the angle between the thing's
  // facing and the direction to the viewer, and only the caller knows the
  // second half.
  //
  // AFTER the camera has been placed, not before. It used to run sixty lines
  // earlier and hand over LAST frame's position, which is a frame of lag on an
  // orbit camera and a much larger error on a path, where the eye can cross the
  // room between two frames.
  if (props) props.update(now / 1000, cam3.position)

  // SECTOR EFFECTS (docs/SECTOR_FX.md): the home level's rain, snow and fog,
  // rebuilt when the level is a different one and drawn where its view is.
  // Neighbours seen over a seam draw none -- they are built from the worker's
  // groups, which do not carry them.
  if (sectorFxFor !== mapData) {
    sectorFx?.dispose()
    sectorFx = (mapData?.fx?.length ?? 0) > 0
      ? new SectorFx({ THREE, map: mapData, touch: wantsTouch(params) })
      : null
    if (sectorFx) scene.add(sectorFx.root)
    sectorFxFor = mapData
  }
  if (sectorFx) {
    const o = view.root.position
    sectorFx.setOrigin(o.x, o.y, o.z)
    sectorFx.update(now / 1000)
  }

  // GRAB on the touch controls while this body's rules hold walls by a
  // button (climb_enabled on, climb_auto off: docs/PLAYER.md) AND the level
  // has a wall to hold -- a DOOM map has none, and a button that does nothing
  // is clutter. setShown does nothing when the answer has not changed.
  if (touch) {
    if (climbableFor !== mapData) {
      climbableFor = mapData
      climbableHere = (mapData?.sides ?? []).some((sd) => (sd.flags & SIDE_FLAG.CLIMBABLE) !== 0)
    }
    const holds = climbableHere && world.getParamRaw(agent, CLIMB_ENABLED_ID) !== 0n &&
                  world.getParamRaw(agent, CLIMB_AUTO_ID) === 0n
    touch.setShown(holds ? ['grab'] : [])
  }
  const tDraw = performance.now()
  sky.update(cam3)
  renderer.render(scene, cam3)
  // The fog pass, over the picture just drawn; a level without fog skips it.
  if (sectorFx?.hasFog) sectorFx.renderFog(renderer, scene, cam3)
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
  // THE HUD, from the module's layout (render/layout.js): the client's own
  // values and the server's (HUD_SET), every frame -- HudView only touches
  // what changed.
  // The typed subtitle, so far -- and its sound, once a frame however many
  // steps the frame covered (a burst of clicks after a hitch is noise). Played
  // AT THE EAR: centred and at full volume, which is at the camera whenever a
  // camera path has taken it somewhere else (the listener, above).
  if (typer) {
    const now = performance.now() / 1000
    hudValues.set('hud.subtitle', typer.shownAt(now))
    const steps = typer.takeSounds(now)
    if (steps.length && typerSound) {
      sfx.play(typerSound, lastEar, { seed: steps.at(-1) })
      typerSounds++
    }
    if (typer.doneAt(now)) typer = null
  }
  if (LIVE && latestHp !== undefined && latestHp !== null) {
    hudView.update({ hp: latestHp, map: homeName ?? '' })
    hudView.update(hudValues)
    // And a panel's decor, which is HUD elements too (docs/PANEL_DECOR_VALUES.md).
    ui?.update({ hp: latestHp, map: homeName ?? '' })
    ui?.update(hudValues)
  }

  // The synth's own numbers, when it has any. A background noise and a synth
  // that is struggling sound alike; steals, drops and queue depth are what
  // separate them, and the sample rate is there because the engine renders at a
  // fixed 48 kHz and does not resample.
  const musicLine = LIVE && debugShown ? music.health() : null

  const st = predictor?.stats
  if (debugShown) {
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

    // The camera path, when there is one. `mix` is the part worth watching: it
    // is the crossfade between the free camera and the path, so an attach or a
    // detach is visible as a number easing rather than as a cut you have to
    // catch by eye.
    if (camPathSpline) {
      const ps = camera.pathState
      hud.textContent +=
        `\npath ${camPathTag} along ${ps.along.toFixed(1)}/${camPathSpline.length.toFixed(1)}` +
        ` mix ${ps.mix.toFixed(2)}${ps.attached ? '' : ' (free)'}${ps.clamped ? ' clamped' : ''}`
    } else if (camPathTag >= 0) {
      hud.textContent += `\npath ${camPathTag} UNRESOLVED`
    }
  }

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
// A sector of the HOME level's own map, by its own index, in its own
// coordinates: what the home view is built from and what followMovers compares.
function liveSector (i) {
  if (!region) return liveStoreSector(i)
  const s = world.sectorByName((homeLevel << 16) | i)
  const out = liveStoreSector(s < 0 ? i : s)
  if (out.points) for (const p of out.points) { p.x -= homeOrigin.x; p.y -= homeOrigin.y }
  if (out.motion) { out.motion.pivotX -= homeOrigin.x; out.motion.pivotY -= homeOrigin.y }
  return out
}

// A sector of the world's store, by the store's index, in world coordinates.
function liveStoreSector (i) {
  const M = core.M
  M._dnp_map_sector(simStore.ptr, i, core.buf64, core.buf32)
  const { i64, u32 } = core.views
  const b = core.buf64 / 8
  const b32 = core.buf32 / 4
  const out = { floorZ: fxToFloat(i64[b]), ceilZ: fxToFloat(i64[b + 1]) }

  // ITS POINTS, for a sector that slides or turns. Read only for the movers,
  // once a frame, and only while something is actually moving -- followMovers
  // is the thing that decides whether a rebuild follows.
  const first = u32[b32]
  const count = u32[b32 + 1]
  if (count > 0) {
    const pts = new Array(count)
    for (let k = 0; k < count; k++) {
      M._dnp_map_point(simStore.ptr, first + k, core.buf64)
      pts[k] = { x: fxToFloat(i64[b]), y: fxToFloat(i64[b + 1]) }
    }
    out.points = pts
  }

  // And where the engine has placed it, for a flat anchored to the sector.
  if (M._dnp_world_sector_motion(world.ptr, i, core.buf64, core.buf32)) {
    out.motion = {
      offX: fxToFloat(i64[b]), offY: fxToFloat(i64[b + 1]),
      pivotX: fxToFloat(i64[b + 2]), pivotY: fxToFloat(i64[b + 3]),
      turns: u32[b32] / 4294967296
    }
  }
  return out
}

// EVERYTHING IN, THEN PLAY.
//
// The last thing waited on is the textures, which is the only asset load that
// is not already awaited above. A failure here is not fatal -- the map renders
// with placeholders and said so -- so this waits for the promise to settle
// rather than for it to succeed.
await texturesReady
stageDone()


say('')
helpShown = true
showDebug(debugShown)
// THE LEVEL'S MUSIC STARTS WITH THE LEVEL: its track was loaded while the
// level loaded (held), so the audio thread is free and it starts clean. Waited
// for if it is still loading, but not for ever.
await Promise.race([firstMusic.then(() => music.whenLoaded()), new Promise((r) => setTimeout(r, 10000))])
music.release()
document.documentElement.dataset.musicStarted = String(performance.now() | 0)
requestAnimationFrame(frame)
