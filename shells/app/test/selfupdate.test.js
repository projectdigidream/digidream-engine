// THE STUDIO UPDATES ITSELF ON A DESKTOP (app/selfupdate.js): what it
// replaces for each system, and a real swap -- a stand-in app in a temporary
// folder, a tar.gz "downloaded" from a fake fetch, a script that waits for a
// process that has already ended -- with the controls that it refuses a plain
// http address and a folder it cannot write.
//
//   node shells/app/test/selfupdate.test.js

import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, existsSync, chmodSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { spawnSync } from 'node:child_process'
import { planFor, swapScript, update } from '../selfupdate.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; if (!ok) { fails++; console.log(`  FAIL ${label}`) } else console.log(`  ok   ${label}`) }
const wait = (ms) => new Promise((r) => setTimeout(r, ms))

const mac = planFor('darwin', '/Applications/Digidream Studio.app/Contents/MacOS/Digidream Studio')
check(mac.kind === 'mac' && mac.target === '/Applications/Digidream Studio.app', `macOS replaces the whole .app bundle (${mac.target})`)
const win = planFor('win32', 'C:\\Games\\Digidream Studio\\Digidream Studio.exe')
check(win.kind === 'windows' && /Digidream Studio$/.test(win.target), `Windows replaces the folder the .exe sits in (${win.target})`)
check(planFor('linux', '/opt/studio/digidream-studio').target === '/opt/studio', 'Linux replaces the folder the binary sits in')
let threw = null
try { planFor('darwin', '/usr/local/bin/node') } catch (e) { threw = e.message }
check(/not running from an .app/.test(threw ?? ''), 'CONTROL: a mac binary outside a bundle is refused')
check(/robocopy .* \/MIR/.test(swapScript(win, 'C:\\t\\new', 42)) && /tasklist \/FI "PID eq 42"/.test(swapScript(win, 'C:\\t\\new', 42)), 'the Windows script waits for the app, then mirrors the new folder over it')

// A real swap: the stand-in "app" is a folder with a version file and a binary.
const root = mkdtempSync(join(tmpdir(), 'dnp-upd-test-'))
const app = join(root, 'Digidream Studio')
mkdirSync(app)
writeFileSync(join(app, 'version'), 'old\n')
const exe = join(app, 'studio')
writeFileSync(exe, '#!/bin/sh\necho started > "$(dirname "$0")/started"\n'); chmodSync(exe, 0o755)
// the new release, packed as the packager packs a Linux studio: one folder at the top
const src = join(root, 'src', 'Digidream Studio')
mkdirSync(src, { recursive: true })
writeFileSync(join(src, 'version'), 'new\n')
writeFileSync(join(src, 'studio'), '#!/bin/sh\necho started > "$(dirname "$0")/started"\n'); chmodSync(join(src, 'studio'), 0o755)
const tgz = join(root, 'studio-linux-x64.tar.gz')
spawnSync('tar', ['-czf', tgz, '-C', join(root, 'src'), 'Digidream Studio'])
const bytes = readFileSync(tgz)
const fetchFn = async () => ({ ok: true, status: 200, arrayBuffer: async () => bytes })
// a process that has ended: the script's wait is over at once
const gone = spawnSync('true').pid

let refused = null
try { await update('http://example.com/x.tar.gz', { fetchFn, platform: 'linux', execPath: exe, pid: gone }) } catch (e) { refused = e.message }
check(/only https/.test(refused ?? ''), 'CONTROL: a plain http address is refused')

await update('https://example.com/studio-linux-x64.tar.gz', { fetchFn, platform: 'linux', execPath: exe, pid: gone })
for (let i = 0; i < 50 && !existsSync(join(app, 'started')); i++) await wait(100)
check(readFileSync(join(app, 'version'), 'utf8') === 'new\n', 'the app folder now holds the new release')
check(!existsSync(app + '.old'), 'and the old one is gone, not left beside it')
check(existsSync(join(app, 'started')), 'and the new one was started')

// A STUDIO UNZIPPED LOOSE (its files among a user's own, in Downloads): the
// swap would replace the whole folder, so nothing is done, and the user's
// file is still there.
const loose = join(root, 'Downloads'); mkdirSync(loose)
writeFileSync(join(loose, 'version'), 'old\n')
const looseExe = join(loose, 'studio'); writeFileSync(looseExe, '#!/bin/sh\n'); chmodSync(looseExe, 0o755)
writeFileSync(join(loose, 'taxes.pdf'), 'mine')
let kept = null
try { await update('https://example.com/studio-linux-x64.tar.gz', { fetchFn, platform: 'linux', execPath: looseExe, pid: gone }) } catch (e) { kept = e.message }
await wait(300)
check(/other files too \(taxes\.pdf\)/.test(kept ?? '') && readFileSync(join(loose, 'taxes.pdf'), 'utf8') === 'mine' && readFileSync(join(loose, 'version'), 'utf8') === 'old\n',
      `a folder holding someone else's files is not replaced (${kept})`)

// CONTROL: a folder this user cannot write is left alone, with a reason
const locked = join(root, 'locked'); mkdirSync(join(locked, 'app'), { recursive: true })
writeFileSync(join(locked, 'app', 'studio'), 'x'); chmodSync(locked, 0o555)
let denied = null
try { await update('https://example.com/a.tar.gz', { fetchFn, platform: 'linux', execPath: join(locked, 'app', 'studio'), pid: gone }) } catch (e) { denied = e.code ?? e.message }
chmodSync(locked, 0o755)
check(denied === 'EACCES' && existsSync(join(locked, 'app', 'studio')), `CONTROL: an app folder it cannot write is not touched (${denied})`)

console.log(`\nselfupdate: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
