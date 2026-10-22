//
// THE STUDIO UPDATES ITSELF on a desktop, as it does on Android (MainActivity
// selfUpdate): the release's archive for this computer is downloaded and
// unpacked beside the app, and a small script, started detached, waits for
// this app to quit, puts the new one in the old one's place and starts it.
// Which release is the studio's to choose (updates.js: a stable build only
// ever a stable one); this only installs what it is handed.
//
//   macOS    the .app bundle the app runs from, replaced whole (zip, ditto)
//   Windows  the folder the .exe sits in, mirrored (zip, PowerShell, robocopy)
//   Linux    the folder the binary sits in, replaced (tar.gz)
//
// Where the app's folder cannot be written (installed by an administrator,
// a read-only disk image), nothing is touched and the caller is told so: the
// studio then opens the release page, as it always did.
//
// Pure apart from `update`, which the shell calls; the tests use the rest.
//
import { spawn } from 'node:child_process'
import { accessSync, constants, mkdtempSync, writeFileSync, readdirSync, statSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, dirname, basename, win32 } from 'node:path'

/** What to replace, for a running app: { kind, target, exe } (target: the bundle or folder). */
export function planFor (platform, execPath) {
  if (platform === 'darwin') {
    const at = execPath.lastIndexOf('.app/')
    if (at < 0) throw new Error(`not running from an .app bundle (${execPath})`)
    const target = execPath.slice(0, at + 4)
    return { kind: 'mac', target, exe: execPath }
  }
  if (platform === 'win32') return { kind: 'windows', target: win32.dirname(execPath), exe: execPath }
  if (platform === 'linux') return { kind: 'linux', target: dirname(execPath), exe: execPath }
  throw new Error(`no self-update on ${platform}`)
}

/** The script that waits for `pid` to end, swaps `fresh` in for `plan.target`, and starts it again. */
export function swapScript (plan, fresh, pid) {
  const q = (s) => `'${String(s).replace(/'/g, `'\\''`)}'`
  if (plan.kind === 'windows') {
    const w = (s) => `"${String(s).replace(/"/g, '')}"`
    return [
      '@echo off',
      `:wait`,
      `tasklist /FI "PID eq ${pid}" | find "${pid}" >nul && (timeout /t 1 /nobreak >nul & goto wait)`,
      // /MIR: what the new folder has, and nothing the old had beyond it
      `robocopy ${w(fresh)} ${w(plan.target)} /MIR /NFL /NDL /NJH /NJS /NP >nul`,
      `start "" ${w(plan.exe)}`,
      `rmdir /s /q ${w(fresh)}`
    ].join('\r\n') + '\r\n'
  }
  const relaunch = plan.kind === 'mac' ? `open ${q(plan.target)}` : `nohup ${q(plan.exe)} >/dev/null 2>&1 &`
  return [
    '#!/bin/sh',
    `while kill -0 ${pid} 2>/dev/null; do sleep 0.2; done`,
    // the old one aside first, so a failed move leaves something that runs
    `rm -rf ${q(plan.target + '.old')}`,
    `mv ${q(plan.target)} ${q(plan.target + '.old')} && mv ${q(fresh)} ${q(plan.target)} && rm -rf ${q(plan.target + '.old')} || mv ${q(plan.target + '.old')} ${q(plan.target)}`,
    relaunch
  ].join('\n') + '\n'
}

/** In an unpacked archive, the thing that replaces the target: the .app (macOS), else the one folder at its top. */
export function freshIn (dir, kind) {
  const top = readdirSync(dir).filter((n) => !n.startsWith('.') && n !== '__MACOSX')
  const pick = kind === 'mac' ? top.find((n) => n.endsWith('.app')) : (top.length === 1 && statSync(join(dir, top[0])).isDirectory() ? top[0] : null)
  if (!pick) throw new Error(`the archive holds no ${kind === 'mac' ? '.app' : 'single folder'} (${top.join(', ')})`)
  return join(dir, pick)
}

const run = (cmd, args) => new Promise((resolve, reject) => {
  const p = spawn(cmd, args, { stdio: 'ignore' })
  p.on('error', reject)
  p.on('exit', (code) => (code === 0 ? resolve() : reject(new Error(`${cmd} failed (${code})`))))
})

/**
 * Download `url` (https), unpack it, and leave the swap to a detached script.
 * Resolves once that script is running; the caller then quits the app.
 * `fetchFn` follows redirects (GitHub sends a release asset on to its storage).
 */
export async function update (url, { fetchFn, platform = process.platform, execPath = process.execPath, pid = process.pid, log = () => {} } = {}) {
  if (!/^https:\/\//.test(String(url))) throw new Error('only https addresses')
  const plan = planFor(platform, execPath)
  // Where the new one will go must be writable, or nothing is done at all.
  accessSync(dirname(plan.target), constants.W_OK)
  accessSync(plan.target, constants.W_OK)
  const work = mkdtempSync(join(tmpdir(), 'dnp-update-'))
  const res = await fetchFn(url)
  if (!res.ok) throw new Error(`download failed (${res.status})`)
  const archive = join(work, basename(new URL(url).pathname))
  writeFileSync(archive, new Uint8Array(await res.arrayBuffer()))
  log(`downloaded ${basename(archive)}`)
  const out = join(work, 'new')
  if (plan.kind === 'mac') await run('ditto', ['-x', '-k', archive, out])
  else if (plan.kind === 'windows') await run('powershell', ['-NoProfile', '-Command', `Expand-Archive -LiteralPath '${archive}' -DestinationPath '${out}' -Force`])
  else { await run('mkdir', ['-p', out]); await run('tar', ['-xzf', archive, '-C', out]) }
  const fresh = freshIn(out, plan.kind)
  const script = join(work, plan.kind === 'windows' ? 'swap.cmd' : 'swap.sh')
  writeFileSync(script, swapScript(plan, fresh, pid), { mode: 0o755 })
  const child = plan.kind === 'windows'
    ? spawn('cmd', ['/c', script], { detached: true, stdio: 'ignore', windowsHide: true })
    : spawn('/bin/sh', [script], { detached: true, stdio: 'ignore' })
  child.unref()
  log(`the swap waits for this app to quit: ${plan.target}`)
  return { target: plan.target }
}
