//
// Following moving sectors, and knowing when they have stopped.
//
// A sector that moves changes the shape of the mesh, so the renderer has to
// re-derive it. The engine owns the heights; this copies them into the render
// model and answers the only question the frame loop actually needs:
//
//     did anything move THIS frame?
//
// WHY THAT QUESTION IS WORTH ASKING. `movers` is append-only for the life of a
// map (main.js `takeMotion`), because a sector that has moved once will very
// likely move again and re-deriving the static/dynamic split per door would be
// a visible stutter. The consequence is that after the first door in a level is
// opened, `movers.length > 0` for ever -- so a rebuild guarded only on that
// runs every frame until the map changes, whether or not a single height has
// changed since. Re-deriving surfaces, re-merging them by texture, allocating
// fresh attribute buffers and re-uploading them to the GPU, sixty times a
// second, to produce the geometry that is already on the GPU.
//
// WHY ONLY `movers` AND NOT THE WHOLE DYNAMIC SET. MapView.setDynamicSectors
// puts each mover's NEIGHBOURS in the dynamic half too, because a wall's upper
// and lower fields are drawn from the neighbouring sector's heights. It is
// tempting to conclude that this should therefore watch the dynamic set. It
// should not, and the reason is worth keeping: a neighbour that is not itself a
// mover cannot change height -- nothing moves it -- so watching it can only
// ever report "unchanged". The movers are the complete set of things that can
// differ from one frame to the next, and rebuilding covers their neighbours
// anyway.
//
// Kept out of main.js and free of both wasm and THREE so it can be driven by a
// scripted door in a test, which is the only way to assert "rebuilt 60 times,
// not 600" without a GPU.
//

/**
 * Copy the engine's live sector geometry into the render model.
 *
 * @param movers    sector indices that have ever been declared as moving
 * @param readLive  (index) -> { floorZ, ceilZ, points?, motion? }
 * @param sectors   the render model's sector array, mutated in place
 * @param points    the render model's shared point array, mutated in place
 * @returns whether anything differs from what was already there
 */
export function followMovers (movers, readLive, sectors, points) {
  let moved = false
  for (const i of movers) {
    const live = readLive(i)
    const s = sectors[i]
    if (!s) continue
    // BOTH heights, and a door is why. A Doom door is a ceiling that drops to
    // the floor and rises again; its floor never moves. Comparing only floorZ
    // passes every "does it rebuild while moving" test that uses a lift, and
    // freezes every door in the game.
    if (s.floorZ !== live.floorZ || s.ceilZ !== live.ceilZ) {
      s.floorZ = live.floorZ
      s.ceilZ = live.ceilZ
      moved = true
    }

    // AND THE FOOTPRINT, which for a long time nothing copied at all.
    //
    // Heights are the whole story for a lift, a door and a crusher, and they
    // are exactly half of it for a sector that slides or turns: the engine
    // moved its points, the client's simulation moved its points, and the
    // render model went on holding the ones the map was baked with. A sliding
    // platform was solid where it had arrived and DRAWN where it started.
    //
    // Guarded on the reader offering any, so a caller that only has heights --
    // and every test of the rebuild budget is one -- behaves as it always did.
    if (points && live.points) {
      for (let k = 0; k < live.points.length; k++) {
        const p = points[s.firstPoint + k]
        const q = live.points[k]
        if (!p || (p.x === q.x && p.y === q.y)) continue
        p.x = q.x
        p.y = q.y
        moved = true
      }
    }

    // How the sector is PLACED, for a flat painted on it rather than on the
    // world. Not geometry -- the points above already carry the movement -- but
    // the frame a texture anchored to this sector is evaluated in.
    const m = live.motion
    if (m) {
      const was = s.motion
      if (!was || was.offX !== m.offX || was.offY !== m.offY ||
          was.turns !== m.turns || was.pivotX !== m.pivotX || was.pivotY !== m.pivotY) {
        s.motion = m
        moved = true
      }
    }
  }
  return moved
}
