//
// The levels a client holds (client/src/seam/neighbourhood.js), in node.
//
// Every sequence here broke the code this replaces, and each of those bugs
// showed as something on screen -- the wrong map next door, a cell missing for
// the rest of the session, a level drawn twice on top of itself, a crossing
// that never finished. Here each is a number.
//
import { Neighbourhood } from '../src/seam/neighbourhood.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  if (!ok) { fails++; console.log(`  FAIL ${label}`) }
}

const P = 40                             // the pitch
const raw = (v) => BigInt(Math.round(v * 2 ** 32))
const bytesOf = (level) => new Uint8Array([level])

// A scene and a worker, recorded. A view knows what it was built from, so a
// view filed under the wrong level is caught by asking it.
function rig () {
  const log = { builds: [], made: 0, warns: [] }
  const views = []
  const hood = new Neighbourhood({
    build: (level, token, bytes) => log.builds.push({ level, token, bytes }),
    make: (built) => {
      const v = { from: built.level, shown: false, disposed: false, dx: NaN, dy: NaN }
      views.push(v)
      log.made++
      return v
    },
    show: (v, dx, dy) => { if (v.disposed) log.warns.push('show after dispose'); Object.assign(v, { shown: true, dx, dy }) },
    hide: (v) => { v.shown = false },
    dispose: (v) => { if (v.disposed) log.warns.push('double dispose'); v.disposed = true; v.shown = false },
    warn: (l) => log.warns.push(l)
  })
  // The server, as the protocol says it behaves: every build finishes when
  // asked to, in order.
  const finish = () => {
    while (log.builds.length) {
      const b = log.builds.shift()
      hood.built(b.level, b.token, { level: b.level })
    }
  }
  const map = (level, cx, cy) => hood.map(level, cx * P, cy * P, raw(cx * P), raw(cy * P), bytesOf(level))
  const move = (level, cx, cy) => hood.move(level, cx * P, cy * P, raw(cx * P), raw(cy * P))
  return { hood, log, views, finish, map, move }
}

// Everything held is drawn where it is, is what it says it is, and is live.
function sound (r) {
  for (const [li, n] of r.hood.held) {
    if (n.bytes[0] !== li) return `level ${li} holds the bytes of ${n.bytes[0]}`
    if (!n.view) continue
    if (n.view.disposed) return `level ${li} holds a disposed view`
    if (n.view.from !== li) return `level ${li} is drawn with the view of ${n.view.from}`
    if (n.placed && (!n.view.shown || n.view.dx !== n.dx || n.view.dy !== n.dy)) {
      return `level ${li} is not drawn at ${n.dx},${n.dy}`
    }
  }
  return null
}

// --- a row of three: 0 at (0,0), 1 at (1,0), 2 at (2,0), radius 1 ------------

{
  const r = rig()
  r.hood.enter(0, null)
  r.map(1, 1, 0)
  r.finish()
  check(r.hood.has(1) && r.views[0].shown && r.views[0].dx === P, 'a neighbour is built and drawn at its offset')

  // ACROSS INTO 1. Its view comes back to be promoted; 0 is held at -P.
  const homeView = { from: 0, shown: true, disposed: false, dx: 0, dy: 0 }
  const got = r.hood.enter(1, { bytes: bytesOf(0), view: homeView })
  check(got && got.view === r.views[0] && got.bytes[0] === 1, 'the level entered is handed back, view and bytes')
  check(got && got.placed && got.dx === P && got.dxRaw === raw(P), 'with the offset the body moves by')
  check(!r.hood.has(1), 'and is no longer a neighbour')
  check(r.hood.has(0) && homeView.shown && homeView.dx === -P, 'the level left is held, drawn at minus a pitch')
  // The server now says what the client already did, and it changes nothing.
  r.move(0, -1, 0)
  r.map(2, 1, 0)
  r.finish()
  check(sound(r) === null, `and everything held is sound (${sound(r)})`)
  check(r.hood.violations === 0, `with no protocol violation (${r.log.warns.join('; ')})`)
}

{
  // OUT AND BACK, twenty times. The commonest thing anybody does at a seam.
  const r = rig()
  r.hood.enter(0, null)
  r.map(1, 1, 0)
  r.finish()
  let home = { from: 0, shown: true, disposed: false, dx: 0, dy: 0 }
  let at = 0
  for (let k = 0; k < 40; k++) {
    const to = at === 0 ? 1 : 0
    const got = r.hood.enter(to, { bytes: bytesOf(at), view: home })
    if (!got) { check(false, `crossing ${k}: the level entered was not held`); break }
    home = got.view
    home.dx = 0; home.dy = 0
    r.move(at, at === 0 ? -1 : 1, 0)           // the server re-states the level left
    at = to
  }
  check(sound(r) === null, `after forty crossings everything held is sound (${sound(r)})`)
  check(r.hood.held.size === 1, `and exactly one level is held (${r.hood.held.size})`)
  check(r.log.made === 1, `and nothing was ever rebuilt (${r.log.made} builds)`)
  check(r.hood.violations === 0 && r.log.warns.length === 0, `with nothing said (${r.log.warns.join('; ')})`)
}

{
  // WALKED INTO A CELL STILL BEING BUILT. The bytes are there; the view is not.
  // The build that finishes afterwards must not be drawn as a neighbour: that
  // level is underfoot now.
  const r = rig()
  r.hood.enter(0, null)
  r.map(1, 1, 0)
  const got = r.hood.enter(1, { bytes: bytesOf(0), view: { from: 0, shown: true, disposed: false } })
  check(got && got.bytes[0] === 1 && got.view === null, 'a cell still building hands back its bytes')
  r.finish()
  check(r.log.made === 0, `and its late build is discarded, not drawn beside you (${r.log.made})`)
  // CONTROL: the same build, not walked into, IS drawn -- so the discard above
  // is the token doing its job and not builds being lost in general.
  const c = rig()
  c.hood.enter(0, null)
  c.map(1, 1, 0)
  c.finish()
  check(c.log.made === 1, 'CONTROL: a build nobody walked into is drawn')
}

{
  // DROPPED, THEN SENT AGAIN, while the first build is still out. Two builds
  // of one level in flight; only the second may land.
  const r = rig()
  r.hood.enter(0, null)
  r.map(1, 1, 0)
  const first = r.log.builds[0]
  r.hood.drop(1)
  r.map(1, 1, 0)
  const second = r.log.builds[1]
  check(!r.hood.built(first.level, first.token, { level: 1 }), 'a build for a dropped copy is refused')
  check(r.hood.built(second.level, second.token, { level: 1 }), 'the build for the copy held is kept')
  check(r.log.made === 1 && sound(r) === null, `and one view exists for it (${r.log.made})`)
}

{
  // A DROP DISPOSES, once, and a drop for something never held is said.
  const r = rig()
  r.hood.enter(0, null)
  r.map(1, 1, 0)
  r.finish()
  r.hood.drop(1)
  check(r.views[0].disposed && !r.hood.has(1), 'a drop lets the view go')
  r.hood.drop(1)
  check(r.hood.violations === 1, 'CONTROL: a second drop is a violation, not silence')
  r.move(7, 0, 1)
  check(r.hood.violations === 2, 'CONTROL: and so is a move for a level never sent')
  check(!r.log.warns.includes('double dispose'), 'and nothing was disposed twice')
}

{
  // AN EXIT: the level entered was not held, so no offset is known. Every
  // held cell is hidden until the server places it, rather than drawn a cell
  // or more out of place.
  const r = rig()
  r.hood.enter(0, null)
  r.map(1, 1, 0)
  r.finish()
  const home = { from: 0, shown: true, disposed: false }
  const got = r.hood.enter(5, { bytes: bytesOf(0), view: home })
  check(got === null, 'an unheld level comes back as nothing (its map follows)')
  check(!r.views[0].shown && !home.shown, 'and every held view is hidden')
  check(r.hood.pitch() === Infinity, 'so no pitch is claimed')
  r.hood.drop(0)
  r.move(1, 0, -1)
  check(home.disposed && r.views[0].shown && r.views[0].dy === -P, 'until the server drops or places each')
  check(r.hood.violations === 0, 'with no violation')
}

{
  // A RESPAWN in the level you are in: nothing held changes.
  const r = rig()
  r.hood.enter(0, null)
  r.map(1, 1, 0)
  r.finish()
  const home = { from: 0, shown: true, disposed: false }
  const got = r.hood.enter(0, { bytes: bytesOf(0), view: home })
  check(got && got.view === home && got.bytes[0] === 0, 'a respawn hands back the home as it was')
  check(r.hood.held.size === 1 && r.views[0].shown, 'and the neighbours stay')
}

{
  // THE PITCH comes from placed cells only.
  const r = rig()
  r.hood.enter(0, null)
  r.map(1, 1, 0)
  r.map(3, 0, 1)
  check(r.hood.pitch() === P, `the pitch is the smallest offset (${r.hood.pitch()})`)
  r.map(4, 1, 1)
  check(r.hood.pitch() === P, 'and a diagonal does not change it')
}

console.log(`\nneighbourhood: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
