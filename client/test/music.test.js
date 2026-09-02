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

  // The crossfade now takes a baked track -- the synth grew
  // crossfadeTrackFromBuffer, and crossfadeFromBuffer delegates to it. This
  // check used to assert the opposite, and failing was how the workaround was
  // noticed as expired rather than somebody having to remember.
  const b2 = readFileSync(TRACK)
  const ptr = synth.songBufferPrepare(b2.length)
  mod.HEAPU8.set(b2, ptr)
  const xf = JSON.parse(synth.crossfadeTrackFromBuffer(1200))
  check(xf.ok === true, `a baked track can be crossfaded (${xf.errors?.[0] ?? 'ok'})`)

  // AND THE REASON A MAP CHANGE STILL SWAPS RATHER THAN CROSSFADING.
  //
  // A .mstk is tree-shaken: it carries only the instruments its own song uses,
  // and the crossfade keeps the content already loaded because slices of one
  // parent share it. Two independently baked tracks do not, so the incoming
  // score plays through the outgoing track's instruments -- and the synth says
  // so rather than failing, which is the kind of wrong that ships.
  //
  // This is what makes swap-vs-crossfade a property of how the tracks were
  // BAKED rather than a preference. If this warning ever stops appearing,
  // independently baked tracks have become interchangeable and music.js can
  // crossfade everything.
  check((xf.warnings ?? []).some((w) => /different content/.test(w)),
        `crossfading a differently-baked track warns (${xf.warnings?.length ?? 0} warnings)`)
}

// --- CUES: slices of one parent, and boundary-timed switching ----------------
//
// This is the arrangement the adaptive-music design rests on, and it is worth
// pinning because it is invisible from the outside: two cues that LOOK
// interchangeable behave completely differently depending on how they were
// baked.
//
//   slices of one parent -> crossfade cleanly, sharing one loaded bank
//   independently baked   -> the incoming score plays through the OUTGOING
//                            track's instruments, with a warning and no error
//
// The queue is the other half: a fight starting mid-bar is gameplay, and the
// music changing mid-bar is a mistake you can hear, so a cue change is held
// until the next bar.
{
  const parent = readFileSync(TRACK)
  const cut = (a, b, name) => {
    const ptr = synth.songBufferPrepare(parent.length)
    mod.HEAPU8.set(parent, ptr)
    const rep = JSON.parse(synth.cutTrackFromBuffer(a, b, name))
    if (!rep.ok) return null
    const size = synth.bakedTrackSize()
    const out = Buffer.alloc(size)
    out.set(new Uint8Array(mod.HEAPU8.buffer, synth.bakedTrackPtr(), size))
    return out
  }

  const calm = cut(0, 3840, 'calm')
  const fight = cut(3840, 7680, 'fight')
  check(calm !== null && fight !== null, 'two cues cut from one parent track')

  if (calm && fight) {
    const put = (buf) => {
      const ptr = synth.songBufferPrepare(buf.length)
      mod.HEAPU8.set(buf, ptr)
    }
    put(calm)
    JSON.parse(synth.loadTrackFromBuffer())
    synth.setLoop(true)
    synth.play()
    for (let i = 0; i < 200; i++) synth.render(128)

    put(fight)
    const sib = JSON.parse(synth.crossfadeTrackFromBuffer(600))
    check(sib.ok === true && (sib.warnings ?? []).length === 0,
          `siblings crossfade with no content warning (${(sib.warnings ?? []).length})`)

    // THE CONTROL. A track baked separately is not a sibling, and the warning
    // is the only thing that says so -- which is why a map change swaps.
    const stranger = readFileSync(join(ROOT, 'modules', 'doom2', 'assets', 'music', 'd_stalks.mstk'))
    put(stranger)
    const alien = JSON.parse(synth.crossfadeTrackFromBuffer(600))
    check((alien.warnings ?? []).some((w) => /different content/.test(w)),
          'CONTROL: a separately baked track still warns')

    // And a cue change can be held for the next bar rather than cutting in.
    put(fight)
    const q = JSON.parse(synth.queueTrackFromBuffer(1, 600))
    check(q.ok === true && synth.queued() === true,
          `a cue change queues for the next bar (${synth.msToBoundary()} ms away)`)
    synth.cancelQueued()
    check(synth.queued() === false, 'and can be cancelled before it lands')
  }
}

// --- WHICH WAY THE WAVETABLE MORPH RUNS --------------------------------------
//
// tools/wad/genmidi_bank.js builds every patch in the Doom bank on one
// assumption: with a DECAYING wtPosEnv (attack 0, sustain 0) the envelope
// starts at the LAST frame, so the bright value belongs in `endIndex`.
//
// Getting that backwards makes all 175 instruments attack dull and bloom
// brighter as they fade -- the opposite of an FM instrument, on every note of
// the soundtrack, with no error anywhere. It shipped that way once. The doc
// warns about it (PATCH_FORMAT.md 4.8, "inverted: put the bright value where
// the envelope starts") and it was still read the wrong way, which is why this
// is a measurement rather than a comment.
//
// Amplitude is held CONSTANT here (attack 0, sustain 1), so the only thing that
// can change the waveform over time is the morph.
{
  const probe = (startIndex, endIndex) => JSON.stringify({
    version: 1,
    programs: { 0: { name: 'probe', layers: [{
      wavetable: { generator: 'fm2op', frameCount: 8, params: { ratio: 3, startIndex, endIndex } },
      mixGain: 0.8,
      ampEnv: { attack: 0, decay: 0, sustain: 1.0, release: 50 },
      wtPosEnv: { attack: 0, decay: 200, sustain: 0, release: 0, depth: 1.0, decayCurve: 'exp' }
    }] } }
  })

  // Crude but sufficient: a harmonically rich waveform changes fast between
  // adjacent samples, a sine barely does. Normalised by amplitude so it
  // measures timbre and not loudness.
  const brightness = (blocks) => {
    let dsum = 0, asum = 0, prev = 0
    for (let b = 0; b < blocks; b++) {
      const p = synth.render(128)
      const h = mod.HEAP16
      const base = p >> 1
      for (let i = 0; i < 128; i++) {
        const v = h[base + 2 * i]
        dsum += Math.abs(v - prev); asum += Math.abs(v); prev = v
      }
    }
    return asum > 0 ? dsum / asum : 0
  }

  const run = (a, b) => {
    synth.loadBank(probe(a, b))
    synth.programChange(0, 0)
    synth.noteOn(0, 60, 110)
    const early = brightness(20)   // ~50 ms in, the morph has barely moved
    brightness(80)                 // let the 200 ms morph finish
    const late = brightness(60)
    synth.noteOff(0, 60)
    for (let i = 0; i < 120; i++) synth.render(128)
    return { early, late }
  }

  const brightAtEnd = run(0, 8)
  const brightAtStart = run(8, 0)

  // ASSERTED AS A DIRECTION, NOT AS A RATIO, and the history is the reason.
  //
  // This used to demand `early > late * 2`. The factor was never the property
  // under test -- it was one engine's contrast, written down. When the synth
  // moved to Q8.24 envelopes with per-sample wavetable-morph interpolation the
  // same patch measured 0.091 -> 0.065, still decaying but only by 1.4x, and
  // the test failed while the thing it exists to catch was fine.
  //
  // The claim is about WHICH END OF THE TABLE THE ENVELOPE STARTS AT, so the
  // sharp form of it is that the two patches disagree: bright-at-end must fall
  // over the note and bright-at-start must rise. That is two-sided, needs no
  // magnitude, and would still fail the inversion this was written for -- which
  // a smaller ratio would also have passed. Weaker in arithmetic, stronger in
  // kind.
  check(brightAtEnd.early > brightAtEnd.late,
        `endIndex is where a decaying wtPosEnv STARTS ` +
        `(${brightAtEnd.early.toFixed(3)} -> ${brightAtEnd.late.toFixed(3)})`)

  // THE CONTROL. The same patch the other way round must invert, or the
  // measurement is not reading the morph at all.
  check(brightAtStart.late > brightAtStart.early,
        `CONTROL: swapping them inverts it ` +
        `(${brightAtStart.early.toFixed(3)} -> ${brightAtStart.late.toFixed(3)})`)

  // AND THEY MUST DISAGREE WITH EACH OTHER. Two runs that both drifted the
  // same way would satisfy the pair above if the probe were reading something
  // other than the morph -- amplitude, say. This is what makes the direction
  // claim mean the morph.
  const fell = brightAtEnd.early - brightAtEnd.late
  const rose = brightAtStart.late - brightAtStart.early
  check(fell > 0 && rose > 0,
        `the two directions oppose (fell ${fell.toFixed(3)}, rose ${rose.toFixed(3)})`)
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
