//
// CONTROL SCHEMES, the pure half (client/src/controls.js).
//
// The thing worth pinning is agreement with the simulation: the brad the view
// sends must point where the simulation's own direction for that brad points,
// or an FPS player shoots where they are not looking. Each claim has a control.
//
import { FpsLook, fpsPush, MAX_PITCH_RAD } from '../src/controls.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}
const near = (a, b, eps = 1e-3) => Math.abs(a - b) < eps

// --- the view turns the way the screen does ------------------------------
const look = new FpsLook()
look.move(100, 0)
check(look.yaw > Math.PI, `moving the mouse right turns clockwise (yaw ${look.yaw.toFixed(3)} rad)`)
const l2 = new FpsLook()
l2.move(-100, 0)
check(l2.yaw > 0 && l2.yaw < Math.PI, 'CONTROL: and left turns the other way')
const up = new FpsLook()
up.move(0, -100)
check(up.pitch > 0, 'moving the mouse up looks up')
up.move(0, -100000)
check(near(up.pitch, MAX_PITCH_RAD), 'and no further than the simulation allows (80 degrees)')
check(up.pitchWire() === 14563, `which is the wire's own clamp (${up.pitchWire()})`)
const down = new FpsLook()
down.move(0, 100000)
check(down.pitchWire() === -14563, 'CONTROL: and the same looking down')

// --- the brad sent is the direction looked -------------------------------
//
// A brad is a turn in 2^32, counter-clockwise from +x, as the simulation's
// fx_dir reads it; the view's yaw and the brad it sends must be the same angle
// both ways round. (That the simulation then walks along it is
// core/tests/test_abilities.cpp's move_relative test.)
let worst = 0
for (const deg of [0, 37, 90, 135, 180, 222, 270, 315, 359.9]) {
  const v = new FpsLook({ yaw: deg * Math.PI / 180 })
  const f = v.forward()
  const back = new FpsLook()
  back.face(v.aimBrad())
  worst = Math.max(worst, Math.abs(Math.cos(back.yaw) - f.x), Math.abs(Math.sin(back.yaw) - f.y))
}
check(worst < 1e-6, `the aim sent turns back into the direction looked (worst ${worst.toExponential(1)})`)
check(new FpsLook({ yaw: Math.PI / 2 }).aimBrad() === 0x40000000, 'a quarter turn is 2^30 brad')

// --- the push ------------------------------------------------------------
const w = fpsPush({ forward: true })
check(w.x === 0 && w.y === 1, 'W is straight ahead')
const d = fpsPush({ right: true })
check(d.x === 1 && d.y === 0, 'D is a sidestep to the right')
const diag = fpsPush({ forward: true, right: true })
check(near(Math.hypot(diag.x, diag.y), 1), `a diagonal is not faster than straight (${Math.hypot(diag.x, diag.y).toFixed(3)})`)
const none = fpsPush({ forward: true, back: true })
check(none.x === 0 && none.y === 0, 'CONTROL: forward and back together cancel')
const stick = fpsPush({ forward: true, stick: { x: 0.3, y: -0.4 } })
check(stick.x === 0.3 && stick.y === -0.4, 'a stick, when pushed, is taken as it is')

console.log(`\ncontrols: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
