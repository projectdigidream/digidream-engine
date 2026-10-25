//
// THE WEB BUILD'S SERVER, HOSTED (docs/WEB.md).
//
// client/wasm/dist/dnp-server.mjs is the real server compiled to WebAssembly
// (scripts/build_server_web.sh). This starts it and turns it: the files it
// reads go into its in-memory file system, main() runs with the usual
// arguments and returns with the server running, and from then on a timer
// calls dnp_web_pump -- a tick when one is due -- while link bytes (the frames
// server/edge/src/framing.js defines, as the edge and the desktop relay send
// them) go in with feed() and come out with take().
//
// Plain JavaScript with no DOM and no Node: the page's Worker runs it
// (server_worker.js), and so does client/test/server_host.test.js under Node.
//

/**
 * Start the server.
 *
 *   createModule  the default export of dnp-server.mjs
 *   files         [[path, Uint8Array]] for its file system, absolute paths
 *   cwd           its working directory
 *   args          its command line, as the native server's (no program name)
 *   onLog(line)   what it prints, stdout and stderr alike
 *
 * Resolves to { feed(bytes), take() -> Uint8Array, pump() -> ms, stop() }, or
 * throws when main() refuses its arguments (the log says why).
 */
export async function startServer ({ createModule, files = [], cwd = '/', args = [], onLog = () => {} }) {
  const M = await createModule({ print: onLog, printErr: onLog })
  for (const [path, bytes] of files) {
    const dir = path.slice(0, path.lastIndexOf('/'))
    if (dir) M.FS.mkdirTree(dir)
    M.FS.writeFile(path, bytes)
  }
  M.FS.mkdirTree(cwd)
  M.FS.chdir(cwd)
  const rc = M.callMain(args)
  if (rc !== 0) throw new Error(`the server refused to start (exit ${rc}); see its log`)

  const SCRATCH = 1 << 16
  const scratch = M._malloc(SCRATCH)
  let timer = null
  let stopped = false

  const host = {
    /** Link bytes from the page: one frame, several, or part of one. */
    feed (bytes) {
      if (stopped || bytes.length === 0) return
      const p = M._malloc(bytes.length)
      M.HEAPU8.set(bytes, p)
      M._dnp_web_feed(p, bytes.length)
      M._free(p)
    },
    /** Link bytes for the page, everything queued since the last take. */
    take () {
      const parts = []
      let total = 0
      for (;;) {
        const n = M._dnp_web_take(scratch, SCRATCH)
        if (n === 0) break
        parts.push(M.HEAPU8.slice(scratch, scratch + n))
        total += n
      }
      if (parts.length === 1) return parts[0]
      const out = new Uint8Array(total)
      let at = 0
      for (const p of parts) { out.set(p, at); at += p.length }
      return out
    },
    /** One turn: what came in is parsed, a tick runs if due. The ms until the next. */
    pump () { return stopped ? -1 : M._dnp_web_pump() },
    /**
     * Turn it by itself: pump, hand what came out to onOut, and come back
     * when the next tick is due. The page's Worker uses this.
     */
    run (onOut) {
      const turn = () => {
        if (stopped) return
        const ms = host.pump()
        const out = host.take()
        if (out.length) onOut(out)
        timer = setTimeout(turn, Math.max(0, Math.min(ms, 16)))
      }
      turn()
    },
    stop () {
      stopped = true
      if (timer !== null) clearTimeout(timer)
    }
  }
  return host
}
