-- The REFERENCE half of a vocabulary's documentation, emitted from the
-- vocabulary itself.
--
-- WHY THIS IS GENERATED WHEN docs/TRIGGERS.md IS NOT. That document is hand
-- written and a test compares its headings against the table, which works
-- because the trigger vocabulary is small. The mob vocabulary is fifty entries
-- with a paragraph of argument documentation each, all of it already written
-- down in mob_vocab.lua's `doc` strings. Copying fifty paragraphs into a second
-- file by hand does not make them more true -- it makes two places to edit and
-- one of them stale, which is the failure the vocabulary exists to prevent.
--
-- So the prose stays hand written and the reference is generated, with the
-- boundary marked in the document by a line the emitter looks for. Everything
-- above the marker is a person's; everything below is this file's, and
-- scripts/check_vocab_docs.sh fails if the two disagree.
--
-- Run as:  luajit tools/vocabdoc/emit.lua <vocab.lua> <global> [title]
--
local vocab_path = arg[1]
local global     = arg[2]

if vocab_path == nil or global == nil then
  io.stderr:write("usage: emit.lua <vocab.lua> <global-name> [title]\n")
  os.exit(2)
end

-- The vocabulary is DATA, which is the whole reason this can be seventy lines:
-- it is a table, so loading it is `dofile` and nothing has to be parsed.
dofile(vocab_path)
local V = _G[global]
if type(V) ~= "table" then
  io.stderr:write(("%s did not define a table called `%s`\n"):format(vocab_path, global))
  os.exit(1)
end

local out = {}
local function w(s) out[#out + 1] = s end

-- Wrapped at 76, because a reference nobody can read in a terminal is one
-- nobody reads. The indent is carried so a nested bullet stays nested.
local function wrap(text, indent)
  indent = indent or ""
  local line, lines = "", {}
  for word in tostring(text):gsub("\n", " "):gmatch("%S+") do
    if #line + #word + 1 > 76 then lines[#lines + 1] = line; line = word
    else line = (line == "") and word or (line .. " " .. word) end
  end
  if line ~= "" then lines[#lines + 1] = line end
  return table.concat(lines, "\n" .. indent)
end

local function type_of(spec)
  if spec.type == "enum" then
    return "enum: " .. table.concat(spec.values or {}, " | ")
  end
  return spec.type
end

local function bullets(list)
  for _, a in ipairs(list or {}) do
    w(("- `%s` — *%s*, %s"):format(a.name, type_of(a),
                                  -- A property is read, or read and set; never "required".
                                  a.access == "read" and "read only"
                                  or a.access == "read_write" and "read and set"
                                  or a.optional and "optional" or "**required**"))
    if a.doc then w("  " .. wrap(a.doc, "  ")) end
  end
end

-- A flat field list: mob_fields, state_fields, weapon_fields and the rest.
local function fields(title, list)
  if list == nil then return end
  w("## " .. title)
  w("")
  bullets(list)
  w("")
end

-- Named entries with arguments: conditions and actions. These get `### `name``
-- headings, which is the anchor shape docs/TRIGGERS.md established and the one
-- the parity tests look for.
local function entries(title, list)
  if list == nil then return end
  w("## " .. title)
  w("")
  for _, e in ipairs(list) do
    w("### `" .. e.name .. "`")
    w("")
    if e.doc then w(wrap(e.doc)); w("") end
    if e.args and #e.args > 0 then bullets(e.args) else w("No arguments.") end
    -- A graph node's PINS (graph_vocab.lua): what it can be linked by.
    if e.exec_in ~= nil then
      local function pins(list) return #list > 0 and ("`" .. table.concat(list, "`, `") .. "`") or "none" end
      w("")
      w(("Exec in: %s. Exec out: %s. Data in: %s. Data out: %s."):format(
        pins(e.exec_in), pins(e.exec_out or {}), pins(e.data_in or {}), pins(e.data_out or {})))
    end
    w("")
  end
end

-- ORDERED EXPLICITLY, not by `pairs`. A generated file whose section order
-- changes between runs would make the currency check fail at random, and a
-- check that fails at random is one somebody turns off.
local SECTIONS = {
  { "mob_fields",     "Mob fields" },
  { "weapon_fields",  "Weapon fields" },
  { "swing_fields",   "`swing`" },
  { "drop_fields",    "`drops` entries" },
  { "drop_choice_fields", "`one_of` choices" },
  { "state_fields",   "State fields" },
  { "phase_fields",   "Phase fields" },
  { "conditions",     "Conditions",  entries },
  { "actions",        "Actions",     entries },
  { "missile_fields", "Missile fields" },
  { "ammo_fields",    "Ammunition fields" },
  { "pickup_fields",  "Pickup fields" },
  { "health_fields",  "Health" },
  { "graph_fields",   "Graph fields" },
  { "node_fields",    "Every node" },
  { "properties",     "Entity properties (get_var, set_var, add_var)" },
  { "nodes",          "Nodes",       entries },
}

for _, sec in ipairs(SECTIONS) do
  local key, title, emit = sec[1], sec[2], sec[3] or fields
  emit(title, V[key])
end

io.write(table.concat(out, "\n"))
