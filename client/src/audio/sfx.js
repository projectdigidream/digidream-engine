//
// Sound effects: the main-thread half of DigidreamSound (sound/).
//
// The engine runs in sfx-worklet.js, in the SAME AudioContext as the music
// (Music hands it over when it has one -- see attach), and both nodes feed the
// destination, which sums them in float.
//
// WHAT ARRIVES HERE IS A NAME OR ITS HASH. The server sends a sound as the
// FNV-1a hash of its name (proto.js soundHash, docs/protocol.md §5.7); the bank
// says which names it has, so the map from hash to the engine's id is built at
// load and nobody keeps a table of numbers in step by hand.
//
// Positions are the SIMULATION's (X/Y ground, Z up, world units) and so is the
// listener's yaw: sound/docs/EMBEDDING.md, 4.
//
import { soundHash } from '../proto.js'

const READY_TIMEOUT_MS = 10000
// Q16.16 from a world-unit float.
const q16 = (v) => Math.round(v * 65536) | 0

export class Sfx {
  /**
   * @param worklet  the worklet's URL
   * @param onDuck   called with the music's gain, 0..1000, when it changes
   */
  constructor ({ worklet = '/src/audio/sfx-worklet.js', onDuck = null, log = () => {} } = {}) {
    this.workletUrl = worklet
    this.onDuck = onDuck
    this.log = log
    this.ctx = null
    this.node = null
    this.ready = false
    this.bankUrl = null
    this.ids = new Map()          // name hash -> engine sound id
    this.loaded = false
    this.missing = new Set()      // hashes asked for and not in the bank, said once
    this.played = 0
    this.lastDiag = null
    // Keys of sources with a following sound, and until when to keep moving
    // them (performance.now() ms). Longer than any sound in a game bank; the
    // worklet drops instances that have finished, this only bounds the list.
    this.followKeys = new Map()
  }

  static FOLLOW_MS = 8000

  /** The keys being followed now; forgets the ones long past. */
  followed (now = performance.now()) {
    const out = []
    for (const [k, until] of this.followKeys) {
      if (until < now) this.followKeys.delete(k)
      else out.push(k)
    }
    return out
  }

  /** The audio context exists (Music started it): stand the engine up in it. */
  async attach (ctx) {
    if (this.ctx === ctx) return
    this.ctx = ctx
    this.ready = false
    try { this.node?.disconnect() } catch {}
    await ctx.audioWorklet.addModule(this.workletUrl)
    this.node = new AudioWorkletNode(ctx, 'game-sfx', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]
    })
    const ready = new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('the sound worklet never reported ready')),
                               READY_TIMEOUT_MS)
      this.node.port.onmessage = (e) => {
        const m = e.data
        if (m.type === 'ready') { clearTimeout(timer); this.ready = true; resolve() }
        else if (m.type === 'fatal') { clearTimeout(timer); reject(new Error(m.error)) }
        else if (m.type === 'bank') this.#banked(m)
        else if (m.type === 'duck') this.onDuck?.(m.milli)
        else if (m.type === 'diag') this.lastDiag = m.diag
      }
    })
    this.node.connect(ctx.destination)
    await ready
    this.log('[sfx] engine ready')
    if (this.bankUrl) await this.#load(this.bankUrl)
  }

  /** The bank to play from. Kept and loaded when the engine is up. */
  async setBank (url) {
    if (this.bankUrl === url && this.loaded) return
    this.bankUrl = url
    if (this.ready) await this.#load(url)
  }

  async #load (url) {
    const res = await fetch(url).catch(() => null)
    if (!res || !res.ok) { this.log(`[sfx] no sound bank at ${url}`); return }
    const json = await res.text()
    this.loaded = false
    this.node.port.postMessage({ type: 'loadBank', json, budgetKb: 16384 })
  }

  #banked (m) {
    this.ids.clear()
    this.missing.clear()
    if (!m.report?.ok) {
      this.log(`[sfx] the bank did not load: ${JSON.stringify(m.report?.errors ?? m.report)}`)
      return
    }
    m.names.forEach((name, id) => {
      const h = soundHash(name)
      // Two names, one hash: the server cannot say which it meant, so neither
      // is playable by hash, and that is said rather than guessed.
      if (this.ids.has(h)) this.log(`[sfx] two sounds hash alike (${name}); rename one`)
      this.ids.set(h, id)
    })
    this.loaded = true
    const warn = m.report.warnings?.length ? `, ${m.report.warnings.length} warning(s)` : ''
    this.log(`[sfx] bank loaded: ${m.names.length} sounds${warn}`)
  }

  /**
   * Play a sound by NAME or by its hash, at a simulation position.
   *
   * `key` makes it follow something (see follow); `seed` should come from
   * something every client agrees on -- a tick, an entity -- so that two
   * clients that agree on the world hear the same thing (EMBEDDING.md, 3).
   */
  play (sound, pos, { seed = 0, key = 0 } = {}) {
    if (!this.ready || !this.loaded) return false
    const h = typeof sound === 'string' ? soundHash(sound) : sound >>> 0
    const id = this.ids.get(h)
    if (id === undefined) {
      if (!this.missing.has(h)) {
        this.missing.add(h)
        this.log(`[sfx] the bank has no sound ${typeof sound === 'string' ? sound : '#' + h.toString(16)}`)
      }
      return false
    }
    if (key) this.followKeys.set(key, performance.now() + Sfx.FOLLOW_MS)
    this.node.port.postMessage({ type: 'play', id, seed: seed >>> 0, key,
                                 x: q16(pos.x), y: q16(pos.y), z: q16(pos.z) })
    this.played++
    return true
  }

  /** Where things being followed are now: [[key, {x,y,z}], ...]. */
  follow (list) {
    if (!this.ready || !list.length) return
    const at = []
    for (const [key, p] of list) at.push(key, q16(p.x), q16(p.y), q16(p.z))
    this.node.port.postMessage({ type: 'move', at })
  }

  /** The listener: a simulation position and a yaw in brad about +Z. */
  listener (pos, yaw) {
    if (!this.ready) return
    this.node.port.postMessage({ type: 'listener', x: q16(pos.x), y: q16(pos.y), z: q16(pos.z),
                                 yaw: yaw >>> 0 })
  }

  stopAll () { this.node?.port.postMessage({ type: 'stopAll', fadeBlocks: 4 }) }

  setVolume (milli) { this.node?.port.postMessage({ type: 'volume', milli: milli | 0 }) }

  health () {
    if (!this.ready) return null
    return `${this.loaded ? this.ids.size + ' sounds' : 'no bank'}  played ${this.played}`
  }
}
