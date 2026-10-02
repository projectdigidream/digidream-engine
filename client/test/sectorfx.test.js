//
// SECTOR EFFECTS, the pure half (client/src/render/sectorfx.js): where
// particles may land, how many each effect gets, and the fog integral the
// shader computes -- held to independent arithmetic (a numeric integral, a
// single box) rather than to itself. Each claim has a control.
//
import * as THREE from 'three'
import {
  triArea, sectorTriangles, samplePoint, inTriangle, bandOf, particleCounts,
  rayPrism, opticalDepth, fogAlpha, SectorFx, KIND, mulberry, BUDGET
} from '../src/render/sectorfx.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-6) => Math.abs(a - b) <= eps

// An L of three triangles (areas 2, 2 and 4): the shape a sector really is,
// non-convex, as mapc bakes it.
const MAP = {
  points: [{ x: 0, y: 0 }, { x: 2, y: 0 }, { x: 2, y: 2 }, { x: 0, y: 2 }, { x: 0, y: 6 }, { x: -2, y: 2 }],
  tris: [0, 1, 2, 0, 2, 3, 3, 4, 5],
  sectors: [{ floorZ: 0, ceilZ: 3, firstTri: 0, triCount: 3, light: 255 }],
  fx: []
}

// --- where particles land ------------------------------------------------------
{
  const st = sectorTriangles(MAP, 0)
  check(near(st.area, 2 + 2 + 4), `the sector's area is its triangles' (${st.area})`)
  const rng = mulberry(7)
  let outside = 0
  let inBig = 0
  const N = 20000
  for (let i = 0; i < N; i++) {
    const p = samplePoint(st, rng(), rng(), rng())
    const where = st.tris.findIndex(([a, b, c]) => inTriangle(p, a, b, c))
    if (where < 0) outside++
    if (where === 2) inBig++
  }
  check(outside === 0, `every sample is inside the sector (${outside} outside of ${N})`)
  check(Math.abs(inBig / N - 0.5) < 0.02, `and spread by area: the triangle of half the area gets half (${(inBig / N).toFixed(3)})`)
  check(!inTriangle({ x: 1.5, y: 3 }, ...st.tris[0]) && !st.tris.some(([a, b, c]) => inTriangle({ x: 1.5, y: 3 }, a, b, c)),
        'CONTROL: a point in the L\'s notch is inside none of them')
  check(triArea({ x: 0, y: 0 }, { x: 0, y: 2 }, { x: 2, y: 0 }) === 2, 'area is unsigned: either winding')
  // A SECOND sector, whose triangles start part-way through the index list:
  // firstTri counts indices, not triangles. Read as triangles it lands on
  // garbage, which is how a snowy room once got no snow at all.
  const two = { ...MAP, sectors: [{ ...MAP.sectors[0], triCount: 2 }, { floorZ: 0, ceilZ: 3, firstTri: 6, triCount: 1 }] }
  const s1 = sectorTriangles(two, 1)
  check(s1.tris.length === 1 && near(s1.area, 4) && s1.tris[0][1] === MAP.points[4],
        `a later sector's triangles are its own (${s1.tris.length}, area ${s1.area})`)
}

// --- the band and the budget ---------------------------------------------------
{
  const s = { floorZ: 1, ceilZ: 5 }
  const b1 = bandOf({ params: { top: -1 } }, s)
  check(b1.lo === 1 && b1.hi === 5, 'no top: floor to ceiling')
  const b2 = bandOf({ params: { bottom: 0.5, top: 2 } }, s)
  check(b2.lo === 1.5 && b2.hi === 3, 'bottom and top are above the floor')
  check(bandOf({ params: { top: 99 } }, s).hi === 5, 'and never above the ceiling')
  const c = particleCounts([30000, 30000, 4000], 32000)
  check(c[0] === 15000 && c[2] === 2000 && c.reduce((a, b) => a + b) <= 32000,
        `over budget, every effect is scaled by the same factor (${c.join(', ')})`)
  check(particleCounts([100, 200], 32000).join() === '100,200', 'CONTROL: under it, nothing changes')
}

// --- the fog integral ----------------------------------------------------------
{
  const a = { x: 0, y: 0 }
  const b = { x: 4, y: 0 }
  const c = { x: 0, y: 4 }
  const o = { x: -1, y: 1, z: 1 }
  const d = { x: 1, y: 0, z: 0 }
  const r = rayPrism(o, d, a, b, c, 0, 2)
  check(r && near(r[0], 1) && near(r[1], 4), `a ray across the prism enters and leaves where it should ([${r?.map((v) => v.toFixed(3))}])`)
  const rr = rayPrism(o, d, a, c, b, 0, 2)
  check(rr && near(rr[0], r[0]) && near(rr[1], r[1]), 'whichever way the triangle is wound')
  const inside = rayPrism({ x: 1, y: 1, z: 1 }, d, a, b, c, 0, 2)
  check(inside && inside[0] === 0 && near(inside[1], 2), 'from inside it, the stretch starts at the eye')
  check(rayPrism({ x: -1, y: 1, z: 3 }, d, a, b, c, 0, 2) === null, 'CONTROL: above the band, it misses')
  check(rayPrism({ x: -1, y: 5, z: 1 }, d, a, b, c, 0, 2) === null, 'CONTROL: past the hypotenuse, it misses')
  const up = rayPrism({ x: 1, y: 1, z: -1 }, { x: 0, y: 0, z: 1 }, a, b, c, 0, 2)
  check(up && near(up[0], 1) && near(up[1], 3), 'a vertical ray crosses the band')

  // Against a numeric integral of the same density, along a sloped ray.
  const D = 0.4
  const fall = 0.7
  const od = { x: 0.6, y: 0, z: 0.8 }
  const oo = { x: 0, y: 0, z: 0.2 }
  let numeric = 0
  const steps = 20000
  for (let i = 0; i < steps; i++) {
    const t = 0.5 + (3 - 0.5) * (i + 0.5) / steps
    numeric += D * Math.pow(0.5, (oo.z + od.z * t - 0) / fall) * (3 - 0.5) / steps
  }
  const closed = opticalDepth(oo, od, 0.5, 3, D, 0, fall)
  check(near(closed, numeric, 1e-6), `the height falloff integral is the numeric one (${closed.toFixed(6)} vs ${numeric.toFixed(6)})`)
  check(near(opticalDepth(oo, { x: 1, y: 0, z: 0 }, 0, 2, D, 0, fall), D * Math.pow(0.5, 0.2 / fall) * 2),
        'a level ray sees the density of its height')
  check(near(opticalDepth(oo, od, 0, 2, D, 0, 0), D * 2), 'falloff 0 is uniform')
  check(!near(closed, D * 2.5, 1e-3), 'CONTROL: and a falloff really thins it')

  // TWO PRISMS ARE ONE VOLUME: the square split along its diagonal, composited
  // in either order, is the box the square makes.
  const sq = [{ x: 0, y: 0 }, { x: 2, y: 0 }, { x: 2, y: 2 }, { x: 0, y: 2 }]
  const ray = { o: { x: -1, y: 0.5, z: 0.5 }, d: { x: 1, y: 0, z: 0 } }
  const parts = [[sq[0], sq[1], sq[2]], [sq[0], sq[2], sq[3]]].map(([p, q, s]) => {
    const seg = rayPrism(ray.o, ray.d, p, q, s, 0, 1)
    return seg ? fogAlpha(opticalDepth(ray.o, ray.d, seg[0], seg[1], D, 0, 0)) : 0
  })
  const combined = 1 - (1 - parts[0]) * (1 - parts[1])
  const box = fogAlpha(D * 2)
  check(parts[0] > 0 && parts[1] > 0 && near(combined, box, 1e-9),
        `two prisms of one fog composite to the whole box's fog (${combined.toFixed(6)} = ${box.toFixed(6)})`)
  check(!near(parts[0] + parts[1], box, 1e-4), 'CONTROL: and simply adding their alphas would not')
  check(fogAlpha(10, 0.6) === 0.6, 'the cap holds')
}

// --- building one map's effects --------------------------------------------------
{
  const map = {
    ...MAP,
    fx: [
      { sector: 0, kind: KIND.RAIN, params: { density: 8, speed: 9, length: 0.35, size: 0.012, color: 0xaab4c8, alpha: 0.35, top: -1 } },
      { sector: 0, kind: KIND.SNOW, params: { density: 4, speed: 0.8, size: 0.04, color: 0xffffff, alpha: 0.9, top: -1 } },
      { sector: 0, kind: KIND.FOG, params: { density: 0.2, color: 0x708090, alpha: 1, top: 1.5, falloff: 0.5 } }
    ]
  }
  const fx = new SectorFx({ THREE, map })
  check(fx.counts[0] === Math.ceil(8 * 8 * 3) && fx.counts[1] === Math.ceil(4 * 8 * 3), `density x area x band (${fx.counts.join(', ')})`)
  check(fx.root.children.length === 2 && fx.fogScene.children.length === 1 && fx.hasFog, 'two particle meshes, one fog')
  const fog = fx.fogScene.children[0].geometry
  check(fog.index.count === 3 * 8 * 3, `three prisms of eight triangles (${fog.index.count / 3})`)
  const rain = fx.root.children[0].geometry
  check(rain.instanceCount === fx.counts[0], 'and one instance a drop')
  const small = new SectorFx({ THREE, map, budget: 100 })
  check(small.counts.reduce((a, b) => a + b) <= 100, 'a small budget is kept')
  check(new SectorFx({ THREE, map: { ...MAP, fx: [] } }).hasFog === false, 'CONTROL: a map with no fog has no fog pass')
  check(BUDGET.touch < BUDGET.desktop, 'a phone gets the smaller budget')
  fx.dispose()
}

console.log(`\nsectorfx: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
