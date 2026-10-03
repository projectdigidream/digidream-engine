--
-- THE GRAPH VOCABULARY (docs/GRAPHS.md): every node a graph may hold, its
-- fields, and its pins.
--
-- NORMATIVE, like trigger_vocab.lua and mob_vocab.lua. graphs.lua validates a
-- graph against this table and refuses at load if the two disagree in either
-- direction; docs/GRAPHS.md's reference is generated from it; the studio reads
-- graph_vocab.json, generated from it by tools/vocabjson. Three programs, one
-- list.
--
-- The world's verbs are NOT here. An `event`, `condition` or `action` node
-- names an entry of trigger_vocab.lua, whose arguments become the node's
-- fields: one vocabulary of things that happen and things to do, run by one
-- implementation (triggers.verbs).
--
-- PINS. `exec_in`/`exec_out` are the white wires -- what happens next.
-- `data_in`/`data_out` carry values. A data input with no link takes the
-- node's own field of the same name, so `{type = "compare", op = "<", b = 3}`
-- compares its linked `a` against three.
--
-- Pure data apart from nothing: no functions, so it can be emitted as JSON.

graph_vocab = {
  version = 1,

  -- The argument types a verb's argument may take from a DATA LINK rather than
  -- its literal field (a node with `data_in_args`). The rest -- action lists,
  -- functions, trigger names -- are structure, not values.
  pinnable_types = { "int", "fx", "string", "bool", "enum", "sector", "value" },

  -- The file's own top level.
  graph_fields = {
    { name = "graph", type = "int",
      doc = "The format version. 1." },
    { name = "name", type = "string",
      doc = "What the graph is called in messages and in the studio." },
    { name = "vars", type = "table", optional = true,
      doc = "The graph's variables, by name, with their starting values: " ..
            "numbers, strings or booleans." },
    { name = "nodes", type = "table",
      doc = "The nodes, as an ARRAY: its order is the order event nodes for " ..
            "the same event run in." },
    { name = "links", type = "table",
      doc = "The links, each [\"<node>.<pin>\", \"<node>.<pin>\"], from an " ..
            "output to an input." },
    { name = "meta", type = "table", optional = true,
      doc = "The editor's, never read by the runtime: layout, comments." },
  },

  -- Fields every node has, whatever its type.
  node_fields = {
    { name = "id", type = "string",
      doc = "Unique in the graph; what links and messages name it by." },
    { name = "type", type = "string",
      doc = "One of the node types below." },
    { name = "meta", type = "table", optional = true,
      doc = "The editor's: position, colour, comment. Never read by the runtime." },
  },

  nodes = {
    -- ------------------------------------------------------- the world's
    { name = "event",
      args = { { name = "event", type = "string",
                 doc = "A trigger_vocab.lua event, or on_player_enter / " ..
                       "on_player_leave in a module's graph." } },
      exec_in = {}, exec_out = { "then" },
      data_in = {}, data_out = { "entity", "target", "side", "sector",
                                 "crush_kind", "ui_id", "widget_id", "value" },
      doc = "Where a run starts: when the event happens. Its data outputs are " ..
            "what the event carried -- nil for what it did not." },
    { name = "condition",
      args = { { name = "c", type = "string",
                 doc = "A trigger_vocab.lua condition; its arguments are this " ..
                       "node's fields, checked by the trigger runtime's checker." },
               { name = "negate", type = "bool", optional = true,
                 doc = "Take the other exit." } },
      exec_in = { "in" }, exec_out = { "true", "false" },
      data_in = {}, data_out = {}, data_in_args = true,
      doc = "A trigger condition, asked about the event this run started from. " ..
            "Each of its arguments of a pinnable type is also a data input of " ..
            "the same name: linked, it takes the value at run time; a value " ..
            "that does not fit the argument skips the verb (false), said once." },
    { name = "action",
      args = { { name = "a", type = "string",
                 doc = "A trigger_vocab.lua action; its arguments are this " ..
                       "node's fields." } },
      exec_in = { "in" }, exec_out = { "then" },
      data_in = {}, data_out = {}, data_in_args = true,
      doc = "A trigger action, done for the event this run started from. " ..
            "Its arguments of a pinnable type are data inputs too, as a " ..
            "condition's; a value that does not fit skips the action, said once." },
    { name = "on_call",
      args = { { name = "name", type = "string",
                 doc = "Which call: what a `call` node names." } },
      exec_in = {}, exec_out = { "then" },
      data_in = {}, data_out = { "entity", "value" },
      doc = "Where a run starts when a `call` names it: from the game's graphs " ..
            "to a level's, from a level's to the game's, or within one VM. " ..
            "Its outputs are what the call sent." },
    { name = "call",
      args = { { name = "to", type = "enum", values = { "game", "level" },
                 doc = "The game's graphs, or the graphs of the level `entity` is in." },
               { name = "name", type = "string",
                 doc = "The `on_call` nodes it starts." },
               { name = "value", type = "value", optional = true,
                 doc = "What it sends; used when nothing is linked to `value`." } },
      exec_in = { "in" }, exec_out = { "then" },
      data_in = { "value", "entity" }, data_out = {},
      doc = "Start the `on_call` nodes of that name, now, and carry on. The " ..
            "entity is the linked one, or the event's; a call to a level needs " ..
            "one, to know which. Calls nest at most four deep." },

    -- --------------------------------------------------------------- flow
    { name = "branch",
      args = { { name = "cond", type = "value", optional = true,
                 doc = "Used when nothing is linked to the `cond` input." } },
      exec_in = { "in" }, exec_out = { "true", "false" },
      data_in = { "cond" }, data_out = {},
      doc = "One way or the other on a value: anything but false and nil is true." },
    { name = "sequence",
      args = { { name = "count", type = "int",
                 doc = "How many exits, 2 to 8, named 0, 1, ..." } },
      exec_in = { "in" }, exec_out = { "0", "1", "2", "3", "4", "5", "6", "7" },
      data_in = {}, data_out = {},
      doc = "Each exit in turn, in the same tick: 0, then 1, and so on." },
    { name = "delay",
      args = { { name = "ticks", type = "int",
                 doc = "How long, at 60 a second. At least 1." } },
      exec_in = { "in" }, exec_out = { "then" },
      data_in = {}, data_out = {},
      doc = "The rest of the run, later, with the same event." },
    { name = "once",
      args = {},
      exec_in = { "in" }, exec_out = { "then" },
      data_in = {}, data_out = {},
      doc = "Through the first time, never again." },

    -- ---------------------------------------------------------- variables
    { name = "get_var",
      args = { { name = "var", type = "string", doc = "A name in the graph's `vars`." } },
      exec_in = {}, exec_out = {},
      data_in = {}, data_out = { "value" },
      doc = "A variable's value." },
    { name = "set_var",
      args = { { name = "var", type = "string", doc = "A name in the graph's `vars`." },
               { name = "value", type = "value", optional = true,
                 doc = "Used when nothing is linked to the `value` input." } },
      exec_in = { "in" }, exec_out = { "then" },
      data_in = { "value" }, data_out = {},
      doc = "Set a variable." },
    { name = "add_var",
      args = { { name = "var", type = "string", doc = "A numeric variable." },
               { name = "value", type = "value", optional = true,
                 doc = "How much; used when nothing is linked to `value`." } },
      exec_in = { "in" }, exec_out = { "then" },
      data_in = { "value" }, data_out = {},
      doc = "Add to a numeric variable: a counter." },

    -- --------------------------------------------------------------- data
    { name = "literal",
      args = { { name = "value", type = "value", doc = "A number, a string or a boolean." } },
      exec_in = {}, exec_out = {},
      data_in = {}, data_out = { "value" },
      doc = "A constant." },
    { name = "compare",
      args = { { name = "op", type = "enum", values = { "==", "~=", "<", "<=", ">", ">=" } },
               { name = "a", type = "value", optional = true },
               { name = "b", type = "value", optional = true } },
      exec_in = {}, exec_out = {},
      data_in = { "a", "b" }, data_out = { "value" },
      doc = "a op b, a boolean. Ordering a number against a non-number is false." },
    { name = "math",
      args = { { name = "op", type = "enum", values = { "+", "-", "*", "/", "min", "max" } },
               { name = "a", type = "value", optional = true },
               { name = "b", type = "value", optional = true } },
      exec_in = {}, exec_out = {},
      data_in = { "a", "b" }, data_out = { "value" },
      doc = "a op b on numbers. Division by zero is zero, not a crash." },
    { name = "logic",
      args = { { name = "op", type = "enum", values = { "and", "or", "not" } },
               { name = "a", type = "value", optional = true },
               { name = "b", type = "value", optional = true } },
      exec_in = {}, exec_out = {},
      data_in = { "a", "b" }, data_out = { "value" },
      doc = "Boolean logic; `not` reads only `a`." },
    { name = "random",
      args = { { name = "lo", type = "int" }, { name = "hi", type = "int" } },
      exec_in = {}, exec_out = {},
      data_in = {}, data_out = { "value" },
      doc = "A whole number from lo to hi, from the level's own stream, so a " ..
            "replayed level draws the same. Once per run: two readers see one draw." },
  },
}
