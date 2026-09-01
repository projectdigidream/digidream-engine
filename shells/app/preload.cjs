//
// The bridge between the window and the shell.
//
// The renderer runs with contextIsolation and no node integration -- it is a
// web page, and it must stay one, because it is the SAME page the browser
// client serves. Anything it could do here that it cannot do in a browser would
// be a difference between the two builds, and the whole point of the desktop
// shell is that there is only one client.
//
// So this exposes exactly four things, and every one of them is a message pipe.
// No filesystem, no process control, no paths. `window.dnp` existing is also
// how client/src/main.js knows it is in the shell at all (net_local.js).
//
const { contextBridge, ipcRenderer } = require('electron')

let readyResolve = null
const ready = new Promise((r) => { readyResolve = r })
ipcRenderer.on('dnp:ready', () => readyResolve?.())

contextBridge.exposeInMainWorld('dnp', {
  /** Resolves once the link to the game server is up. */
  ready: () => ready,

  /**
   * One whole control message, or one datagram.
   *
   * The Uint8Array is copied across the boundary by structured clone. That is
   * a real cost per input frame -- but an input bundle is tens of bytes at
   * 60 Hz, and the alternative is a SharedArrayBuffer ring with its own
   * synchronisation, which is a great deal of machinery to save a copy nobody
   * has measured a problem with. Measure before changing this.
   */
  send: (kind, bytes) => ipcRenderer.send('dnp:up', kind, bytes),

  /** @param cb called as ({ kind, data }) with kind control|datagram|close */
  onMessage: (cb) => ipcRenderer.on('dnp:down', (_e, msg) => cb(msg)),

  close: () => ipcRenderer.send('dnp:close')
})
