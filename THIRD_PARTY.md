# Third-party components

The engine's own code, and the starter module in `modules/starter`, are MIT (see LICENSE).
What it is built with or ships beside it keeps its own licence:

| What | How it gets here | Licence |
|---|---|---|
| LuaJIT (OpenResty's fork) | `scripts/setup_luajit.sh`, pinned, into `third_party/` | MIT |
| three.js | `client/package.json` (npm) | MIT |
| Electron (desktop apps) | the app builds | MIT; Chromium's licences ship inside every Electron app |
| Emscripten (the wasm builds) | `scripts/setup_emsdk.sh` | MIT / University of Illinois |
