//
// The writable game tree: the studio's files land where the server will look,
// nothing lands anywhere else, and seeding never touches what the user made.
//
import { mkdtempSync, mkdirSync, writeFileSync, existsSync, readFileSync } from 'node:fs'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { studioFiles, safeRelative } from '../studio_files.js'

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

const root = mkdtempSync(join(tmpdir(), 'dnp-rw-'))
const f = studioFiles(root)

await f.writeFile('modules/mine/maps/first.dnpm', new Uint8Array([1, 2, 3]))
await f.writeFile('modules/mine/module.txt', new TextEncoder().encode('id mine\n'))
check(existsSync(join(root, 'modules', 'mine', 'maps', 'first.dnpm')), 'a write creates the directories and the file')
const back = await f.readFile('modules/mine/maps/first.dnpm')
check(back && back.length === 3 && back[2] === 3, 'and reads back the same bytes')
check((await f.readFile('modules/none')) === null, 'a missing file reads as null, not an error')
const listed = await f.listFiles('modules/mine')
check(listed.length === 2 && listed.includes('modules/mine/maps/first.dnpm'), `listing walks the tree (${listed.join(', ')})`)
check(await f.deleteFile('modules/mine/maps/first.dnpm') && !(await f.exists('modules/mine/maps/first.dnpm')), 'delete deletes')
check(!(await f.deleteFile('modules/mine/maps/first.dnpm')), 'and says so when there was nothing')

// CONTROL: nothing outside the root, whatever the page asks for.
for (const bad of ['../x', '/etc/passwd', 'a/../../b', 'C:/x', 'a\\b', '', 'a//b']) {
  let refused = false
  try { await f.writeFile(bad, new Uint8Array(1)) } catch { refused = true }
  check(refused && safeRelative(bad) === null, `CONTROL: ${JSON.stringify(bad)} is refused`)
}
check(safeRelative('modules/mine/maps/a.dnpm') !== null, 'CONTROL: a plain path passes')

// Seeding: bundled modules come in, the user's stay, and it happens once per stamp.
const bundled = mkdtempSync(join(tmpdir(), 'dnp-bundled-'))
mkdirSync(join(bundled, 'starter', 'maps'), { recursive: true })
writeFileSync(join(bundled, 'starter', 'maps', 'first.dnpm'), 'baked')
check(await f.seed(bundled, 'build-1') === true, 'the first seed copies')
check(readFileSync(join(root, 'modules', 'starter', 'maps', 'first.dnpm'), 'utf8') === 'baked', 'the bundled module is in the tree')
check(existsSync(join(root, 'modules', 'mine', 'module.txt')), "the user's module survived the seed")
check(await f.seed(bundled, 'build-1') === false, 'the same stamp seeds nothing')
writeFileSync(join(bundled, 'starter', 'maps', 'first.dnpm'), 'rebaked')
check(await f.seed(bundled, 'build-2') === true && readFileSync(join(root, 'modules', 'starter', 'maps', 'first.dnpm'), 'utf8') === 'rebaked',
      'a new stamp replaces the bundled module')
const mods = await f.modules()
check(mods.length === 2 && mods.every((m) => m.startsWith(root)), `modules() lists both (${mods.length})`)

console.log(`\nstudio_files: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
