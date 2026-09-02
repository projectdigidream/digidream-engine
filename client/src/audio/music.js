//
// Music: the midisynth engine, hosted in an AudioWorklet.
//
// WHAT THIS IS NOT. It is not a player for audio files. `midisynth/` is a
// deterministic wavetable synthesiser that renders MIDI in fixed-point integer
// maths, and what ships is a `.mstk` -- a baked track carrying the song, the
// instruments it actually uses, and its loop points in one integrity-sealed
// file. So "load a track" means handing the synth a score, not a recording.
//
// WHY A WORKLET AND NOT THE MAIN THREAD. Audio has a hard deadline: miss it and
// you hear it, which is not true of a dropped frame. The whole engine runs
// inside the AudioWorklet as a single-file ES6 module and the main thread only
// posts messages to it -- midisynth's own tool does exactly this, and the
// arrangement deliberately avoids SharedArrayBuffer and the COOP/COEP headers
// it drags in. See midisynth/README.md, "Deliberate deviations".
//
// THE ENGINE'S DETERMINISM IS NOT THIS FILE'S PROBLEM, and that is worth saying
// because everything else in this repository is preoccupied with it. Music is
// not simulation: it is never hashed, never predicted, never replicated, and
// two clients hearing slightly different things is nobody's bug. The synth is
// deterministic for its own reasons.
//
// AUTOPLAY. A browser refuses to start an AudioContext until the user has
// interacted with the page, so this cannot begin at load. `arm()` attaches
// one-shot listeners and starts on the first real input -- silence until the
// player touches something is the browser's rule, not a fault to work around.
//

const READY_TIMEOUT_MS = 10000

export class Music {
  /**
   * @param baseUrl     where synth-worklet.js and its dist/ live
   * @param contentUrl  where gm_bank.json and gm_kit.json live
   * @param onStatus    called ~10 Hz with the worklet's status feed
   */
  constructor ({ baseUrl = '/synth', contentUrl = '/synth-content',
                 onStatus = null, log = () => {} } = {}) {
    this.baseUrl = baseUrl
    this.contentUrl = contentUrl
    this.onStatus = onStatus
    this.log = log

    this.ctx = null
    this.node = null
    this.ready = false
    this.started = false
    this.current = null      // the url of the track playing, or null
    this.lastStatus = null
    // What to play once the context is allowed to exist. A map is loaded and a
    // track chosen long before the player has touched anything, so the request
    // is remembered rather than dropped.
    this.pending = null
    this.failed = null
    // name -> url, for the map currently loaded. See setCues.
    this.cues = {}
    // Is what is playing a sibling of the cues in `this.cues`? Only then may a
    // cue change crossfade rather than swap. See play().
    this.sameContent = false
  }

  /**
   * Begin on the first user gesture, and never before.
   *
   * One-shot on several event kinds because "interaction" is whatever the
   * browser decided it is, and a game is as likely to be started with a key as
   * with a click.
   */
  arm () {
    if (this.started || this.failed) return

    const listen = () => {
      const go = () => {
        for (const ev of ['pointerdown', 'keydown', 'touchstart']) removeEventListener(ev, go)
        this.start().catch((e) => { this.failed = String(e?.message ?? e) })
      }
      for (const ev of ['pointerdown', 'keydown', 'touchstart']) addEventListener(ev, go)
    }

    // TRY FIRST, then wait. Some contexts are allowed to start without a
    // gesture -- a page the player has already interacted with, a browser
    // configured to permit it, a headless run under test -- and waiting anyway
    // would mean silence until they happened to press something.
    //
    // If it comes up suspended it is torn down again rather than left half
    // built: a context that exists and is not running holds an output device
    // open for nothing, and start() must be able to run cleanly later.
    this.start().then(() => {
      if (this.ctx && this.ctx.state !== 'running') {
        this.log('[music] audio is blocked until the first key or click')
        this.#teardown()
        listen()
      }
    }).catch(() => { this.#teardown(); listen() })
  }

  #teardown () {
    try { this.node?.disconnect() } catch {}
    try { this.ctx?.close() } catch {}
    this.node = null
    this.ctx = null
    this.ready = false
    this.started = false
  }

  /** Stand the engine up. Safe to call twice; the second is a no-op. */
  async start () {
    if (this.started) return
    this.started = true

    this.ctx = new (window.AudioContext ?? window.webkitAudioContext)()
    // Even after a gesture a context can come up suspended; resuming is
    // idempotent and costs nothing when it is already running.
    if (this.ctx.state === 'suspended') await this.ctx.resume()

    await this.ctx.audioWorklet.addModule(`${this.baseUrl}/synth-worklet.js`)
    this.node = new AudioWorkletNode(this.ctx, 'midisynth', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2]
    })

    const ready = new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error('the synth worklet never reported ready')),
        READY_TIMEOUT_MS)
      this.node.port.onmessage = (e) => {
        const m = e.data
        if (m.type === 'ready') { clearTimeout(timer); this.ready = true; resolve() }
        else if (m.type === 'fatal') { clearTimeout(timer); reject(new Error(m.error)) }
        else if (m.type === 'status') { this.lastStatus = m; this.onStatus?.(m) }
        // A report is the synth's verdict on something it was asked to load.
        // Surfaced rather than swallowed: a bank that failed to parse is a
        // silent game, and silence is exactly what a music bug looks like.
        else if (m.type === 'report' && m.report && m.report.ok === false) {
          this.log(`[music] ${m.what} failed: ${JSON.stringify(m.report)}`)
        }
      }
    })

    this.node.connect(this.ctx.destination)
    await ready

    // The instruments. Both are plain JSON the synth parses itself.
    const [bank, kit] = await Promise.all([
      fetch(`${this.contentUrl}/patches/gm_bank.json`).then((r) => r.text()),
      fetch(`${this.contentUrl}/drums/gm_kit.json`).then((r) => r.text())
    ])
    this.node.port.postMessage({ type: 'loadBank', json: bank })
    this.node.port.postMessage({ type: 'loadDrums', json: kit })

    this.log('[music] synth ready')
    if (this.pending) {
      const p = this.pending
      this.pending = null
      await this.play(p.url, p.opts)
    }
  }

  /**
   * Play a baked track, crossfading from whatever is playing.
   *
   * Crossfade rather than stop-then-start: a map change should not punch a hole
   * in the soundtrack, and the synth can hold both songs long enough to blend
   * them. `fadeMs` of 0 is a hard cut, which is what starting from silence
   * wants.
   */
  async play (url, { loop = true, fadeMs = 0, crossfade = false } = {}) {
    if (!this.ready) { this.pending = { url, opts: { loop, fadeMs, crossfade } }; return }
    if (this.current === url) return

    const bytes = await fetch(url).then((r) => r.arrayBuffer())

    // TWO WAYS TO CHANGE TRACK, and they are not interchangeable.
    //
    // CROSSFADE is for cues baked from the SAME parent content. A .mstk is
    // tree-shaken -- it carries only the instruments its own song uses -- and
    // the synth's crossfade keeps the content already loaded, because slices of
    // one parent share it. Crossfading between two INDEPENDENTLY baked tracks
    // therefore plays the incoming score through the outgoing track's
    // instruments: the synth warns, and the result is a song rendered with the
    // wrong instruments rather than an error anybody would notice.
    //
    // SWAP is for everything else: fade the master down, load the track --
    // which brings its own instruments with it -- and fade back up. The two
    // songs never overlap, which is the price of them being unrelated pieces
    // of music.
    //
    // HOW TO AUTHOR CUES THAT CROSSFADE: cut them from one parent track.
    // midisynth's cut_track (and the cutter tool) slices a .mstk into regions
    // and gives each the parent's embedded content VERBATIM -- "every slice of
    // one parent therefore shares the same content hash, and a game can
    // crossfade freely between them over one loaded bank/kit"
    // (midisynth/src/track/track.cpp). Baking several cues independently from
    // separate MIDI files does NOT give you that, and the mismatch is a warning
    // rather than an error.
    //
    // So a map change swaps and a cue change within one map crossfades. That is
    // a property of how the tracks were BAKED, not a preference, which is why
    // cue() decides it rather than the caller.
    if (crossfade && this.current !== null) {
      this.node.port.postMessage({ type: 'crossfadeTrack', bytes, fadeMs }, [bytes])
      this.current = url
      this.log(`[music] crossfading to ${url}`)
      return
    }

    const dipping = fadeMs > 0 && this.current !== null
    const half = Math.max(1, Math.floor(fadeMs / 2))
    if (dipping) {
      this.node.port.postMessage({ type: 'duck', milli: 0, rampMs: half })
      await new Promise((r) => setTimeout(r, half))
    }

    this.node.port.postMessage({ type: 'loadTrack', bytes }, [bytes])
    this.node.port.postMessage({ type: 'setLoop', on: loop })
    this.node.port.postMessage({ type: 'play' })

    if (dipping) this.node.port.postMessage({ type: 'duck', milli: 1000, rampMs: half })

    this.current = url
    this.log(`[music] ${dipping ? 'swapping to' : 'playing'} ${url}`)
  }

  /**
   * The cues available in the map currently loaded: name -> url.
   *
   * A map has SEVERAL loops and something switches between them while you are
   * in it; `default` is the one to start with. Every DOOM II map has exactly
   * one, which is the degenerate case of the same table rather than a different
   * arrangement.
   */
  setCues (cues) {
    this.cues = cues ?? {}
    // A NEW TABLE IS NEW CONTENT. The next track to play was baked
    // independently of whatever is sounding, so it must be swapped in with its
    // own instruments rather than crossfaded onto the old ones. Cue changes
    // after that stay inside one map's set and may crossfade.
    this.sameContent = false
  }

  /**
   * Play a named cue from the current map.
   *
   * Crossfades between cues of one map and swaps when the map changed -- see
   * play() for why those are different operations rather than a preference.
   *
   * This is the seam the server will drive. When a script can say
   * `dnp.music("combat", 800)` the handler for that message is one call to
   * here: the resolution from a NAME to a url stays on the client, where the
   * manifest already lives, so the wire only ever carries the name.
   */
  async cue (name, opts = {}) {
    // `cue(name, 800)` still means a fade, because that is how it read before
    // there was anywhere else to put the argument.
    const { fadeMs = 800, at = 'nextBar' } =
      typeof opts === 'number' ? { fadeMs: opts } : opts

    const url = this.cues?.[name]
    if (!url) {
      // Named, not silent. A cue that does not exist is a map asking for music
      // it did not ship, and the symptom is nothing happening.
      this.log(`[music] no cue '${name}' in this map (have: ${Object.keys(this.cues ?? {}).join(', ') || 'none'})`)
      return
    }
    if (this.current === url) return

    // ON A MUSICAL BOUNDARY, not the instant the game asked.
    //
    // A fight starting mid-bar and the music changing mid-bar are two different
    // things: the first is gameplay, the second is a mistake you can hear. The
    // synth can hold a transition until the next bar or the end of the loop
    // (MusicDirector::transition), so a cue change is QUEUED and the engine
    // decides when it lands.
    //
    // Only for siblings. queueTrackFromBuffer takes the incoming SONG and keeps
    // the loaded content, exactly as the crossfade does, so it carries the same
    // constraint: cues must be slices of one parent. A map change is not, and
    // falls through to the swap below.
    if (this.sameContent && this.ready && at !== 'immediate') {
      const bytes = await fetch(url).then((r) => r.arrayBuffer())
      this.node.port.postMessage(
        { type: 'queueTrack', bytes, mode: at === 'loopEnd' ? 'loopEnd' : 'nextBar', fadeMs },
        [bytes])
      this.current = url
      this.log(`[music] queued ${name} for the ${at === 'loopEnd' ? 'end of the loop' : 'next bar'}`)
      return
    }

    await this.play(url, { loop: true, fadeMs, crossfade: this.sameContent })
    // From here on, the cues in this table are siblings of what is playing.
    this.sameContent = true
  }

  /**
   * Drop a queued cue change that has not landed yet.
   *
   * The fight ended before the bar did, and the music should stay where it is
   * rather than switching to a cue nothing is waiting for any more.
   */
  cancelQueuedCue () {
    this.node?.port.postMessage({ type: 'cancelQueued' })
  }

  /** Master volume, 0..1000 (the synth works in milli). */
  setVolume (milli) {
    this.node?.port.postMessage({ type: 'masterVolume', milli: milli | 0 })
  }

  /**
   * Duck the music, 0..1000, over `rampMs`.
   *
   * For anything that has to be heard over it. Kept as a plain pass-through
   * because what deserves ducking is a gameplay decision and this file has no
   * business having an opinion about it.
   */
  duck (milli, rampMs = 120) {
    this.node?.port.postMessage({ type: 'duck', milli: milli | 0, rampMs })
  }

  stop () {
    this.node?.port.postMessage({ type: 'stop' })
    this.current = null
  }
}
