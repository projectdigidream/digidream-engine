//
// The camera field ids, in two languages with no header between them.
//
// CAMERA_SET addresses its fields by POSITION: the index into
// CAMERA_FIELD_NAMES in server/src/lua/world_binding.cpp is the wire id, and
// the index into CAMERA_FIELD in client/src/proto.js is what the client applies
// it as. Nothing connects the two but the discipline of appending to both.
//
// Get it wrong and a map asking for a longer lead changes the pitch instead --
// in every level already shipped, silently, because both sides still parse a
// well-formed message. There is no length mismatch to notice and no value out
// of range. That is the failure this file exists for, and until now the eleven
// original ids had no test at all.
//
// Static analysis of a source file, which this project already does once:
// client/test/tdz.test.js reads main.js as text to check declaration order.
// Reading C++ from JavaScript is not elegant; it is the only place the two
// lists can be compared without inventing a third file for them to drift from.
//
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { CAMERA_FIELD, CAMERA_MAX_FIELDS, SCREEN_FIELD, SCREEN_MAX_FIELDS, MSG, encodeControlMessage, decodeControlMessage } from '../src/proto.js'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// snake_case -> camelCase, and the `cam_` prefix the path fields carry on the
// Lua side. The prefix exists because a Lua table's keys sit beside `distance`
// and `pitch` and `path` alone would read as a file path; the client has no
// such neighbours, so it drops it.
const jsName = (lua) => lua.replace(/^cam_/, '').replace(/_([a-z])/g, (_, c) => c.toUpperCase())

console.log('\n--- the two field lists are one list ---')
{
  const cpp = readFileSync(join(ROOT, 'server', 'src', 'lua', 'world_binding.cpp'), 'utf8')
  const block = /CAMERA_FIELD_NAMES\[\]\s*=\s*\{([\s\S]*?)\};/.exec(cpp)
  check(block !== null, 'found CAMERA_FIELD_NAMES in the C++ source')

  // Strip comments before pulling strings out, or a name mentioned in the prose
  // above a field would be read as a field.
  const body = block[1].replace(/\/\/[^\n]*/g, '')
  const names = [...body.matchAll(/"([a-z0-9_]+)"/g)].map((m) => m[1])

  // THE CONTROL ON THE EXTRACTOR ITSELF. A regex that matched nothing would
  // compare two empty lists and pass, which is the classic way a test like this
  // is worth nothing. Pinning the count means a field added on one side alone
  // fails here rather than in a bug report about the pitch.
  check(names.length === 27, `extracted ${names.length} names (expected 27, cone_rate the last)`)
  check(CAMERA_FIELD.length === 27, `the client names ${CAMERA_FIELD.length} fields`)

  const mapped = names.map(jsName)
  let firstBad = -1
  for (let i = 0; i < Math.max(mapped.length, CAMERA_FIELD.length); i++) {
    if (mapped[i] !== CAMERA_FIELD[i]) { firstBad = i; break }
  }
  check(firstBad < 0,
    firstBad < 0
      ? 'every id means the same field on both sides'
      : `id ${firstBad}: C++ says '${names[firstBad]}' (-> '${mapped[firstBad]}'), client says '${CAMERA_FIELD[firstBad]}'`)

  // Spot-check the two ends, so a mapping that agreed by being empty in the
  // middle still has to name something real.
  check(mapped[0] === 'distance' && CAMERA_FIELD[0] === 'distance', 'id 0 is distance on both sides')
  check(mapped[17] === 'shakeTime' && CAMERA_FIELD[17] === 'shakeTime', 'id 17 is shakeTime on both sides')

  // CONTROL on the comparison: a deliberately wrong list must fail it. Without
  // this, a loop with an off-by-one that never executed would also report
  // "every id means the same field".
  const wrong = CAMERA_FIELD.slice()
  const t = wrong[3]; wrong[3] = wrong[4]; wrong[4] = t
  let sawDiff = false
  for (let i = 0; i < mapped.length; i++) if (mapped[i] !== wrong[i]) sawDiff = true
  check(sawDiff, 'CONTROL: swapping two client names is detected')
}

console.log('\n--- the cap is the same number on both sides ---')
{
  const hpp = readFileSync(join(ROOT, 'core', 'include', 'dnp', 'proto', 'messages.hpp'), 'utf8')
  const m = /CAMERA_MAX_FIELDS\s*=\s*(\d+)/.exec(hpp)
  check(m !== null, 'found CAMERA_MAX_FIELDS in the C++ header')
  check(Number(m[1]) === CAMERA_MAX_FIELDS,
    `cap agrees: C++ ${m[1]}, client ${CAMERA_MAX_FIELDS}`)

  // And there is still room. The array is what read_camera_table fills by
  // walking the name list, so the day the names outgrow it the overrun is
  // silent -- this is the check that says so a release early.
  check(CAMERA_FIELD.length < CAMERA_MAX_FIELDS,
    `${CAMERA_FIELD.length} fields against a cap of ${CAMERA_MAX_FIELDS}`)
}

// SCREEN_SET: the same discipline for its own, shorter list, and the record
// as the server's encode_screen_set writes it (u16 count, count x {u8, i32}).
console.log('\n--- the screen fields are one list too ---')
{
  const cpp = readFileSync(join(ROOT, 'server', 'src', 'lua', 'world_binding.cpp'), 'utf8')
  const block = /SCREEN_FIELD_NAMES\[\]\s*=\s*\{([\s\S]*?)\};/.exec(cpp)
  check(block !== null, 'found SCREEN_FIELD_NAMES in the C++ source')
  const names = [...block[1].replace(/\/\/[^\n]*/g, '').matchAll(/"([a-z0-9_]+)"/g)].map((m) => m[1])
  check(names.length === 5 && SCREEN_FIELD.length === 5, `5 screen fields on both sides (C++ ${names.length}, client ${SCREEN_FIELD.length})`)
  check(names.map(jsName).every((n, i) => n === SCREEN_FIELD[i]), 'every screen id means the same field on both sides')
  check(names[4] === 'quit' && SCREEN_FIELD[4] === 'quit', 'id 4 is quit on both sides')
  const hpp = readFileSync(join(ROOT, 'core', 'include', 'dnp', 'proto', 'messages.hpp'), 'utf8')
  check(Number(/SCREEN_MAX_FIELDS\s*=\s*(\d+)/.exec(hpp)?.[1]) === SCREEN_MAX_FIELDS, 'the screen cap agrees')

  const record = (fields) => {
    const out = new Uint8Array(2 + fields.length * 5)
    const v = new DataView(out.buffer)
    v.setUint16(0, fields.length, true)
    fields.forEach(([id, milli], i) => { v.setUint8(2 + i * 5, id); v.setInt32(3 + i * 5, milli, true) })
    return out
  }
  const m = decodeControlMessage(encodeControlMessage(MSG.SCREEN_SET, record([[0, 1000], [4, 1000]])))
  check(m?.type === MSG.SCREEN_SET && m.fields.length === 2, 'a fade and a quit decode')
  const q = m?.fields.find((f) => SCREEN_FIELD[f.id] === 'quit')
  check(q?.value === 1, 'the quit field reads as 1')
  const unknown = decodeControlMessage(encodeControlMessage(MSG.SCREEN_SET, record([[99, 5], [4, 1000]])))
  check(unknown?.fields.length === 2 && SCREEN_FIELD[99] === undefined, 'an unknown id is carried, not refused (the handler skips it)')
  check(decodeControlMessage(encodeControlMessage(MSG.SCREEN_SET, record(Array(17).fill([0, 0])))) === null,
    'CONTROL: over the cap is refused')
  // The handler: main.js reads `quit` and calls quitGame, which tries the shell's quit.
  const main = readFileSync(join(ROOT, 'client', 'src', 'main.js'), 'utf8')
  check(/name === 'quit'/.test(main) && /if \(quit\) quitGame\(\)/.test(main) && /window\.dnp\?\.quit/.test(main),
    'the SCREEN_SET handler acts on quit through the shell')
}

console.log(`\nproto_camera: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
