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

### Added

- **Movement states, defined by data** (docs/MOVEMENT.md, step 1 of 5). A
  module declares up to 8 states with `movestate` lines in its module.txt:
  - a group: `floor`, `air`, `liquid` (3D, JUMP up and CROUCH down) or `none`;
  - the numbers that differ from the player's own: speed, accel, friction,
    gravity, buoyancy, jump;
  - flags.

  `transition` lines take a body in and out: grounded or airborne, rising or
  falling, a button pressed, held or released (crouch included), speed above
  or below, time in the state, the floor's tag. They can jump, stop or give an
  impulse as they happen. A crouch-slide is three lines. The client predicts
  all of it: a slide on a live server mispredicted nothing. State 0,
  `default`, is the movement as it always was. The debug overlay shows the
  state.
- **Liquids, and how deep a body stands in them** (docs/MOVEMENT.md, step 2
  of 5). `sector type liquid` is an effect sector that is water, its `ceil`
  the surface (map version 18, only for a map with one). Transitions can ask
  `depth none|feet|waist|eyes` and `depth below <level>`. In a `liquid` state
  buoyancy lifts only while the water is at the waist or above, and swimming
  up stops at the surface. Wading, swimming and climbing or jumping out of a
  pool are seven lines; `maps/pool.map` is a pool to try them in.
- **Meters** (docs/MOVEMENT.md, step 3 of 5): up to 8 numbers per body, named
  by the module (breath, stamina), with `meter <name> max <n> [start] [rate]`
  lines. A state, with conditions or without, can set a meter's rate.
  Transitions ask `meter <name> empty|full|below|above` and can
  `do meter <name> add|set <n>`. Predicted, like the states. Breath that runs
  out under water, and a sprint that costs stamina, are a few lines each.
- **Layered moves** (docs/MOVEMENT.md, step 4 of 5): `move <name> add|set
  facing|back|up|down <speed> for <seconds>` lines, with a priority, a meter
  cost and `when` conditions that start them. They hold over any state and
  through a change of state, one per axis. A transition can `do move <name>`.
  A dash that costs stamina, a jump pad and a boost into a slide are one line
  each. Predicted.
- **Movement states for scripts, graphs, animation and the HUD**
  (docs/MOVEMENT.md, step 5 of 5):
  - Lua `e:movement_state()`, `e:ask_movement_state(name)`, `e:meter(name)`,
    `e:set_meter(name, v)`, `e:start_move(name)`.
  - Graph nodes for the same, and the start nodes
    `when_movement_state_entered` / `_left` and `when_meter_empty` / `_full`.
  - The `asked` and `touching wall` conditions.
  - A state's `anim` plays when the avatar names it (`avatar <id> when swim
    state 7`).
  - The HUD bindings `meter.<name>` and `meter.<name>_max`.

## 0.2.3.10 -- 2026-10-26

### Added

- **Props, monsters and players are lit by the map**, as its walls and floors
  are: their sector's light level, plus the lamps near them added on top. A
  monster in a dark room is dark, and one beside a torch takes its colour. A
  kind marked `bright` on its `prop` line is its own light and is drawn at
  full brightness: DOOM II's projectiles and the lost soul are. `?proplight=0`
  draws everything unlit, as before, for comparing. The server reads past
  `bright` as it does `scale`, so a server older than this drops any prop that
  has it.

## 0.2.3.9 -- 2026-10-26

### Fixed

- **The automatic camera could show what is behind a wall** when it was
  pulled in close to one. With walls on, it now has a size: a ball 0.35 units
  across that it keeps clear of the map. It is checked both where the camera
  wants to be and where its easing has taken it, so it cannot cut through a
  corner on the way. Its near plane is smaller while walls are on (0.15), so
  the picture starts inside that ball. It also no longer holds a minimum
  distance of one unit from the player, which could put it behind a wall
  right behind the player.

- **Saving a deeply nested table could write past the end of Lua's stack.**
  The save encoder and decoder (`dnp.persist`, saved levels, cross-VM calls)
  pushed three values per level without asking Lua for room, and Lua promises
  only twenty. A table seven levels deep could damage the server's memory.
  They now ask, and refuse with "out of Lua stack" if Lua cannot give it.
- **A negative angle passed from Lua was undefined behaviour**: `dnp.dir`, a
  prop's facing, an effect's direction and a scripted aim. The angle is now
  wrapped to a turn first, so a negative angle turns the other way.

### Changed

- **Scripts should keep to Lua 5.1**: no `goto`, no `ffi`, no `jit`. The
  single-player web build (below) runs its server on PUC Lua 5.1, because LuaJIT
  cannot run in WebAssembly. The `bit` library stays, bundled there too. The
  engine's own scripts keep to it, the one `goto` in `mobs.lua` is gone, and
  `scripts/test.sh lua51` runs the Lua tests on it (`scripts/setup_lua51.sh`
  installs it).

### Added

- **The server in the page, for single-player web games**: the real server,
  C++ and Lua, compiled to WebAssembly and run in a Web Worker beside the
  page. Nothing runs anywhere else, so any static host can serve the game,
  GitHub Pages included. `?local` on the page turns it on. With it, the dev
  server alone, given its `--module`s, plays a whole game: the DOOM II module
  declared its 32 levels and played MAP01 with its 31 monsters, with no
  mispredictions.
  `scripts/build_server_web.sh` builds the server, and
  `node tools/webgame/build.mjs <out> --module <name>` builds the whole game as
  a static folder that works at a site's root or under a path, as on a GitHub
  Pages project site (`DNP_BASE`, client/src/base.js). Docs: docs/WEB.md. Not
  done yet: saving to the browser, and a web target in the Packager.
- **A test stack your own devices reach from anywhere**: `scripts/run_stack.sh
  --tailscale` serves the page and the game over this machine's Tailscale name,
  with its real certificate (`tailscale cert`), so a phone on mobile data with
  Tailscale on plays it with nothing to accept. The edge takes `--cert` and
  `--key`, and the dev server serves HTTPS when given a certificate.
- **Effect sectors** (`sector type effect`): a volume for effects and events
  with no floor of its own. Nothing collides with it or stands in it, it draws
  no floor, ceiling or walls, and it may overlap anything: rooms, platforms and
  other effect sectors. It carries `fx` lines (fog, snow, rain, dust, embers),
  and the `when_sector_entered` / `when_sector_left` graph nodes fire for it.
  A player can be in several at once, and each is entered and left on its own.
  Lua reads the list with `e:effect_sectors()`. They are saved in map version
  17, which only a map that has one is written as. Docs: SECTOR_FX.md §7; demo
  map: `maps/effect_sectors.map`.

## 0.2.3.8 -- 2026-10-25

### Fixed

- **The automatic camera on a platform**: walls pulling the camera in took
  the platform's own open edge for a wall, so on a platform the camera
  snapped in close, and beside one it did not see the platform at all and
  went in under it or through it. The platform's edge now lets the camera
  out over it, and its slab pulls the camera in like a wall. Markers hidden
  behind a platform are hidden too.

### Changed

- **A ledge is caught only on the way down.** Rising past one, a jump goes
  on by; the ledge is caught as the body falls back past it.

### Added

- **A variable jump**, for a module that wants one: with `variable_jump` on,
  letting go of JUMP while the jump still rises cuts it short, so a tap is a
  short hop and a held press the full jump. `jump_release_keep` (0.5) is how
  much of the rising speed is kept at the release. Off by default; double
  jumps and wall jumps follow it too, a ledge's hop does not.
- **A `change_player_setting` graph node**: one of the player's settings,
  chosen from a list of exactly those (jump height, speed, double jump,
  climbing, dash, ledge grab and the rest), set to a value, added to or
  multiplied, for good or for a number of seconds. It uses the engine's own
  effects, so the player's client predicts it at once. A setting that isn't
  a player's is refused when the graph loads.
- **The camera's height, over the player**: `camera_height` sets how high
  above the player's feet the camera sits; `distance` is then how far behind,
  and the vertical angle follows from the two. `aim_height` sets how high
  above the feet it looks and swings round, the head rather than the feet.
  They are CAMERA_SET fields 23 and 24, Lua `dnp.camera`, and the
  `camera_follow` node's `camera_height` and `aim_height`. A negative
  `camera_height` goes back to the angle. Try them with
  `?camheight=3&aimheight=1.5`.
- **A `hop` animation off a ledge**: JUMP while hanging plays `hop`
  (`avatar <id> when hop state <n>`) instead of `jump`, for the small hop
  that catches the ledge again and for the pull-up onto it. It falls back to
  `jump`'s state when a catalogue draws none. Leaping off is still a jump.
- **The studio's Play can show the colliders**: a launch with `colliders` opens
  the game with `?colliders=1`, in the desktop and Android shells.

## 0.2.3.7 -- 2026-10-23

### Fixed

- **A crowded level could stop sending snapshots altogether.** When the
  entities filled a datagram to within a byte of its end, there was no room
  left for the count that follows them. The snapshot was then dropped every
  tick for as long as the sizes landed that way, and the player saw the world
  freeze. The writer now always keeps that room.
- **Coyote time was unlimited**: walking off an edge kept the ground jump for
  the whole fall. A new player rule, `coyote_time` (0.1 s by default), sets
  how long it lasts. After it, a body that fell rather than jumped has only
  the air jumps `max_jumps` leaves.

### Added

- **Ledge grab** (`ledge_grab`, off by default): in the air, pushing into the
  side of a higher floor whose top is a little above your reach
  (`ledge_reach`, 0.5) catches it, on any side, climbable or not. The body
  hangs head level with the top. JUMP with the stick at the ledge pulls up
  onto it; JUMP with the stick still is a small hop that falls back and
  catches it again; JUMP with the stick away leaps off. Pushing away or
  CROUCH lets go. A `hang` animation condition (`avatar <id> when hang state
  <n>`) draws it, falling back to `climb`'s state.
- **An automatic camera**: it swings smoothly round behind the player by
  itself, the short way, unless they turned it themselves within the last
  `auto_delay` seconds (2 by default), at `auto_rate`. With `collide`, walls
  pull it in front of them at once, and it eases back out to its `distance`.
  It stays at the preferred `distance` and `pitch`, as always. Set it with
  CAMERA_SET (`auto_turn`, `auto_delay`, `auto_rate`, `collide`: Lua
  `dnp.camera`, or the `camera_follow` node's `auto`, `auto_delay`,
  `auto_rate` and `collide`), or try it with `?autocam=1`. It is never used
  in first person, nor while a map locks the yaw.
- **Colliders drawn over the frame**, a debug view: every body's collision
  cylinder in a colour for what it is (the player green, solid bodies orange,
  props nothing collides with grey). Turn it on with `?colliders=1`, or
  switch it with F4.

## 0.2.3.6 -- 2026-10-22

### Added

- **Layers on a player**: up to eight sprites drawn on the body together, for
  armour or a weapon in hand, in the body's own animation, clock and facing.
  Use `e:set_layer(slot, prop)` (a kind or a name; nil takes it off) and
  `e:layer(slot)`.
- **Animations redefined per player**: `e:set_anim(from, to)` plays state
  `to` wherever the player would play `from`, its layers too, so an attack
  can look different with each weapon. Up to eight at once; `nil` puts the
  state back.
- **Graph nodes for how a player looks**: `set_avatar`, `set_layer` and
  `set_animation_for`.

### Changed

- **A climbing body faces the wall it is on**, whichever way it moves along
  it. A dash or grapple from the wall still goes where the stick points, and
  a first-person body is left alone.

## 0.2.3.5 -- 2026-10-22

### Fixed

- **0.2.3.4's desktop kits could not start**: the shell's new `selfupdate.js` was
  not copied into the app, which main.js imports. It is, and a test now fails
  for any module the shell imports that the app would not have.

## 0.2.3.4 -- 2026-10-22 (withdrawn: its desktop kits could not start; see 0.2.3.5)

### Fixed

- **A desktop studio's self-update could replace a folder that was not its
  own.** On Windows and Linux it replaces the whole folder the program sits
  in, so a studio unzipped loose into Downloads would have taken everything
  else there with it. It now updates only a folder that holds nothing but the
  studio, and otherwise says to unpack it into a folder of its own. It also
  installs only a studio release from the project's GitHub releases.

### Added

- **The desktop studio updates itself** (macOS, Windows, Linux), as the Android
  one does: `dnp.studio.selfUpdate(url)` downloads the release's archive,
  unpacks it beside the app, and a script started on its own waits for the app
  to quit, puts the new one in its place and starts it. An app folder it cannot
  write is left alone, saying so. `dnp.studio.platform` says the system and
  processor, so the studio picks the right archive.

## 0.2.3.3 -- 2026-10-22

### Fixed

- **A platform could not cross the border between two rooms.** mapc refused
  a sector that only partly overlapped another unless the two were separated
  in height. A raised platform is now taken across any number of rooms, as a
  nested one already was: a body stands on it over either room, and from the
  rooms its side is a wall. Two level floors sharing space, or a sunk floor
  across a border (a pit is a hole cut from one room), are still refused.

## 0.2.3.2 -- 2026-10-21

### Fixed

- **On a phone, the subtitle covered the touch buttons.** It now narrows to
  the room between the stick and the buttons on a phone held sideways, and
  rises above the buttons on one held upright. This applies to a module's own
  subtitle line too, when it is placed at the bottom.
- **A panel's decor ignored `show: touch | desktop`**, showing the keyboard
  line on a phone and never the touch one. It now follows the game's screen,
  as the HUD does.

### Changed

- **The starter's controls are a panel**: its `welcome` game graph opens the
  panel `controls` near the top of the screen for ten seconds (the keys on a
  computer, the on-screen controls on a touch screen; closeable), instead of
  saying the keys on the subtitle line, which stays free for the game's own
  words.

## 0.2.3.1 -- 2026-10-21

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
