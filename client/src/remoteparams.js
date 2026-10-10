//
// OTHER BODIES' PARAMETERS, as the server sent them (docs/protocol.md 10.1).
//
// A PARAM_UPDATE names the body it is about. The client's OWN body's go into
// the world it predicts with; everybody else's come here -- a monster's swing,
// another player's weapon -- so the client can draw what they do with the
// shape their parameters give it. Applying them to the client's own body, as
// it once did whatever the handle said, made another player's weapon change
// reshape YOUR swing.
//
// Kept by entity index, with the handle's generation: a slot reused by a new
// body starts from the defaults rather than inheriting the last one's. A
// field never sent is the engine's default (core paramDefaultRaw).
//
const FULL = 1   // proto param_flag::FULL

export class RemoteParams {
  /** `defaultRaw(id)` -> BigInt, `paramId(name)` -> id: the client core's. */
  constructor ({ defaultRaw, paramId }) {
    this.defaultRaw = defaultRaw
    this.paramId = paramId
    this.bodies = new Map()   // index -> { gen, fields: Map<id, BigInt> }
  }

  /** Take one PARAM_UPDATE about somebody else. */
  take (msg) {
    const index = (msg.handle & 0x00ffffff) - 1
    const gen = msg.handle >>> 24
    if (index < 0) return
    let b = this.bodies.get(index)
    if (!b || b.gen !== gen || (msg.flags & FULL)) {
      b = { gen, fields: new Map() }
      this.bodies.set(index, b)
    }
    for (const f of msg.fields) b.fields.set(f.id, BigInt(f.raw))
  }

  /** Forget a body (it was destroyed). */
  drop (index) { this.bodies.delete(index) }

  /**
   * One body's parameters in the shape render/swing.js readSwingShape reads
   * a world with: getParamRaw(_, id) and paramId(name).
   */
  view (index) {
    const b = this.bodies.get(index)
    return {
      getParamRaw: (_idx, id) => {
        const v = b?.fields.get(id)
        return v !== undefined ? v : this.defaultRaw(id)
      },
      paramId: (name) => this.paramId(name)
    }
  }
}

/** Is this PARAM_UPDATE about the client's own body (entity index `self`)? */
export const isOwnParams = (msg, self) => ((msg.handle & 0x00ffffff) - 1) === self
