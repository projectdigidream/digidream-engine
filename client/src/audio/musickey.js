//
// WHICH ENTRY OF A MUSIC MANIFEST A LEVEL PLAYS (assets/music/index.json).
//
// By the level's NAME ("map05"), which the server sends in WELCOME and HOME. A
// manifest used to be keyed by the map's content hash, and a re-bake changes
// every hash: each re-bake silently unmapped the whole soundtrack, and the
// doomworld copies of the DOOM II maps never had one at all.
//
// A level named `<prefix>_<name>` ("dw_map05") plays `<name>`'s music when it
// has none of its own -- what a module that re-lays another module's levels
// wants. And a hash key still works, for a map nobody named (the studio files
// its modules' music that way).
//
export function musicKeyFor (index, hash, name) {
  if (name) {
    if (index[name]) return name
    const cut = name.lastIndexOf('_')
    if (cut >= 0 && index[name.slice(cut + 1)]) return name.slice(cut + 1)
  }
  if (hash === null || hash === undefined) return null
  const h = hash.toString(16)
  return index[h] ? h : null
}
