//
// The edge's link to the authoritative C++ server.
//
// Plain TCP on loopback. It carries link frames (see framing.js) in both
// directions and knows nothing about the game protocol -- deliberately: the
// edge must stay a dumb pipe, or the wire format ends up with two
// implementations that have to agree, which is exactly the ecosystem hazard
// design section 2 is trying to avoid.
//
import net from 'node:net'
import { EventEmitter } from 'node:events'
import { encodeLinkFrame, createLinkReader, decodeLinkFrame, LINK } from './framing.js'

export class Upstream extends EventEmitter {
  constructor ({ host = '127.0.0.1', port = 34500, reconnectMs = 500 }) {
    super()
    this.host = host
    this.port = port
    this.reconnectMs = reconnectMs
    this.socket = null
    this.connected = false
    this.stopped = false
    this.reader = createLinkReader()
  }

  connect () {
    if (this.stopped) return
    const socket = net.connect({ host: this.host, port: this.port })

    // Nagle batches small writes to save header overhead. That trade is exactly
    // wrong here: every frame on this link is small and latency-critical, and
    // holding a snapshot back for up to 40 ms to coalesce it would add more
    // delay than the entire network path it is trying to optimise.
    socket.setNoDelay(true)

    socket.on('connect', () => {
      this.socket = socket
      this.connected = true
      this.reader = createLinkReader()
      this.emit('connected')
    })

    socket.on('data', (chunk) => {
      let frames
      try {
        frames = this.reader.push(chunk)
      } catch (err) {
        this.emit('error', err)
        socket.destroy()
        return
      }
      for (const f of frames) this.emit('frame', decodeLinkFrame(f))
    })

    const retry = () => {
      if (this.connected) this.emit('disconnected')
      this.connected = false
      this.socket = null
      if (!this.stopped) setTimeout(() => this.connect(), this.reconnectMs)
    }

    socket.on('close', retry)
    socket.on('error', (err) => {
      // A refused connection while the C++ server is restarting is routine, not
      // an incident. Surface it once at debug level and let the retry handle it.
      this.emit('link-error', err)
    })
  }

  send (type, sessionId, payload) {
    if (!this.connected || !this.socket) return false
    return this.socket.write(encodeLinkFrame(type, sessionId, payload))
  }

  sessionOpen (sessionId, info) {
    return this.send(LINK.SESSION_OPEN, sessionId, Buffer.from(JSON.stringify(info)))
  }

  sessionClose (sessionId, reason = 0) {
    const b = Buffer.allocUnsafe(2)
    b.writeUInt16LE(reason, 0)
    return this.send(LINK.SESSION_CLOSE, sessionId, b)
  }

  stop () {
    this.stopped = true
    if (this.socket) this.socket.destroy()
  }
}
