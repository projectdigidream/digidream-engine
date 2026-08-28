import { existsSync } from 'node:fs'

/** Path to the baked test map, whichever arch directory it was built into. */
export function bakedMapPath (root) {
  if (process.env.DNP_MAP) return process.env.DNP_MAP
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = `${root}/build/${arch}/test.dnpm`
    if (existsSync(p)) return p
  }
  return null
}

/**
 * Mirror the bodies and props in a snapshot into a predicting client's world.
 *
 * A predicting client has to stop where the server will stop it, and the server
 * stops it against three things: geometry, solid props, and other BODIES. It
 * parses the geometry itself, so what is left is the entities -- and a client
 * that ignores them walks through a barrel, or through a monster, and then
 * mispredicts every tick it is overlapping one. That has now been diagnosed
 * twice as a determinism fault when it was a client missing a collider, which
 * is why this is one function instead of a loop copied into each test.
 *
 * Bodies go in the reserved prop band (`mirrorBase`, from the engine): they need
 * to be cylinders this world stops at, and they must NOT be simulated by it --
 * a remote body is interpolated, never predicted.
 *
 * `kinds` is carried across calls because `display_id` is a delta-encoded field:
 * it arrives on the tick an entity appears and never again.
 */
export function mirrorEntities (world, snap, {
  catalogue, kinds, mirrorBase, selfIndex = null,
  bodyRadius = 0.5, bodyHeight = 2, maxAgents = 64, ENT_FIELD, handleIndex
}) {
  for (const e of snap.entities ?? []) {
    const index = handleIndex(e.handle)
    const isBody = index < maxAgents
    if (e.mask & ENT_FIELD.DESTROYED) {
      kinds.delete(index)
      world.removeProp(isBody ? mirrorBase + index : index)
      continue
    }
    if (e.displayId) kinds.set(index, e.displayId)
    if (!(e.mask & ENT_FIELD.POS)) continue

    if (isBody) {
      if (index === selfIndex) continue
      // Solid is delta-encoded: it arrives on the tick it changes and never
      // again, so the remembered value is what counts. A corpse is drawn and
      // not walked into.
      if (e.mask & ENT_FIELD.SOLID) kinds.set(`solid:${index}`, e.solid !== 0)
      if (kinds.get(`solid:${index}`) === false) {
        world.removeProp(mirrorBase + index)
        continue
      }
      // A monster's size is its catalogue entry; a player's is the map's
      // declared body. Kind 0 is a player.
      const kind = kinds.get(index)
      const def = kind ? catalogue.kinds.get(kind) : { radius: bodyRadius, height: bodyHeight }
      if (!def) continue
      world.placeProp(mirrorBase + index, 0, e.pos,
                      { radius: def.radius, height: def.height, flags: 1 /* SOLID */ })
      continue
    }

    const kind = kinds.get(index)
    const def = kind ? catalogue.kinds.get(kind) : null
    if (def) world.placeProp(index, kind, e.pos, def)
  }
}
