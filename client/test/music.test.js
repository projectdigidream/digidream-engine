//
// The music path: the synth engine, the instruments, and a baked track.
//
// WHAT THIS COVERS. Everything between "a file on disk" and "samples coming
// out": the WASM engine instantiates, gm_bank.json and gm_kit.json parse, the
// .mstk baked track loads, and rendering it produces audio that is not silence.
// Those are the parts that break when content moves, a path is wrong, or a
// track is rebuilt by a newer baker.
//
// WHAT IT DOES NOT COVER, said plainly so nobody reads more into a pass than is
// there: the AudioWorklet wiring in client/src/audio/music.js. That needs a
// real browser -- AudioWorklet and AudioContext exist nowhere else -- and
// stubbing them would only assert that this file's message names match this
// file's message names. midisynth's own tool_smoke.mjs covers the worklet and
// needs puppeteer-core, which is not installed here.
//
// The engine runs under Node because Emscripten builds do; midisynth's golden
// harness already cross-checks the WASM build against the native one that way.
//
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
const WASM = join(ROOT, 'midisynth', 'tool', 'dist', 'midisynth.mjs')
const BANK = join(ROOT, 'midisynth', 'content', 'patches', 'gm_bank.json')
const KIT = join(ROOT, 'midisynth', 'content', 'drums', 'gm_kit.json')
// DOOM's own MAP01 music, converted out of the WAD by tools/wad/mus2mid.js and
// baked by tools/wad/music_all.js. The engine's assets/music/swan_lake.mstk is
// the fallback the module overrides, and testing the module's track is testing
// what a player actually hears.
const TRACK = join(ROOT, 'modules', 'doom2', 'assets', 'music', 'd_runnin.mstk')
const INDEX = join(ROOT, 'modules', 'doom2', 'assets', 'music', 'index.json')
const MAPS = join(ROOT, 'modules', 'doom2', 'maps')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// Built, not committed. Skipping beats failing: the wasm is produced by
// midisynth/scripts/build_wasm.sh and a checkout that has not run it is not
// broken, it is unbuilt.
if (!existsSync(WASM)) {
  console.log('music: skipped (no midisynth wasm -- run midisynth/scripts/build_wasm.sh)')
  process.exit(0)
}

console.log('music: the synth, its instruments and a baked track')

const createMidiSynth = (await import(WASM)).default
const mod = await createMidiSynth()
const synth = new mod.WebSynth()

const bank = JSON.parse(synth.loadBank(readFileSync(BANK, 'utf8')))
check(bank.ok === true, `the instrument bank loads (${bank.errors?.length ?? '?'} errors)`)
const kit = JSON.parse(synth.loadDrums(readFileSync(KIT, 'utf8')))
check(kit.ok === true, `the drum kit loads (${kit.errors?.length ?? '?'} errors)`)

// --- the baked track ---------------------------------------------------------
const bytes = readFileSync(TRACK)
const load = (buf) => {
  const ptr = synth.songBufferPrepare(buf.length)
  mod.HEAPU8.set(buf, ptr)
  return JSON.parse(synth.loadTrackFromBuffer())
}
const track = load(bytes)
check(track.ok === true, `the baked track loads (${TRACK.split('/').pop()})`)
check(synth.durationMs() > 1000, `and has a duration (${synth.durationMs()} ms)`)

// --- it makes a sound --------------------------------------------------------
//
// `playing()` is a transport flag: it would be true for a track whose
// instruments all failed to load, which is exactly the failure a missing bank
// produces. A peak sample is the engine saying it has notes in its hands.
synth.setLoop(true)
synth.play()
let peak = 0
for (let block = 0; block < 400; block++) {
  const p = synth.render(128)
  const heap = mod.HEAP16
  const base = p >> 1
  for (let i = 0; i < 256; i++) {
    const v = Math.abs(heap[base + i])
    if (v > peak) peak = v
  }
}
check(synth.playing() === true, 'the transport reports playing')
check(synth.activeVoices() > 0, `with voices sounding (${synth.activeVoices()})`)
check(peak > 100, `and the output is not silence (peak ${peak} of 32767)`)

// --- THE CONTROL -------------------------------------------------------------
//
// A .mstk is integrity-sealed and "corrupt tracks are rejected outright"
// (docs/TRACK_FORMAT.md). Without this, every assertion above is satisfied by a
// loader that says yes to anything -- and "it loaded" would mean nothing.
{
  const corrupt = Buffer.from(bytes)
  // Somewhere past the header, so this is a damaged track rather than a file
  // that was never one.
  corrupt[Math.floor(corrupt.length / 2)] ^= 0xff
  const bad = load(corrupt)
  check(bad.ok === false, `CONTROL: a corrupted track is refused (${bad.ok})`)
}

// --- CHANGING MAP MUST NOT STOP THE MUSIC ------------------------------------
//
// The second map of a progression had no music at all. The client asked the
// synth to CROSSFADE to the next track, and the synth's crossfade takes an SMF:
// handing it a baked .mstk reports "not an SMF file (missing MThd)" and leaves
// the transport STOPPED -- so the swap did not merely fail to fade, it ended
// the soundtrack for the rest of the session.
//
// This is the transition the client performs now: load the next track and play
// it. The volume dip around it is a master-volume ramp and cannot affect
// whether the transport is running, which is what this asserts.
{
  const next = readFileSync(join(ROOT, 'modules', 'doom2', 'assets', 'music', 'd_stalks.mstk'))
  const rep = load(next)
  synth.setLoop(true)
  synth.play()
  let peak2 = 0
  for (let block = 0; block < 400; block++) {
    const p = synth.render(128)
    const heap = mod.HEAP16
    const base = p >> 1
    for (let i = 0; i < 256; i++) {
      const v = Math.abs(heap[base + i])
      if (v > peak2) peak2 = v
    }
  }
  check(rep.ok === true, 'the next map\'s track loads')
  check(synth.playing() === true, 'and the transport is still running after the swap')
  check(peak2 > 100, `and it is audible (peak ${peak2})`)

  // THE CONTROL, and it is the actual bug: the crossfade path refuses a baked
  // track and stops the transport. If this ever starts succeeding, the synth
  // has grown a track-aware crossfade and music.js should go back to using it.
  const b2 = readFileSync(TRACK)
  const ptr = synth.songBufferPrepare(b2.length)
  mod.HEAPU8.set(b2, ptr)
  const xf = JSON.parse(synth.crossfadeFromBuffer(1200))
  check(xf.ok === false,
        `CONTROL: crossfadeFromBuffer still refuses a .mstk (${xf.errors?.[0] ?? 'accepted!'})`)
}

// --- the map-to-track manifest -----------------------------------------------
//
// The client looks a track up by the map's CONTENT HASH, which is the only
// identity it has for a map. A rebake changes every hash, so the manifest going
// stale is the normal failure -- and it is silent: the game simply plays
// nothing. This is the check that notices.
if (existsSync(INDEX) && existsSync(MAPS)) {
  const index = JSON.parse(readFileSync(INDEX, 'utf8'))
  check(Object.keys(index).length >= 30,
        `the manifest maps ${Object.keys(index).length} maps`)

  // Every cue names a track that is actually there. A manifest is generated and
  // the tracks are not committed with it, so an entry pointing at a file nobody
  // baked is the way this goes wrong -- and it goes wrong silently.
  let danglingCues = []
  for (const [key, cues] of Object.entries(index)) {
    for (const [name, file] of Object.entries(cues)) {
      if (!existsSync(join(ROOT, 'modules', 'doom2', 'assets', 'music', file))) {
        danglingCues.push(`${key}/${name} -> ${file}`)
      }
    }
  }
  check(danglingCues.length === 0,
        `every cue names a track that exists${danglingCues.length ? ': ' + danglingCues.slice(0, 3).join(', ') : ''}`)

  // The hash is the trailing eight bytes of a .dnpm, little-endian, and the
  // client formats it with BigInt.toString(16) -- so the manifest key has to be
  // exactly that, with no padding and no 0x.
  let matched = 0
  let missing = []
  for (let i = 1; i <= 32; i++) {
    const dnpm = join(MAPS, `map${String(i).padStart(2, '0')}.dnpm`)
    if (!existsSync(dnpm)) continue
    const bytes = readFileSync(dnpm)
    const key = bytes.readBigUInt64LE(bytes.length - 8).toString(16)
    // A cue TABLE, not a filename: a map has several loops and switches between
    // them. `default` is the one it starts on, and a map whose entry lacks it
    // starts silent -- which is the failure worth catching, because it looks
    // exactly like no entry at all.
    if (index[key] && typeof index[key] === 'object' && index[key].default) matched++
    else missing.push(`map${String(i).padStart(2, '0')}`)
  }
  check(missing.length === 0,
        `every baked map has a track (${matched} matched${missing.length ? ', missing ' + missing.join(',') : ''})`)
}

console.log(`\nmusic: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
