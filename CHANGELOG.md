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

### Fixed

- **A fade to black hid the subtitles.** The subtitle line is now drawn over
  the fade and the letterbox bars, as the studio's scene preview shows it.
- **A long subtitle lost its last lines.** HUD text was cut to 128 bytes,
  about three lines at a large size. The limit is now 1024 bytes, and a line
  is cut at a whole letter.
- **Q and R did not turn the view in first person**, though they turn the
  camera in the other control schemes. They now turn it, as the arrow keys do.
- **Walls dithered before they hid the player**, from some camera angles. The
  window that fades what stands in front of the player was as wide near the
  camera as at the player, so near the camera it covered much more of the
  screen than the player. It is now a cone: on screen, the player's outline
  and a margin, at any depth and zoom.

## 0.2.3 -- 2026-10-20

Everything since 0.2.2. (The development releases 0.2.2.1 and 0.2.2.2 below
had it piece by piece.)

### Added

- **Aim assist chosen per weapon**: `autoaim = true` turns it on even in first
  person (the shot turns to the nearest body in the cone and climbs or drops
  to its exact middle; with the cone empty it goes where the view looks),
  `autoaim = false` turns it off, and `autoaim_cone` (degrees) gives the weapon
  its own cone. Without either the rule is unchanged. Decided in the core, so
  the shooter's client predicts it.
- **A wall's texture scaled along it and up it on its own**: `xscale` and
  `yscale` on its `vertex` line, beside `xoff` and `yoff` (`scale` still sets
  both). A map that uses it is `.dnpm` version 16; every other map is written
  and loaded exactly as before.
- **A studio APK updates itself**: `dnp.studio.selfUpdate(url)` downloads the
  newer APK and installs it through Android's installer. A studio's manifest
  asks for REQUEST_INSTALL_PACKAGES; a game's never does. Every studio APK
  carries its version as versionCode (0.4.2 is 40200), so a later one installs
  over an earlier one.

### Changed

- **The starter module** shows the player's death on every map, says its keys
  once as the game starts, has its debug overlay off, draws its spirit twice
  the size with a gentler bite, carries a muzzle flash, a tracer and hit sparks
  (particle kinds 20-22), and its door opens on E and stays open four seconds.

## 0.2.2.2 -- 2026-10-20

### Added

- **Aim assist chosen per weapon**: `autoaim = true` turns it on even in
  first person. The shot turns to the nearest body in the cone and climbs or
  drops to that body's exact middle; when the cone is empty, it goes where
  the view looks. `autoaim = false` turns it off. `autoaim_cone` (degrees)
  gives the weapon its own cone in place of the game's 5.625. Without either
  field the rule is unchanged. The core decides it, so the shooter's client
  predicts it.

## 0.2.2.1 -- 2026-10-20

### Added

- A wall's texture can be scaled differently along it and up it: `xscale` and
  `yscale` on its `vertex` line, beside `xoff` and `yoff` (`scale` still sets
  both). A map that uses it is `.dnpm` version 16; every other map is written
  and loaded exactly as before.
- **A dev studio APK updates itself** (studio versions with four parts only):
  `dnp.studio.selfUpdate(url)` downloads the newer APK and installs it through
  Android's installer. Only a dev studio's manifest asks for
  REQUEST_INSTALL_PACKAGES. Every studio APK now carries its version as
  versionCode (0.4.1.1 is 40101), so a later one installs over an earlier one.

### Changed

- **The starter module** shows the player's death on every map (a game graph:
  the screen goes dark, a line is said, it clears), says its keys once as the
  game starts, has its debug overlay off, draws its spirit twice the size with a
  body to match and a gentler bite, carries a muzzle flash, a tracer and hit
  sparks (particle kinds 20-22), and its door opens on E and stays open four
  seconds (E no longer shuts it).

## 0.2.2 -- 2026-10-17

### Fixed

- **A link out of the Android app trapped it.** Every address off the app's
  own was answered with "Digidream has no page at ...", so a link the user
  followed (the studio's Package page offered one for a kit) turned the whole
  app into that page, and Back sent the app away with the page still in it.
  A link out now opens in the phone's browser, and Back on such a page goes
  back.
- **The desktop app said its game server had stopped when it had not.**
  Starting a play-test right after another, or the first after making or
  changing a module, could show an error box quoting the previous server's
  log, drop back to the studio, or quit the app. The previous server's normal
  stop, which comes in late, was taken for the new one failing.
- A monster's `melee` reached through walls and shut doors to whoever stood
  behind them. It now needs a clear reach, as a swing does; a ledge does not
  stop it, its own height test judges that.
- A trigger with the `entity_in_sector_tag` condition raised an error on every
  event it judged, and cut short whatever had set the event off -- a monster's
  attack on the player among them.
- `mobs.hurt(target, amount, attacker)` raised an error when given the
  attacking entity (as a module's own `on_attack` hands it over) and the target
  fights back. It takes an entity now, as well as a monster's record.
- In the starter module, and every module made from it, each swing that landed
  cost twice: its own script charged one beside the engine's. It now sets what
  a player's swing costs instead (`mobs.player_damage`).
- A missile flying under a platform burst on nothing the moment it came down
  at all: the platform's floor, above it, was taken for its own.
- A missile that gives no `z_offset` started at its shooter's feet, and at that
  height every step and sill it crossed stopped it: walls nobody could see. It
  now starts at the shooter's middle, as Doom launches.

### Added

- `when_dies` and `when_hurt` work in a module's game graph, for a death on
  any map: one graph of the module's can answer a player's death everywhere --
  a death screen -- a map drawn later included. `by` is empty there when the
  killer is out of reach.
- A time scale, for automated testing: `--time-scale 0.25` on the server, or
  `DNP_TIME_SCALE=0.25` in the environment of whatever starts it (the desktop
  app, `run_stack.sh`), runs the game at a quarter speed in real time -- or up
  to eight times faster. Nothing simulates differently; the client is told and
  keeps pace, and its numbers block shows `time x0.25`. The solo page takes
  `?timescale=`. A page publishes its prediction counters for a test to read
  (`__dnpStats.prediction()`).

- A missile can trail particles for its whole flight: `trail = <particle kind>`
  on the missile. It comes out from launch until the missile bursts or expires,
  through bounces, and on the shooter's own predicted rocket too, at the
  kind's own rate and look. `carry 1` on a particles line moves the specks with
  the body that let them out, a flame around a fireball, instead of leaving them
  behind as smoke. Scripts can hang a trail on any body: `e:set_trail(kind)`.

## 0.2.1.2 -- 2026-10-17

### Added

- Particles can be drawn as something other than a soft dot: `shape disc`,
  `square`, `hexagon` or `star` on a `vfx ... particles` line, hard-edged; or
  `sprite <name>`, one of the module's own pictures (sprites/, then textures/).
  Both are tinted by the colour ramp, so one white picture makes a burst of any
  colour, and a picture's own transparency cuts its outline. A line that says
  nothing draws as before. The sprite wins when both are given.

## 0.2.1.1 -- 2026-10-17

### Added

- A swing is an attack like any other: the `swing` action takes the swing
  fields as its own arguments (`damage`, `reach`, `half_arc`, `sweep_from`,
  `sweep_to`, `ticks`, `knockback`, ...), for monsters and weapons alike.
  Given, they are that swing's; left out, the monster's or the weapon's `swing`
  applies, so existing modules swing as they did. A jab and a slam are now two
  steps with their own numbers. docs/SWINGS.md.
- Weapons can run `swing` in a phase's sequence, at its tick, like `hitscan`.
  The player's own client predicts it, so it appears the moment it happens. A
  weapon with a swing step does not swing on the attack button: the button
  runs its phases and the steps say when.
- Other players see a swing step's own shape, not the weapon's: the snapshot
  names the step (`ATTACK_STEP`) and every client already holds the weapon
  table.

### Changed

- A swing that starts while another is still sweeping replaces it: the first
  stops where it is, and the second starts fresh and can hit the same target
  again. A swing inside the last one's cooldown does not happen. (A monster's
  second swing mid-sweep used to be dropped.)
- A weapon whose swing step would cost nothing, with no `damage` of its own and
  no `swing` to take one from, is refused at load. A monster's `swing` step is
  refused the same way, and no longer needs a `swing` field when it gives its own
  `damage`.

### Fixed

- A monster's `swing` field offered `pitch_from`, `pitch_to` and `half_pitch`,
  and nothing wrote them: an overhead blow swung flat. They now shape the
  swing as a weapon's do.

## 0.2.1 -- 2026-10-17

### Fixed

- Android: Back quit the app from wherever it was pressed. It now goes back
  in the page -- a dialog closes, the studio's previous page, a play-test
  back to the studio -- and only where there is nowhere further back does the
  app go to the background, keeping its place for when it comes back.
- Android: the keyboard covered the field being typed in, and a tap meant for
  the next field typed into this one. Full screen, Android never shortened
  the window for the keyboard; the page is now lifted by its height.
- Touch: a quick tap on a button (FIRE) was lost when it went down and up
  between two input samples, one tap in two on a slow phone. A press is now
  seen by at least one sample.

### Added

- Markers: small UI hung on things in the world, such as a health bar over a
  monster, a sign over a door or a pointer over a goal (docs/MARKERS.md).
  Each is drawn from a template in the module's UI layout (a new `markers`
  section of the HUD's own elements; `health`, `label`, `marker` and
  `debug_health` are built in). It follows an entity or sits at a place, and
  is hidden behind walls, out of `range`, or when not meant for this player.
  - Lua: `dnp.mark`, `dnp.mark_set`, `dnp.unmark`, `dnp.debug_overlay`.
  - Graphs: the `mark`, `set_marker` and `remove_marker` nodes.
  - Monsters: `health_bar = "health"` keeps a bar over a monster while it
    lives.
- With the module's debug overlay on, every monster shows its health as a
  counter over it while the overlay is showing. Nothing is sent when the
  module ships with `debug off`.

## 0.2.0 -- 2026-10-16

### Fixed

- Client prediction: a prediction made from a different input than the one
  on record (protocol R1) went unnoticed when the next snapshot answered past
  that tick before any replay ran it. Whether it was caught depended on the
  client's lead over the snapshot interval, so `prediction.test.js` failed
  about half the time. Every tick the server consumed since the last snapshot
  is now checked against the input its prediction came from.
- A player's own rocket jump mispredicted whenever the server ran a tick on a
  repeated input during the rocket's flight: the rocket flies in server ticks,
  the client times the shove in its input ticks, and the shove landed early on
  the server. It now waits for the owner's input (new `e:input_tick()`).
- On an uneven link (a busy machine, a slow phone) the server kept running out
  of the client's input, and every repeated tick moved the rest a tick apart.
  When the server keeps running dry (repeats in two or more of the last four
  seconds' snapshots), the client's input buffer grows by a tick per extra
  repeat, up to four, and shrinks back as they age out. One hitch, or a link
  that never runs dry, keeps the two-tick buffer it had.

- A monster's `set_param` added an effect every time it ran and never took
  one away. The effect stack holds 256 for the whole world, so a monster
  setting its speed on entering each of its states used it up within minutes,
  and from then on every effect anywhere (a weapon's swing, a slow) silently
  failed. It now replaces its own last value.
- A saved game brought monsters back in their first state with fresh vars, so
  a boss's "only once" phase change (a hidden var plus the phase's states)
  was undone by every reload. A save now keeps each monster's state, where it
  was in it, and its vars, cooldowns as the time left. It resumes rather than
  re-enters, so a `set_cooldown` on the way into a phase does not run again.
- mobs.lua's own runtime (monsters not on the C++ loop) did two things the
  C++ loop does not, found by running a teleporting, slamming monster through
  both:
  - A monster whose action sent it to another state, such as flinching from
    its own blast, carried on with the rest of the old sequence from the
    start, so it slammed twice.
  - A turn that an action made without sending it was then skipped, so the
    monster faced the old way.

  Both now match the C++ loop.
- A monster's volley fired due east (aim near 0) flew its left-hand shots
  along the middle one: the offsets made the heading negative, which reads
  as 0. A mancubus's three shots were two lines.

### Changed

- Missile `gravity` is a fall, in units a second squared, acting on a vertical
  speed: a missile with it flies an arc. It used to be added to the height
  every tick, a steady climb. A monster throws such a missile on the one arc
  that lands on its target (`speed` is horizontal); a player's starts along
  their aim, and their own client flies the same arc. Cinderfall's
  `slag_glob` (`gravity = "9"`) now falls as it was written to.
- `weapons.lua` costs about a third less per tick (median 99 -> 64 us for one
  player on DOOM II): the sorted ammunition list is kept, the HUD and predicted
  sounds compare values instead of building strings, the inventory is handed
  to the core only when it changed, and `dnp.weapon_step` / `dnp.weapon_state`
  stop building fresh tables every tick (`weapon_state(e, into)` refills one).

### Added

- A monster's `pain` can go back to what it was doing: `next = "@previous"`
  returns to the state the hit interrupted and resumes it where it was, its
  elapsed time and its sequence, so a hit during a barrage or a summon does
  not run it again from the start. A boss with phases is no longer sent back
  to phase 1 by every hit. Works in any state a module enters as a reaction,
  in both mob runtimes (docs/MOBS.md).
- Weapon conditions take `negate = true`, as a monster's do: "the counter is
  not 0" is `var_is` with `negate`, without falling back to `lua`. Predicted
  like the rest of the weapon (docs/WEAPONS.md).
- Weapons have cooldowns, the monster pair: `set_cooldown` writes a deadline
  into a var and `cooldown_ready` waits for it. A BFG can fire once every 96
  ticks without its firing phase lasting that long, so the player can still
  switch weapons. A cooldown outlives putting the weapon away; a respawn
  clears it. Predicted on the client like the rest of the weapon. The weapon
  block of a snapshot grows by a byte (and four per cooldown in use), so a
  client and a server must both be this version.
- Monsters can teleport: `teleport` with `to = "behind"` (the target's back,
  or a little to one side of it) or `"near"` (a random spot around it that
  it could walk to the target from), an `effect` at both ends, facing the
  target on arrival. It only lands where its body fits (`dnp.place`, new:
  a floor, headroom, no wall or body in the way). Clients draw any teleport as
  a jump rather than a slide (`ent_field::TELEPORTED`, new), including the
  ones maps and scripts already did.
- Missiles can make missiles: a `missile` action in a missile's `on_hit`,
  `on_expire` or new `sequence` (actions by ticks of flight) launches from
  where it is, on its heading (`towards = "back"` / `"target"`), within a
  per-shot budget (`max_children`, default 16).
- Aiming: `lead = true` on a monster's `missile` aims where a moving target
  will be. `homing_after` delays a homing missile's turn and `homing_cone`
  makes it give up a target too far round. A player's homing missile locks
  onto what their auto-aim picks; their client leaves it to the server rather
  than flying a straight copy. `fx.sqrt`, the core's square root, for scripts.
- Missiles can bounce: `bounces = N` off walls (the heading reflected about
  the wall), floors and ceilings (the climb reversed), keeping `bounce_keep`
  of their speed each time and running `on_bounce` there. `throw = "low"` /
  `"high"` makes a gravity missile's `speed` its launch speed, and a monster's
  throw takes the flat angle or the lob that lands. `dnp.sweep_hit` now also
  returns what the map stopped it with: a wall's normal, or floor/ceiling.
- Wall textures can be turned and scaled like a flat's: `angle` (degrees) and
  `scale` on a wall's `vertex` line, turned about the side's texture origin,
  then shifted by `xoff`/`yoff`, then scaled (docs/MAP_AUTHOR.md, "Walls").
  `.dnpm` version 15, written only when a wall uses them, so every existing map
  stays byte for byte what it was. The studio's side is briefed in
  docs/WALL_STYLES.md.
- Sprites from one ThreeSprite sheet that are on screen together draw as one
  buffer: one draw call per sheet instead of one per sprite (Gem Hollow's
  level: 220 draws -> 54, the picture unchanged). A sheet with one sprite on
  screen, a sprite mid-cross-fade and per-PNG (DOOM) sprites draw as before.
  `?batch=0` turns it off; the debug overlay says what was batched.
- `--perf` reports the Lua time by callback ("on_tick in weapons.lua"), across
  the game's VM and every level's.

## 0.1.5 -- 2026-10-15

No changes to the engine itself: its kits, built again for studio 0.3.4.

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
