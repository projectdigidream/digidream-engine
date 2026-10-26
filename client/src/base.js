//
// WHERE THE GAME'S FILES ARE, as a path prefix: textures/, sprites/, props.txt
// and the rest are fetched as BASE + their name.
//
// The site's root, which is where every shell serves them (the dev server, the
// desktop app, the Android app). A static web build (docs/WEB.md) can be hosted
// under a path instead -- a GitHub Pages project site is https://user.github.io/
// game/ -- so its index.html sets DNP_BASE to its own directory.
//
export const BASE = globalThis.DNP_BASE ?? '/'
