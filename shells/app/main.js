//
// The desktop shell.
//
// Three things the stack already needed, moved inside one process:
//
//   scripts/run_stack.sh          this file
//   ---------------------------   ------------------------------------------
//   dnp-server --port N           spawnServer()   (a child process, as before)
//   server/edge  (WebTransport)   Relay           (relay.js -- IPC, no HTTP/3)
//   tools/devserver.js            startStatic()   (static.js, loopback only)
//   "open this URL in a browser"  a BrowserWindow
//
// run_stack.sh is bash and does not survive to Windows, so this supervision has
// to live somewhere else whatever shell is chosen. The three-process split
// itself is NOT collapsed: the C++ server is still its own process, still
// transport-agnostic, still speaking the same link frames. That split was a
// deliberate Phase 1 decision and hiding it behind one binary would make the
// architecture less honest, not simpler -- the same reason run_stack.sh gives.
//
// IF THIS LINE APPEARS TO BE THE PROBLEM, CHECK `ELECTRON_RUN_AS_NODE` FIRST.
//
// With that variable set, the Electron binary runs as plain Node: no app, no
// BrowserWindow, no builtin electron module at all. Every way of importing it
// then fails, and each failure is a convincing accusation against the import:
//
//   import { app } from 'electron'       SyntaxError: no export 'BrowserWindow'
//   import electron from 'electron'      TypeError: electron.app is undefined
//   require('electron')                  Error: Cannot find module 'electron'
//
// Three errors, three plausible diagnoses, one actual cause -- and none of them
// is the import. Under a real Electron the named form below and the default
// form both work; this was verified by putting each back and launching. Two
// rounds of "fixing" this line happened before the variable was found, and both
// made working code worse.
//
// `process.type` is the tell: 'browser' is a real main process, undefined means
// you are in Node. app/test/launch.test.js strips the variable and asserts this
// file loads at all.
import { app, BrowserWindow, ipcMain, dialog } from 'electron'
import { spawn } from 'node:child_process'
import { createServer } from 'node:net'
import { readFileSync, writeFileSync, existsSync, mkdirSync, createWriteStream, appendFileSync } from 'node:fs'
import { readFile as readFileAsync, writeFile as writeFileAsync, readdir } from 'node:fs/promises'
import { dirname, join, basename } from 'node:path'
import { fileURLToPath } from 'node:url'
import { startStatic } from './static.js'
import { Relay } from './relay.js'
import { studioFiles } from './studio_files.js'

const HERE = dirname(fileURLToPath(import.meta.url))
// The staged game tree sits beside the shell. scripts/build_app.sh lays it out.
const GAME = join(HERE, '..')

const manifest = JSON.parse(readFileSync(join(GAME, 'game.json'), 'utf8'))
const log = (...a) => console.log('[app]', ...a)

let serverProc = null
let relay = null
let win = null
let statics = null

// THE WRITABLE GAME TREE (docs/STUDIO.md). A packaged app's own files are
// read-only, so what the studio writes goes under the user's data directory
// in the same modules/<id>/ layout, and the server is pointed there. The
// bundled modules are seeded into it so the server, the static server and the
// studio all see one tree; a module the user made is never touched.
const GAME_RW = join(app.getPath('userData'), 'game')
const files = studioFiles(GAME_RW)
// The editor the app opens and returns to: the studio, plugins and all
// (docs/PLUGINS.md). The suite, its fork with the plugins, became it at 0.2.0.
const EDITOR = '/studio/'
// Set while a play-test restarts the server, so its exit is not mistaken for
// a crash by the handler below.
let restarting = false

/**
 * A free TCP port for the link between the shell and the game server.
 *
 * Bind port 0, read what the OS gave us, let it go, hand the number to the
 * server. There IS a race: something else could take the port in between. It is
 * accepted deliberately -- the alternative is teaching the C++ server to report
 * the port it bound, which is a protocol between two programs added to solve a
 * problem that on a single desktop machine essentially does not happen. If the
 * server fails to bind it exits and says so, which is a loud failure rather
 * than a confusing one.
 */
function freePort () {
  return new Promise((resolve, reject) => {
    const s = createServer()
    s.on('error', reject)
    s.listen(0, '127.0.0.1', () => {
      const { port } = s.address()
      s.close(() => resolve(port))
    })
  })
}

function serverBinary () {
  const name = process.platform === 'win32' ? 'dnp-server.exe' : 'dnp-server'
  return join(GAME, 'bin', name)
}

function spawnServer (linkPort, extra = {}) {
  const bin = serverBinary()
  if (!existsSync(bin)) throw new Error(`no game server at ${bin}`)

  const args = ['--port', String(linkPort)]
  // With the studio, the bundled modules are the SEEDED copies: one tree, so
  // a module the studio rewrote is the one the server loads, and no catalogue
  // is loaded twice (an id collision refuses to start).
  const bundledRoot = manifest.studio ? join(GAME_RW, 'modules') : join(GAME, 'modules')
  const roots = new Set(manifest.modules.map((m) => join(bundledRoot, m)))
  for (const m of extra.modules ?? []) roots.add(m)
  for (const m of roots) args.push('--module', m)
  // The first map is where players start; the rest are what a script may send
  // them to (change_map). A play-test passes the map being tested, then every
  // other map of its module: with only the first, a graph going to another
  // map is refused as naming one the server does not have.
  const maps = extra.maps ?? (extra.map ? [extra.map] : [])
  for (const m of maps) args.push('--map', m)
  if (maps.length) args.push('--spawn', 'start')
  // Where the server writes -- save games, level state, the graphs' saved
  // variables (docs/GRAPHS.md "Saved on disk"): the user's data directory. Its
  // default, .dnp-state in its cwd, is inside a packaged app, which is
  // read-only (and on macOS, sealed).
  args.push('--state-dir', join(app.getPath('userData'), 'state'))

  // cwd is the GAME directory, not wherever the app was launched from. The
  // server resolves assets/props.txt and modules/ against its working
  // directory, and a packaged app on macOS starts with cwd '/'. Setting it here
  // costs nothing; the alternative is teaching the server a --root flag for a
  // problem the launcher can simply not create.
  log(`server: ${bin} ${args.join(' ')}`)
  const p = spawn(bin, args, { cwd: GAME, stdio: ['ignore', 'pipe', 'pipe'] })
  // Tag only what is not already tagged. The server prefixes its own lines with
  // [srv]/[lua]/[link], and prefixing again produced "[srv] [srv] tick 2768 ..."
  // in every log anybody pasted.
  const relay_out = (stream) => (b) => {
    for (const line of b.toString().split('\n')) {
      if (line === '') continue
      stream.write(line.startsWith('[') ? line + '\n' : '[srv] ' + line + '\n')
    }
  }
  // A play-test's server output also goes to the studio's Play log
  // (logs/play-server.log), started afresh, for the studio to read back.
  const file = extra.logFile ? (mkdirSync(dirname(extra.logFile), { recursive: true }), createWriteStream(extra.logFile, { flags: 'w' })) : null
  file?.write(`$ ${bin} ${args.join(' ')}\n`)
  const both = (stream) => { const out = relay_out(stream); return (b) => { out(b); file?.write(b) } }
  p.stdout.on('data', both(process.stdout))
  p.stderr.on('data', both(process.stderr))
  p.on('exit', (code, sig) => { file?.end(`[shell] the server exited (code=${code} signal=${sig})\n`) })
  p.on('exit', (code, sig) => {
    // SIGTERM here is almost always US: before-quit kills it. Anything else is
    // the server actually failing, and the two deserve different words.
    log(sig === 'SIGTERM' && app.isQuiting
        ? 'server stopped (SIGTERM from the shell, as intended)'
        : `server exited unexpectedly (code=${code} signal=${sig})`)
    // The server going away is not something to paper over: without it there
    // is no game, and a window that keeps rendering a frozen world while
    // saying nothing is the failure the edge already learned to report.
    // A play-test's server failing is a content error, not the app's: back to
    // the studio, whose Play log shows what the server said, rather than quit.
    if (!app.isQuiting && !restarting && extra.map && manifest.studio && win && !win.isDestroyed() && statics) {
      playLog(`[shell] the server stopped during the play-test (code=${code} signal=${sig}); back to the studio`)
      win.loadURL(`http://127.0.0.1:${statics.port}${EDITOR}`)
      return
    }
    if (!app.isQuiting && !restarting) {
      dialog.showErrorBox('The game server stopped',
        `It exited with ${code ?? sig}. Check the console for what it said.`)
      app.quit()
    }
  })
  return p
}

// Stop the running server and start another, for a play-test on what the
// studio just wrote. A fresh port each time: the old process may hold the
// last one for a moment after it is gone.
async function restartServer (extra) {
  restarting = true
  relay?.stop()
  if (serverProc && serverProc.exitCode === null) {
    await new Promise((resolve) => { serverProc.once('exit', resolve); serverProc.kill() })
  }
  const linkPort = await freePort()
  serverProc = spawnServer(linkPort, extra)
  relay = new Relay({ port: linkPort, log })
  relay.start((msg) => { if (win && !win.isDestroyed()) win.webContents.send('dnp:down', msg) })
  restarting = false
}

// Told AFTER the page is loaded, so the listener registered by net_local.js
// is certainly in place. Sending it earlier would be a race the client could
// only lose silently -- it would wait forever for a link that was already up.
function announceWhenUp () {
  const announce = () => { if (win && !win.isDestroyed()) win.webContents.send('dnp:ready') }
  if (relay.open) announce()
  else relay.upstream.once('connected', announce)
}

// The game page's log for the studio, while a play-test runs (logs/play-game.log).
let gameLog = null
function playLog (line) {
  if (!gameLog) return
  try { appendFileSync(gameLog, line + '\n') } catch {}
}

/** The studio's Play: serve the module it wrote, then show the game. */
async function launch ({ module, map }) {
  if (!(await files.exists(`${module}/maps/${map}`))) throw new Error(`no such level: ${module}/maps/${map}`)
  const modDir = join(GAME_RW, module)
  // The Play log, started afresh: the server's output and the game page's console.
  const logs = join(GAME_RW, 'logs')
  mkdirSync(logs, { recursive: true })
  gameLog = join(logs, 'play-game.log')
  appendFileSync(gameLog, '', { flag: 'w' })
  playLog(`[shell] play-test of ${module}/maps/${map}`)
  try {
    const entry = join(modDir, 'maps', map)
    const others = (await readdir(join(modDir, 'maps')).catch(() => [])).filter((n) => n.endsWith('.dnpm') && n !== map).sort().map((n) => join(modDir, 'maps', n))
    await restartServer({ modules: [modDir], map: entry, maps: [entry, ...others].slice(0, 64), logFile: join(logs, 'play-server.log') })
  } catch (e) { playLog(`[shell] the game did not start: ${e.message ?? e}`); throw e }
  // Its files first, ahead of every other module in the tree (static.js).
  statics.setFirst?.(modDir)
  await win.loadURL(`http://127.0.0.1:${statics.port}/index.html?studio=1`)
  announceWhenUp()
  return true
}

async function start () {
  if (manifest.studio) {
    if (await files.seed(join(GAME, 'modules'), manifest.builtAt ?? 'unknown')) log(`seeded ${GAME_RW}`)
  }
  const linkPort = await freePort()
  serverProc = spawnServer(linkPort)

  // THE SAME ORIGIN EVERY LAUNCH. The page's storage (the studio's work in
  // IndexedDB, localStorage) belongs to its origin, port included; a new port
  // each launch was a new, empty store -- what was not yet played was gone.
  const portFile = join(app.getPath('userData'), 'static-port')
  const lastPort = Number((() => { try { return readFileSync(portFile, 'utf8') } catch { return '' } })()) || 0
  statics = await startStatic({ root: GAME, modules: manifest.modules, rwRoot: manifest.studio ? GAME_RW : null, log, port: lastPort })
  if (statics.port !== lastPort) { try { writeFileSync(portFile, String(statics.port)) } catch (e) { log(`could not keep the port: ${e.message}`) } }

  relay = new Relay({ port: linkPort, log })
  relay.start((msg) => { if (win && !win.isDestroyed()) win.webContents.send('dnp:down', msg) })

  win = new BrowserWindow({
    width: 1280,
    height: 800,
    backgroundColor: '#0b0d12',
    title: manifest.name,
    webPreferences: {
      // .cjs: an Electron preload is CommonJS (it needs require), and the
      // app's package.json sets "type": "module", which would otherwise
      // make a .js preload an ES module and fail to load.
      preload: join(HERE, 'preload.cjs'),
      // The renderer is a web page and stays one. See preload.js.
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false           // preload needs require(); the page still cannot
    }
  })
  win.setMenuBarVisibility(false)

  // THE PAGE'S OWN CONSOLE, into the same log as everything else.
  //
  // A renderer's console goes to devtools, which is precisely where nobody is
  // looking at the moment the window dies. An uncaught exception in the client
  // -- the thing most likely to take the window down and, through it, the game
  // server -- otherwise leaves no trace anywhere.
  win.webContents.on('console-message', (e, level, message, line, sourceId) => {
    // The game page's console, into the Play log while a play-test runs.
    playLog(`[${['verbose', 'info', 'warning', 'error'][level] ?? e?.level ?? level}] ${message ?? e?.message}${sourceId ? ` (${String(sourceId).replace(/^https?:\/\/[^/]+/, '')}:${line})` : ''}`)
    if (level < 2) return          // 0/1 are log and info; keep warnings and errors
    const where = sourceId ? `${sourceId.split('/').pop()}:${line}` : '?'
    console.error(`[page] ${message}  (${where})`)
  })
  win.webContents.on('unresponsive', () => console.error('[app] the window stopped responding'))

  ipcMain.on('dnp:up', (_e, kind, bytes) => relay?.send(kind, bytes))
  ipcMain.on('dnp:close', () => relay?.stop())

  // The studio's seven calls (app/preload.cjs). Files stay under GAME_RW --
  // studio_files.js refuses anything else -- and the two dialogs are the
  // platform's own.
  ipcMain.handle('dnp:studio', async (_e, op, a, b) => {
    switch (op) {
      case 'writeFile': return files.writeFile(a, b)
      case 'readFile': return files.readFile(a)
      case 'listFiles': return files.listFiles(a ?? '')
      case 'deleteFile': return files.deleteFile(a)
      case 'launch': return launch(a ?? {})
      case 'share': {
        const r = await dialog.showSaveDialog(win, { defaultPath: a })
        if (r.canceled || !r.filePath) return false
        await writeFileAsync(r.filePath, b)
        return true
      }
      case 'importFile': {
        const r = await dialog.showOpenDialog(win, { properties: ['openFile'] })
        if (r.canceled || !r.filePaths[0]) return null
        return { name: basename(r.filePaths[0]), bytes: new Uint8Array(await readFileAsync(r.filePaths[0])) }
      }
      default: throw new Error(`unknown studio call: ${op}`)
    }
  })

  await win.loadURL(`http://127.0.0.1:${statics.port}${manifest.studio ? EDITOR : '/'}`)
  announceWhenUp()
}

app.whenReady().then(start).catch((e) => {
  dialog.showErrorBox('Could not start', String(e.message ?? e))
  app.quit()
})

// WHY THE APP IS GOING DOWN, said out loud.
//
// The shell kills the game server on quit (before-quit, below), so ANY route to
// quitting surfaces as "[app] server exited (SIGTERM)" and nothing else. That
// reads as the server crashing, which is the one thing it did not do -- the
// SIGTERM came from here. Reported exactly that way, and it sent the
// investigation after the server for a long time.
//
// So every path that can end the app names itself first.

// The renderer dying is the likeliest of them and the least visible: the window
// vanishes, window-all-closed fires, and the server is terminated behind it. A
// page rendering two hundred monsters is a plausible place to run out of memory
// or lose the GPU process.
app.on('render-process-gone', (_e, _wc, details) => {
  console.error(`[app] THE WINDOW DIED: reason=${details.reason} exitCode=${details.exitCode}`)
  console.error('[app] the game server did not crash -- it is about to be stopped because ' +
                'the window it was serving is gone')
})

// A GPU or utility process going is survivable, but it is worth knowing about
// when it happens just before everything else does.
app.on('child-process-gone', (_e, details) => {
  console.error(`[app] child process gone: type=${details.type} reason=${details.reason} ` +
                `name=${details.name ?? '-'}`)
})

app.on('window-all-closed', () => {
  console.error('[app] all windows closed; quitting')
  app.quit()
})

app.on('before-quit', () => {
  console.error('[app] quitting; stopping the game server')
  app.isQuiting = true
  relay?.stop()
  statics?.server?.close()
  // The server is a child process and dies with us on every platform this
  // ships to -- but only if we actually ask. An orphaned server holding a port
  // is how the next launch fails for a reason that looks unrelated.
  serverProc?.kill()
})
