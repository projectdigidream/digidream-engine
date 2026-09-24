//
// The Android shell's half of the bridge: `window.dnp`, over a byte pipe.
//
// STAGED OVER client/shell.js by scripts/build_apk.sh. That file is empty on
// purpose and explains why; this is the thing it makes room for. Electron does
// the same job with a preload, and the three shells -- browser, desktop,
// Android -- share one client because of it.
//
// WHAT THIS IS. It is the edge, minus WebTransport and minus a socket:
// app/relay.js with Electron IPC replaced by an Android JavascriptInterface.
// It maps one session onto the link, forwards control messages and datagrams
// upstream and forwards link frames back down, and it does not parse the game
// protocol, buffer state or decide anything. If it ever starts to, the wire
// format has two implementations that have to agree, which is the hazard
// design section 2 exists to avoid.
//
// THE FRAMING IS NOT REIMPLEMENTED HERE. `framing.js` is copied verbatim from
// server/edge/src/ by the build, exactly as build_app.sh copies it into the
// desktop shell -- so the edge, the desktop relay and this one encode the same
// bytes because they run the same function. It reads Uint8Array rather than
// Buffer so that it can be one function in three runtimes at all.
//
// WHY A CLASSIC SCRIPT WITH A DYNAMIC IMPORT. `window.dnp` has to EXIST before
// client/src/main.js runs, because main.js picks its transport at module scope
// -- but the framing arrives by `import()`, which is a promise. So the object
// is published synchronously and every method that needs the framing waits on
// `ready()`, which is exactly the promise net_local.js already awaits before
// it sends anything. Nothing had to change on the client side to allow it.
//
(function () {
  'use strict'

  const native = window.dnpNative
  if (!native) return            // not the Android shell; leave window.dnp unset

  const log = (m) => { try { native.log(String(m)) } catch (e) {} }

  // ONE SESSION. This app is one player on one device, so the id is a
  // constant -- the link protocol carries one per frame anyway, because that
  // is what lets a real edge multiplex, and sending 1 for ever costs nothing
  // and keeps the frames identical to the ones a real edge sends. The server
  // cannot tell the difference, which is the point.
  const SESSION = 1

  let LINK = null
  let encodeLinkFrame = null
  let reader = null
  let onMessage = () => {}
  let open = false
  let closed = false

  const b64encode = (bytes) => {
    let s = ''
    // In chunks, because String.fromCharCode.apply blows the argument limit on
    // a large frame -- and a map's first snapshot is large.
    for (let i = 0; i < bytes.length; i += 0x8000) {
      s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000))
    }
    return btoa(s)
  }
  const b64decode = (s) => {
    const raw = atob(s)
    const out = new Uint8Array(raw.length)
    for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i)
    return out
  }

  const ready = import('/framing.js').then((F) => {
    LINK = F.LINK
    encodeLinkFrame = F.encodeLinkFrame
    reader = F.createLinkReader()

    // Bytes arriving from the server. Raw, and NEVER whole frames: the Java
    // side hands over whatever one socket read produced, which may be half a
    // snapshot or six of them. The incremental reader is the whole reason
    // this is not a bug -- see framing.js's own header.
    window.__dnpDown = (b64) => {
      if (closed) return
      let frames
      try {
        frames = reader.push(b64decode(b64))
      } catch (e) {
        log('link framing: ' + e.message)
        return
      }
      for (const f of frames) {
        const { type, sessionId, payload } = F.decodeLinkFrame(f)
        if (sessionId !== SESSION) continue    // stale: a no-op, never an error
        // COPIED out of the reader's buffer. `payload` is a view into it and
        // the client holds what it is given across a tick -- the edge learned
        // this one the hard way, with netsim keeping a view across a timer.
        if (type === LINK.DATAGRAM) onMessage({ kind: 'datagram', data: new Uint8Array(payload) })
        else if (type === LINK.CONTROL) onMessage({ kind: 'control', data: new Uint8Array(payload) })
        else if (type === LINK.SESSION_CLOSE) {
          closed = true
          onMessage({ kind: 'close', reason: 'the server closed the session' })
        }
      }
    }

    window.__dnpClosed = (reason) => {
      if (closed) return
      closed = true
      open = false
      onMessage({ kind: 'close', reason: reason || 'the link to the game server failed' })
    }

    // The session is announced the moment the link is up, exactly as the edge
    // does for a new WebTransport session. Without it the server has never
    // heard of us and every frame we send is addressed to a session that does
    // not exist -- which it drops in silence.
    const info = new TextEncoder().encode(JSON.stringify({ path: '/dnp' }))
    native.up(b64encode(encodeLinkFrame(LINK.SESSION_OPEN, SESSION, info)))
    open = true
    log('link up')
  }).catch((e) => {
    log('shell failed: ' + (e && e.message))
    throw e
  })

  // PUBLISHED SYNCHRONOUSLY, with the same four methods app/preload.cjs
  // exposes. net_local.js registers its handler, awaits ready(), and only then
  // sends -- so the import being in flight at this moment is fine, and is
  // checked by nothing except that order, which is why it is spelled out here.
  window.dnp = {
    ready: () => ready,

    send (kind, bytes) {
      if (!open || closed) return false
      // Each complete control message goes up individually, so the server
      // never has to reassemble a stream -- the same contract the edge honours.
      const type = kind === 'control' ? LINK.CONTROL : LINK.DATAGRAM
      native.up(b64encode(encodeLinkFrame(type, SESSION, new Uint8Array(bytes))))
      return true
    },

    onMessage (cb) { onMessage = cb },

    close () {
      closed = true
      open = false
      try { native.close() } catch (e) {}
    }
  }
})()
