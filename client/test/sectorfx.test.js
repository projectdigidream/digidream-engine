//
// SECTOR EFFECTS, the pure half (client/src/render/sectorfx.js): where
// particles may land, how many each effect gets, and the fog integral the
// shader computes -- held to independent arithmetic (a numeric integral, a
// single box) rather than to itself. Each claim has a control.
//
import * as THREE from 'three'
import {
  triArea, sectorTriangles, samplePoint, inTriangle, bandOf, particleCounts,
  rayPrism, opticalDepth, fogAlpha, SectorFx, KIND, mulberry, BUDGET, fillShell, inFill
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

// --- a fill's shell: outward, one face per outline edge --------------------------
{
  // MAP's sector, one triangle wound the other way, as a bake may hand it over.
  const st = sectorTriangles(MAP, 0)
  const tris = st.tris.map(([a, b, c], i) => (i === 1 ? [a, c, b] : [a, b, c]))
  const shell = fillShell(tris)
  const signed = ([a, b, c]) => (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)
  check(signed(tris[1]) < 0 && shell.tris.every((t) => signed(t) > 0), 'every top triangle faces up, however it came')
  // The square's diagonal is shared; the triangle on top touches it at a corner.
  check(shell.outline.length === 7, `the outline is the edges one triangle has (${shell.outline.length})`)
  const isDiagonal = ([p, q]) => (p === MAP.points[0] && q === MAP.points[2]) || (p === MAP.points[2] && q === MAP.points[0])
  check(!shell.outline.some(isDiagonal), 'CONTROL: and the shared diagonal is not in it')
  // Outward, by the shape itself: just right of each edge is outside, just left inside.
  const inside = (p) => tris.some(([a, b, c]) => inTriangle(p, a, b, c))
  const sideOf = ([p, q], k) => {
    const dx = q.x - p.x
    const dy = q.y - p.y
    const n = Math.hypot(dx, dy)
    return { x: (p.x + q.x) / 2 + k * dy / n, y: (p.y + q.y) / 2 - k * dx / n }
  }
  check(shell.outline.every((e) => !inside(sideOf(e, 1e-3)) && inside(sideOf(e, -1e-3))), 'each outline edge has the volume on its left')
  check(!shell.outline.every((e) => !inside(sideOf([e[1], e[0]], 1e-3))), 'CONTROL: reversed, they would not')
  check(inFill({ x: 1, y: 0.5, z: 1 }, shell.tris, 0, 2) && !inFill({ x: 1, y: 0.5, z: 2.5 }, shell.tris, 0, 2),
        'inside the band and the footprint is in; above the band is not')
  check(!inFill({ x: 3, y: 0.5, z: 1 }, shell.tris, 0, 2), 'CONTROL: and beside the footprint is not')
}

// --- a fill, built: its top shares the map's picture; the eye inside tints -------
{
  const map = { ...MAP, sectors: [{ ...MAP.sectors[0], effect: true, ceilTex: 0, ceilStyle: { alpha: 0.5 } }],
                fx: [{ sector: 0, kind: KIND.FILL, params: { color: 0x3a6a8a, alpha: 0.4, top: 2, surface: 0.4 } }] }
  const pic = { map: { value: new THREE.Texture() }, repeat: { value: new THREE.Vector2(0.25, 0.25) } }
  const fx = new SectorFx({ THREE, map, surfaces: (i) => (i === 0 ? pic : null) })
  const [top, body, tint] = fx.root.children
  check(top.name === 'fill:top:0' && body.name === 'fill:body:0' && tint === fx.tint, 'a fill: its top, its body, and one tint')
  check(top.material.uniforms.map === pic.map && top.material.uniforms.repeat === pic.repeat,
        "the top shares the map's own uniforms, so it shows the picture as it loads and animates")
  check(body.material.uniforms.textured.value === 0 && near(top.material.uniforms.surface.value, 0.4 * 0.5),
        "CONTROL: the body has no picture; the top's opacity is its `surface` times the ceiling's alpha")
  check(top.renderOrder > -1 && top.renderOrder < 0 && body.renderOrder === top.renderOrder,
        'drawn after every sprite (-1, which writes depth) and before rain and effects (0)')
  check(top.material.side === THREE.DoubleSide && body.material.side === THREE.FrontSide, 'the top both ways, the rest outward only')
  check(body.geometry.attributes.position.count === 7 * 6 + 3 * 3, 'seven sides of two triangles, and a bottom of three')
  check(!fx.hasFog, 'a fill takes no fog pass')
  const solid = new SectorFx({ THREE, map: { ...map, sectors: [{ ...map.sectors[0], effect: false }] }, surfaces: () => pic })
  check(solid.root.children[0].material.uniforms.textured.value === 0, "CONTROL: a solid room's fill has a plain top -- its ceiltex is its ceiling")
  solid.dispose()
  fx.update(0, new THREE.Vector3(1, 1, -0.5))                   // sim (1, 0.5, 1): in it
  check(tint.visible && near(tint.material.uniforms.alpha.value, 0.4), 'the eye in the fill tints the view, by its alpha')
  fx.update(0, new THREE.Vector3(1, 2.5, -0.5))                 // above its top
  check(!tint.visible, 'CONTROL: above it, no tint')
  fx.dispose()
}

// --- the fog pass, made cheaper ------------------------------------------------
{
  const map = { ...MAP, fx: [{ sector: 0, kind: KIND.FOG, params: { density: 0.2, color: 0x708090, alpha: 1, top: 1.5 } }] }
  const fx = new SectorFx({ THREE, map })
  const box = fx.fogScene.children[0].geometry.boundingBox
  const mid = box.getCenter(new THREE.Vector3())
  const cam = new THREE.PerspectiveCamera(50, 1, 0.1, 500)
  cam.position.set(mid.x + 20, mid.y + 20, mid.z + 20)
  cam.lookAt(mid)
  check(fx.fogInView(cam), 'NONE IN VIEW, NO PASS: a camera looking at the fog sees it')
  cam.lookAt(mid.x + 60, mid.y + 60, mid.z + 60)
  check(!fx.fogInView(cam), 'and one looking away does not -- the whole pass is skipped')
  cam.position.copy(mid)
  cam.lookAt(mid.x + 50, mid.y, mid.z)
  check(fx.fogInView(cam), 'standing in it, it is in view whichever way you look')

  // The depth pass's scene: opaque surfaces on one cheap material, cut-outs on
  // their own, and what writes no depth left out -- then all put back.
  const scene = new THREE.Scene()
  const opaque = new THREE.Mesh(new THREE.PlaneGeometry(), new THREE.MeshLambertMaterial({ side: THREE.DoubleSide }))
  const cutout = new THREE.Mesh(new THREE.PlaneGeometry(), new THREE.MeshBasicMaterial({ alphaTest: 0.5, transparent: true }))
  const particles = new THREE.Mesh(new THREE.PlaneGeometry(), new THREE.MeshBasicMaterial({ depthWrite: false }))
  const line = new THREE.Line(new THREE.BufferGeometry(), new THREE.LineBasicMaterial())
  scene.add(opaque, cutout, particles, line)
  const was = [opaque.material, cutout.material]
  const undo = fx.leanDepthScene(scene)
  check(opaque.material !== was[0] && opaque.material.colorWrite === false && opaque.material.side === THREE.DoubleSide,
        'an opaque surface draws its depth with a cheap material, on the same side(s)')
  check(cutout.material === was[1], "a cut-out keeps its own, so the fog shows through a sprite's holes")
  check(!particles.visible && !line.visible, 'what writes no depth (particles) and lines are not drawn')
  undo()
  check(opaque.material === was[0] && particles.visible && line.visible, 'and everything is put back')
  fx.dispose()
}

console.log(`\nsectorfx: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
