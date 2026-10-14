# Digidream Engine

A small multiplayer game engine for sector-based levels -- rooms with floors and
ceilings at any height, doors, lifts, platforms -- built around one promise: **every
machine computes exactly the same world**. The simulation is fixed-point C++, so a
server on arm64 and a player on x86-64 or in a browser (WebAssembly) agree bit for bit,
tick for tick. That is what lets the client predict instead of wait.

- `core/` -- the deterministic simulation: physics, sectors, bodies, sight, navigation
- `server/` -- the authoritative server (C++, with LuaJIT for map and game scripts) and
  the WebTransport edge that browsers connect to (`server/edge`, Node)
- `client/` -- the browser client: rendering (three.js), prediction, HUD, sound, music
- `tools/` -- the map compiler `mapc` and the checkers the tests use
- `shells/` -- desktop (Electron) and mobile (Android, iOS) shells
- `modules/starter` -- **the starter module**: a room, a door, a pit, a platform and a
  spirit. Everything below runs it.

Games are **modules**: a folder of maps, scripts, textures, sprites, sounds and music,
named on the command line (`--module starter`). The engine itself carries no game.

## Get started with the starter module

Prerequisites: a C++17 compiler (clang), Node 22, OpenSSL, git and bash. The two
pinned dependencies, Emscripten (for the browser builds) and LuaJIT (for the
server's scripting), are fetched by scripts.

Clone into a folder of its own, because Emscripten and the other Digidream
repositories go BESIDE the engine, not inside it:

```sh
mkdir digidream && cd digidream
git clone https://github.com/projectdigidream/digidream-engine.git engine
cd engine
./scripts/setup_emsdk.sh          # Emscripten, pinned, into ../emsdk
./scripts/setup_luajit.sh         # LuaJIT, pinned
source ../emsdk/emsdk_env.sh      # in every new shell that builds wasm
```

Build the server, the client's wasm core and the map compiler, and install the
Node packages:

```sh
./scripts/build_server.sh
./scripts/build_wasm.sh
./scripts/build_native.sh native mapc    # the map compiler, for this machine
(cd client && npm ci) && (cd server/edge && npm ci)
```

Make the development certificate. Browsers only speak WebTransport over TLS,
even to localhost; this makes a self-signed one the page is told to trust. It
lasts 13 days: run it again when the client stops connecting.

```sh
(cd server/edge && npm run certs)
```

Run the starter module -- server, WebTransport edge and the web page:

```sh
./scripts/run_stack.sh --module starter
```

and open the address it prints (`http://localhost:8080/?server=...`), in Chrome
or another Chromium browser (WebTransport with a self-signed certificate).

## Digidream Studio from source

The studio (github.com/projectdigidream/digidream-studio) runs on this engine's
code, so from source it needs the two side by side, as `engine` and `studio`:

```sh
cd ..                                    # the folder that holds engine/
git clone https://github.com/projectdigidream/digidream-studio.git studio
cd engine
./scripts/build_mapc_wasm.sh             # the map compiler, for the studio's bakes
./scripts/run_stack.sh --module starter
```

then open `http://localhost:8080/studio/`. Its own README has the rest.

## Make it an app

Every packaging script builds the starter module unless another is named:

```sh
./scripts/make_app.sh                 # a desktop app (Electron), in a folder
./scripts/make_app.sh --package       # ...and its installer
./scripts/build_apk.sh                # an Android APK, debug-signed, for sideloading
./scripts/build_ios.sh                # an iOS build (needs Xcode)
```

`--module <name>` picks another module: one beside the engine (`../modules/<name>`) or
in its own `modules/` folder.

## Make your own game

Copy `modules/starter` to a new folder, give it a new `id` and `name` in its
`module.txt`, and change its maps and scripts. Or use **Digidream Studio**, the editor
built on this engine (github.com/projectdigidream/digidream-studio), which starts every
new module from this one and packages it for every platform.

## Tests

```sh
./scripts/test.sh            # the everyday run: guards, core, tools, Lua, client
./scripts/test.sh --list     # every suite and what it covers
./scripts/check_determinism.sh   # native arm64, native x86-64 and wasm must agree
```

Some of the engine's tests exercise its monster and line-special runtimes with
fixtures that are not part of this repository (a private module and its bestiary).
Where they are absent those checks cannot run.

## Licence

MIT -- see LICENSE. Third-party parts keep their own licences: THIRD_PARTY.md.
