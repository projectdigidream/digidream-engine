//
// The clock other bodies are drawn by (render/renderclock.js): smooth and
// never backwards, with snapshots arriving at 30 Hz, jittered, while frames
// run at 60 or 144 Hz -- and the formula it replaced, as the control.
//
import { RenderClock } from '../src/render/renderclock.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  if (!ok) { fails++; console.log(`  FAIL ${label}`) }
}

// A deterministic jitter, up to +-`j` ms.
let seed = 12345
const rand = () => { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296 }

function simulate ({ fps, jitterMs, seconds = 10, clock }) {
  const frameMs = 1000 / fps
  // Snapshot for tick 2k is sent at 2k/60 s and arrives 40 ms later, jittered.
  const arrivals = []
  for (let k = 0; k < seconds * 30; k++) arrivals.push({ t: 2 * k + 2, at: (2 * k + 2) / 60 * 1000 + 40 + (rand() * 2 - 1) * jitterMs })
  arrivals.sort((a, b) => a.at - b.at)
  let ai = 0
  let newest = 0
  let acc = 0
  const ticks = []
  for (let now = 0; now < seconds * 1000; now += frameMs) {
    while (ai < arrivals.length && arrivals[ai].at <= now) { newest = Math.max(newest, arrivals[ai].t); clock?.heard(arrivals[ai].t); ai++ }
    acc += frameMs / (1000 / 60)
    acc -= Math.floor(acc)
    if (newest === 0) continue
    ticks.push(clock ? clock.advance(frameMs / 1000) : newest + acc)
  }
  return ticks.slice(60)   // past the start
}

const stats = (ticks) => {
  let backwards = 0, worstJump = 0
  for (let i = 1; i < ticks.length; i++) {
    const d = ticks[i] - ticks[i - 1]
    if (d < -1e-9) backwards++
    worstJump = Math.max(worstJump, d)
  }
  const rate = (ticks.at(-1) - ticks[0]) / (ticks.length - 1)
  return { backwards, worstJump, rate }
}

for (const fps of [60, 144]) {
  for (const jitterMs of [0, 15]) {
    const s = stats(simulate({ fps, jitterMs, clock: new RenderClock() }))
    const perFrame = 60 / fps
    check(s.backwards === 0, `${fps} fps, ${jitterMs} ms jitter: never backwards (${s.backwards})`)
    check(s.worstJump < perFrame * 2.5, `${fps} fps, ${jitterMs} ms jitter: no jumps (worst ${s.worstJump.toFixed(2)} ticks, a frame is ${perFrame.toFixed(2)})`)
    check(Math.abs(s.rate / perFrame - 1) < 0.02, `${fps} fps, ${jitterMs} ms jitter: runs at the tick rate (${(s.rate / perFrame).toFixed(3)})`)
  }
}

// CONTROL: the formula this replaced -- newest tick plus the local fraction.
// At 60 fps it stands still for a frame and then jumps two ticks; faster than
// the tick rate it goes backwards every snapshot. Without this, the checks
// above could pass against a clock nobody had ever seen misbehave.
{
  const s60 = stats(simulate({ fps: 60, jitterMs: 0 }))
  const t60 = simulate({ fps: 60, jitterMs: 0 })
  let still = 0
  for (let i = 1; i < t60.length; i++) if (t60[i] === t60[i - 1]) still++
  check(still > 100 && s60.worstJump >= 1.5, `CONTROL: at 60 fps the old render time stalls (${still} frames) and jumps (${s60.worstJump.toFixed(2)} ticks)`)
  const s144 = stats(simulate({ fps: 144, jitterMs: 0 }))
  check(s144.backwards > 100, `CONTROL: at 144 fps it goes backwards (${s144.backwards} times in 9 s)`)
}

// A NEW LEVEL is a jump in the server's clock: snapped, not eased for seconds.
{
  const c = new RenderClock()
  c.heard(100)
  c.advance(0.016)
  c.heard(5000)
  check(Math.abs(c.advance(0.016) - 5000) < 1, 'a jump in the server clock is followed at once')
}

console.log(`renderclock: ${checks} checks, ${fails} failures`)
process.exit(fails ? 1 : 0)
