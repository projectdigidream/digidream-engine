-- The trigger vocabulary: every event, condition and action, as data.
--
-- NORMATIVE. This table is the definition -- docs/TRIGGERS.md is written from
-- it, maps/scripts/triggers.lua validates against it, and the editor's trigger
-- UI is generated from it. That is the one-implementation rule (§1.3 of the
-- Phase 6 briefing): three programs need the same list of actions, so two of
-- them ask the third rather than carrying a copy.
--
-- It is DATA, deliberately. Nothing here is a function: a table is something an
-- editor can render, re-edit and write back, which generated Lua source is not
-- once a person has touched it.
--
-- WHERE THE NAMES COME FROM. Not from taste:
--
--   events      exactly HOOK_NAMES in server/src/lua/vm.cpp. An event the
--               engine does not report cannot be a trigger event, and adding
--               one means adding an engine report -- a principal-agent
--               decision, not a line in this file.
--   actions     the bindings in server/src/lua/world_binding.cpp. Every action
--               below is one or two calls into that file and nothing more.
--   conditions  pure reads of the same bindings. A condition never mutates,
--               never allocates an entity, and never touches the RNG -- see
--               "chance" in docs/TRIGGERS.md for the one that was left out and
--               why.
--
-- THERE IS NO `wait`. Time is a tick deadline: `run_after` schedules another
-- trigger to run at a named tick, and the runtime drains due timers on
-- on_tick. See §4.3 of the briefing and the "No wait" section of the document.
--
-- Types, for the editor and for the runtime's argument checking:
--
--   "int"      a Lua number, integral
--   "fx"       exact fixed-point: a string like "0.25", or an integer
--   "string"   text
--   "bool"     true/false
--   "sector"   a sector index, or "event" (the sector the event named), or
--              "behind" (the neighbour of the side the event named)
--   "trigger"  the `name` of another trigger in the same table
--   "enum"     one of `values`
--
-- `optional` args may be omitted; `one_of` groups are mutually exclusive and
-- exactly one member must be present.

trigger_vocab = {
  version = 1,

  -- ------------------------------------------------------- trigger fields
  --
  -- What a trigger itself may carry, beside its conditions and actions. Named
  -- here so an editor can offer them and the runtime can refuse a typo; the
  -- runtime's own list is TRIGGER_FIELDS in maps/scripts/triggers.lua.
  trigger_fields = {
    { name = "sequence", type = "actions",
      doc = "Actions with a tick each, for a cutscene. Every entry is an " ..
            "ordinary action plus `when`: the number of ticks after this trigger " ..
            "fires that it runs on. `when` is a DEADLINE, not a countdown, and " ..
            "`when = 0` runs with the trigger's own actions rather than one " ..
            "tick later.\n" ..
            "`when`, not the mob vocabulary's `at`, because `music` already has " ..
            "an `at` argument and a Lua table cannot hold the key twice -- and " ..
            "changing the music is exactly what a cutscene wants to do. No " ..
            "action may declare an argument called `when`; the runtime refuses " ..
            "one at load.\n" ..
            "Sugar over `run_after`, which is what a cutscene had to be written " ..
            "as before: one trigger per beat, chained by hand. A sequence needs " ..
            "no engine support and suspends nothing; the beats are deadlines on " ..
            "the same queue `run_after` uses.\n" ..
            "The trigger's conditions are checked when it FIRES and not again. " ..
            "A cutscene that re-tested them would stop halfway because the " ..
            "player walked out of the sector that started it." },
  },

  -- --------------------------------------------------------------- events
  --
  -- `params` names what the engine passes, in order. A condition or action
  -- that reads one of these reads it from the event currently being
  -- dispatched; outside that event it is absent, and the runtime says so at
  -- load time rather than at midnight in production.
  events = {
    { name = "on_tick", params = {},
      doc = "Every tick, before the world steps. The runtime always registers " ..
            "this one, whether a map uses it or not, because it is where tick " ..
            "deadlines are drained." },

    { name = "on_post_tick", params = {},
      doc = "Every tick, after the world has stepped and positions are final." },

    { name = "on_interact", params = {"entity", "side", "sector"},
      doc = "An entity pressed use while facing a side within reach. `side` is " ..
            "an index; `sector` is the sector the entity was standing in. " ..
            "Edge-triggered: holding the button works a switch once." },

    { name = "on_cross", params = {"entity", "side", "sector"},
      doc = "An entity's movement crossed a side flagged `trigger`. Reported " ..
            "as the side of the sector the entity LEFT, so the two directions " ..
            "across one line are two different side indices and a trip-wire " ..
            "that fires both ways needs both." },

    { name = "on_touch", params = {"entity", "prop"},
      doc = "An entity is standing inside a prop. Reported EVERY TICK the " ..
            "overlap holds -- 'once' is something the trigger does by removing " ..
            "the prop or by setting `once`." },

    { name = "on_prop_use", params = {"entity", "prop"},
      doc = "An entity pressed use on a prop the catalogue marks `usable`." },

    { name = "on_attack", params = {"entity", "target"},
      doc = "A swing connected. `entity` is the attacker; `target` is an " ..
            "entity or a prop -- ask with entity_kind_is / prop_name_is rather " ..
            "than by comparing numbers." },

    { name = "on_crush", params = {"entity", "sector", "crush_kind"},
      doc = "A moving sector closed on an entity. `crush_kind` is \"vertical\" " ..
            "or \"pinned\". Reported on the FIRST tick of an episode only." },

    { name = "on_ui_event", params = {"entity", "ui_id", "widget_id", "value"},
      doc = "A player worked a widget in a panel a script opened. `value` has " ..
            "already been clamped to what the widget itself declared." },
  },

  -- ----------------------------------------------------------- conditions
  --
  -- Every condition in a trigger must hold for its actions to run (AND). Each
  -- one may carry `negate = true`; there is no OR and no nesting, because two
  -- triggers on the same event are how you write an OR and an editor can draw
  -- that. See docs/TRIGGERS.md, "Why there is no boolean tree".
  conditions = {
    { name = "side_is",
      args = { { name = "side", type = "int" },
               { name = "expect_neighbour", type = "int",
                 doc = "The sector index on the other side of that side when " ..
                       "this trigger was written, or -1 for a plain wall. " ..
                       "REQUIRED -- see below." } },
      events = {"on_interact", "on_cross"},
      doc = "The event's side index is this one. The commonest condition " ..
            "there is: a switch IS a side index.\n\n" ..
            "`expect_neighbour` is not optional, and it is not ceremony. Side " ..
            "indices come from the order mapc walks the .map, so inserting a " ..
            "single vertex anywhere renumbers every side after it -- and a " ..
            "table full of stale indices loads perfectly and quietly watches " ..
            "the wrong walls. Measured: one added vertex pointed all four of " ..
            "maps/doors_trig.lua's sides at blank wall while the runtime " ..
            "reported '11 triggers on 4 events'. Checked at load, the whole " ..
            "table is refused instead. doom.lua carries the same guard on " ..
            "every converted line, for the same reason." },

    { name = "sector_is",
      args = { { name = "sector", type = "sector" } },
      events = {"on_interact", "on_cross", "on_crush"},
      doc = "The sector the event named is this one." },

    { name = "sector_tag_is",
      args = { { name = "tag", type = "int" },
               { name = "sector", type = "sector", optional = true,
                 doc = "Which sector to read; defaults to the event's." } },
      doc = "A sector's `tag` is this number. A tag is a number the map " ..
            "carries and nothing else -- what it MEANS is the trigger that " ..
            "reads it." },

    { name = "entity_is_player",
      args = {},
      doc = "The event's entity is a person, not a scripted agent. A player " ..
            "is an agent with no catalogue kind: nobody placed it, so it " ..
            "walked in." },

    { name = "entity_kind_is",
      args = { { name = "name", type = "string" } },
      doc = "The event's entity is a catalogue entry of this name (\"imp\"). " ..
            "By name, so renumbering assets/props.txt cannot silently point " ..
            "this at something else." },

    { name = "entity_in_sector_tag",
      args = { { name = "tag", type = "int" } },
      doc = "The entity is standing in a sector carrying this tag -- which is " ..
            "not the same as the sector the event named." },

    { name = "target_name_is",
      args = { { name = "name", type = "string" } },
      events = {"on_touch", "on_prop_use", "on_attack"},
      doc = "The thing the event was done TO -- the prop you walked into, what " ..
            "your swing reached -- is this catalogue entry. Props and agents " ..
            "answer the same question, so one condition covers both; a player " ..
            "has no catalogue entry and is never a match." },

    { name = "crush_kind_is",
      args = { { name = "kind", type = "enum", values = {"vertical", "pinned"} } },
      events = {"on_crush"},
      doc = "Which way the crush happened." },

    { name = "widget_is",
      args = { { name = "widget", type = "int" },
               { name = "value", type = "int", optional = true } },
      events = {"on_ui_event"},
      doc = "The panel event names this widget id, and -- if `value` is " ..
            "given -- carries this value." },

    { name = "sector_at_rest",
      args = { { name = "sector", type = "sector" } },
      doc = "Nothing this runtime started on that sector is still in flight. " ..
            "The guard a repeatable door needs: pressing a moving door again " ..
            "must not restart it half-way." },

    { name = "sector_offset_at_least",
      args = { { name = "sector", type = "sector" },
               { name = "z", type = "fx" } },
      doc = "The sector's current vertical offset is at or above `z`. This is " ..
            "how 'the door is open' is asked without the engine having any " ..
            "idea what a door is: the offset is the truth a client also sees, " ..
            "so a flag stored here could disagree with it and this cannot." },

    { name = "property_is",
      args = { { name = "name",  type = "string" },
               { name = "value", type = "value" } },
      doc = "A named property this trigger table keeps equals this value. The " ..
            "same store counters use, so a counter and a property of one name " ..
            "are one thing rather than two that shadow each other. Compared " ..
            "with `==`, so a property set to the string \"open\" is not equal " ..
            "to the number 0 and never accidentally matches." },

    { name = "counter_at_least",
      args = { { name = "counter", type = "string" },
               { name = "value", type = "int" } },
      doc = "A named counter this trigger table keeps is at or above `value`. " ..
            "Counters start at zero and are per map, not per player." },

    { name = "trigger_enabled",
      args = { { name = "trigger", type = "trigger" } },
      doc = "Another trigger's enabled flag. Reading it, never writing it -- " ..
            "`enable`/`disable` are the actions." },

    { name = "tick_at_least",
      args = { { name = "tick", type = "int" } },
      doc = "The world tick is at or past this number. dnp.tick() is the only " ..
            "clock: a trigger that read a wall clock would destroy replay." },
  },

  -- -------------------------------------------------------------- actions
  --
  -- Each action reports whether it DID something. That answer is what `once`
  -- spends itself on: a one-shot that fired while its door was already moving
  -- is not spent, which is Doom's own behaviour and the reason a player who
  -- walked in early is not locked out of a room for good.
  -- --------------------------------------------------------------- panels
  --
  -- A panel is a LAYOUT, and a layout is data an editor draws -- it is not an
  -- argument list, which is why `ui_open` names a key in the trigger table's
  -- `panels` rather than carrying widgets inline.
  --
  -- Described here so the editor can generate a panel editor from the same
  -- table it generates the action editor from. Without this the widget kinds
  -- would live only in server/src/lua/world_binding.cpp's ui_kind_of, and the
  -- editor would carry a hand-copied list of them -- which is the one thing
  -- this file exists to prevent.
  --
  -- The names and the limits are the protocol's (core/include/dnp/proto/
  -- messages.hpp), not this file's opinion: a panel with more widgets than
  -- UI_MAX_WIDGETS is refused by the binding, and a script error there is a
  -- panel that never opens.
  panels = {
    max_widgets = 32,           -- proto::UI_MAX_WIDGETS
    max_options = 32,           -- proto::UI_MAX_OPTIONS
    fields = {
      { name = "title", type = "string",
        doc = "The panel's heading." },
    },
    widgets = {
      { name = "label",
        args = { { name = "id",    type = "int" },
                 { name = "label", type = "string" } },
        doc = "Static text. Sends nothing -- an on_ui_event naming a label did " ..
              "not come from a person, and the server drops it." },

      { name = "button",
        args = { { name = "id",    type = "int" },
                 { name = "label", type = "string" } },
        doc = "Sends value 1 when pressed." },

      { name = "toggle",
        args = { { name = "id",    type = "int" },
                 { name = "label", type = "string" },
                 { name = "value", type = "int", optional = true } },
        doc = "Sends 0 or 1. Any other value is clamped to one of those before " ..
              "a trigger sees it." },

      { name = "slider",
        args = { { name = "id",    type = "int" },
                 { name = "label", type = "string" },
                 { name = "lo",    type = "int" },
                 { name = "hi",    type = "int" },
                 { name = "value", type = "int", optional = true } },
        doc = "An integer in [lo, hi]. The server clamps to the range the " ..
              "widget itself declared, so a trigger never sees a value this " ..
              "panel could not have produced." },

      { name = "select",
        args = { { name = "id",      type = "int" },
                 { name = "label",   type = "string" },
                 { name = "options", type = "string_list" },
                 { name = "value",   type = "int", optional = true } },
        doc = "An index into `options`. An index outside the list is dropped " ..
              "rather than clamped: there is no nearest valid answer." },
    },
  },

  -- Appended to every condition and action that names a sector, at the bottom
  -- of this file. Declared once because it is one idea; expanded per entry
  -- because the runtime and the editor both want a flat argument list.
  sector_guard = {
    name = "expect_tag", type = "int", optional = true,
    doc = "The tag that sector carried when this trigger was written. " ..
          "Sector indices come from declaration order, so inserting a sector " ..
          "renumbers the ones after it; this catches that at load. Optional, " ..
          "because addressing a sector by `tag` instead of by index needs no " ..
          "guard at all -- and because \"event\" and \"behind\" resolve from " ..
          "the event, so there is nothing to check until one arrives." },

  actions = {
    { name = "sector_move",
      args = {
        { name = "sector",  type = "sector", optional = true, one_of = "target" },
        { name = "tag",     type = "int",    optional = true, one_of = "target",
          doc = "Every sector carrying this tag." },
        { name = "surface", type = "enum", values = {"floor", "ceiling", "both"} },
        { name = "to",      type = "enum", optional = true,
          values = {"base_floor", "base_ceiling",
                    "lowest_neighbour_floor", "highest_neighbour_floor",
                    "next_higher_floor",
                    "lowest_neighbour_ceiling", "highest_neighbour_ceiling",
                    "absolute"},
          doc = "Where the surface ends up. Neighbour targets are resolved AT " ..
                "TRIGGER TIME, because a staircase raised by an earlier " ..
                "trigger changes what 'the next higher floor' means." },
        { name = "height",  type = "fx", optional = true,
          doc = "The height, when `to` is \"absolute\"." },
        { name = "offset",  type = "fx", optional = true,
          doc = "Added to the resolved height. A Doom door opens to the " ..
                "lowest neighbouring ceiling less four Doom units, which is " ..
                "to = lowest_neighbour_ceiling with offset = \"-0.0834\"." },
        { name = "dx",      type = "fx", optional = true,
          doc = "How far the footprint travels along the map's x axis. This " ..
                "is a SIDEWAYS platform, and it is a different thing from a " ..
                "lift: a lift leaves its footprint where the map says it is, " ..
                "and a slider stops being there -- so it pushes what is in " ..
                "its way and carries what stands on it. `surface` does not " ..
                "apply to it, because a sector cannot have its floor in one " ..
                "place and its ceiling in another." },
        { name = "dy",      type = "fx", optional = true,
          doc = "The same along y. Give both for a diagonal; `speed` is then " ..
                "along the line actually travelled, not along either axis." },
        { name = "speed",   type = "fx",
          doc = "Units per tick. Never zero: a door that shut instantly would " ..
                "crush whoever stood under it with no warning." },
        { name = "loop",    type = "enum", optional = true,
          values = {"once", "loop", "pingpong"},
          doc = "Defaults to \"once\". A pingpong crusher needs no further " ..
                "bookkeeping at all -- it is a pure function of the tick, so " ..
                "the client rides it without another message." },
        { name = "interrupt", type = "bool", optional = true,
          doc = "Move even if this runtime already has something in flight " ..
                "there. Off by default." },
      },
      doc = "PARAMETRIC sector motion: a start offset, an end offset, a " ..
            "duration. Replicated once and computed by every client for ticks " ..
            "the server has not reached, so a door costs one message and " ..
            "predicts free.\n" ..
            "Vertical by default -- name `dx` or `dy` and the footprint " ..
            "slides instead, which the engine has always been able to do and " ..
            "this vocabulary could not ask for. The two combine: a lift that " ..
            "also travels is one action." },

    { name = "sector_stop",
      args = { { name = "sector", type = "sector", optional = true, one_of = "target" },
               { name = "tag",    type = "int",    optional = true, one_of = "target" } },
      doc = "Freeze a sector where it is." },

    { name = "sector_offset",
      args = { { name = "sector", type = "sector", optional = true, one_of = "target" },
               { name = "tag",    type = "int",    optional = true, one_of = "target" },
               { name = "z",      type = "fx" } },
      doc = "SCRIPTED-DISCRETE motion: whatever this trigger decided, with no " ..
            "closed form. The server streams it in every snapshot and riders " ..
            "fall back to anchor-relative prediction. It costs bandwidth every " ..
            "tick it moves and it costs prediction; reach for sector_move " ..
            "unless you specifically need something no client could derive." },

    { name = "teleport",
      args = { { name = "x", type = "fx" }, { name = "y", type = "fx" },
               { name = "z", type = "fx" },
        { name = "who", type = "enum", optional = true,
          values = {"entity", "target"},
          doc = "Which of the event's two handles to act on: the one that DID " ..
                "it (default) or the one it was done to. Ambiguous otherwise, " ..
                "and guessing wrong is silent." },
             },
      doc = "Move the event's entity. Deferred to the tick's flush point like " ..
            "every other mutation, so a trigger that reads the position back " ..
            "in the same callback still sees the old one." },

    { name = "set_hp",
      args = { { name = "value", type = "int", optional = true, one_of = "amount" },
               { name = "add",   type = "int", optional = true, one_of = "amount",
                 doc = "Relative. -10 is ten points of damage." },
        { name = "who", type = "enum", optional = true,
          values = {"entity", "target"},
          doc = "Which of the event's two handles to act on: the one that DID " ..
                "it (default) or the one it was done to. Ambiguous otherwise, " ..
                "and guessing wrong is silent." },
             },
      doc = "Health is the server's; the engine has no notion of it and a " ..
            "client only ever learns its own." },

    { name = "set_solid",
      args = { { name = "solid", type = "bool" },
        { name = "who", type = "enum", optional = true,
          values = {"entity", "target"},
          doc = "Which of the event's two handles to act on: the one that DID " ..
                "it (default) or the one it was done to. Ambiguous otherwise, " ..
                "and guessing wrong is silent." },
             },
      doc = "Whether other bodies collide with the event's entity or prop. A " ..
            "corpse left solid is a wall that appeared where you were about " ..
            "to run." },

    { name = "set_animation",
      args = { { name = "state", type = "int" },
        { name = "who", type = "enum", optional = true,
          values = {"entity", "target"},
          doc = "Which of the event's two handles to act on: the one that DID " ..
                "it (default) or the one it was done to. Ambiguous otherwise, " ..
                "and guessing wrong is silent." },
             },
      doc = "The animation STATE, never a frame and never a duration -- which " ..
            "frame is on screen is the client's decision." },

    { name = "despawn",
      args = {
        { name = "who", type = "enum", optional = true,
          values = {"entity", "target"},
          doc = "Which of the event's two handles to act on: the one that DID " ..
                "it (default) or the one it was done to. Ambiguous otherwise, " ..
                "and guessing wrong is silent." },
             },
      doc = "Remove the event's prop, or a scripted agent. Refused for a " ..
            "player, loudly: a script cannot delete a person." },

    { name = "spawn_prop",
      args = { { name = "name", type = "string" },
               { name = "x", type = "fx" }, { name = "y", type = "fx" },
               { name = "z", type = "fx" } },
      doc = "By catalogue name. A name the operator has not installed is an " ..
            "error naming the file, not a silent nothing." },

    { name = "spawn_enemy",
      args = { { name = "name", type = "string" },
               { name = "x", type = "fx" }, { name = "y", type = "fx" },
               { name = "z", type = "fx" } },
      doc = "An agent nobody is sitting at, with the catalogue's body and " ..
            "speed, moved through the same controller a player is." },

    { name = "add_effect",
      args = { { name = "param", type = "string",
                 doc = "A movement parameter name; the table is in core." },
               { name = "op",    type = "enum", values = {"mul", "add", "set"} },
               { name = "value", type = "fx" },
               { name = "ticks", type = "int", optional = true,
                 doc = "Omit for permanent." },
        { name = "who", type = "enum", optional = true,
          values = {"entity", "target"},
          doc = "Which of the event's two handles to act on: the one that DID " ..
                "it (default) or the one it was done to. Ambiguous otherwise, " ..
                "and guessing wrong is silent." },
             },
      doc = "A timed change to the RULES of movement for the event's entity. " ..
            "Travels on the reliable channel with an epoch, not as a snapshot " ..
            "flag, because a client running different physics from the server " ..
            "mispredicts every tick until it is told." },

    { name = "change_map",
      args = { { name = "level", type = "int", optional = true,
                 doc = "Omit for the next level in the server's progression." } },
      doc = "Moves THAT player, and nobody else. Someone still fighting " ..
            "through the level is not yanked along because a faster player " ..
            "found the switch." },

    { name = "music",
      args = { { name = "cue", type = "string",
                 doc = "A cue of this map's soundtrack, by name. `default` is the one " ..
                       "the map starts with; the rest are what the module's " ..
                       "assets/music/index.json lists for the map." },
               { name = "fade", type = "int", optional = true,
                 doc = "Milliseconds of crossfade; 800 when omitted." },
               { name = "at", type = "enum", values = {"next_bar", "loop_end", "now"},
                 optional = true,
                 doc = "When the change lands: on the next bar of what is playing " ..
                       "(the default), when its loop ends, or at once." },
               { name = "everyone", type = "bool", optional = true,
                 doc = "Every player in the level, rather than the event's player." } },
      doc = "Changes which cue of the map's soundtrack plays. The client resolves " ..
            "the name from the map's manifest; cues cut from one parent track " ..
            "crossfade into each other, unrelated tracks swap. Not remembered: a " ..
            "player joining later starts on `default`." },

    { name = "ui_open",
      args = { { name = "panel", type = "string",
                 doc = "The key of an entry in the trigger table's `panels`. " ..
                       "A panel is a layout, and a layout is data an editor " ..
                       "draws -- it is not an argument list." } },
      doc = "Open a panel for the event's entity." },

    { name = "ui_close",
      args = { { name = "panel", type = "string" } },
      doc = "Close a panel this trigger table opened for the event's entity." },

    { name = "camera",
      args = { { name = "field", type = "enum",
                 values = {"distance", "pitch", "smoothing", "max_accel",
                           "lead", "lead_smoothing", "deadzone_x", "deadzone_y",
                           "yaw", "lock_yaw", "lock_zoom",
                           -- Camera paths (design 4.1). A path is authored in
                           -- the .map file as `path tag N`; this names one.
                           "cam_mode", "cam_path", "cam_path_speed",
                           "cam_path_attach", "cam_path_blend",
                           "cam_shake", "cam_shake_time"} },
               { name = "value", type = "fx" } },
      doc = "How the camera follows, for whoever is watching this entity, and " ..
            "where it stands: `yaw` and `pitch` in turns, `distance` in units, " ..
            "`lock_yaw`/`lock_zoom` 1 or 0. Never where it is: the camera is " ..
            "the client's, and Lua configures and triggers it only.\n" ..
            "`cam_mode` picks what it does with a path: 0 follow, 1 track " ..
            "(position from the path, still aimed at the player), 2 flythrough " ..
            "(position AND aim from the path, at `cam_path_speed` units a " ..
            "second), 3 auto (track while the player is within " ..
            "`cam_path_attach` units of it). `cam_path` is the path's TAG, and " ..
            "negative means none -- an untagged path is tag 0, so 0 cannot mean " ..
            "it. `cam_path_blend` is how fast it crossfades on and off.\n" ..
            "`cam_shake` is the one field here that is an EVENT rather than a " ..
            "setting: it shakes when it arrives, for `cam_shake_time` " ..
            "milliseconds, and a second shake replaces the first rather than " ..
            "adding to it." },

    { name = "screen",
      args = { { name = "fade", type = "fx", optional = true,
                 doc = "Opacity of a black curtain over everything, 0 to 1." },
               { name = "fade_ms", type = "int", optional = true,
                 doc = "Milliseconds to reach it. 0, or absent, is at once." },
               { name = "letterbox", type = "fx", optional = true,
                 doc = "Height of each bar as a fraction of the screen, 0 to 0.5." },
               { name = "letterbox_ms", type = "int", optional = true,
                 doc = "Milliseconds to reach that height." },
               { name = "everyone", type = "bool", optional = true,
                 doc = "Every player in the level, rather than the event's player." } },
      doc = "The screen itself: a fade to black, and letterbox bars. The other " ..
            "half of a cutscene beside `camera`. Only the fields given are " ..
            "sent, so raising the bars does not restate the fade.\n" ..
            "Not remembered, like `music`: a player who joins halfway through a " ..
            "cutscene arrives at a screen they can see out of rather than at a " ..
            "black rectangle nothing will lift." },

    { name = "vfx",
      args = { { name = "kind",  type = "int" },
               { name = "x", type = "fx" }, { name = "y", type = "fx" },
               { name = "z", type = "fx" },
               { name = "param", type = "int", optional = true } },
      doc = "A transient visual with no identity: a tracer, a flash, an " ..
            "impact. A client that missed one has missed nothing it needs." },

    { name = "counter_add",
      args = { { name = "counter", type = "string" },
               { name = "by", type = "int", optional = true,
                 doc = "Defaults to 1." } },
      doc = "Per map, not per player. Three switches that between them open " ..
            "one door are a counter and a condition." },

    { name = "set_property",
      args = { { name = "name",  type = "string" },
               { name = "value", type = "value" } },
      doc = "Remember something under a name: a number, a string or a boolean. " ..
            "Where counter_add counts, this NAMES a state -- \"open\", " ..
            "\"locked\", true -- which is the thing a counter cannot hold and " ..
            "the thing an author reaches for when a number would be a code." },

    { name = "lua",
      args = {
        { name = "fn",   type = "function", optional = true, one_of = "body",
          doc = "A Lua function taking the event table. Works under every " ..
                "trust profile; an editor cannot round-trip it." },
        { name = "code", type = "string", optional = true, one_of = "body",
          doc = "Lua source, compiled once at load with `ev` in scope. An " ..
                "editor can round-trip it; it needs `loadstring`, which " ..
                "Trust::Strict removes." },
      },
      doc = "THE ESCAPE HATCH, and the only place a trigger stops being a " ..
            "bounded vocabulary. Everything else here is a named verb over a " ..
            "binding; this is arbitrary script, with the whole sandbox in " ..
            "reach. Compiled at LOAD, so a syntax error names the trigger " ..
            "instead of surfacing the first time somebody presses the switch. " ..
            "Returning false means it did nothing, which is what `once` " ..
            "spends itself on." },

    { name = "counter_set",
      args = { { name = "counter", type = "string" },
               { name = "value", type = "int" } },
      doc = "Set a counter outright." },

    { name = "enable",
      args = { { name = "trigger", type = "trigger" } },
      doc = "Arm another trigger." },

    { name = "disable",
      args = { { name = "trigger", type = "trigger" } },
      doc = "Disarm another trigger. A disabled trigger is still dispatched " ..
            "to and still costs its lookup; it just does nothing." },

    { name = "run",
      args = { { name = "trigger", type = "trigger" } },
      doc = "Run another trigger's actions NOW, re-checking its conditions " ..
            "and carrying the current event with it. Nesting is bounded: see " ..
            "MAX_RUN_DEPTH in maps/scripts/triggers.lua." },

    { name = "run_after",
      args = { { name = "trigger", type = "trigger" },
               { name = "ticks", type = "int" },
               { name = "after", type = "sector", optional = true,
                 doc = "Count the ticks from when this sector's motion " ..
                       "settles, rather than from now. A door waits after it " ..
                       "has finished opening, not after the button." } },
      doc = "The replacement for `wait`, and the only one. Schedules another " ..
            "trigger to run at tick + `ticks`; the runtime drains due timers " ..
            "on on_tick. Nothing suspends, so nothing can be resumed -- see " ..
            "docs/TRIGGERS.md, \"No wait\"." },

    { name = "log",
      args = { { name = "text", type = "string" } },
      doc = "A line on the server's log, prefixed with the map and the " ..
            "trigger's name. The debugging surface an author actually has: " ..
            "\"this switch does nothing\" is answerable when the log says " ..
            "which side was reached and which trigger declined it." },
  },
}

-- Expand the shared sector guard into every entry that names a sector.
--
-- A loop in a data file, which is a thing to justify: the alternative is the
-- same eight lines copied into nine argument lists, and the ninth is the one
-- that gets missed. The table an editor and the runtime read is fully expanded
-- either way -- this only decides where the text lives.
do
  local guard = trigger_vocab.sector_guard
  local function guard_all(list)
    for _, spec in ipairs(list) do
      local names_a_sector = false
      for _, arg in ipairs(spec.args) do
        if arg.type == "sector" then names_a_sector = true end
      end
      if names_a_sector then spec.args[#spec.args + 1] = guard end
    end
  end
  guard_all(trigger_vocab.conditions)
  guard_all(trigger_vocab.actions)
end
