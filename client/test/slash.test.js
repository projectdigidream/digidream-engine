//
// The swing as the game shows it (render/slash.js): the module's look, read and
// resolved; the streak's geometry along the real sweep; pieces laid tick by
// tick and faded.
//
import * as THREE from 'three'
import { parseSwingStyles, styleFor, slashBand, slashPiece, SlashView, DEFAULT_SLASH } from '../src/render/slash.js'
import { aimSwing } from '../src/render/swing.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

console.log('slash: the swing as the game shows it')

// --- the module's look -------------------------------------------------------
{
  const p = parseSwingStyles(JSON.stringify({
    version: 1,
    default: { color: '#ff0000', fade: 0.5 },
    weapons: { chainsaw: { color: '#00ff00', blend: 'normal', texture: 'fx/saw.png' } },
    monsters: { demon: { show: false } }
  }))
  check(p.ok && p.errors.length === 0, `a good file reads clean (${p.errors.join('; ')})`)
  const d = styleFor(p.styles, {})
  check(d.color === '#ff0000' && d.fade === 0.5 && d.width === DEFAULT_SLASH.width, 'the default: the file\'s fields over the engine\'s')
  const saw = styleFor(p.styles, { weapon: 'chainsaw' })
  check(saw.color === '#00ff00' && saw.blend === 'normal' && saw.fade === 0.5 && saw.texture === 'fx/saw.png', 'a weapon\'s: its fields over the default')
  check(styleFor(p.styles, { monster: 'demon' }).show === false, 'a monster\'s, by its catalogue name')
  check(styleFor(p.styles, { weapon: 'fist' }).color === '#ff0000', 'CONTROL: a weapon with no entry gets the default')
  const bad = parseSwingStyles(JSON.stringify({ default: { colour: '#fff', opacity: 3, texture: '../x.png' }, weapons: { fist: { blend: 'screen' } }, extra: 1 }))
  check(!bad.ok && bad.errors.length === 5, `mistakes are said, each (${bad.errors.length}: ${bad.errors.join(' | ')})`)
  check(styleFor(bad.styles, { weapon: 'fist' }).blend === DEFAULT_SLASH.blend && styleFor(bad.styles, {}).opacity === DEFAULT_SLASH.opacity,
        'and left out: the rest is used')
  check(!parseSwingStyles('{ nope').ok, 'not JSON is said')
}

// --- the band and its geometry ------------------------------------------------
{
  const shape = { reach: 2, inner: 0.5, halfArc: 0.05, from: -0.125, to: 0.125, pitchFrom: 0, pitchTo: 0, halfPitch: 0, ticks: 16 }
  const band = slashBand(shape, 0.4)
  check(Math.abs(band.outer - 2) < 1e-9 && Math.abs(band.inner - 1.2) < 1e-9, `width 0.4: the outer 40% of the reach (${band.inner}..${band.outer})`)
  check(slashBand(shape, 1).inner === 0.5, 'width 1: down to the swing\'s own inner reach, not past it')
  // A piece at offsets 0.05..0.1 must sit where aimSwing puts the blade at
  // those offsets: rotate (r, 0, 0) by the offset and compare.
  const piece = slashPiece({ inner: 1, outer: 2, from: 0.05, to: 0.1, steps: 2, uOf: (o) => o })
  const blade = (r, o) => {
    const m = new THREE.Object3D()
    aimSwing(m, 0, o)
    m.updateMatrixWorld(true)
    return new THREE.Vector3(r, 0, 0).applyMatrix4(m.matrixWorld)
  }
  const at = (i) => new THREE.Vector3(piece.pos[i * 3], piece.pos[i * 3 + 1], piece.pos[i * 3 + 2])
  const near = (u, v) => u.distanceTo(v) < 1e-9
  check(near(at(0), blade(1, 0.05)) && near(at(1), blade(2, 0.05)), 'the piece starts where the blade was (inner and tip)')
  check(near(at(4), blade(1, 0.1)) && near(at(5), blade(2, 0.1)), 'and ends where it is')
  check(!near(at(4), blade(1, -0.1)), 'CONTROL: not mirrored -- the other side of the facing is somewhere else')
  check(piece.idx.length === 2 * 2 * 3 && piece.uv[0] === 0.05 && piece.uv[1] === 0 && piece.uv[3] === 1, 'two quads, u along the sweep, v across the band')
}

// --- pieces laid tick by tick, and faded -----------------------------------------
{
  const scene = new THREE.Scene()
  const view = new SlashView({ THREE, scene, loader: { load: (url) => ({ url }) } })
  const shape = { reach: 2, inner: 0, halfArc: 0.05, from: -0.125, to: 0.125, pitchFrom: 0, pitchTo: 0, halfPitch: 0, ticks: 16 }
  const style = { ...DEFAULT_SLASH, fade: 0.2 }
  const at = { facingTurns: 0, x: 0, y: 1, z: 0, style, shape, pitchTurns: 0 }
  check(!view.offer('self', { ...at, left: 0, offsetTurns: null, now: 0 }), 'CONTROL: no swing, nothing drawn')
  check(view.offer('self', { ...at, left: 15, offsetTurns: -0.1, now: 0 }), 'a swing starting: a piece')
  check(!view.offer('self', { ...at, left: 15, offsetTurns: -0.1, now: 0.01 }), 'the same tick again: no second piece')
  check(view.offer('self', { ...at, left: 12, offsetTurns: 0, now: 0.05 }), 'the swing moved on: another piece')
  check(view.count === 2 && scene.children.length === 2, `two pieces in the scene (${view.count})`)
  const first = scene.children[0]
  check(first.material.blending === THREE.AdditiveBlending && first.material.color.getHexString() === 'fff2c8', 'drawn in the style: glow, its colour')
  view.update(0.1)
  check(first.material.opacity > 0 && first.material.opacity < style.opacity, `fading (${first.material.opacity.toFixed(2)})`)
  view.update(0.3)
  check(view.count === 0 && scene.children.length === 0, 'and gone, freed, after `fade`')
  // Another swinger, its own track; a hidden style draws nothing but still tracks.
  check(!view.offer(7, { ...at, style: { ...style, show: false }, left: 10, offsetTurns: 0, now: 1 }) && view.swingers.has(7),
        'a style with show:false draws nothing')
  check(view.offer(8, { ...at, style: { ...style, texture: 'fx/saw.png' }, left: 10, offsetTurns: 0, now: 1 }) &&
        scene.children.at(-1).material.map?.url === '/sprites/fx/saw.png', 'a texture comes from /sprites/')
  // A thrust (from == to) still shows: a sliver of its arc.
  const thrust = { ...shape, from: 0, to: 0 }
  check(view.offer(9, { ...at, shape: thrust, left: 10, offsetTurns: 0, now: 1 }), 'a thrust draws a sliver rather than nothing')
}

console.log(`\nslash: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
