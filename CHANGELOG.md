# Changelog

What changed in each version of the engine, for the people who build games
with it. Newest first. Each entry says what a player or a module author would
notice, not how it was done; the commit has the how.

**Keeping it:** every fix, and every change a module author would notice, adds
a line under **Unreleased** in the same commit. A release renames Unreleased to
its version and date and starts a new, empty one. `scripts/release.mjs` puts
the version's section at the top of its GitHub release notes.

Sections: **Fixed** (it was wrong and is now right), **Added**, **Changed**
(it works differently, and a module may need to know), **Removed**.

## Unreleased

## 0.1.4.1 -- 2026-10-15

### Fixed

- **The studio on Android could not reach GitHub**: the shell answers every
  request off the app's own origin with a 404 (so a game reaches nothing),
  and that refused the GitHub sync plugin and the studio's update check too.
  A studio build now lets through https to the hosts its bundled plugins
  declare (`net:<host>`) and api.github.com; a game build still reaches
  nothing.

## 0.1.3 -- 2026-10-15

### Added

- The studio's shells open a web page in the system's browser (`openUrl`,
  https only): the studio's "a new version is out" notice links to its
  release without replacing the studio (Android) or opening a bare window.

## 0.1.2 -- 2026-10-15

### Fixed

- Weapon swings written by the studio (`half_arc = { 1, 8 }`, a fraction) were
  refused by the native weapon table, so the weapons ran unpredicted in Lua:
  hits landed but the first-person weapon never played its swing (the studio's
  Play on Android, Cinderfall).
- Phones in first person: a tap asked for a pointer lock the WebView never
  grants, and "Click to look around" stayed on screen for good. With the touch
  controls up there is no lock, no hint and no desktop legend.

## 0.1.1.1 -- 2026-10-15

A rebuild of 0.1.1 (VERSION stayed 0.1.1) with everything below.

### Fixed

- Pointer lock in the macOS app (Tauri) was always refused, with "requires the
  window to have focus": the web view's delegate never granted it. The runner
  now does, and the pointer is taken back after a cutscene or a modal panel.
- Pointer lock granted over a modal panel (a title screen) is released, so the
  panel's buttons can be clicked.
- Region levels (maps on a metamap grid sharing one world):
  - props placed by a level's scripts come back where they were left when a
    persisted level reloads, and a taken one does not;
  - `when_player_arrives` / `when_player_leaves` fire for the level actually
    walked into, not for every level next door;
  - a level's graphs see its own spawn points and camera paths;
  - a level that failed to load can be entered again once fixed;
  - a player killed across a seam is their own level's death: its respawn,
    its `when_dies`;
  - a fade or music cue sent to a player in the tick they cross a seam reaches
    them.
- Monster melee measures height too: a claw no longer reaches a ledge
  overhead or a body below.
- First-person weapon phases: a phase's `anim` is applied (it was read and
  ignored).
- Graphs: a `sector_move` by tag no longer dies; push straight up pushes up; a
  player is seen arriving after the level stood empty.
- A sliding platform's open side lets a lander aboard instead of shoving it
  away.
- A weapon pickup whose prop was already removed is ignored, not an error.
- HUD images fill their `w` x `h` instead of spilling at their natural size.
- Phones: the level's music starts with the level, without stutter, and
  nothing plays while the app is hidden.
- A server that dies while starting says why on screen, outside the studio
  too.
- Android games start the server with their whole module, not only the entry
  map; desktop games start on the map the packager chose, and Tauri games load
  their whole module.
- Play runs only the module played; modules no longer shadow each other's
  files.
- No music or sound effects in Android games: the synth's worklet shipped
  without the synth it imports and without the sound engine ("Failed to load
  worklet module script"). `build_apk.sh` carries the synth whole or not at
  all (the packager's kits had the same fault; see its changelog).
- `run_stack.sh --module starter` failed from a fresh clone: the web server
  did not look in the engine's own `modules/`.
- `setup_emsdk.sh` installed Emscripten inside the engine while every script
  looks beside it (`../emsdk`); it installs there now.
- mapc refused a map whose declared script was not where its path said, even
  with the script beside the map; it falls back to that copy and warns.

### Added

- First-person weapon animations per phase (docs/VIEWMODEL_PHASES.md).
- Swings drawn in the game, debug or not: a slash streak per body, its look per
  weapon and per monster.
- Typed subtitles: a subtitle node's `typed` mode, with a sound per step.
- Camera: smoothed at speed, and a lead that grows with forward speed
  (`lead_velocity`).
- A `quit_game` graph node, for a main menu's Quit.
- Games can write a log file (`build_apk.sh` / `build_app.sh --log`, the
  packager's "Write a log file").

### Changed

- Fog costs far less to draw: skipped when out of view, a lean depth pass,
  half resolution.
- A missile's own `damage` field is refused at load: it was accepted and never
  read.
- The engine no longer ships DOOM content; the starter module is in
  `modules/starter`. Everything is MIT.
