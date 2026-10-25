//
// The page's end of the server in a Worker (server_worker.js, docs/WEB.md):
// the bridge LocalConnection (net_local.js) speaks to, as it speaks to the
// desktop shell's window.dnp -- so the server in the page is the desktop
// transport with a Worker behind it, and nothing after main.js's choice of
// transport can tell.
//
// `manifest` is the game's server files and command line (server_worker.js).
//
export function workerBridge (manifest) {
  const worker = new Worker(new URL('./server_worker.js', import.meta.url), { type: 'module' })
  let listener = () => {}
  let ready
  // A failure to start resolves it too: the close that comes with it is what
  // the page shows, and LocalConnection must not wait for ever first.
  const started = new Promise((resolve) => { ready = resolve })
  worker.onmessage = (e) => {
    const m = e.data
    if (m.kind === 'ready') { ready(); return }
    if (m.kind === 'log') { console.log('[server]', m.line); return }
    if (m.kind === 'close') ready()
    listener(m)
  }
  worker.onerror = (e) => {
    ready()
    listener({ kind: 'close', reason: `the game's server failed: ${e.message ?? 'in its worker'}` })
  }
  worker.postMessage({ type: 'start', manifest: new URL(manifest, location.href).href })
  return {
    onMessage (cb) { listener = cb },
    ready: () => started,
    send (kind, data) { worker.postMessage({ type: 'send', kind, data }) },
    close () { worker.terminate() }
  }
}
