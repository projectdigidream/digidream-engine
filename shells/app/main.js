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
import { readFileSync, existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { startStatic } from './static.js'
import { Relay } from './relay.js'

const HERE = dirname(fileURLToPath(import.meta.url))
// The staged game tree sits beside the shell. scripts/build_app.sh lays it out.
const GAME = join(HERE, '..')

const manifest = JSON.parse(readFileSync(join(GAME, 'game.json'), 'utf8'))
const log = (...a) => console.log('[app]', ...a)

let serverProc = null
let relay = null
let win = null
let statics = null

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

function spawnServer (linkPort) {
  const bin = serverBinary()
  if (!existsSync(bin)) throw new Error(`no game server at ${bin}`)

  const args = ['--port', String(linkPort)]
  for (const m of manifest.modules) args.push('--module', join(GAME, 'modules', m))

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
  p.stdout.on('data', relay_out(process.stdout))
  p.stderr.on('data', relay_out(process.stderr))
  p.on('exit', (code, sig) => {
    // SIGTERM here is almost always US: before-quit kills it. Anything else is
    // the server actually failing, and the two deserve different words.
    log(sig === 'SIGTERM' && app.isQuiting
        ? 'server stopped (SIGTERM from the shell, as intended)'
        : `server exited unexpectedly (code=${code} signal=${sig})`)
    // The server going away is not something to paper over: without it there
    // is no game, and a window that keeps rendering a frozen world while
    // saying nothing is the failure the edge already learned to report.
    if (!app.isQuiting) {
      dialog.showErrorBox('The game server stopped',
        `It exited with ${code ?? sig}. Check the console for what it said.`)
      app.quit()
    }
  })
  return p
}

async function start () {
  const linkPort = await freePort()
  serverProc = spawnServer(linkPort)

  statics = await startStatic({ root: GAME, modules: manifest.modules, log })

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
  win.webContents.on('console-message', (_e, level, message, line, sourceId) => {
    if (level < 2) return          // 0/1 are log and info; keep warnings and errors
    const where = sourceId ? `${sourceId.split('/').pop()}:${line}` : '?'
    console.error(`[page] ${message}  (${where})`)
  })
  win.webContents.on('unresponsive', () => console.error('[app] the window stopped responding'))

  ipcMain.on('dnp:up', (_e, kind, bytes) => relay?.send(kind, bytes))
  ipcMain.on('dnp:close', () => relay?.stop())

  await win.loadURL(`http://127.0.0.1:${statics.port}/`)

  // Told AFTER the page is loaded, so the listener registered by net_local.js
  // is certainly in place. Sending it earlier would be a race the client could
  // only lose silently -- it would wait forever for a link that was already up.
  const announce = () => win.webContents.send('dnp:ready')
  if (relay.open) announce()
  else relay.upstream.once('connected', announce)
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
