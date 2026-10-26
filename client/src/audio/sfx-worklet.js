//
// Sound effects: the DigidreamSound engine (sound/), in the audio thread.
//
// The game's own worklet rather than the sound editor's (sound/tool/
// sfx-worklet.js): the editor's answers every play with a handle and reports
// diagnostics many times a second for its Play tab, and a game firing a dozen
// sounds a tick wants neither. Same engine module, same calls.
//
// A sibling of the music's worklet, NOT the same one: the two engines are
// separate instances so a burst of explosions cannot steal the music's voices
// (sound/docs/EMBEDDING.md, "Two engines, summed in float"). Both nodes feed
// the context's destination, which sums them in float.
//
// Positions are the SIMULATION's, in Q16.16 world units: X/Y on the ground, Z
// up. The listener's yaw is a brad about +Z.
//
import createSfx from '../../sfx/dsound.mjs'   // the site's root: src/ and sfx/ are siblings in every layout

const CONTROL = 64       // the engine's control block; frames must be a multiple

class GameSfx extends AudioWorkletProcessor {
  constructor () {
    super()
    this.ready = false
    this.loaded = false
    this.pending = []
    this.sfx = null
    this.mod = null
    // Instances that follow something, by the caller's key (the source
    // entity's handle): moved together, forgotten when they finish.
    this.following = new Map()
    this.duck = -1
    this.sinceDuck = 0

    this.port.onmessage = (e) => {
      if (!this.ready) this.pending.push(e.data)
      else this.handle(e.data)
    }
    createSfx().then((mod) => {
      this.mod = mod
      this.sfx = new mod.WebSfx()
      this.ready = true
      for (const m of this.pending) this.handle(m)
      this.pending = []
      this.port.postMessage({ type: 'ready' })
    }).catch((err) => this.port.postMessage({ type: 'fatal', error: String(err) }))
  }

  handle (m) {
    const s = this.sfx
    switch (m.type) {
      case 'loadBank': {
        s.stopAll(0)
        this.following.clear()
        const report = JSON.parse(s.loadBankJson(m.json))
        this.loaded = !!report.ok
        let cache = null
        if (this.loaded) cache = JSON.parse(s.buildCache(m.budgetKb | 0))
        const names = []
        if (this.loaded) for (let i = 0; i < s.soundCount(); i++) names.push(s.soundName(i))
        this.port.postMessage({ type: 'bank', report, cache, names })
        break
      }
      case 'play': {
        // A handle is not a promise of audibility: cooldowns, instance caps,
        // the bus budget and priorities still decide (EMBEDDING.md, 2).
        const h = s.play(m.id | 0, -1, m.seed >>> 0, m.x | 0, m.y | 0, m.z | 0)
        if (h && m.key) {
          const list = this.following.get(m.key)
          if (list) list.push(h)
          else this.following.set(m.key, [h])
        }
        break
      }
      case 'move':
        // [key, x, y, z, key, x, y, z, ...] -- one message a frame for every
        // source anything is following.
        for (let i = 0; i + 3 < m.at.length; i += 4) {
          const list = this.following.get(m.at[i])
          if (!list) continue
          for (let j = list.length - 1; j >= 0; j--) {
            if (!s.alive(list[j])) { list.splice(j, 1); continue }
            s.setPosition(list[j], m.at[i + 1] | 0, m.at[i + 2] | 0, m.at[i + 3] | 0)
          }
          if (list.length === 0) this.following.delete(m.at[i])
        }
        break
      case 'listener':
        s.setListener(m.x | 0, m.y | 0, m.z | 0, m.yaw >>> 0)
        break
      case 'stopAll':
        s.stopAll(m.fadeBlocks | 0)
        this.following.clear()
        break
      case 'volume':
        s.masterVolumeMilli(m.milli | 0)
        break
      case 'diag':
        this.port.postMessage({ type: 'diag', diag: JSON.parse(s.diagJson()) })
        break
    }
  }

  process (_inputs, outputs) {
    const out = outputs[0]
    if (!out || out.length === 0) return true
    const left = out[0]
    const right = out.length > 1 ? out[1] : out[0]
    const n = left.length
    if (!this.ready || !this.loaded) {
      left.fill(0)
      if (right !== left) right.fill(0)
      return true
    }
    const frames = Math.floor(n / CONTROL) * CONTROL
    if (frames > 0) {
      const ptr = this.sfx.render(frames)
      if (ptr) {
        const pcm = this.mod.HEAP16
        const base = ptr >> 1
        for (let i = 0; i < frames; i++) {
          left[i] = pcm[base + i * 2] / 32768
          right[i] = pcm[base + i * 2 + 1] / 32768
        }
      }
    }
    for (let i = frames; i < n; i++) { left[i] = 0; right[i] = 0 }

    // HOW MUCH THE MUSIC SHOULD DUCK, polled once a few quanta and said only
    // when it changes (EMBEDDING.md, 6): the music's worklet applies it.
    this.sinceDuck += n
    if (this.sinceDuck >= 512) {
      this.sinceDuck = 0
      const d = this.sfx.musicDuckMilli() | 0
      if (d !== this.duck) {
        this.duck = d
        this.port.postMessage({ type: 'duck', milli: d })
      }
    }
    return true
  }
}

registerProcessor('game-sfx', GameSfx)
