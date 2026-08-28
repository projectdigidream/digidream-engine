//
// The edge: WebTransport on one side, a link socket to the C++ server on the
// other, and as little judgement as possible in between.
//
// It maps each WebTransport session to a session id, forwards control-stream
// messages and datagrams upstream, and forwards link frames back down the
// matching channel. It does not parse the game protocol, does not buffer state,
// and does not make gameplay decisions -- the authoritative server is the only
// thing that does, and keeping that true is what lets this whole process be
// replaced later without touching docs/protocol.md.
//
import { readFileSync } from 'node:fs'
import { Http3Server } from '@fails-components/webtransport'
import { Upstream } from './upstream.js'
import { Impairment } from './netsim.js'
import { createControlReader, LINK } from './framing.js'

export class Edge {
  constructor (opts) {
    this.opts = {
      port: 4433,
      host: '0.0.0.0',
      path: '/dnp',
      upstreamHost: '127.0.0.1',
      upstreamPort: 34500,
      ...opts
    }
    // Impairment sits here rather than in the client so the code under test is
    // untouched. Two independent instances: a real path's uplink and downlink
    // are not correlated, and sharing one would make loss suspiciously
    // symmetric in a way that hides direction-specific bugs.
    this.up = new Impairment({ ...(this.opts.netsim ?? {}), seed: (this.opts.netsim?.seed ?? 1) })
    this.down = new Impairment({ ...(this.opts.netsim ?? {}), seed: (this.opts.netsim?.seed ?? 1) + 7919 })

    this.sessions = new Map() // sessionId -> { session, controlWriter, datagramWriter }
    this.nextSessionId = 1
    this.server = null
    this.upstream = null
    this.log = this.opts.log ?? ((...a) => console.log('[edge]', ...a))
  }

  async start () {
    const { opts } = this

    this.upstream = new Upstream({ host: opts.upstreamHost, port: opts.upstreamPort })
    this.upstream.on('frame', (f) => this._onUpstreamFrame(f))
    this.upstream.on('connected', () => this.log(`upstream connected ${opts.upstreamHost}:${opts.upstreamPort}`))
    this.upstream.on('disconnected', () => {
      this.log('upstream lost; dropping all sessions')
      // The authoritative server is the only thing that knows the state of a
      // session. If the link drops, every session is orphaned -- holding them
      // open would let clients keep sending input into a void and believe they
      // were still playing.
      for (const id of [...this.sessions.keys()]) this._closeSession(id, 1)
    })
    this.upstream.on('link-error', () => {})
    this.upstream.connect()

    this.server = new Http3Server({
      port: opts.port,
      host: opts.host,
      secret: opts.secret ?? 'dnp-edge',
      cert: opts.cert ?? readFileSync(opts.certPath),
      privKey: opts.privKey ?? readFileSync(opts.keyPath)
    })

    this.server.startServer()
    await this.server.ready
    this.log(`listening https://${opts.host}:${opts.port}${opts.path}`)
    if (this.up.enabled) this.log(`netsim active: ${this.up.describe()}`)

    this._acceptLoop().catch((err) => this.log('accept loop ended:', err.message))
  }

  async _acceptLoop () {
    const reader = this.server.sessionStream(this.opts.path).getReader()
    for (;;) {
      const { value: session, done } = await reader.read()
      if (done) break
      this._onSession(session).catch((err) => this.log('session error:', err.message))
    }
  }

  async _onSession (session) {
    await session.ready
    const id = this.nextSessionId++

    const entry = {
      session,
      controlWriter: null,
      datagramWriter: session.datagrams.createWritable().getWriter()
    }
    this.sessions.set(id, entry)
    this.log(`session ${id} open`)

    // NO SERVER BEHIND US: say so, and close.
    //
    // The edge is its own process, so it stays up and accepts WebTransport
    // sessions perfectly happily when the C++ server is down or has exited --
    // and the client, which has genuinely connected, sits on "connected;
    // waiting for WELCOME..." until it times out. That is an accurate message
    // about entirely the wrong thing, and it has now cost a debugging session:
    // the server had refused to start over a missing script and nothing
    // downstream said so.
    //
    // sessionOpen is a fire-and-forget send that silently returns false with no
    // link, so without this the failure has no observer anywhere.
    if (!this.upstream.connected) {
      this.log(`session ${id} rejected: no upstream link`)
      this._closeSession(id, 1, 'no upstream: the game server is not running')
      return
    }
    this.upstream.sessionOpen(id, { path: this.opts.path })

    session.closed
      .catch(() => {})
      .finally(() => this._closeSession(id, 0))

    this._pumpDatagrams(id, session).catch(() => {})
    this._pumpControl(id, session).catch(() => {})
  }

  async _pumpDatagrams (id, session) {
    const reader = session.datagrams.readable.getReader()
    for (;;) {
      const { value, done } = await reader.read()
      if (done) break
      if (!this.sessions.has(id)) break
      // Forwarded whole. One datagram is one protocol message, so there is no
      // framing to do and no reassembly to get wrong.
      const payload = Buffer.from(value)
      this.up.datagram(() => {
        if (this.sessions.has(id)) this.upstream.send(LINK.DATAGRAM, id, payload)
      })
    }
  }

  async _pumpControl (id, session) {
    const streamReader = session.incomingBidirectionalStreams.getReader()
    const { value: stream, done } = await streamReader.read()
    if (done || !stream) return

    const entry = this.sessions.get(id)
    if (!entry) return
    entry.controlWriter = stream.writable.getWriter()

    const reader = stream.readable.getReader()
    const framer = createControlReader()
    for (;;) {
      const { value, done: d } = await reader.read()
      if (d) break
      if (!this.sessions.has(id)) break
      let messages
      try {
        messages = framer.push(Buffer.from(value))
      } catch (err) {
        this.log(`session ${id} bad control framing: ${err.message}`)
        this._closeSession(id, 2)
        return
      }
      // Each complete control message goes upstream individually, so the C++
      // server never has to do stream reassembly of its own.
      for (const m of messages) this.upstream.send(LINK.CONTROL, id, m)
    }
  }

  _onUpstreamFrame ({ type, sessionId, payload }) {
    const entry = this.sessions.get(sessionId)
    if (!entry) return // stale session: a defined no-op, never an error

    if (type === LINK.DATAGRAM) {
      // Copy before deferring. `payload` aliases the link reader's buffer, and
      // netsim can hold it across a timer -- far beyond this synchronous
      // dispatch. Today's reader happens to allocate a fresh buffer per
      // concat, so the view would survive; that is an implementation detail of
      // FrameReader, not a guarantee it makes. A future reader that reuses its
      // buffer would corrupt delayed packets in a way that only appears with
      // netsim enabled, which is the worst place to hide a bug.
      const copy = Buffer.from(payload)
      this.down.datagram(() => {
        const e = this.sessions.get(sessionId)
        if (e) e.datagramWriter.write(copy).catch(() => {})
      })
    } else if (type === LINK.CONTROL) {
      const copy = Buffer.from(payload)
      this.down.reliable(() => {
        const e = this.sessions.get(sessionId)
        if (e?.controlWriter) e.controlWriter.write(copy).catch(() => {})
      })
    } else if (type === LINK.SESSION_CLOSE) {
      this._closeSession(sessionId, payload.length >= 2 ? payload.readUInt16LE(0) : 0)
    }
  }

  // `why`, when given, is closed WITH the session so the browser can print
  // something better than a timeout. A WebTransport close reason is the only
  // channel left once the control stream is not going to carry anything.
  _closeSession (id, reason, why = null) {
    const entry = this.sessions.get(id)
    if (!entry) return
    this.sessions.delete(id)
    this.log(`session ${id} closed (reason ${reason}${why ? `: ${why}` : ''})`)
    this.upstream.sessionClose(id, reason)
    try {
      entry.session.close(why ? { closeCode: reason, reason: why } : undefined)
    } catch {}
  }

  netsimReport () { return { up: this.up.report(), down: this.down.report() } }

  async stop () {
    this.up.stop()
    this.down.stop()
    for (const id of [...this.sessions.keys()]) this._closeSession(id, 0)
    if (this.upstream) this.upstream.stop()
    if (this.server) this.server.stopServer()
  }
}
