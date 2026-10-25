//
// THE SERVER, IN THE PAGE (docs/WEB.md): a module Worker running the real
// server compiled to wasm (server_host.js), for a game with no server anywhere.
//
// It fetches the game's server files from a manifest -- { root, args, files }:
// files relative to the manifest, written under `root` in the server's file
// system, and the server's command line -- starts it, and opens one session as
// the edge would. Between it and the page (net_worker.js) go the messages the
// desktop shell's relay passes (net_local.js): whole control messages and
// whole datagrams, either way, and a close.
//
// The link framing is server/edge/src/framing.js, served as /framing.js as the
// Android shell has it; never a second copy of the wire format.
//
import { startServer } from './server_host.js'
import createModule from '../wasm/dist/dnp-server.mjs'
import {
  LINK, encodeLinkFrame, createLinkReader, decodeLinkFrame, createControlReader
} from '/framing.js'

const SESSION = 1
const links = createLinkReader()
const controls = createControlReader()
const early = []   // what the page sent before the server was up
let host = null

const fail = (reason) => postMessage({ kind: 'close', reason })

function out (bytes) {
  for (const f of links.push(bytes)) {
    const { type, payload } = decodeLinkFrame(f)
    if (type === LINK.CONTROL) {
      for (const c of controls.push(payload)) postMessage({ kind: 'control', data: c.slice() })
    } else if (type === LINK.DATAGRAM) {
      postMessage({ kind: 'datagram', data: payload.slice() })
    } else if (type === LINK.SESSION_CLOSE) {
      fail('the game ended')
    }
  }
}

async function start (manifestUrl) {
  const res = await fetch(manifestUrl)
  if (!res.ok) throw new Error(`no game at ${manifestUrl} (${res.status})`)
  const m = await res.json()
  const files = await Promise.all(m.files.map(async (f) => {
    const r = await fetch(new URL(f, manifestUrl))
    if (!r.ok) throw new Error(`${f}: ${r.status}`)
    return [`${m.root}/${f}`, new Uint8Array(await r.arrayBuffer())]
  }))
  host = await startServer({
    createModule, files, cwd: m.root, args: m.args,
    onLog: (line) => postMessage({ kind: 'log', line })
  })
  host.feed(encodeLinkFrame(LINK.SESSION_OPEN, SESSION, new TextEncoder().encode('{"path":"/dnp"}')))
  for (const f of early.splice(0)) host.feed(f)
  host.run(out)
  postMessage({ kind: 'ready', files: files.length })
}

onmessage = (e) => {
  const m = e.data
  if (m.type === 'start') {
    start(m.manifest).catch((err) => fail(`the game could not start: ${err.message}`))
  } else if (m.type === 'send') {
    const frame = encodeLinkFrame(m.kind === 'control' ? LINK.CONTROL : LINK.DATAGRAM, SESSION, m.data)
    if (host) host.feed(frame)
    else early.push(frame)
  }
}
