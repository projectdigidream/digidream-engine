//
// THE SCRIPTS TRAVEL WITH THE MAP, AND THEY HAVE TO BE REWRITTEN TO SURVIVE.
//
// compose moves a converted Doom level into its cell and cuts doorways out onto
// the ground around it. Two things in the level's own Lua stop being true when it does:
//
//   * doom.load's `lines` table is keyed by SIDE INDEX, and cutting a doorway
//     splices two sides into the middle of the file.
//   * mobs.place, `props` and `teleports` carry absolute coordinates, and the
//     map moved.
//
// Neither failure is quiet in the same way. The side keys announce themselves --
// doom.lua checks every entry's recorded neighbour and disables the whole table
// when one disagrees, which is every door in the level shut for ever. The
// coordinates do not: the monsters are placed, successfully, in the sea beside
// the map.
//
// Each claim below is paired with the same check run against the ORIGINAL file,
// which must fail. Without that pair this suite would also pass on a compose
// that rewrote nothing, in a world where the indices happened not to move.
//
//   node tools/compose/test.mjs
//
import { readFileSync, mkdtempSync, existsSync } from 'node:fs'
import { execFileSync } from 'node:child_process'
import { tmpdir } from 'node:os'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { parseMap, attr } from '../../editor/src/mapfmt.js'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '../..')
const SRC = join(ROOT, 'modules/doom2/maps')
let pass = 0, fail = 0
const check = (ok, what) => { ok ? ++pass : (++fail, console.log(`  FAIL ${what}`)) }

// Two adjacent cells is the whole shape of the problem: one seam, two maps that
// each had to be cut. Nine would prove nothing a pair does not and cost a
// minute of anybody's suite.
const out = mkdtempSync(join(tmpdir(), 'compose-'))
execFileSync('node', [join(ROOT, 'tools/compose/compose.mjs'),
  '--metamap', join(ROOT, 'modules/doomworld/metamap.txt'),
  '--maps', SRC, '--seams', out, '--prefix', 'dw_', '--only', '6,2,7,2'],
  { stdio: 'pipe' })

// The side index a .map file gives a vertex: sectors in file order, vertices in
// walk order within each. Proven against mapc by the checks below -- every
// check_neighbor in a converted level's Lua is mapc's own answer, written down
// at conversion time.
// The pitch the run chose, read back rather than assumed: compose measures the
// maps and picks it, so hard-coding it here would be a second copy of a number
// this tool computes.
const PITCH = Number(readFileSync(join(out, 'metamap.txt'), 'utf8')
  .match(/^pitch\s+(\S+)/m)[1])

const walk = (item, fn) => {
  for (const ch of item.children ?? []) {
    if (ch.kind === 'vertex') fn(ch); else walk(ch, fn)
  }
}
const sidesOf = (text) => {
  const doc = parseMap(text)
  const out = []
  for (const s of doc.sectors) walk(s, (v) => out.push(v))
  return { doc, sides: out }
}
const neighbourAt = (sides, i) =>
  sides[i] ? Number(attr(sides[i], 'neighbor')?.v ?? -1) : null

// Every `[n] = { ... check_neighbor=m ... }` entry, as (n, m).
const linesIn = (lua) =>
  [...lua.matchAll(/\[(\d+)\] = \{([^\n]*check_neighbor=(-?\d+)[^\n]*)\}/g)]
    .map((m) => ({ side: Number(m[1]), want: Number(m[3]) }))

const disagreements = (sides, lines) =>
  lines.filter((l) => neighbourAt(sides, l.side) !== l.want).length

for (const [map, lua] of [['map01', 'map01.lua'], ['map12', 'map12.lua']]) {
  const composed = sidesOf(readFileSync(join(out, `dw_${map}.map`), 'utf8'))
  const rewritten = readFileSync(join(out, lua), 'utf8')
  const original = readFileSync(join(SRC, lua), 'utf8')

  const lr = linesIn(rewritten), lo = linesIn(original)
  check(lr.length > 0 && lr.length === lo.length,
        `${map}: the same ${lo.length} line action(s) survive`)
  check(disagreements(composed.sides, lr) === 0,
        `${map}: every re-keyed line finds its own neighbour`)
  // WHETHER THE INDICES MOVED AT ALL. They used to on every level, because
  // doorways were cut into its walls; now the ground meets a level all round
  // and nothing is spliced into it, so for such a level the original keys
  // simply still fit -- and the rewrite must have left them alone. A level the
  // ground could not meet still gets doorways cut, and then they must not fit.
  // Sides of the level's OWN sectors: the ground added after them has sides too.
  const own = sidesOf(readFileSync(join(SRC, `${map}.map`), 'utf8'))
  let ownSides = 0
  for (const sec of composed.doc.sectors.slice(0, own.doc.sectors.length)) walk(sec, () => { ++ownSides })
  const shifted = ownSides !== own.sides.length
  if (shifted) {
    check(disagreements(composed.sides, lo) > 0,
          `${map}: CONTROL -- the original keys do NOT fit the composed map`)
  } else {
    check(lr.every((l, i) => l.side === lo[i].side),
          `${map}: no side moved, so no key was changed`)
  }
  // ... and the reverse, so "the keys moved" is not just "the keys are wrong".
  const plain = sidesOf(readFileSync(join(SRC, `${map}.map`), 'utf8'))
  check(disagreements(plain.sides, lo) === 0,
        `${map}: CONTROL -- the original keys DO fit the original map`)

  // Coordinates. How far the map moved is read off the GEOMETRY -- the spawn
  // point is the same point in both files -- and every placement in the script
  // must have moved by exactly that, in Doom units. A bounding box would not
  // do: a placement can land inside the box by accident, and half of map12's
  // do.
  // Read off the first vertex of the first sector, which compose moves and
  // never cuts. Not the spawn: in a terrain world the spawn is moved outdoors.
  const firstVertex = (doc) => { let v = null; walk(doc.sectors[0], (x) => { v ??= x }); return v }
  const a = firstVertex(plain.doc), b = firstVertex(composed.doc)
  const dx = Math.round((b.x.value - a.x.value) * 48)
  const dy = Math.round((b.y.value - a.y.value) * 48)
  check(dx !== 0 || dy !== 0, `${map}: the map moved (${dx}, ${dy} Doom units)`)

  const places = (lua) => [...lua.matchAll(/\bx=(-?\d+), y=(-?\d+)/g)]
    .map((m) => ({ x: Number(m[1]), y: Number(m[2]) }))
  const pr = places(rewritten), po = places(original)
  check(pr.length > 20 && pr.length === po.length,
        `${map}: the same ${po.length} placement(s) survive`)
  const moved = pr.filter((p, i) => p.x - po[i].x === dx && p.y - po[i].y === dy).length
  check(moved === pr.length,
        `${map}: every placement moved with the map (${moved}/${pr.length})`)
  // CONTROL: none of them sat there already, so "moved" is not "unchanged".
  const still = pr.filter((p, i) => p.x === po[i].x && p.y === po[i].y).length
  check(still === 0, `${map}: CONTROL -- none was already there (${still})`)

  // THE GROUND IS THERE, and lit and textured like anything else.
  const added = composed.doc.sectors.slice(plain.doc.sectors.length)
  // At least the ring round the level. (MAP12 is walled by one rectangle, and
  // in a two-cell layout the ground round it is eight triangles.)
  check(added.length >= 4, `${map}: ${added.length} ground sector(s) were added`)
  check(added.every((sec) => attr(sec, 'light') && attr(sec, 'floortex') && attr(sec, 'ceiltex')),
        `${map}: every added sector names a light level and both flats`)

  // A teleport destination is keyed by SECTOR TAG, not by side, and must come
  // through with its key untouched even though its coordinates moved.
  const tags = (lua) => {
    const t = lua.match(/teleports = \{([\s\S]*?)\n  \}/)
    return t ? [...t[1].matchAll(/\[(\d+)\]/g)].map((m) => m[1]).join(',') : ''
  }
  check(tags(rewritten) === tags(original),
        `${map}: teleport tags unchanged ("${tags(original)}")`)
}

// BOTH SIDES OF THE BOUNDARY MEET, AT THE SAME HEIGHT.
//
// The two cells are generated by one run but never consult each other: they
// meet because their tiles are the same grid and their heights are the same
// function of world coordinates (terrain.mjs). So every open edge on the shared
// boundary has a partner on the other side, at the same floor.
{
  const openEdges = []
  for (const [map, cell] of [['map01', { x: 6, y: 2 }], ['map12', { x: 7, y: 2 }]]) {
    const doc = parseMap(readFileSync(join(out, `dw_${map}.map`), 'utf8'))
    for (const sec of doc.sectors) {
      const vs = []
      walk(sec, (v) => vs.push(v))
      for (let i = 0; i < vs.length; i++) {
        if (!attr(vs[i], 'open')) continue
        const a = vs[i], b = vs[(i + 1) % vs.length]
        const mid = [(a.x.value + b.x.value) / 2 + cell.x * PITCH,
                     (a.y.value + b.y.value) / 2 + cell.y * PITCH]
        openEdges.push({ map, key: `${mid[0].toFixed(3)},${mid[1].toFixed(3)}`,
                         floor: Number(attr(sec, 'floor').v.value) })
      }
    }
  }
  const pairs = new Map()
  for (const e of openEdges) {
    if (!pairs.has(e.key)) pairs.set(e.key, [])
    pairs.get(e.key).push(e)
  }
  const both = [...pairs.values()].filter((v) => v.length === 2 && v[0].map !== v[1].map)
  if (process.env.COMPOSE_TEST_VERBOSE) {
    for (const [p, q] of both) if (Math.abs(p.floor - q.floor) >= 1e-6) console.log(`  ${p.key}: ${p.floor} vs ${q.floor}`)
  }
  check(both.length >= 5, `the two cells meet along the boundary (${both.length} edge pair(s))`)
  // WALKABLE, not equal. A tile beside a level can be pulled to the level's
  // floor, which leaves a step of at most STEP (0.5) at a few boundary tiles.
  // A body climbs that; the generator is scaffolding and not worth more.
  const worst = Math.max(0, ...both.map(([p, q]) => Math.abs(p.floor - q.floor)))
  check(worst <= 0.5 + 1e-6, `and every pair is within one step (worst ${worst})`)
  // CONTROL: the boundary is not flat, so "they agree" is the shared function
  // at work and not a constant that happens to match itself.
  const floors = new Set(both.map(([p]) => p.floor.toFixed(3)))
  check(floors.size > 1, `CONTROL -- while the ground along it takes ${floors.size} heights`)
}

// EVERY JUNCTION BETWEEN ADDED SECTORS IS A WHOLE SHARED EDGE.
//
// The engine joins two sectors only when each names the other AND the two edges
// are the same segment reversed. The ground here is rectangles
// placed by arithmetic, and one wrong bound puts two of them over the same
// ground naming each other across edges that do not meet -- joined
// neither way, which from inside is a passage with a wall across it, drawn
// where the stray edge carried a texture and invisible where it did not.
//
// compose refuses to write a world that fails this, so the assertion here is
// that the refusal WORKS. It is checked again on the baked maps because mapc
// subdivides edges at T-junctions, and a link that is whole in the .map is
// still whole afterwards.
{
  let links = 0, broken = 0
  for (const map of ['map01', 'map12']) {
    const doc = parseMap(readFileSync(join(out, `dw_${map}.map`), 'utf8'))
    const secs = doc.sectors
    const loops = secs.map((sec) => { const vs = []; walk(sec, (v) => vs.push(v)); return vs })
    const edge = new Map()
    loops.forEach((vs, si) => vs.forEach((v, i) => {
      const w = vs[(i + 1) % vs.length]
      edge.set(`${si}#${v.x.value},${v.y.value}|${w.x.value},${w.y.value}`, v)
    }))
    // compose's own rectangles only, for the reason its self-check gives.
    const first = parseMap(readFileSync(join(SRC, `${map}.map`), 'utf8')).sectors.length
    loops.forEach((vs, si) => {
      if (si < first) return
      vs.forEach((v, i) => {
        const nb = attr(v, 'neighbor')
        if (!nb || Number(nb.v) < first) return
        ++links
        const w = vs[(i + 1) % vs.length]
        const back = edge.get(`${nb.v}#${w.x.value},${w.y.value}|${v.x.value},${v.y.value}`)
        if (!back || Number(attr(back, 'neighbor')?.v) !== si) ++broken
      })
    })
  }
  check(links > 10, `${links} junction(s) to check`)
  check(broken === 0, `every one is a whole shared edge (${broken} are not)`)
}

// A BARE-NAMED SCRIPT THAT DESCRIBES NO MAP IS COPIED, NOT REWRITTEN, and it
// has to be: weapons_doom2.lua is the same file for all thirty-two levels and
// they all land in one directory, so a per-map rewrite would leave whichever
// map was written last. (A declaration with a directory in it is not compose's
// business at all -- mapc flattens it and copies the source beside the .dnpm.)
check(existsSync(join(out, 'weapons_doom2.lua')) &&
      readFileSync(join(out, 'weapons_doom2.lua'), 'utf8') ===
      readFileSync(join(SRC, 'weapons_doom2.lua'), 'utf8'),
      'a shared script is copied byte for byte')

console.log(`compose: ${pass} passed, ${fail} failed`)
process.exit(fail === 0 ? 0 : 1)
