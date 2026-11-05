//
// Composite textures ON THE GPU (render/composite.js).
//
// composite.test.js holds the JS twin to its properties; this holds the GLSL
// to the JS twin. A real MapView, built from a hand-made room and loaded
// through the real loader, draws composite floors and a wall in headless
// Chrome. Each member picture's pixels ENCODE where they are -- red is x,
// green is y, blue is which picture -- so every pixel read back says exactly
// which member texel the shader chose, and it is compared with what
// compositeSources says it should have chosen.
//
// Opt-in like the other page tests (DNP_CLIENT_PAGE_TEST=1): it launches a
// headless browser. It needs no stack: a small server here serves the client's
// sources, three.js and the pictures this test writes.
//
import { createServer } from 'node:http'
import { spawn } from 'node:child_process'
import { existsSync, mkdtempSync, readFileSync, writeFileSync, mkdirSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, normalize, extname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { deflateSync, crc32 } from 'node:zlib'

const HERE = dirname(fileURLToPath(import.meta.url))
const CLIENT = join(HERE, '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const done = () => {
  console.log(`\ncompositepage: ${checks} checks, ${fails} failures`)
  process.exit(fails === 0 ? 0 : 1)
}

console.log('compositepage: the shader picks what the JS twin picks')
if (!process.env.DNP_CLIENT_PAGE_TEST) {
  console.log('  skip (set DNP_CLIENT_PAGE_TEST=1 -- it launches a headless browser)')
  done()
}
const chrome = [
  process.env.DNP_CHROME,
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/Applications/Chromium.app/Contents/MacOS/Chromium',
  '/usr/bin/google-chrome',
  '/usr/bin/chromium',
  '/usr/bin/chromium-browser'
].filter(Boolean).find((p) => existsSync(p))
if (!chrome) {
  console.log('  skip (no Chrome; set DNP_CHROME to run it)')
  done()
}

// --- the pictures -------------------------------------------------------------

function png (w, h, rgba) {
  const raw = Buffer.alloc((w * 4 + 1) * h)
  for (let y = 0; y < h; y++) rgba.copy(raw, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4)
  const chunk = (type, data) => {
    const len = Buffer.alloc(4)
    len.writeUInt32BE(data.length)
    const body = Buffer.concat([Buffer.from(type), data])
    const crc = Buffer.alloc(4)
    crc.writeUInt32BE(crc32(body) >>> 0)
    return Buffer.concat([len, body, crc])
  }
  const ihdr = Buffer.alloc(13)
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4)
  ihdr[8] = 8; ihdr[9] = 6
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))])
}
const CELL = 16
const PICS = ['PA', 'PB', 'PC']
const dir = mkdtempSync(join(tmpdir(), 'dnp-composite-'))
mkdirSync(join(dir, 'tex'))
process.on('exit', () => rmSync(dir, { recursive: true, force: true }))
const sizes = {}
PICS.forEach((name, id) => {
  const b = Buffer.alloc(CELL * CELL * 4)
  for (let y = 0; y < CELL; y++) for (let x = 0; x < CELL; x++) {
    const i = (y * CELL + x) * 4
    b[i] = x * 16 + 8; b[i + 1] = y * 16 + 8; b[i + 2] = id * 60 + 20; b[i + 3] = 255
  }
  writeFileSync(join(dir, 'tex', `${name}.png`), png(CELL, CELL, b))
  sizes[name] = [CELL, CELL]
})
for (const [name, v] of [['GREY50', 128], ['WHITE', 255]]) {
  writeFileSync(join(dir, 'tex', `${name}.png`), png(8, 8, Buffer.alloc(8 * 8 * 4, v)))
  sizes[name] = [8, 8]
}
writeFileSync(join(dir, 'tex', 'index.json'), JSON.stringify({ sizes }))

const CATALOGUE = [
  'composite HARD PA 3 PB PC flip turn',
  'composite SHIFTD PA PB flip x shift',
  'composite SHIFTB PA PB shift seam blend',
  'composite PATCHD PA PB PC patches size 2',
  'composite PATCHB PA PB PC patches size 2 seam blend',
  'composite GREY PA PB overlay GREY50 strength 1',
  'composite BRIGHT PA PB overlay WHITE strength 0.5',
  'composite WALLC PA PB PC flip'
].join('\n')

// --- the page -------------------------------------------------------------------

const PAGE = `<!doctype html><meta charset="utf-8">
<script type="importmap">{ "imports": { "three": "/vendor/three/three.module.js" } }</script>
<script type="module">
import * as THREE from 'three'
import { MapView } from '/src/render/scene.js'
import { parseAnimations } from '/src/render/animtex.js'
import { compositeSources, uniformsFor } from '/src/render/composite.js'
import { buildSectorSurfaces, SURFACE } from '/src/mapgeom.js'
import { NO_TEXTURE, NO_SECTOR } from '/src/mapdata.js'
const errors = []
const err0 = console.error
console.error = (...a) => { errors.push(a.map(String).join(' ').slice(0, 400)); err0(...a) }
window.addEventListener('error', (e) => errors.push(String(e.message)))
try {
  const cat = parseAnimations(${JSON.stringify(CATALOGUE)})
  MapView.composites = cat.composites
  const S = 16, PPU = 32, R = 1024, PICS = ${JSON.stringify(PICS)}
  const renderer = new THREE.WebGLRenderer({ antialias: false })
  renderer.setPixelRatio(1)
  renderer.setSize(64, 64)
  const target = new THREE.WebGLRenderTarget(R, R)
  const room = (tex) => {
    const side = (a, b) => ({ a, b, neighbor: NO_SECTOR, flags: 0, upper: NO_TEXTURE, middle: 1, lower: NO_TEXTURE, peg: 0, xOffset: 0, yOffset: 0 })
    return {
      points: [{ x: 0, y: 0 }, { x: S, y: 0 }, { x: S, y: S }, { x: 0, y: S }],
      sectors: [{ floorZ: 0, ceilZ: 8, firstPoint: 0, pointCount: 4, firstSide: 0, sideCount: 4, tag: 0,
                  firstTri: 0, triCount: 2, floorTex: 1, ceilTex: NO_TEXTURE, light: 255 }],
      sides: [side(0, 1), side(1, 2), side(2, 3), side(3, 0)],
      tris: Uint16Array.from([3, 0, 1, 1, 2, 3]),
      textures: ['', tex], lights: []
    }
  }
  const color = (name, x, y) => [x * 16 + 8, y * 16 + 8, PICS.indexOf(name) * 60 + 20]
  async function shoot (tex, { wall = false, level = 0, predictSeed = null } = {}) {
    const map = room(tex)
    const view = new MapView(map)
    await view.loadTextures({ baseUrl: '/tex', pxPerUnit: PPU, composites: cat.composites, compositeSeed: level })
    const scene = new THREE.Scene()
    scene.add(view.root)
    let cam
    if (!wall) {
      cam = new THREE.OrthographicCamera(-S / 2, S / 2, S / 2, -S / 2, 0.1, 100)
      cam.position.set(S / 2, 50, -S / 2); cam.up.set(0, 0, -1); cam.lookAt(S / 2, 0, -S / 2)
    } else {
      cam = new THREE.OrthographicCamera(-S / 2, S / 2, 4, -4, 0.1, 100)
      cam.position.set(S / 2, 4, -S / 2); cam.lookAt(S / 2, 4, 0)
    }
    cam.updateMatrixWorld(); cam.updateProjectionMatrix()
    renderer.setRenderTarget(target)
    renderer.setClearColor(0x000000, 1)
    renderer.render(scene, cam)
    const buf = new Uint8Array(R * R * 4)
    renderer.readRenderTargetPixels(target, 0, 0, R, R, buf)
    renderer.setRenderTarget(null)
    const def = cat.composites.get(tex)
    const p = uniformsFor(def, { cell: [${CELL}, ${CELL}], level })
    const seed = predictSeed ?? (wall ? buildSectorSurfaces(map, 0).find((s) => s.kind === SURFACE.MIDDLE && s.side === 0).seed : 0)
    const v = new THREE.Vector3()
    let n = 0, wrong = 0, worst = 0, sample = null
    for (let j = 1; j < R; j += 4) {
      for (let i = 1; i < R; i += 4) {
        v.set((i + 0.5) / R * 2 - 1, (j + 0.5) / R * 2 - 1, 0).unproject(cam)
        let gx, gy
        if (!wall) { gx = Math.floor(v.x * PPU); gy = Math.floor(v.z * PPU) }        // u = x, v = -y = three z
        else { gx = Math.floor((S - v.x) * PPU); gy = Math.floor((8 - v.y) * PPU) } // u from b, v down from the ceiling
        const k = (j * R + i) * 4
        const got = [buf[k], buf[k + 1], buf[k + 2]]
        const want = [0, 0, 0]
        for (const s of compositeSources(p, gx, gy, seed)) {
          const c = color(def.members[s.member].name, s.x, s.y)
          for (let q = 0; q < 3; q++) want[q] += s.w * c[q]
        }
        const d = Math.max(...got.map((g, q) => Math.abs(g - want[q])))
        n++
        worst = Math.max(worst, d)
        if (d > 2) { wrong++; if (!sample) sample = { gx, gy, got, want: want.map(Math.round) } }
      }
    }
    let a = 0
    let h = 2166136261
    for (let k = 0; k < buf.length; k++) { if (k % 4 === 0) a += buf[k]; h = Math.imul(h ^ buf[k], 16777619) >>> 0 }
    return { n, wrong, worst, sample, mean: a / (buf.length / 4), sum: h }
  }
  const out = {}
  for (const t of ['HARD', 'SHIFTD', 'SHIFTB', 'PATCHD', 'PATCHB', 'GREY', 'BRIGHT']) out[t] = await shoot(t)
  out.HARD1 = await shoot('HARD', { level: 1 })
  out.WALLC = await shoot('WALLC', { wall: true })
  // CONTROL: the wall predicted as if it had no seed of its own must NOT
  // match, or the check above proves nothing about the seed.
  out.WALLC0 = await shoot('WALLC', { wall: true, predictSeed: 0 })
  window.__result = { out, errors }
} catch (e) {
  window.__result = { error: String(e && e.stack || e), errors }
}
</script>`
writeFileSync(join(dir, 'page.html'), PAGE)

// --- the server -------------------------------------------------------------------

const TYPES = { '.js': 'text/javascript', '.html': 'text/html', '.json': 'application/json', '.png': 'image/png' }
const roots = [
  ['/src/', join(CLIENT, 'src')],
  ['/vendor/three/', join(CLIENT, 'node_modules', 'three', 'build')],
  ['/tex/', join(dir, 'tex')],
  ['/', dir]
]
const server = createServer((req, res) => {
  const url = decodeURIComponent(req.url.split('?')[0])
  for (const [prefix, root] of roots) {
    if (!url.startsWith(prefix)) continue
    const file = normalize(join(root, url.slice(prefix.length)))
    if (!file.startsWith(root) || !existsSync(file)) continue
    res.writeHead(200, { 'content-type': TYPES[extname(file)] ?? 'application/octet-stream' })
    res.end(readFileSync(file))
    return
  }
  res.writeHead(404); res.end()
})
await new Promise((r) => server.listen(0, '127.0.0.1', r))
const PORT = server.address().port

// --- the browser ------------------------------------------------------------------

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const debugPort = 9950 + Math.floor(Math.random() * 40)
const browser = spawn(chrome, [
  '--headless=new', '--mute-audio', '--no-sandbox', '--disable-dev-shm-usage',
  `--user-data-dir=${join(dir, 'profile')}`, `--remote-debugging-port=${debugPort}`,
  // SwiftShader: no GPU here, and its float and integer arithmetic is IEEE,
  // which is what the twin is held to.
  '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
  'about:blank'
], { stdio: 'ignore' })
process.on('exit', () => { try { browser.kill('SIGKILL') } catch {} })

let result = null
try {
  let targets = null
  for (let i = 0; i < 100 && !targets; i++) {
    targets = await fetch(`http://127.0.0.1:${debugPort}/json`).then((r) => r.json()).catch(() => null)
    if (!targets) await sleep(100)
  }
  const page = targets?.find((t) => t.type === 'page')
  if (!page) throw new Error('no page target')
  const ws = new WebSocket(page.webSocketDebuggerUrl)
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j })
  let id = 0
  const pending = new Map()
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id) } }
  const send = (method, params = {}) => new Promise((r) => { const k = ++id; pending.set(k, r); ws.send(JSON.stringify({ id: k, method, params })) })
  await send('Page.navigate', { url: `http://127.0.0.1:${PORT}/page.html` })
  for (let i = 0; i < 1200 && !result; i++) {
    const r = await send('Runtime.evaluate', { expression: 'window.__result ? JSON.stringify(window.__result) : null', returnByValue: true })
    if (r.result?.result?.value) result = JSON.parse(r.result.result.value)
    else await sleep(100)
  }
  ws.close()
} catch (e) {
  result = { error: String(e?.message ?? e), errors: [] }
}
server.close()

if (!result || result.error) {
  check(false, `the page ran (${result?.error ?? 'no result in 120 s'})`)
  for (const e of result?.errors ?? []) console.log(`    ${e}`)
  done()
}
const o = result.out
check(result.errors.length === 0, `no shader or page errors${result.errors.length ? ': ' + result.errors[0] : ''}`)
const say = (r) => `${r.wrong} of ${r.n} pixels off${r.sample ? ` (first at ${r.sample.gx},${r.sample.gy}: ${r.sample.got} for ${r.sample.want})` : ''}`
check(o.HARD.wrong === 0, `variants, flips and turns: every pixel the member texel the twin picks -- ${say(o.HARD)}`)
check(o.SHIFTD.wrong === 0, `shift, dithered seams -- ${say(o.SHIFTD)}`)
check(o.SHIFTB.wrong <= o.SHIFTB.n * 0.001, `shift, blended seams, within 2 of the twin's mix -- ${say(o.SHIFTB)}`)
check(o.PATCHD.wrong <= o.PATCHD.n * 0.002, `patches, dithered (a square root may round a hair differently) -- ${say(o.PATCHD)}`)
check(o.PATCHB.wrong <= o.PATCHB.n * 0.002, `patches, blended -- ${say(o.PATCHB)}`)
check(o.GREY.wrong === 0, `a mid-grey overlay changes nothing -- ${say(o.GREY)}`)
check(o.BRIGHT.wrong > o.BRIGHT.n * 0.9 && o.BRIGHT.mean > o.GREY.mean * 1.3,
      `a white one at half strength brightens by half (mean red ${o.GREY.mean.toFixed(0)} -> ${o.BRIGHT.mean.toFixed(0)})`)
check(o.HARD1.wrong === 0 && o.HARD1.sum !== o.HARD.sum, `another level's number lays it out differently, as the twin says -- ${say(o.HARD1)}`)
check(o.WALLC.wrong === 0, `a wall, with its own seed passed flat to every pixel -- ${say(o.WALLC)}`)
check(o.WALLC0.wrong > o.WALLC0.n * 0.2, `CONTROL: predicted without its seed, the wall does not match (${o.WALLC0.wrong} of ${o.WALLC0.n} off)`)
done()
