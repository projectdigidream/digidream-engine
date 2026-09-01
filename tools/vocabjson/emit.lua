-- Serialise the trigger vocabulary to JSON, for readers that are not Lua.
--
-- WHY THIS EXISTS. maps/scripts/trigger_vocab.lua is normative: the runtime
-- validates against it and docs/TRIGGERS.md is written from it. The editor is
-- JavaScript in a browser, and the third reader cannot be a second
-- implementation -- a Lua parser in JS is exactly the trap the .map parser
-- already has to pay a hash control for.
--
-- So the table is read by the REAL interpreter, once, and emitted as JSON. The
-- editor reads that. There is still one definition; there is just a machine
-- translating it instead of a person retyping it.
--
-- DETERMINISTIC, because the output is checked in and compared byte for byte:
-- object keys are sorted, `pairs` order never reaches the file, and the
-- indentation is fixed. A generator whose output moved on its own would make
-- the parity check unfalsifiable noise.

local function is_array(t)
  local n = 0
  for k in pairs(t) do
    if type(k) ~= "number" then return false end
    n = n + 1
  end
  -- An empty table emits as `[]`. Every empty table in the vocabulary is an
  -- empty argument list, so that is the right guess, and it is a guess: Lua
  -- cannot tell {} from {}.
  return n == #t
end

-- Only the escapes JSON requires, plus every control character as \u00XX.
-- Doing this by hand rather than reaching for a library, because there is no
-- library in the sandbox and the alphabet is nine cases long.
local ESCAPES = {
  ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
  ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function quote(s)
  local out = s:gsub('[%c"\\]', function (c)
    return ESCAPES[c] or ("\\u%04x"):format(c:byte())
  end)
  return '"' .. out .. '"'
end

local function number(n)
  if n == math.floor(n) and n > -2 ^ 53 and n < 2 ^ 53 then
    return ("%d"):format(n)
  end
  -- Nothing in the vocabulary is fractional today. If something ever is, this
  -- must not silently round it: %.17g round-trips a double exactly.
  return ("%.17g"):format(n)
end

local encode

local function encode_array(t, indent, out)
  if #t == 0 then out[#out + 1] = "[]" return end
  local inner = indent .. "  "
  out[#out + 1] = "[\n"
  for i = 1, #t do
    out[#out + 1] = inner
    encode(t[i], inner, out)
    out[#out + 1] = (i < #t) and ",\n" or "\n"
  end
  out[#out + 1] = indent .. "]"
end

local function encode_object(t, indent, out)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  if #keys == 0 then out[#out + 1] = "{}" return end
  table.sort(keys)
  local inner = indent .. "  "
  out[#out + 1] = "{\n"
  for i = 1, #keys do
    out[#out + 1] = inner .. quote(tostring(keys[i])) .. ": "
    encode(t[keys[i]], inner, out)
    out[#out + 1] = (i < #keys) and ",\n" or "\n"
  end
  out[#out + 1] = indent .. "}"
end

encode = function (v, indent, out)
  local t = type(v)
  if t == "table" then
    if is_array(v) then encode_array(v, indent, out) else encode_object(v, indent, out) end
  elseif t == "string" then
    out[#out + 1] = quote(v)
  elseif t == "number" then
    out[#out + 1] = number(v)
  elseif t == "boolean" then
    out[#out + 1] = v and "true" or "false"
  else
    -- Refuse rather than emit null. A function or a userdata in here means the
    -- vocabulary grew something an editor cannot render, and saying so beats
    -- shipping a hole.
    error(("the vocabulary contains a %s, which cannot be described to a " ..
           "non-Lua reader"):format(t))
  end
end

-- Which global to serialise. The caller sets VOCAB_NAME; the default keeps
-- every existing invocation meaning what it meant.
local name = VOCAB_NAME or "trigger_vocab"
local vocab = _G[name]
if vocab == nil then
  error(("%s.lua does not define a global called '%s'"):format(name, name))
end

local out = {}
encode(vocab, "", out)
VOCAB_JSON = table.concat(out) .. "\n"
