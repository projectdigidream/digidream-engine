//
// A TEST WORLD: SEVERAL MAPS ON A METAMAP'S GRID, JOINED BY GENERATED GROUND.
//
// TEST SCAFFOLDING, NOT A PRODUCT. It exists so the seam -- the client's held
// levels, the server's residency and crossings -- can be exercised across many
// real maps at once. Production transitions between levels are authored per
// pair of maps; nothing in the engine or the protocol may depend on what this
// generates, and its geometry is not worth polishing unless a test needs it.
//
//   node tools/compose/compose.mjs --metamap <f> --maps <dir> --seams <out dir>
//                                  [--prefix p] [--pitch N] [--only x0,y0,x1,y1]
//
// Each map stays its own level and file, is moved into its cell, and the rest
// of the cell becomes ground tiles whose height is a function of world
// coordinates (terrain.mjs), so two cells agree at their shared boundary
// without comparing notes. Each level's own scripts are rewritten to match
// (rewriteScript), and a metamap is written beside the maps.
//
// It MUTATES each map's parsed document and re-emits it with the editor's own
// emitter rather than writing the format again: editor/src/mapfmt.js is proven
// byte-for-byte against mapc over every map in the repository.
//
import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { parseMap, emitMap, numFrom, attr, setAttr, setInt, setNum } from '../../editor/src/mapfmt.js'
import { heightAt, waterAt, TILE } from './terrain.mjs'

// A body steps up half a unit and no more (sim::constants STEP_HEIGHT), so a
// corridor that meets a room's floor at the wrong height is a wall from one
// side and a pit from the other -- and the nav graph agrees, which is how this
// announced itself: eight corridors built, and not one of them changed how much
// of the world was reachable.
const W = 2.5                 // how wide a doorway cut into a level is
const EDGE_TOL = 60           // how far inside the map an opening may sit
const STEP = 0.5
// HOW TALL A CORRIDOR IS: ONE NUMBER FOR THE WHOLE COMPOSED WORLD.
//
// Two separate mistakes meet here, and the fix has to answer both.
//
// The engine's default is 6, which is right for a level drawn in engine units
// and four times too tall for a converted Doom level, where the body is 1.1667
// and a room clears 1.67. Built to the default, the first nine maps had slot
// canyons for corridors. So the height has to follow the SCALE OF THE CONTENT.
//
// But it cannot follow the scale of one ROOM. The two halves of a seam are cut
// by two runs that never compare notes, and a height taken from each side's own
// room made six of eight seams meet at two different ceilings -- one pair 3.5
// against 1.5, which from inside the taller corridor is a wall with a letterbox
// in it. That is the same problem SEAM_Z already solves for the floor, and it
// has the same shape of answer: a number both sides arrive at without talking.
//
// So: derived from the tallest body any map in the world declares, and then
// constant for all of them. The floor at a boundary is SEAM_Z on both sides and
// the height is this on both sides, so the two openings are the same rectangle
// by construction rather than by coincidence.
const HEAD_RATIO = 2.3        // 1.1667 * 2.3 = 2.68, which is Doom's own 128 units
const HEAD_FALLBACK = 6       // a map that declares no body: the engine's default
const WALL_TEX = 'METAL1'      // the walls of anything this adds
const DU = 48                 // Doom units to the engine's one; what the scripts count in

const DIAG = process.env.COMPOSE_DIAG === '1'
const diag = []
const argv = process.argv.slice(2)
const arg = (name, dflt = null) => {
  const i = argv.indexOf(name)
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : dflt
}

const metamapPath = arg('--metamap')
const mapsDir = arg('--maps')
const seamsOut = arg('--seams')
// A NAME OF ITS OWN for every map written. The sources keep theirs, and two
// files called map01.map in one repository is ambiguous to everything that
// resolves a map by its basename -- which the editor's round-trip test does,
// over every map there is, and it said so.
const prefix = arg('--prefix', '')
if (!metamapPath || !mapsDir || !seamsOut) {
  console.error('usage: compose.mjs --metamap <f> --maps <dir> --seams <dir> ' +
                '[--prefix p] [--pitch N] [--only x0,y0,x1,y1]')
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

const GROUND_TEX = 'GRASS1'
const WATER_TEX = 'FWATER1'
const SKY_TEX = 'F_SKY1'
// One ceiling for the whole outdoors, well clear of the highest ground, so the
// sky is a sky rather than a lid that follows the hills.
const SKY_Z = 12
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

// ONE HEIGHT FOR THE WHOLE WORLD, from the tallest body any of its maps
// declares. Read off the maps rather than passed in, because a map that says
// `body height 1.1667` has already answered "what scale is this drawn at" and
// asking the operator again is the second place for one number to live.
const bodyHeights = loaded.map((m) => {
  const it = m.doc.items.find((i) => i.kind === 'body')
  const h = it ? attr(it, 'height') : null
  return h ? h.v.value : 0
}).filter((h) => h > 0)
const headroom = bodyHeights.length
  ? round4(Math.max(...bodyHeights) * HEAD_RATIO)
  : HEAD_FALLBACK
if (DIAG) console.log(`compose: added sectors ${headroom} tall ` +
  `(tallest body ${bodyHeights.length ? Math.max(...bodyHeights) : 'none declared'})`)

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
let pitch = Number(arg('--pitch', declaredPitch ?? autoPitch))
// A WHOLE NUMBER OF TILES. Two cells agree about the ground because they
// evaluate the same function at the same world points -- which they only do if
// their tile grids are the same grid. A pitch that is not a multiple of the
// tile size offsets every cell's lattice from its neighbour's and the argument
// collapses into a seam of cliffs.
pitch = Math.ceil(pitch / TILE) * TILE

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

const byCell = new Map()
for (const m of loaded) byCell.set(`${m.x},${m.y}`, m)

// --- each map in its own cell ------------------------------------------------
//
// Every map stays its own level and its own file; its coordinates are moved so
// it sits inside [0, pitch) on both axes. The engine adds the cell origin. The
// cell boundary is `open` ground on both sides, at the same height because it
// is the same function of the same world coordinates, so a body walks off one
// level's last tile onto the next level's first, and the server's seam check
// hands it across at the same world point.
const SEAM_Z = 0

// A MAP'S OWN SCRIPT, MOVED WITH THE MAP.
//
// Two things in it stopped being true the moment the map was recentred and cut:
//
//   * every coordinate. mobs.place, doom.load's `props` and its `teleports` all
//     carry absolute positions in Doom units, and the map moved by (sx, sy).
//   * every side index. doom.load's `lines` table is keyed by side, and a cut
//     doorway splices two sides into the middle of the file.
//
// Both are mechanical, and NEITHER is optional: leaving the coordinates puts
// MAP01's monsters in the sea beside it, and leaving the side keys makes
// doom.lua refuse the whole table -- "10 of 13 lines do not match the loaded
// map; actions disabled" -- which is every door in the level, shut for ever.
//
// Regex rather than a Lua parser because these files are GENERATED by
// tools/wad/convert.js and their shape is one line per record. The two patterns
// are asserted against a count, so a convert.js that changes shape fails here
// rather than silently rewriting nothing.
function rewriteScript (src, m, remap, path) {
  const dx = Math.round(m.sx * DU), dy = Math.round(m.sy * DU)
  // AND UP OR DOWN, in the terrain world: a level is lifted to meet the ground
  // outside it, and everything the level's script places is placed relative to
  // its floors. Left behind, every monster in the world would stand at the
  // height the level used to be -- sunk into the ground or hanging over it.
  const dz = Math.round((m.dz ?? 0) * DU)
  let moved = 0, keyed = 0
  // ONE PASS, with z optional.
  //
  // Two passes and a negative lookahead is the obvious way and it is wrong: the
  // digit group backtracks to make the lookahead succeed, so `y=1248, z=8`
  // matches with y=124 and the placement lands a thousand units away. Caught by
  // tools/compose/test.mjs, which is exactly what it is for.
  let out = src.replace(/\bx=(-?\d+), y=(-?\d+)(, z=(-?\d+))?/g, (_, a, b, zPart, c) => {
    ++moved
    return `x=${Number(a) + dx}, y=${Number(b) + dy}` +
           (zPart ? `, z=${Number(c) + dz}` : '')
  })
  // Only entries that carry a check_neighbor: that is what makes a record a
  // LINE rather than a teleport destination, which is keyed by sector tag and
  // must not be touched.
  const at = (k) => {
    const v = remap.get(Number(k))
    if (v === undefined) {
      console.error(`compose: ${path}: side ${k} is not in ${m.map}`)
      process.exit(1)
    }
    return v
  }
  out = out.replace(/\[(\d+)\] = \{([^\n]*check_neighbor=-?\d+[^\n]*)\}/g,
    (_, k, body) => {
      ++keyed
      return `[${at(k)}] = {${body.replace(/\bpair=(\d+)/g, (_2, q) => `pair=${at(q)}`)}}`
    })
  if (/\bdoom\.load\s*\{/.test(src) && /check_neighbor=/.test(src) && keyed === 0) {
    console.error(`compose: ${path} has line actions and none were re-keyed`)
    process.exit(1)
  }
  if (DIAG) diag.push(`${m.map}: ${path} ${moved} coord(s) +(${dx},${dy}), ${keyed} line(s) re-keyed`)
  return out
}

if (seamsOut) {

  for (const m of loaded) {
    // Centred in its own cell, in its OWN coordinates: the engine adds the cell
    // origin. A map whose numbers already sat outside [0, pitch) would be in
    // its neighbour's cell before anybody moved.
    //
    // SNAPPED TO A WHOLE DOOM UNIT, and that is not tidiness. The scripts that
    // travel with the map place monsters, props and teleport destinations in
    // Doom's integer units, and they are shifted by this same offset below. A
    // fractional offset would make those coordinates fractional, and doom.lua
    // turns them into fixed point with fx.frac(n, 48) -- a rational, whose
    // numerator is an integer or is not a number at all.
    m.sx = Math.round(((pitch - m.w) / 2 - m.minx) * DU) / DU
    m.sy = Math.round(((pitch - m.h) / 2 - m.miny) * DU) / DU
  }

  for (const m of loaded) {
    // NO DOORS ARE OPENED HERE, unlike the merge. A map that stays its own file
    // keeps its own scripts, its own sector numbering and therefore its own
    // doors -- corridor sectors are APPENDED, and appending cannot move a
    // sector index anything already refers to. Reachability counts a shut door
    // as passable for the same reason: it opens when somebody presses it.
    //
    // SIDE indices are a different matter, and believing otherwise is what
    // disabled every door in the first nine maps this wrote. Cutting a doorway
    // SPLICES two vertices into an existing sector's loop, so every side after
    // it moves -- and map01.lua's line table is keyed by side index. Hence the
    // identity recorded here and the remap built at write time.
    m.reachable = reachable(m)

    m.sideAt = new Map()
    { let i = 0; for (const s of m.doc.sectors) walkVertices(s, (v) => m.sideAt.set(v, i++)) }

    // The measurements have to be made in the frame the map will be written in.
    for (const v of m.verts) { v.x = numFrom(v.x.value + m.sx); v.y = numFrom(v.y.value + m.sy) }
    for (const it of m.doc.items) {
      if (it.kind === 'spawn' || it.kind === 'light') {
        it.x = numFrom(it.x.value + m.sx)
        it.y = numFrom(it.y.value + m.sy)
      }
    }
    m.minx += m.sx; m.maxx += m.sx; m.miny += m.sy; m.maxy += m.sy
    m.boxes = boxesOf(m)
    m.extra = []
    m.base = 0
    m.next = m.sectors
  }


  // --- GROUND, instead of a thread across the void ------------------------
  //
  // Every tile of the cell that the level does not occupy becomes a sector, at
  // the height the world-space field says. Two things fall out of that which a
  // corridor had to work for:
  //
  //   the WHOLE cell boundary is seam, not a 2.5-unit doorway, and the tiles on
  //   either side of it are the same height because they are the same function
  //   of the same coordinates -- so a body walks across anywhere along it;
  //
  //   a level is joined by opening ANY outward wall onto the ground beside it,
  //   so there is nothing to route and nothing for the two sides to agree on.
  //   The four levels that could take no corridor at all are not special here.
  {
    let tiles = 0, mouths = 0, skies = 0, outdoors = 0, sealed = []
    for (const m of loaded) {
      const N = Math.round(pitch / TILE)
      const ox = m.x * pitch, oy = m.y * pitch
      const idx = new Map()
      const free = []
      for (let j = 0; j < N; j++) {
        for (let i = 0; i < N; i++) {
          const x0 = i * TILE, y0 = j * TILE
          // clearOf over the level's sector boxes: a tile that overlaps the
          // level is the level's ground, not ours. Bounding boxes over-reject,
          // which leaves a little unclaimed apron around a level and is the
          // right direction to be wrong in -- ground INSIDE a room would be two
          // floors in one place.
          // A hair larger than the tile: mapc counts a level corner sitting
          // exactly on a tile's boundary as inside it, so ground stops just
          // short of touching a level's own floor.
          if (hitsLevel(m, x0 - 0.1, y0 - 0.1, x0 + TILE + 0.1, y0 + TILE + 0.1)) continue
          idx.set(`${i},${j}`, m.next + m.base + free.length)
          free.push({ i, j, x0, y0,
                      h: heightAt(ox + x0 + TILE / 2, oy + y0 + TILE / 2),
                      wet: waterAt(ox + x0 + TILE / 2, oy + y0 + TILE / 2) })
        }
      }
      m.next += free.length

      // WHERE THE LEVEL OPENS ONTO IT.
      //
      // THE LEVEL MOVES, NOT THE GROUND. A converted Doom level's floors are
      // wherever Doom put them -- tens of units up or down -- and the ground is
      // a gentle field a unit or two either side of zero. Insisting they meet
      // by accident opened eighteen walls in twenty-eight levels and sealed
      // seventeen of them.
      //
      // Bending the ground to reach each level instead would need a ramp longer
      // than the margin it has to fit in, and would break the one property this
      // whole design rests on: that the ground is a function of world position
      // and of nothing else. So the LEVEL is lifted or dropped as a whole until
      // its chosen doorway is level with the ground outside it, which is a
      // translation and changes nothing about the level except where it sits --
      // exactly as it is already translated in x and y.
      const NB = [[1, 0, 0], [-1, 0, 1], [0, 1, 2], [0, -1, 3]]
      const BACK = ['west', 'east', 'south', 'north']
      const tileFor = (o, dx, dy) => {
        const mx = (o.a.x.value + o.b.x.value) / 2 + dx * TILE * 0.5
        const my = (o.a.y.value + o.b.y.value) / 2 + dy * TILE * 0.5
        const ti = Math.floor(mx / TILE), tj = Math.floor(my / TILE)
        return { ti, tj, sector: idx.get(`${ti},${tj}`),
                 ground: free.find((f) => f.i === ti && f.j === tj) }
      }
      // WALLS YOU CAN ACTUALLY WALK TO, FIRST.
      //
      // opening() ranks by a reachability that counts a shut door as passable,
      // because a door opens when somebody presses it. That is right for asking
      // whether a level is connected and wrong for asking where to let somebody
      // OUT of it: every way out of MAP01 landed in a room behind a door, and
      // from the spawn seven of its seven hundred and seventy-six sectors were
      // walkable. There was an outdoors and no way to reach it.
      //
      // So candidates that can be walked to with nothing opened are taken
      // first, and the rest are kept as a fallback -- a level whose only outward
      // wall is behind a door is still better joined than sealed.
      const atOnce = reachable(m, false)
      // THE ROOM YOU ARRIVE IN, above all.
      //
      // "Reachable with nothing opened" was still not enough: MAP01's ways out
      // landed in the rooms NEXT to the spawn, and seven of its seven hundred
      // and seventy-six sectors were walkable from where the player stands. A
      // world you can see and cannot step into is worse than no world.
      //
      // So the sector holding the spawn is ranked first, and -- below -- the
      // level's lift is chosen from ITS wall, which makes that one doorway
      // match the ground by construction rather than by luck.
      const spawnPt = m.doc.items.find((it) => it.kind === 'spawn')
      let spawnSector = -1
      if (spawnPt) {
        m.doc.sectors.forEach((sec, si) => {
          const vs = []
          walkVertices(sec, (v) => vs.push(v))
          let inside = false
          for (let i = 0, j = vs.length - 1; i < vs.length; j = i++) {
            const xi = vs[i].x.value, yi = vs[i].y.value
            const xj = vs[j].x.value, yj = vs[j].y.value
            if ((yi > spawnPt.y.value) !== (yj > spawnPt.y.value) &&
                spawnPt.x.value < (xj - xi) * (spawnPt.y.value - yi) / (yj - yi) + xi) {
              inside = !inside
            }
          }
          if (inside) spawnSector = si
        })
      }
      const candidates = []
      for (const [dx, dy, dir] of NB) {
        for (const o of opening(m, dir)) {
          const t = tileFor(o, dx, dy)
          if (t.sector === undefined || !t.ground) continue
          const si = m.doc.sectors.indexOf(o.sector)
          candidates.push({ o, dir, ...t, si,
                            home: si === spawnSector,
                            open: atOnce.has(si) })
        }
      }
      candidates.sort((a, b) => (b.open ? 1 : 0) - (a.open ? 1 : 0))
      // The lift that suits the most doorways, not merely the first: a level
      // whose walls sit at three different heights should be entered at
      // whichever of them the ground agrees with oftenest.
      // Weighted toward the reachable ones: a lift that suits six doorways
      // nobody can walk to is worth less than one that suits a single doorway
      // in the room you arrive in.
      // THE LIFT COMES FROM THE ROOM YOU ARRIVE IN, when there is a wall
      // there: that doorway then matches the ground exactly and the way out is
      // a step, not a hope. Failing that, whichever lift suits the most
      // doorways, weighted heavily toward ones that can be walked to.
      let dz = 0, bestFit = -1
      for (const c of candidates) {
        const lift = Math.round((c.ground.h - floorOf(c.o.sector)) / STEP) * STEP
        let fit = 0
        for (const d of candidates) {
          if (Math.abs(d.ground.h - (floorOf(d.o.sector) + lift)) <= STEP) fit += d.open ? 10 : 1
        }
        if (fit > bestFit) { bestFit = fit; dz = lift }
      }
      m.dz = dz
      if (dz) for (const sec of m.doc.sectors) {
        for (const k of ['floor', 'ceil']) {
          const a = attr(sec, k)
          if (a) setNum(sec, k, round4(a.v.value + dz))
        }
      }

      // ONE SKY OVER THE WHOLE WORLD.
      //
      // A Doom level's open air is a sector with F_SKY1 on its ceiling, and the
      // engine that drew it treated that as "no ceiling". Here it is a ceiling
      // like any other, at whatever height the level put it -- so standing on
      // the ground outside, the level's own sky is a slab a few units up with a
      // wall holding it, and that wall is the enormous white cliff that made
      // the levels look like they were standing in a quarry.
      //
      // Given the same ceiling as the ground outside, there is no step between
      // them and so no wall face to draw: the level's open air becomes the
      // world's open air. Sectors that are not open to the sky keep their own
      // roofs, which is what makes a building still a building.
      let opened = 0
      for (const sec of m.doc.sectors) {
        const ct = attr(sec, 'ceiltex')
        if (!ct || String(ct.v).toUpperCase() !== SKY_TEX) continue
        // Never below its own floor, and never LOWERED: a sky sector already
        // taller than the world's is a tower, and squashing it would put its
        // roof under its own walls.
        if (floorOf(sec) + STEP >= SKY_Z || ceilOf(sec) >= SKY_Z) continue
        setNum(sec, 'ceil', round4(SKY_Z))
        ++opened
      }
      skies += opened

      // EVERY WALL THAT QUALIFIES, not a handful. The point of putting ground
      // outside is that a level stops being a box with a door in it: where its
      // edge meets ground at a height a body can step, that edge opens.
      const link = new Map()
      let cutHere = 0
      for (const c of candidates) {
        if (Math.abs(c.ground.h - floorOf(c.o.sector)) > STEP) continue
        if (link.has(`${c.ti},${c.tj}`)) continue
        cut(m, c.o, c.sector)
        link.set(`${c.ti},${c.tj}`, { side: BACK[c.dir], sector: c.o.sector.index + m.base })
        ++cutHere
        ++mouths
      }
      if (!cutHere) sealed.push(m.map)

      // AND YOU ARRIVE OUTDOORS.
      //
      // A Doom level starts you inside a building, because a Doom level IS the
      // building. Here it is one landmark in a landscape, and starting sealed
      // inside it means the whole world is on the other side of whichever door
      // that level opens with: MAP01 puts seven of its seven hundred and
      // seventy-six sectors within reach of the spawn, and every way out is in
      // one of the other seven hundred and sixty-nine.
      //
      // Tried the other way first -- open a wall of the room you arrive in --
      // and it fails on the same fact that causes the problem: a spawn room is
      // an interior, so it has no outward wall to open. Twenty of twenty-eight
      // levels got worse.
      //
      // So the player arrives on the ground beside the level's own doorway,
      // facing it. The level is still there to be gone into; it is just no
      // longer the only place you can be.
      if (cutHere && spawnPt) {
        const first = candidates.find((c) => link.has(`${c.ti},${c.tj}`))
        if (first) {
          spawnPt.x = numFrom(round4(first.ti * TILE + TILE / 2))
          spawnPt.y = numFrom(round4(first.tj * TILE + TILE / 2))
          // A hair above the ground: a spawn exactly on the floor is a spawn
          // the mover has to resolve out of it.
          if (spawnPt.z !== undefined) spawnPt.z = numFrom(round4(first.ground.h + 0.2))
          ++outdoors
        }
      }

      const style = { light: 192, floortex: GROUND_TEX, ceiltex: SKY_TEX }
      for (const t of free) {
        const at = (di, dj) => {
          const i = t.i + di, j = t.j + dj
          // OFF THE EDGE OF THE CELL IS THE SEAM, everywhere along it.
          if (i < 0 || j < 0 || i >= N || j >= N) return 'open'
          return idx.get(`${i},${j}`) ?? null      // null: the level's own wall
        }
        const sides = { west: at(-1, 0), east: at(1, 0), south: at(0, -1), north: at(0, 1) }
        const l = link.get(`${t.i},${t.j}`)
        if (l) sides[l.side] = l.sector
        m.extra.push(box(t.x0, t.x0 + TILE, t.y0, t.y0 + TILE, sides, t.h,
                         { ...style, floortex: t.wet ? WATER_TEX : GROUND_TEX,
                           head: SKY_Z - t.h }))
        ++tiles
      }
    }
    console.log(`compose: ${tiles} ground tiles, ${mouths} way(s) out of the levels` +
                `, ${skies} sky sector(s) opened to the world's` +
                `, ${outdoors} spawn(s) moved outdoors` +
                (sealed.length ? `, ${sealed.length} sealed: ${sealed.join(' ')}` : ''))
  }

  // THE TOOL CHECKS ITS OWN OUTPUT, and refuses to write a world it has broken.
  //
  // This engine joins two sectors only across a WHOLE SHARED EDGE: each names
  // the other, and the two edges are the same segment reversed. Every corridor
  // here is five or more rectangles laid end to end by arithmetic, and getting
  // one bound wrong produces two sectors over the same ground whose "shared"
  // edges are a corridor's width apart. The engine then joins neither, which
  // from inside is a passage blocked by a wall -- drawn where the stray edge
  // carried a texture and invisible where it did not, which is exactly how it
  // was reported.
  //
  // Cheap, total, and the thing that was missing: a corridor whose pieces do
  // not meet is a corridor nobody can walk down, and no amount of looking at
  // the map from above shows it.
  {
    let broken = 0
    for (const m of loaded) {
      const sectors = m.doc.items.filter((i) => i.kind === 'sector').concat(m.extra)
      const at = new Map()
      sectors.forEach((sec, si) => {
        const vs = []
        walkVertices(sec, (v) => vs.push(v))
        vs.forEach((v, i) => {
          const w = vs[(i + 1) % vs.length]
          at.set(`${si}#${v.x.value},${v.y.value}|${w.x.value},${w.y.value}`, v)
        })
      })
      // ONLY THE RECTANGLES THIS TOOL LAID OUT.
      //
      // A converted Doom level names neighbours across edges the far side
      // covers with two or three shorter ones, and mapc subdivides when it
      // bakes -- 360 of them in MAP17 alone, every one of them fine. What has
      // no excuse is two rectangles this file placed by arithmetic failing to
      // meet, and the doorway cut into the level is excluded for the same
      // reason: it splits an existing wall and is a T-junction by design.
      const mine = (i) => i >= m.doc.items.filter((x) => x.kind === 'sector').length
      sectors.forEach((sec, si) => {
        if (!mine(si)) return
        const vs = []
        walkVertices(sec, (v) => vs.push(v))
        vs.forEach((v, i) => {
          const nb = attr(v, 'neighbor')
          if (!nb || !mine(Number(nb.v))) return
          const w = vs[(i + 1) % vs.length]
          const back = at.get(`${nb.v}#${w.x.value},${w.y.value}|${v.x.value},${v.y.value}`)
          if (back && Number(attr(back, 'neighbor')?.v) === si) return
          ++broken
          if (broken <= 8) {
            console.error(`compose: ${m.map}: sector ${si} names ${nb.v} across ` +
              `(${v.x.value},${v.y.value})-(${w.x.value},${w.y.value}), ` +
              (back ? `but that edge names ${attr(back, 'neighbor')?.v ?? 'nothing'}`
                    : 'which is not an edge of it'))
          }
        })
      })
    }
    if (broken > 0) {
      console.error(`compose: ${broken} sector link(s) are not a whole shared edge; ` +
                    'nothing written')
      process.exit(1)
    }
  }

  mkdirSync(seamsOut, { recursive: true })
  for (const m of loaded) {
    // The map's own scripts travel with it. A bare name is a file beside the
    // .dnpm, so it is copied; a path with directories in it is relative to the
    // map, and works unchanged as long as the output sits at the same depth --
    // which is why this writes into a module's `maps/` and not anywhere.
    // THE SIDE REMAP, built the same way the side indices themselves are: by
    // walking the sectors in file order and counting vertices. The walk is
    // proven against mapc -- every check_neighbor in every converted map01.lua
    // agrees with the neighbour found at that index. The corridor sectors are
    // appended after all of these, so they take indices beyond the last one
    // here and shift nothing.
    const remap = new Map()
    { let i = 0; for (const s of m.doc.sectors) walkVertices(s, (v) => {
        const was = m.sideAt.get(v)
        if (was !== undefined) remap.set(was, i)
        i++
      }) }

    for (const it of m.doc.items) {
      if (it.kind !== 'script') continue
      if (it.path.includes('/')) continue
      const src = readFileSync(join(mapsDir, it.path), 'utf8')
      // A MAP'S OWN SCRIPT is rewritten; a shared one is copied. The two are
      // told apart by what they DO rather than by name: a file that calls
      // doom.load or mobs.place is talking about this map's sides and this
      // map's coordinates, and both of those moved.
      if (/\b(?:doom\.load|mobs\.place)\s*\{/.test(src)) {
        writeFileSync(join(seamsOut, it.path), rewriteScript(src, m, remap, it.path))
      } else {
        copyFileSync(join(mapsDir, it.path), join(seamsOut, it.path))
      }
    }
    m.doc.items = m.doc.items.concat(m.extra)
    writeFileSync(join(seamsOut, `${prefix}${m.map}.map`),
      `# GENERATED by tools/compose/compose.mjs --seams -- do not edit.\n` +
      `# ${m.map} moved into cell (${m.x},${m.y}) of a ${pitch}-unit grid, with\n` +
      `# generated ground around it. Test scaffolding: see the top of compose.mjs.\n\n` +
      emitMap(m.doc) + '\n')
  }
  const meta = ['# GENERATED by tools/compose/compose.mjs --seams -- do not edit.',
                '#', '# The pitch is what composes these maps in world space: a cell at (2,-1)',
                '# starts at (2*pitch, -1*pitch), and a stub that ends at local x = pitch is',
                '# the same line as its neighbour\'s stub beginning at local x = 0.',
                `pitch ${pitch} ${pitch}`, 'radius 1', '']
  for (const m of loaded) meta.push(`cell ${m.x} ${m.y} ${prefix}${m.map}`)
  writeFileSync(join(seamsOut, 'metamap.txt'), meta.join('\n') + '\n')
  console.log(`compose: wrote ${loaded.length} map(s) and a metamap to ${seamsOut}`)
  process.exit(0)
}

// Every vertex of a map, as loops, in world coordinates -- with the item so the
// split can be written back into the document it came from.
function round4 (v) { return Math.round(v * 10000) / 10000 }

// The floor a sector sits at, as the file states it.
function floorOf (s) {
  const a = attr(s, 'floor')
  return a ? a.v.value : 0
}

// And the ceiling. 32 for a sector that names none, which is the engine's own
// answer and is only ever a fallback in a map nothing converted.
function ceilOf (s) {
  const a = attr(s, 'ceil')
  return a ? a.v.value : 32
}

// How tall, how bright and what the flats are, for a corridor leaving `sector`.
//
// Clamped hard on the height. A Doom sector's ceiling is sometimes the sky and
// sometimes 512 units up a lift shaft, and a corridor that inherited either
// would be the trench this replaced.
// How bright a corridor is and what it is made of, taken from the room it
// leaves. NOT how tall -- see HEAD_RATIO. Light and flats may differ across a
// seam and nothing breaks: that is two rooms lit differently. A height that
// differs is a wall.
function styleOf (sector) {
  const light = attr(sector, 'light')
  return {
    light: light ? Number(light.v) : 128,
    floortex: attr(sector, 'floortex')?.v ?? null,
    ceiltex: attr(sector, 'ceiltex')?.v ?? null
  }
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
// `throughDoors` false asks the stricter question: what can be walked to with
// nothing opened yet -- which is what a player has when they arrive.
function reachable (m, throughDoors = true) {
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
      const a = attr(other, 'ceil')
      const ceilOther = a ? a.v.value : 32
      const gap = ceilOther - floorOf(other)
      // A SHUT DOOR IS A DOOR. Doom stores one as a sector with no gap at all,
      // and in a map that keeps its scripts it opens the first time somebody
      // presses it -- so counting it as a wall would rule out every room behind
      // every door in the level. A genuinely low gap is still a crawlspace
      // nobody gets through.
      if (gap > 0 && gap < 1.2) continue
      if (!throughDoors && gap <= 0) continue
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

// DOES THIS RECTANGLE TOUCH THE LEVEL ITSELF, rather than merely its bounding
// box?
//
// The box version over-rejects, and around a converted Doom level it
// over-rejects by about the area of the level: its sectors are full of
// diagonals, and a diagonal's box is mostly not the diagonal. Ground refused on
// that evidence left a ring of void around every level -- 4.4% of the cell,
// all of it in the one place a player walks through, and a hole you fall into
// rather than a patch of missing scenery.
//
// Three ways a rectangle and a polygon can meet, and all three have to be
// asked: a corner of one inside the other, either way round, or two edges
// crossing with no corner inside anything.
function hitsLevel (m, x0, y0, x1, y1) {
  const inRect = (x, y) => x > x0 && x < x1 && y > y0 && y < y1
  const cross = (ax, ay, bx, by, cx, cy, dx, dy) => {
    const d = (bx - ax) * (dy - cy) - (by - ay) * (dx - cx)
    if (Math.abs(d) < 1e-12) return false
    const t = ((cx - ax) * (dy - cy) - (cy - ay) * (dx - cx)) / d
    const u = ((cx - ax) * (by - ay) - (cy - ay) * (bx - ax)) / d
    return t > 0 && t < 1 && u > 0 && u < 1
  }
  for (const { loop } of loopsOf(m)) {
    let bx0 = Infinity, by0 = Infinity, bx1 = -Infinity, by1 = -Infinity
    for (const v of loop) {
      bx0 = Math.min(bx0, v.x.value); bx1 = Math.max(bx1, v.x.value)
      by0 = Math.min(by0, v.y.value); by1 = Math.max(by1, v.y.value)
    }
    if (bx1 <= x0 || bx0 >= x1 || by1 <= y0 || by0 >= y1) continue

    let inside = false           // is the rectangle's corner inside this sector
    for (let i = 0, j = loop.length - 1; i < loop.length; j = i++) {
      const xi = loop[i].x.value, yi = loop[i].y.value
      const xj = loop[j].x.value, yj = loop[j].y.value
      if (inRect(xi, yi)) return true
      if (cross(xj, yj, xi, yi, x0, y0, x1, y0)) return true
      if (cross(xj, yj, xi, yi, x1, y0, x1, y1)) return true
      if (cross(xj, yj, xi, yi, x1, y1, x0, y1)) return true
      if (cross(xj, yj, xi, yi, x0, y1, x0, y0)) return true
      if ((yi > y0) !== (yj > y0) &&
          x0 < (xj - xi) * (y0 - yi) / (yj - yi) + xi) inside = !inside
    }
    if (inside) return true
  }
  return false
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
function cut (m, o, intoSector, at = null) {
  const ux = o.ex / o.len, uy = o.ey / o.len
  // WHERE ALONG THE EDGE. The middle unless somebody asks for a place, and the
  // one caller that asks wants the gate -- the middle of the cell boundary,
  // which is the only point the two sides of a seam agree on without talking.
  // An opening cut there needs no dog-leg to reach it, and a corridor with no
  // dog-leg has no junctions to get wrong.
  let mx = (o.a.x.value + o.b.x.value) / 2
  let my = (o.a.y.value + o.b.y.value) / 2
  if (at !== null) {
    // Clamped so the hole stays inside the edge with a whole wall either side.
    const lo = Math.min(o.a.x.value * Math.abs(ux) + o.a.y.value * Math.abs(uy),
                        o.b.x.value * Math.abs(ux) + o.b.y.value * Math.abs(uy))
    const hi = lo + o.len
    const c = Math.min(hi - W / 2, Math.max(lo + W / 2, at))
    if (Math.abs(ux) > 0.5) { mx = c } else { my = c }
  }
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
  const afterA = kids.indexOf(o.a)
  kids.splice(afterA + 1, 0, mk(p1, intoSector), mk(p2, null))
  return { p1, p2 }
}

// ONE RECTANGLE OF CORRIDOR.
//
// Wound counter-clockwise starting at the top-left, so the four edges leave in
// a fixed order -- west, south, east, north -- and a neighbour can be named per
// side rather than per vertex index. A vertex's `neighbor` applies to the edge
// that STARTS at it, which is the same rule the openings above are cut by.
function box (x0, x1, ylo, yhi, { west = null, south = null, east = null, north = null } = {},
              floor = 0, style = null) {
  const s = { kind: 'sector', line: 0, trail: '', attrs: [], children: [], endTrail: '', index: 0 }
  setNum(s, 'floor', round4(floor))
  // `head` lets a caller state the ceiling itself. The outdoors uses it to put
  // one sky over ground of every height, where a corridor wants a ceiling that
  // follows its floor.
  setNum(s, 'ceil', round4(floor + (style?.head ?? headroom)))
  // LIGHT AND FLATS, INHERITED. A sector that names neither is drawn black and
  // untextured, which beside a lit Doom level does not read as "a corridor the
  // tool added" -- it reads as the renderer having failed. There is no
  // brightness a generated corridor could pick for itself that would be right
  // in both a floodlit courtyard and a dark maintenance shaft; the room it
  // leaves already answered that question.
  if (style) {
    setInt(s, 'light', style.light)
    if (style.floortex) setAttr(s, { n: 'floortex', kind: 'name', v: style.floortex })
    if (style.ceiltex) setAttr(s, { n: 'ceiltex', kind: 'name', v: style.ceiltex })
  }
  const pts = [[x0, yhi], [x0, ylo], [x1, ylo], [x1, yhi]]
  const nbs = [west, south, east, north]
  pts.forEach(([x, y], i) => {
    const v = { kind: 'vertex', line: 0, trail: '', x: numFrom(x), y: numFrom(y), attrs: [] }
    if (nbs[i] === null) {
      setAttr(v, { n: 'wall', kind: 'flag' })
      setAttr(v, { n: 'tex', kind: 'name', v: WALL_TEX })
    } else if (nbs[i] === 'open') {
      // THE SEAM. The sector simply ends here -- `open` rather than `wall`,
      // because a wall is a barrier and this is the edge of the cell. A body
      // walks off it and the server hands it to the next level on the same
      // tick.
      setAttr(v, { n: 'open', kind: 'flag' })
    } else {
      setInt(v, 'neighbor', nbs[i])
    }
    s.children.push(v)
  })
  return s
}

// --- the pieces the ground is made of ------------------------------------

