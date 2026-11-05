//
// Composite textures (render/composite.js, docs/COMPOSITE_TEXTURES.md).
//
// The GPU does the drawing and GLSL cannot be run here, so this holds the JS
// twin to its properties -- and the two copies of the hash to each other by
// their constants. What a broken composite looks like is a floor that repeats
// anyway, or one that reshuffles itself between visits; both are quiet, which
// is why they are asserted rather than looked for.
//
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import {
  parseCompositeLine, mix32, hash2, nameSeed, sideSeed, viewSeed, cumulativeWeights,
  uniformsFor, compositeSources, flagsOf, COMPOSITE_GLSL, CP, COMPOSITE_MAX_MEMBERS, atlasColumns
} from '../src/render/composite.js'
import { parseAnimations } from '../src/render/animtex.js'
import { FRAGMENT_SHADER, VERTEX_SHADER } from '../src/render/lighting.js'

const HERE = dirname(fileURLToPath(import.meta.url))

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const line = (s) => parseCompositeLine(s.split(/\s+/))

console.log('composite: one texture name, several pictures')

console.log('\n--- the line ---')
{
  const { def } = line('composite FLOOR1 STONE 4 STONE_CRACKED STONE_MOSS flip turn overlay GRIME scale 8 strength 0.5')
  check(def && def.name === 'FLOOR1' && def.members.map((m) => `${m.name}:${m.weight}`).join(' ') === 'STONE:4 STONE_CRACKED:1 STONE_MOSS:1',
        'members in order, a weight after its texture, 1 when unsaid')
  check(def.flipX && def.flipY && def.turn && !def.shift && !def.patches, "'flip' is both ways, 'turn' quarter turns")
  check(def.overlay?.name === 'GRIME' && def.overlay.scale === 8 && def.overlay.strength === 0.5, 'the overlay, its scale and strength')
  check(def.seed === nameSeed('FLOOR1') && def.seed < 2 ** 24, 'the seed comes from the name, in 24 bits')

  const w = line('composite WALL1 brick 6 BRICK_CRACKED flip x shift').def
  check(w.members[0].name === 'BRICK' && w.flipX && !w.flipY && w.shift && w.seam === 'dither',
        "names are uppercased; 'flip x' is one way; 'shift' dithers its seams unless told")
  check(line('composite W BRICK flip y shift seam blend').def.seam === 'blend', "'seam blend' fades them")
  const y = line('composite YARD GRASS patches size 2.5 seed 7').def
  check(y.patches === 2.5 && y.seed === 7, "'patches size' and 'seed'")
  check(line('composite YARD GRASS patches').def.patches === 3, 'patches are 3 tiles unless told')
  check(line('composite O STONE overlay GRIME').def.overlay.scale === 4 && line('composite O STONE overlay GRIME').def.overlay.strength === 0.5,
        'an overlay is 4 times its size at half strength unless told')

  const bad = [
    ['composite', 'no name'],
    ['composite EMPTY', 'no textures'],
    ['composite EMPTY flip', 'keywords but no textures'],
    [`composite BIG ${Array.from({ length: COMPOSITE_MAX_MEMBERS + 1 }, (_, k) => `T${k}`).join(' ')}`, `more than ${COMPOSITE_MAX_MEMBERS} textures`],
    ['composite C 4 STONE', 'a weight before any texture'],
    ['composite C STONE 0', 'a weight of 0'],
    ['composite C STONE 2 3', 'two weights for one texture'],
    ['composite C STONE seam blend', "'seam' with neither shift nor patches"],
    ['composite C STONE shift seam soft', "a 'seam' that is not dither or blend"],
    ['composite C STONE shift patches', "'shift' and 'patches' together"],
    ['composite C STONE size 3', "'size' not after 'patches'"],
    ['composite C STONE patches size 0', 'a patch of 0 tiles'],
    ['composite C STONE overlay', "'overlay' with no texture"],
    ['composite C STONE overlay 3', "'overlay' followed by a number"],
    ['composite C STONE overlay G strength 2', 'a strength above 1'],
    ['composite C STONE overlay G scale 0', 'an overlay scale of 0'],
    ['composite C STONE scale 2', "'scale' not after an overlay"],
    ['composite C STONE seed x', 'a seed that is not a whole number'],
    ['composite C STONE C', 'itself as one of its textures']
  ]
  for (const [text, why] of bad) check(!!line(text).error, `refused: ${why} (${line(text).error ?? 'accepted'})`)
}

console.log('\n--- in the catalogue ---')
{
  const cat = parseAnimations([
    'anim 4 NUKAGE1 NUKAGE2 NUKAGE3',
    'sheet LAVAFALL vertical 4',
    'sheet TORCH horizontal 4',
    'composite FLOOR1 STONE STONE_CRACKED',
    'composite FLOOR1 STONE STONE_MOSS flip   # a later line wins',
    'composite MIXED STONE TORCH FLOOR1 overlay TORCH',
    'composite ONLYSHEETS TORCH',
    'composite LAVAFALL STONE',
    'composite BROKEN STONE seam blend',
    'something new later clients will know'
  ].join('\n'))
  check(cat.groups.length === 3 && cat.sheets.size === 2, 'anim and sheet lines read as before; composites make no animation')
  check(cat.composites.get('FLOOR1').members.map((m) => m.name).join(' ') === 'STONE STONE_MOSS' && cat.composites.get('FLOOR1').flipX,
        'a later line for the same name wins')
  const mixed = cat.composites.get('MIXED')
  check(mixed && mixed.members.map((m) => m.name).join(' ') === 'STONE' && mixed.overlay === null,
        'a sheet or a composite as a texture, or as the overlay, is left out')
  check(!cat.composites.has('ONLYSHEETS'), 'and with nothing left, there is no composite')
  check(cat.warnings.some((w) => /line 6: MIXED: TORCH is a sheet/.test(w)) && cat.warnings.some((w) => /line 6: MIXED: FLOOR1 is a composite/.test(w)),
        'each said, with its line')
  check(cat.warnings.some((w) => /LAVAFALL is a sheet as well; the composite wins/.test(w)), 'a name that is both is said')
  check(cat.warnings.some((w) => /^line 9: BROKEN:/.test(w)) && !cat.composites.has('BROKEN'), 'a bad line is said and skipped')
  check(!cat.warnings.some((w) => /line 10/.test(w)), 'an unknown first word is still skipped silently, as older clients skip this one')
}

console.log('\n--- the hash ---')
{
  // One hash, written twice: here and in the GLSL. Its constants must match.
  for (const k of ['0x7feb352d', '0x846ca68b', '0x9E3779B1', '0x68E31DA4', '0xB5297A4D', '0x632BE5AB', '0x5EED']) {
    check(COMPOSITE_GLSL.toLowerCase().includes(k.toLowerCase()), `the shader carries ${k}`)
  }
  check(COMPOSITE_GLSL.includes('precision highp int'), 'and asks for 32-bit ints, or the hash is not the hash')
  const golden = ['0x0', '0x688990c0', '0xd1132181', '0xe628c683']
  check([0, 1, 2, 0xdeadbeef].map(mix32).map((x) => '0x' + x.toString(16)).join() === golden.join(),
        'lowbias32 gives the values it always gave')
  check([0, 1, 2, 0xdeadbeef].every((x) => mix32(x) >>> 0 === mix32(x)), 'unsigned 32-bit throughout')
  check(nameSeed('FLOOR1') === nameSeed('FLOOR1') && nameSeed('FLOOR1') !== nameSeed('FLOOR2'), 'names seed apart')
  check(viewSeed({ seed: 5 }, 0) !== viewSeed({ seed: 5 }, 1) && viewSeed({ seed: 5 }, 3) < 2 ** 24,
        "a level's number turns the picks, inside 24 bits")
  const s = sideSeed(0, 0, 10, 0, 2)
  check(s === sideSeed(0, 0, 10, 0, 2) && s !== sideSeed(10, 0, 0, 0, 2) && s !== sideSeed(0, 0, 10, 0, 1),
        'a side seed: the same twice; the reverse side and another field differ')
  check(sideSeed(0, 0, 10, 0, 2) === sideSeed(0.01, 0, 10, 0, 2), 'quantised to 1/16, so a re-bake rounding a hair differently keeps it')
}

const cell = [64, 64]
const params = (text, extra = {}) => uniformsFor(line(text).def, { cell, ...extra })

console.log('\n--- picking members ---')
{
  const cum = cumulativeWeights([{ weight: 4 }, { weight: 1 }, { weight: 1 }])
  check(cum instanceof Float32Array && cum.length === COMPOSITE_MAX_MEMBERS && cum[2] === 1 && cum[0] < cum[1],
        'cumulative, float32 as the GPU holds them, the last exactly 1')
  const p = params('composite F STONE 4 CRACKED MOSS')
  const n = [0, 0, 0]
  const N = 320
  for (let ty = 0; ty < N; ty++) for (let tx = 0; tx < N; tx++) n[compositeSources(p, tx * 64, ty * 64)[0].member]++
  const total = N * N
  check(Math.abs(n[0] / total - 4 / 6) < 0.01 && Math.abs(n[1] / total - 1 / 6) < 0.01 && Math.abs(n[2] / total - 1 / 6) < 0.01,
        `weights 4:1:1 over ${total} tiles: ${n.map((v) => (v / total).toFixed(3)).join(' ')}`)
  const first = compositeSources(p, 3 * 64, 7 * 64)[0]
  check([5, 33, 63].every((dx) => compositeSources(p, 3 * 64 + dx, 7 * 64 + 40)[0].member === first.member),
        'one member for the whole of a tile')
  const plain = params('composite F STONE 4 CRACKED MOSS')
  check(compositeSources(plain, 100, 200)[0].x === 100 - 64 && compositeSources(plain, 100, 200)[0].y === 200 - 192,
        'no flip, turn or shift: each texel is the member\'s own texel')

  const g = params('composite FLOOR1 STONE 4 STONE_CRACKED STONE_MOSS flip turn')
  const row = Array.from({ length: 12 }, (_, t) => { const s = compositeSources(g, t * 64 + 5, 7)[0]; return [s.member, s.x, s.y] })
  const GOLDEN = '[[0,5,56],[1,5,56],[0,7,58],[2,58,7],[0,56,5],[0,5,56],[0,58,56],[0,7,5],[0,7,58],[0,7,5],[0,56,58],[1,56,58]]'
  check(JSON.stringify(row) === GOLDEN, 'the same picks as ever: a floor laid out once stays laid out')
  const reseeded = uniformsFor({ ...line('composite FLOOR1 STONE 4 STONE_CRACKED STONE_MOSS flip turn').def, seed: 1 }, { cell })
  check(JSON.stringify(Array.from({ length: 12 }, (_, t) => { const s = compositeSources(reseeded, t * 64 + 5, 7)[0]; return [s.member, s.x, s.y] })) !== GOLDEN,
        'CONTROL: another seed lays it out differently, so the golden table is a real check')
  check(compositeSources(g, 5, 7, sideSeed(0, 0, 8, 0, 2))[0].x !== undefined &&
        Array.from({ length: 16 }, (_, t) => compositeSources(g, t * 64, 0, 1)[0].member).join() !==
        Array.from({ length: 16 }, (_, t) => compositeSources(g, t * 64, 0, 2)[0].member).join(),
        'two walls with their own seeds lay their tiles out differently')
}

console.log('\n--- flips and turns ---')
{
  const p = params('composite F STONE flip x')
  // A mirrored tile meets its neighbour with the same column of pixels.
  let pairs = 0
  let same = 0
  for (let t = 0; t < 400; t++) {
    const left = compositeSources(p, t * 64 + 63, 0)[0]
    const right = compositeSources(p, (t + 1) * 64, 0)[0]
    const lf = left.x === 0
    const rf = right.x === 63
    if (lf !== rf) { pairs++; if (left.x === right.x) same++ }
  }
  check(pairs > 50 && same === pairs, `where only one of two tiles is mirrored, the same column meets on both sides (${same}/${pairs})`)
  check(compositeSources(p, 0, 0)[0].y === 0 && compositeSources(p, 0, 63)[0].y === 63, "'flip x' never mirrors up and down")
  const t = params('composite F STONE turn')
  let bijective = true
  for (let tile = 0; tile < 8; tile++) {
    const seen = new Set()
    for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) {
      const s = compositeSources(t, tile * 64 + x, y)[0]
      seen.add(s.y * 64 + s.x)
    }
    if (seen.size !== 64 * 64) bijective = false
  }
  check(bijective, 'a turned tile shows every pixel of its member exactly once')
  const kinds = new Set(Array.from({ length: 64 }, (_, k) => { const s = compositeSources(t, k * 64, 0)[0]; return `${s.x},${s.y}` }))
  check(kinds.size === 4, `all four quarter turns occur (${[...kinds].join(' ')})`)
  const rect = uniformsFor(line('composite F STONE turn').def, { cell: [64, 32] })
  check(!(rect.flags & CP.TURN) && compositeSources(rect, 10, 5)[0].x === 10, 'a texture that is not square is not turned')
}

console.log('\n--- shift (Quilez 1) ---')
{
  const p = params('composite W BRICK shift')
  const one = compositeSources(p, 200, 300)
  check(one.length === 1 && Number.isInteger(one[0].x) && one[0].x >= 0 && one[0].x < 64 && one[0].y < 64,
        'dithered: one source, a whole texel inside the member')
  const inner = compositeSources(p, 5 * 64 + 6, 64 + 6)[0]
  const own = compositeSources(params('composite W BRICK shift'), 5 * 64 + 7, 64 + 7)[0]
  check(inner.member === own.member && inner.x - own.x === -1 && inner.y - own.y === -1,
        "near a tile's top-left, its own copy, every texel the next one along")
  const sources = new Set()
  for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) {
    const s = compositeSources(p, 640 + x, 640 + y)[0]
    sources.add(`${(s.x - x + 640 + 6400) % 64},${(s.y - y + 640 + 6400) % 64}`)
  }
  check(sources.size >= 2 && sources.size <= 4, `a tile is drawn from its own copy and its neighbours': ${sources.size} offsets`)
  const b = params('composite W BRICK shift seam blend')
  const mixed = compositeSources(b, 640 + 50, 640 + 50)
  check(mixed.length === 4 && Math.abs(mixed.reduce((a, s) => a + s.w, 0) - 1) < 1e-6, 'blended: four copies, weights summing to 1')
  check(compositeSources(b, 640 + 2, 640 + 2).filter((s) => s.w > 0).length === 1, 'and only its own copy away from the seams')
}

console.log('\n--- patches ---')
{
  const p = params('composite Y GRASS patches size 3')
  // Along a row: runs of one copy. The pure ones are the patches; the short
  // ones are the dithered border between two.
  const copyAt = (x, y) => { const s = compositeSources(p, x, y)[0]; return `${s.member}:${(s.x - x % 64 + 64) % 64},${(s.y - y % 64 + 64) % 64}` }
  const runs = []
  let prev = null
  let len = 0
  for (let x = 0; x < 64 * 300; x++) {
    const k = copyAt(x, 777)
    if (k === prev) { len++; continue }
    if (prev !== null) runs.push(len)
    prev = k; len = 1
  }
  const pure = runs.filter((r) => r >= 12)
  const mean = pure.reduce((a, r) => a + r, 0) / pure.length
  const border = runs.filter((r) => r < 12).reduce((a, r) => a + r, 0) / (64 * 300)
  check(mean > 96, `a patch is wider than a tile: ${mean.toFixed(0)} texels on average (a tile is 64, size 3 is 192)`)
  check(border < 0.15, `and the dithered borders are a small part of it: ${(border * 100).toFixed(1)}%`)
  const big = params('composite Y GRASS patches size 6')
  let changes = 0
  let last = null
  for (let x = 0; x < 64 * 300; x += 16) { const s = compositeSources(big, x, 777)[0]; const k = `${s.member}:${(s.x - x % 64 + 64) % 64}`; if (k !== last) changes++; last = k }
  let changes3 = 0
  last = null
  for (let x = 0; x < 64 * 300; x += 16) { const k = copyAt(x, 777); if (k !== last) changes3++; last = k }
  check(changes < changes3, `'size' makes them bigger: ${changes} changes at size 6 against ${changes3} at 3`)
  check(compositeSources(p, 10, 10).length === 1, 'dithered: one source')
  const bl = compositeSources(params('composite Y GRASS patches seam blend'), 1000, 1000)
  check(bl.length === 2 && Math.abs(bl[0].w + bl[1].w - 1) < 1e-6, 'blended: two copies, weights summing to 1')
}

console.log('\n--- the shaders ---')
{
  // The plain map shader is the old one once COMPOSITE is not defined.
  const plain = (src) => src.replace(/#ifdef COMPOSITE[\s\S]*?(#else([\s\S]*?))?#endif/g, (_, __, keep) => keep ?? '')
  const p = plain(FRAGMENT_SHADER)
  check(!/cp[A-Z]|compositeTexel|vTileSeed/.test(p) && (p.match(/texture2D\(map, vUv \* repeat\)/g) ?? []).length === 1,
        'without COMPOSITE the fragment shader samples map once, as it always did')
  check(!/tileSeed/.test(plain(VERTEX_SHADER)), 'and the vertex shader reads no seed')
  check(/compositeTexel\(vUv \* repeat, vTileSeed\)/.test(FRAGMENT_SHADER) && /flat varying float vTileSeed/.test(VERTEX_SHADER),
        'with it, the composite path, with the wall\'s seed passed flat')
  const fx = readFileSync(join(HERE, '..', 'src', 'render', 'sectorfx.js'), 'utf8')
  check(/compositeTexel\(vUv \* repeat, 0\.0\)/.test(fx), "a fill's top draws a composite ceiling picture too")
  check(flagsOf(line('composite F A flip x overlay O').def) === (CP.FLIP_X | CP.OVERLAY), 'flags are one bit each')
  check(atlasColumns(1) === 1 && atlasColumns(4) === 2 && atlasColumns(5) === 3 && atlasColumns(8) === 3, 'the atlas as square as it gets')
  check(hash2(1, 2, 3) !== hash2(2, 1, 3), 'the tile hash is not symmetric')
}

console.log(`\ncomposite: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
