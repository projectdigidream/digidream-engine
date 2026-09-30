//
// SEVERAL MAPS, LAID OUT ON A GRID, IN ONE FILE.
//
// The metamap says where each level sits and the server keeps the neighbours in
// memory. This takes the same arrangement and produces ONE map out of it, with
// every cell translated into its place -- so the question "does the geometry fit
// together" can be answered by walking around the answer rather than reasoned
// about. Whether the cells are the size the pitch claims, whether a level
// overlaps the one beside it, how much empty margin each one leaves: all of it
// is visible the moment it is one map.
//
//   node tools/compose/compose.mjs --metamap <f> --maps <dir> --out <f.map>
//                                  [--pitch N] [--only x0,y0,x1,y1]
//
// A KNOWN GAP, recorded rather than left to be rediscovered: a map this tool
// writes does NOT round-trip through the editor's in-memory builder to the same
// content hash. editor/test/build.test.js compares the two over every map in the
// repository and agrees on all of them; on a composed one it produced
// 8A1D4126C98D51D9 against mapc's 867D90CAF937FCF5. Both load and both play; one
// of the two builders is doing something with this file that the other is not,
// and finding out which is worth a morning. mapc's answer is the authority.
//
// It MUTATES each map's parsed document and re-emits it with the editor's own
// emitter rather than writing the format again. editor/src/mapfmt.js is proven
// byte-for-byte against mapc over every map in the repository, and a second
// writer of a grammar whose only definition is a C++ file is the largest
// correctness risk anything here could take on.
//
// WHAT IT DROPS, and says so out loud: scripts. A converted level's Lua
// addresses its own sectors by INDEX, and every index moves when maps are
// concatenated -- so a composed map with map01.lua attached would place MAP01's
// monsters in whatever sectors now hold those numbers. Geometry, spawns, lights
// and textures survive; doors, switches and monsters do not. That is the right
// trade for a piece whose whole question is "does it line up".
//
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { parseMap, emitMap, numFrom, attr, setAttr, setInt, setNum } from '../../editor/src/mapfmt.js'

// A body steps up half a unit and no more (sim::constants STEP_HEIGHT), so a
// corridor that meets a room's floor at the wrong height is a wall from one
// side and a pit from the other -- and the nav graph agrees, which is how this
// announced itself: eight corridors built, and not one of them changed how much
// of the world was reachable.
const STEP = 0.5
const HEADROOM = 6
const CORRIDOR_TEX = 'METAL1'

const DIAG = process.env.COMPOSE_DIAG === '1'
const diag = []
const argv = process.argv.slice(2)
const arg = (name, dflt = null) => {
  const i = argv.indexOf(name)
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : dflt
}

const metamapPath = arg('--metamap')
const mapsDir = arg('--maps')
const outPath = arg('--out')
if (!metamapPath || !mapsDir || !outPath) {
  console.error('usage: compose.mjs --metamap <f> --maps <dir> --out <f.map> ' +
                '[--pitch N] [--only x0,y0,x1,y1]')
  process.exit(2)
}

// --- the arrangement ---------------------------------------------------------
const cells = []
let declaredPitch = null
for (const raw of readFileSync(metamapPath, 'utf8').split('\n')) {
  const line = raw.split('#')[0].trim()
  if (!line) continue
  const t = line.split(/\s+/)
  if (t[0] === 'cell' && t.length >= 4) cells.push({ x: +t[1], y: +t[2], map: t[3] })
  else if (t[0] === 'pitch' && t.length >= 3) declaredPitch = +t[1]
}

const noDoors = argv.includes('--keep-doors-shut')
const only = arg('--only')
const win = only ? only.split(',').map(Number) : null
const chosen = win
  ? cells.filter((c) => c.x >= win[0] && c.y >= win[1] && c.x <= win[2] && c.y <= win[3])
  : cells
if (chosen.length === 0) { console.error('compose: no cells selected'); process.exit(1) }
chosen.sort((a, b) => (a.y - b.y) || (a.x - b.x))

// --- read every chosen map, and MEASURE it -----------------------------------
//
// The extent is measured rather than assumed: a converted Doom level is whatever
// size its author drew, and a grid whose cells are smaller than the maps in them
// is a world where every level overlaps the one beside it.
const loaded = []
for (const c of chosen) {
  const doc = parseMap(readFileSync(join(mapsDir, `${c.map}.map`), 'utf8'))
  if (doc.errors.length) {
    console.error(`compose: ${c.map} does not parse:`)
    for (const e of doc.errors.slice(0, 5)) console.error(`  line ${e.line}: ${e.msg}`)
    process.exit(1)
  }
  let minx = Infinity, miny = Infinity, maxx = -Infinity, maxy = -Infinity
  const verts = []
  for (const s of doc.sectors) walkVertices(s, (v) => {
    verts.push(v)
    if (v.x.value < minx) minx = v.x.value
    if (v.x.value > maxx) maxx = v.x.value
    if (v.y.value < miny) miny = v.y.value
    if (v.y.value > maxy) maxy = v.y.value
  })
  loaded.push({ ...c, doc, verts, minx, miny, maxx, maxy,
                w: maxx - minx, h: maxy - miny, sectors: doc.sectors.length })
}

function walkVertices (item, fn) {
  for (const ch of item.children ?? []) {
    if (ch.kind === 'vertex') fn(ch)
    else walkVertices(ch, fn)
  }
}

const widest = Math.max(...loaded.map((m) => m.w))
const tallest = Math.max(...loaded.map((m) => m.h))
// A tenth of the largest cell as margin on each side, so neighbouring maps never
// touch and the gap between them is where a corridor would go.
const autoPitch = Math.ceil(Math.max(widest, tallest) * 1.2)
const pitch = Number(arg('--pitch', declaredPitch ?? autoPitch))

console.log(`compose: ${loaded.length} map(s)`)
for (const m of loaded) {
  console.log(`  (${m.x},${m.y}) ${m.map}  ${m.w.toFixed(1)} x ${m.h.toFixed(1)}` +
              `  ${m.sectors} sectors, ${m.verts.length} points`)
}
console.log(`  widest ${widest.toFixed(1)}, tallest ${tallest.toFixed(1)}, pitch ${pitch}`)
if (pitch < Math.max(widest, tallest)) {
  console.error(`compose: pitch ${pitch} is smaller than the largest map ` +
                `(${Math.max(widest, tallest).toFixed(1)}); cells would overlap`)
  process.exit(1)
}

// --- compose -----------------------------------------------------------------
//
// Each map is translated so its bounding box is CENTRED in its cell. Centring
// rather than corner-aligning because the maps are wildly different sizes --
// MAP07 is a third the width of MAP15 -- and corner-aligned cells read as a
// ragged staircase rather than as a grid.
// --- doors, opened in the geometry -------------------------------------------
//
// Doom stores a shut door as a sector whose ceiling rests on its floor, and a
// composed map has no scripts to open one -- so without this every door in
// every level is shut for ever and the spawn room is the whole world. The
// server says so out loud: "35 of 1759 sectors reachable on foot".
//
// Opened the way the trigger vocabulary opens one, statically: the lowest
// neighbouring ceiling, less Doom's four map units. A sector with no neighbour
// is left alone, because there is nothing for it to open onto.
function openDoors (m) {
  const sectors = m.doc.sectors
  const ceilOf = (s) => {
    const a = attr(s, 'ceil')
    return a ? a.v.value : 32
  }
  const floorOf = (s) => {
    const a = attr(s, 'floor')
    return a ? a.v.value : 0
  }
  let opened = 0
  for (const s of sectors) {
    if (ceilOf(s) > floorOf(s)) continue
    let lowest = Infinity
    walkVertices(s, (v) => {
      const nb = attr(v, 'neighbor')
      if (!nb) return
      const other = sectors[nb.v]
      if (other) lowest = Math.min(lowest, ceilOf(other))
    })
    if (!Number.isFinite(lowest)) continue
    const open = lowest - 0.0834
    if (open <= floorOf(s)) continue
    setNum(s, 'ceil', open)
    ++opened
  }
  return opened
}

const cellX0 = Math.min(...loaded.map((m) => m.x))
const cellY0 = Math.min(...loaded.map((m) => m.y))

const out = []
out.push('# GENERATED by tools/compose/compose.mjs -- do not edit.')
out.push('#')
out.push(`# ${loaded.length} map(s) from ${mapsDir}, on a ${pitch}-unit grid.`)
out.push('#')
out.push('# Scripts are deliberately absent: a converted level addresses its own')
out.push('# sectors by index, and every index moves when maps are concatenated. This')
out.push('# is geometry, spawns, lights and textures -- no doors, no monsters.')
out.push('')

// THE SECTOR EACH MAP STARTS AT, decided before anything is written: a corridor
// has to name sectors in two different maps, and it cannot do that while the
// numbering is still being discovered one map at a time.
let base = 0
for (const m of loaded) { m.base = base; base += m.sectors }
let nextSector = base

const spawnTags = new Set()
let totalSectors = 0, totalPoints = 0

for (const m of loaded) {
  const ox = (m.x - cellX0) * pitch + (pitch - m.w) / 2 - m.minx
  const oy = (m.y - cellY0) * pitch + (pitch - m.h) / 2 - m.miny
  // The bounding box moves with the map. The corridors are placed against it.
  m.minx += ox; m.maxx += ox; m.miny += oy; m.maxy += oy

  // Doors first, while this map's sector indices are still its own -- and the
  // flood fill with them, for the same reason and it is the sharper one: the
  // renumbering below turns every `neighbor` into a GLOBAL index, and a fill
  // that reads one as a local index walks out of the array on its first step.
  // Every map but the first reported exactly one reachable sector.
  const opened = noDoors ? 0 : openDoors(m)
  m.reachable = reachable(m)
  if (DIAG) console.log(`  diag ${m.map}: ${m.reachable.size} of ${m.sectors} reachable`)

  // Every vertex moves, and every neighbour reference shifts by however many
  // sectors came before this map.
  for (const v of m.verts) {
    v.x = numFrom(v.x.value + ox)
    v.y = numFrom(v.y.value + oy)
    const nb = attr(v, 'neighbor')
    if (nb) setInt(v, 'neighbor', nb.v + m.base)
  }

  const keep = []
  for (const it of m.doc.items) {
    if (it.kind === 'script') continue                 // see the header
    if (it.kind === 'rail' || it.kind === 'path') continue
    // One texscale and one body for the whole file, from the first map. Two
    // maps at different scales cannot be composed anyway, and mapc would take
    // the last one it read.
    if ((it.kind === 'texscale' || it.kind === 'body') && m !== loaded[0]) continue
    if (it.kind === 'spawn') {
      // Every map's spawns survive, named after the map, so you can start
      // anywhere in the composed world. The FIRST map's `start` keeps its name
      // because that is the tag a server asks for by default.
      const tag = (m === loaded[0] && it.tag === 'start') ? 'start' : `${m.map}_${it.tag}`
      if (spawnTags.has(tag)) continue
      spawnTags.add(tag)
      it.tag = tag
      it.x = numFrom(it.x.value + ox)
      it.y = numFrom(it.y.value + oy)
    }
    if (it.kind === 'light') {
      it.x = numFrom(it.x.value + ox)
      it.y = numFrom(it.y.value + oy)
    }
    keep.push(it)
  }
  m.doc.items = keep

  m.header = `# --- ${m.map}  cell (${m.x},${m.y})  ${m.w.toFixed(1)} x ${m.h.toFixed(1)}` +
             `  ${opened} door(s) opened ---`
  totalSectors += m.sectors
  totalPoints += m.verts.length
}

// --- corridors ---------------------------------------------------------------
//
// A LINK IS A HOLE IN EACH MAP'S OUTER WALL AND A RECTANGLE BETWEEN THEM.
//
// The opening has to be on the map's OUTER EDGE, not merely on a sector facing
// the right way: a corridor leaving an inner wall would run through the rest of
// the level on its way out, and two sectors covering the same ground is a world
// where "which sector is this point in" has two answers. Everything between the
// maps is empty margin, which is the only place a generated corridor can safely
// be.
//
// Doom's levels are drawn on a grid, so there is nearly always an axis-aligned
// wall at the extreme; a link with no candidate on either side is REPORTED and
// skipped rather than forced, because a corridor placed anyway is a corridor
// through somebody's bedroom.
const W = 2.5                 // how wide a corridor is
const EDGE_TOL = 60           // how far inside the map an opening may sit

const byCell = new Map()
for (const m of loaded) byCell.set(`${m.x},${m.y}`, m)

// Every vertex of a map, as loops, in world coordinates -- with the item so the
// split can be written back into the document it came from.
const round4 = (v) => Math.round(v * 10000) / 10000

// The floor a sector sits at, as the file states it.
function floorOf (s) {
  const a = attr(s, 'floor')
  return a ? a.v.value : 0
}

// How many steps it takes to climb from one floor to another without any single
// one being more than a body can manage.
function stepsBetween (fa, fb) {
  return Math.max(1, Math.ceil(Math.abs(fb - fa) / STEP))
}

function loopsOf (m) {
  const out = []
  for (const s of m.doc.sectors) {
    const loop = []
    for (const ch of s.children ?? []) if (ch.kind === 'vertex') loop.push(ch)
    if (loop.length >= 3) out.push({ sector: s, loop })
  }
  return out
}

// WHICH OF A MAP'S SECTORS A PLAYER CAN ACTUALLY STAND IN.
//
// A corridor cut into a wall nobody can reach is a corridor nobody can use, and
// that is not a theory: the first eight of these were built into whatever
// sector happened to own the outermost wall, and the server went on reporting
// exactly the same 110 sectors reachable from the spawn afterwards as before.
//
// The same flood fill the server prints at load, done here so the answer can
// steer where the hole goes: from the sector the spawn is in, through every
// doorway whose floors are within a body's step of each other.
function reachable (m) {
  const sectors = m.doc.sectors
  const loops = loopsOf(m)
  const spawn = m.doc.items.find((it) => it.kind === 'spawn')
  const seen = new Set()
  if (!spawn) return seen

  // Which sector the spawn is in: ray casting over each outer loop. Holes are
  // ignored, which can only ever include a sector that should have been
  // excluded -- and a spawn inside a pillar is not a map anybody shipped.
  let from = -1
  for (const { sector, loop } of loops) {
    let inside = false
    for (let i = 0, j = loop.length - 1; i < loop.length; j = i++) {
      const xi = loop[i].x.value, yi = loop[i].y.value
      const xj = loop[j].x.value, yj = loop[j].y.value
      if ((yi > spawn.y.value) !== (yj > spawn.y.value) &&
          spawn.x.value < (xj - xi) * (spawn.y.value - yi) / (yj - yi) + xi) inside = !inside
    }
    if (inside) { from = sectors.indexOf(sector); break }
  }
  if (from < 0) return seen

  const queue = [from]
  seen.add(from)
  while (queue.length) {
    const si = queue.pop()
    const entry = loops.find((l) => sectors.indexOf(l.sector) === si)
    if (!entry) continue
    for (const v of entry.loop) {
      const nb = attr(v, 'neighbor')
      if (!nb || seen.has(nb.v)) continue
      const other = sectors[nb.v]
      if (!other) continue
      // UP is limited and DOWN is not: a body climbs half a unit and falls any
      // distance. Treating the two the same reported three of MAP01's sixty
      // sectors as reachable, because every drop counted as a wall.
      if (floorOf(other) - floorOf(entry.sector) > STEP) continue
      const a = attr(other, 'ceil'), b = attr(entry.sector, 'ceil')
      const ceilOther = a ? a.v.value : 32
      if (ceilOther - floorOf(other) < 1.2) continue      // no headroom
      seen.add(nb.v)
      queue.push(nb.v)
    }
  }
  return seen
}

// The best place to cut a hole in `m`'s outer wall facing `dir`.
//
// `dir` is 0..3 for +x, -x, +y, -y. A candidate is a wall edge (no neighbour)
// that is perpendicular to `dir`, long enough to take the corridor, and out at
// the map's own extreme in that direction.
function opening (m, dir) {
  const [dx, dy] = [[1, 0], [-1, 0], [0, 1], [0, -1]][dir]
  const extreme = dx > 0 ? m.maxx : dx < 0 ? m.minx : dy > 0 ? m.maxy : m.miny
  const found = []
  const canReach = m.reachable ?? null
  for (const { sector, loop } of loopsOf(m)) {
    // Only where a player can get to. See reachable().
    if (canReach && !canReach.has(m.doc.sectors.indexOf(sector))) continue
    for (let i = 0; i < loop.length; i++) {
      const a = loop[i]
      const b = loop[(i + 1) % loop.length]
      if (attr(a, 'neighbor')) continue          // already a doorway
      const ex = b.x.value - a.x.value
      const ey = b.y.value - a.y.value
      const len = Math.hypot(ex, ey)
      if (len < W + 1) continue
      // Perpendicular to the direction of travel, so the corridor leaves it
      // square. An angled opening would need an angled corridor and every
      // rectangle below would become a trapezium.
      if (dx !== 0 && Math.abs(ex) > 0.05) continue
      if (dy !== 0 && Math.abs(ey) > 0.05) continue
      // Facing OUT. A sector's loop is counter-clockwise, so the outward normal
      // of a->b is (ey, -ex).
      const nx = ey / len, ny = -ex / len
      if (nx * dx + ny * dy < 0.9) continue
      const at = dx !== 0 ? a.x.value : a.y.value
      const away = Math.abs(at - extreme)
      if (DIAG) diag.push({ map: m.map, dir, away, len })
      if (away > EDGE_TOL) continue
      // Nearest the extreme wins; a tie goes to the longer wall.
      const score = -away * 10 + len
      found.push({ score, sector, loop, i, a, b, ex, ey, len, nx, ny, at })
    }
  }
  // BEST FIRST, and the caller decides. Whether an opening is usable depends on
  // where the corridor leaving it would have to go, which this cannot see: a
  // wall facing the right way with the rest of its own level in front of it is
  // no use, and two sectors covering the same ground is a world where "which
  // sector is this point in" has two answers.
  found.sort((p, q) => q.score - p.score)
  return found
}

// Does this rectangle cross anything the map already has?
//
// Bounding boxes, which over-reject -- a sector's box is not the sector -- and
// that is the right direction to be wrong in. A corridor refused for crossing
// a room it would have missed is a link that does not exist; a corridor built
// through one is a world with two floors in the same place.
function clearOf (m, x0, y0, x1, y1, exceptIndex) {
  for (const b of m.boxes) {
    if (b.index === exceptIndex) continue
    if (b.x1 <= x0 + 0.01 || b.x0 >= x1 - 0.01) continue
    if (b.y1 <= y0 + 0.01 || b.y0 >= y1 - 0.01) continue
    return false
  }
  return true
}

// Every sector's bounding box, in world coordinates, measured once.
function boxesOf (m) {
  const out = []
  for (const { sector, loop } of loopsOf(m)) {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity
    for (const v of loop) {
      x0 = Math.min(x0, v.x.value); x1 = Math.max(x1, v.x.value)
      y0 = Math.min(y0, v.y.value); y1 = Math.max(y1, v.y.value)
    }
    out.push({ index: m.doc.sectors.indexOf(sector), x0, y0, x1, y1 })
  }
  return out
}

// Cut the hole: two new vertices in the middle of the chosen edge, and the
// segment between them made a doorway onto `intoSector`.
function cut (m, o, intoSector) {
  const ux = o.ex / o.len, uy = o.ey / o.len
  const mx = (o.a.x.value + o.b.x.value) / 2
  const my = (o.a.y.value + o.b.y.value) / 2
  const p1 = { x: mx - ux * W / 2, y: my - uy * W / 2 }
  const p2 = { x: mx + ux * W / 2, y: my + uy * W / 2 }

  const mk = (pt, nb) => {
    const v = { kind: 'vertex', line: 0, trail: '', x: numFrom(pt.x), y: numFrom(pt.y), attrs: [] }
    if (nb !== null) setInt(v, 'neighbor', nb)
    else setAttr(v, { n: 'wall', kind: 'flag' })
    // The wall texture the edge already carried, so the hole does not read as
    // untextured next to the wall it was cut from.
    const tex = attr(o.a, 'tex') ?? attr(o.a, 'middle')
    if (tex) setAttr(v, { n: 'tex', kind: 'name', v: tex.v })
    return v
  }

  // The first new vertex OPENS the doorway (its edge runs to the second); the
  // second closes it and carries on as wall.
  const kids = o.sector.children
  const at = kids.indexOf(o.a)
  kids.splice(at + 1, 0, mk(p1, intoSector), mk(p2, null))
  return { p1, p2 }
}

// ONE RECTANGLE OF CORRIDOR.
//
// Wound counter-clockwise starting at the top-left, so the four edges leave in
// a fixed order -- west, south, east, north -- and a neighbour can be named per
// side rather than per vertex index. A vertex's `neighbor` applies to the edge
// that STARTS at it, which is the same rule the openings above are cut by.
function box (x0, x1, ylo, yhi, { west = null, south = null, east = null, north = null } = {},
              floor = 0) {
  const s = { kind: 'sector', line: 0, trail: '', attrs: [], children: [], endTrail: '', index: 0 }
  setNum(s, 'floor', round4(floor))
  setNum(s, 'ceil', round4(floor + HEADROOM))
  const pts = [[x0, yhi], [x0, ylo], [x1, ylo], [x1, yhi]]
  const nbs = [west, south, east, north]
  pts.forEach(([x, y], i) => {
    const v = { kind: 'vertex', line: 0, trail: '', x: numFrom(x), y: numFrom(y), attrs: [] }
    if (nbs[i] === null) {
      setAttr(v, { n: 'wall', kind: 'flag' })
      setAttr(v, { n: 'tex', kind: 'name', v: CORRIDOR_TEX })
    } else {
      setInt(v, 'neighbor', nbs[i])
    }
    s.children.push(v)
  })
  return s
}

// --- join the cells ----------------------------------------------------------
//
// Five rectangles per link: out of A, a turn, the run between the cells, a
// second turn, and into B. Every junction is a WHOLE shared edge because the
// turns are square and exactly as wide as the corridor -- two sectors that
// overlap along part of an edge are not neighbours in this engine, they are two
// sectors in the same place.
const corridors = []
let joined = 0, refused = 0
for (const m of loaded) m.boxes = boxesOf(m)

// Can a corridor actually leave this opening towards the gate without crossing
// the level it is leaving?
function leaves (m, o, dir, gate) {
  const half = W / 2
  const mx = (o.a.x.value + o.b.x.value) / 2
  const my = (o.a.y.value + o.b.y.value) / 2
  const si = m.doc.sectors.indexOf(o.sector)
  if (dir === 0) return gate > mx + 1 && clearOf(m, mx, my - half, gate, my + half, si)
  if (dir === 1) return gate < mx - 1 && clearOf(m, gate, my - half, mx, my + half, si)
  if (dir === 2) return gate > my + 1 && clearOf(m, mx - half, my, mx + half, gate, si)
  return gate < my - 1 && clearOf(m, mx - half, gate, mx + half, my, si)
}

for (const a of loaded) {
  for (const [dx, dy, dir, back] of [[1, 0, 0, 1], [0, 1, 2, 3]]) {
    const b = byCell.get(`${a.x + dx},${a.y + dy}`)
    if (!b) continue
    // Best first on both sides, and the first pair whose corridor crosses
    // nothing wins. A wall facing the right way with half its own level in
    // front of it is no use, and only the rectangle knows that.
    const gate = dx === 1 ? (a.maxx + b.minx) / 2 : (a.maxy + b.miny) / 2
    const pickA = opening(a, dir).find((o) => leaves(a, o, dir, gate))
    const pickB = opening(b, back).find((o) => leaves(b, o, back, gate))
    if (!pickA || !pickB) {
      ++refused
      console.log(`  no corridor ${a.map} -> ${b.map}: ` +
                  `${pickA ? '' : a.map + ' has no clear way out'}` +
                  `${(!pickA && !pickB) ? ', ' : ''}` +
                  `${pickB ? '' : b.map + ' has no clear way out'}`)
      continue
    }
    const oa = pickA, ob = pickB

    const fa = floorOf(oa.sector), fb = floorOf(ob.sector)
    const n = stepsBetween(fa, fb)

    // Reserved before anything is built, because a corridor names sectors in
    // two maps and in itself, and every one of those numbers has to be known
    // before the first vertex is written.
    const iQ1 = nextSector++, iT1 = nextSector++
    const run = []
    for (let i = 0; i < n; i++) run.push(nextSector++)
    const iT2 = nextSector++, iQ3 = nextSector++

    // THE RUN IS A STAIRCASE. The two rooms it joins are at whatever heights
    // their own levels put them, and a body climbs half a unit at a time -- so
    // the height is spent over the length of the run rather than at one end,
    // where it would be a wall from one side and a pit from the other.
    const treads = []
    for (let i = 0; i < n; i++) treads.push(fa + ((fb - fa) * (i + 1)) / n)

    const ca = cut(a, oa, iQ1)
    const cb = cut(b, ob, iQ3)
    const nbA = oa.sector.index + a.base
    const nbB = ob.sector.index + b.base

    if (dx === 1) {
      const gx = (a.maxx + b.minx) / 2
      const gx0 = gx - W / 2, gx1 = gx + W / 2
      const ay0 = Math.min(ca.p1.y, ca.p2.y), ay1 = Math.max(ca.p1.y, ca.p2.y)
      const by0 = Math.min(cb.p1.y, cb.p2.y), by1 = Math.max(cb.p1.y, cb.p2.y)
      if (gx0 - ca.p1.x < 1 || cb.p1.x - gx1 < 1) { ++refused; continue }

      corridors.push(box(ca.p1.x, gx0, ay0, ay1, { west: nbA, east: iT1 }, fa))
      const up = by0 >= ay1
      const lo = up ? ay1 : by1
      const hi = up ? by0 : ay0
      if (hi - lo < n * 0.6) {
        console.log(`  no corridor ${a.map} -> ${b.map}: ${(hi - lo).toFixed(1)} units of run ` +
                    `for ${n} step(s) of height`)
        ++refused
        continue
      }
      corridors.push(box(gx0, gx1, ay0, ay1, { west: iQ1, [up ? 'north' : 'south']: run[up ? 0 : n - 1] }, fa))
      const seg = (hi - lo) / n
      for (let i = 0; i < n; i++) {
        const y0 = lo + i * seg, y1 = y0 + seg
        // Ordered from T1 towards T2 when the run goes north, and the other way
        // when it goes south -- so `treads` is always read from A's end.
        const k = up ? i : n - 1 - i
        const south = up ? (i === 0 ? iT1 : run[i - 1]) : (i === 0 ? iT2 : run[i - 1])
        const north = up ? (i === n - 1 ? iT2 : run[i + 1]) : (i === n - 1 ? iT1 : run[i + 1])
        corridors.push(box(gx0, gx1, y0, y1, { south, north }, treads[k]))
      }
      corridors.push(box(gx0, gx1, by0, by1, { east: iQ3, [up ? 'south' : 'north']: run[up ? n - 1 : 0] }, fb))
      corridors.push(box(gx1, cb.p1.x, by0, by1, { west: iT2, east: nbB }, fb))
    } else {
      const gy = (a.maxy + b.miny) / 2
      const gy0 = gy - W / 2, gy1 = gy + W / 2
      const ax0 = Math.min(ca.p1.x, ca.p2.x), ax1 = Math.max(ca.p1.x, ca.p2.x)
      const bx0 = Math.min(cb.p1.x, cb.p2.x), bx1 = Math.max(cb.p1.x, cb.p2.x)
      if (gy0 - ca.p1.y < 1 || cb.p1.y - gy1 < 1) { ++refused; continue }

      corridors.push(box(ax0, ax1, ca.p1.y, gy0, { south: nbA, north: iT1 }, fa))
      const east = bx0 >= ax1
      const lo = east ? ax1 : bx1
      const hi = east ? bx0 : ax0
      if (hi - lo < n * 0.6) {
        console.log(`  no corridor ${a.map} -> ${b.map}: ${(hi - lo).toFixed(1)} units of run ` +
                    `for ${n} step(s) of height`)
        ++refused
        continue
      }
      corridors.push(box(ax0, ax1, gy0, gy1, { south: iQ1, [east ? 'east' : 'west']: run[east ? 0 : n - 1] }, fa))
      const seg = (hi - lo) / n
      for (let i = 0; i < n; i++) {
        const x0 = lo + i * seg, x1 = x0 + seg
        const k = east ? i : n - 1 - i
        const west = east ? (i === 0 ? iT1 : run[i - 1]) : (i === 0 ? iT2 : run[i - 1])
        const eastNb = east ? (i === n - 1 ? iT2 : run[i + 1]) : (i === n - 1 ? iT1 : run[i + 1])
        corridors.push(box(x0, x1, gy0, gy1, { west, east: eastNb }, treads[k]))
      }
      corridors.push(box(bx0, bx1, gy0, gy1, { north: iQ3, [east ? 'west' : 'east']: run[east ? n - 1 : 0] }, fb))
      corridors.push(box(bx0, bx1, gy1, cb.p1.y, { south: iT2, north: nbB }, fb))
    }
    ++joined
  }
}

// --- write it out ------------------------------------------------------------
for (const m of loaded) {
  out.push(m.header)
  out.push(emitMap(m.doc))
  out.push('')
}
if (corridors.length) {
  out.push(`# --- ${joined} corridor(s), ${corridors.length} sectors ---`)
  out.push(emitMap({ items: corridors }))
  out.push('')
}

mkdirSync(dirname(outPath), { recursive: true })
writeFileSync(outPath, out.join('\n'))
if (DIAG) {
  const best = new Map()
  for (const d of diag) {
    const k = `${d.map} dir${d.dir}`
    if (!best.has(k) || d.away < best.get(k).away) best.set(k, d)
  }
  for (const [k, d] of [...best].sort()) {
    console.log(`  diag ${k}: nearest reachable outward wall is ${d.away.toFixed(1)} ` +
                `from the edge, ${d.len.toFixed(1)} long`)
  }
}
console.log(`compose: ${joined} corridor(s), ${refused} refused`)
console.log(`compose: wrote ${outPath} -- ${totalSectors + corridors.length} sectors, ` +
            `${totalPoints} points`)
