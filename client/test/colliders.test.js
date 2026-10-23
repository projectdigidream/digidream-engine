// THE COLLIDER VIEW (render/colliders.js): one cylinder per body, the body's
// own size and place, its colour by what it is, pooled across frames.
import { ColliderView, cylinderEdges } from '../src/render/colliders.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; if (!ok) { fails++; console.log(`  FAIL ${label}`) } else console.log(`  ok   ${label}`) }

const v3 = () => ({ x: 0, y: 0, z: 0, set (x, y, z) { this.x = x; this.y = y; this.z = z } })
let lines = 0
const THREE = {
  Group: class { constructor () { this.children = []; this.visible = true } add (c) { this.children.push(c) } },
  BufferGeometry: class { setAttribute () {} },
  BufferAttribute: class {},
  LineBasicMaterial: class { constructor (o) { Object.assign(this, o) } },
  LineSegments: class { constructor (g, m) { lines++; this.geometry = g; this.material = m; this.position = v3(); this.scale = v3(); this.visible = true } }
}
const scene = { add () {} }
const view = new ColliderView({ THREE, scene })

const e = cylinderEdges(8)
check(e.length === (8 * 2 + 4) * 6, `the unit cylinder is two rings and four uprights (${e.length / 6} segments)`)
check(Math.max(...e.filter((_, i) => i % 3 === 1)) === 1 && Math.min(...e.filter((_, i) => i % 3 === 1)) === 0, 'feet at 0, head at 1')

view.update([{ x: 1, y: 2, z: 3, radius: 0.5, height: 2, kind: 'player' }])
check(lines === 0, 'CONTROL: hidden, nothing is built')
view.setShown(true)
const bodies = [
  { x: 1, y: 2, z: 3, radius: 0.5, height: 2, kind: 'player' },
  { x: 4, y: 0, z: -1, radius: 0.3, height: 0.9, kind: 'ghost' }
]
view.update(bodies)
const [a, b] = view.group.children
check(view.group.children.length === 2 && a.position.z === 3 && a.scale.x === 0.5 && a.scale.y === 2, 'each body its own place and size')
check(a.material.color === 0x4cff6a && b.material.color === 0x9aa4b8 && a.material.depthTest === false, 'coloured by what it is, drawn over everything')
view.update(bodies.slice(0, 1))
check(lines === 2 && !b.visible && a.visible, 'a body gone hides its line, and the pool is kept')

console.log(`\ncolliders: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
