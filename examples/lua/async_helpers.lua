-- examples/lua/async_helpers.lua
-- Canonical async helpers and OnScriptUnload patterns for DoomNetPlatformer

local async = {}

-- Simple in-memory handle registry so OnScriptUnload can cancel outstanding
-- work. Implementations should be engine-provided helpers that dispatch to a
-- worker pool; this file shows usage patterns.
local _handles = {}
local _next_handle = 0

local function new_handle()
  _next_handle = _next_handle + 1
  return tostring(_next_handle)
end

-- Example: async.http_get(url, callback)
-- In a real engine this would hand the work to a worker thread pool and return
-- immediately; the callback fires on the main tick with (err, body).
function async.http_get(url, cb)
  local h = new_handle()
  _handles[h] = true

  -- This is a placeholder: the engine should provide an actual non-blocking
  -- implementation that invokes `cb(err, body)` later. Here we simulate by
  -- scheduling via engine timer (pseudocode) to avoid blocking.
  engine.schedule(0.01, function()
    if not _handles[h] then return end -- cancelled
    local ok, body_or_err = pcall(function()
      -- placeholder synchronous call should be replaced by worker pool
      return engine.net_http_get(url)
    end)
    if ok then cb(nil, body_or_err) else cb(body_or_err, nil) end
    _handles[h] = nil
  end)

  return h
end

-- Cancel a pending async handle. Engine helpers should provide cancellation.
function async.cancel(handle)
  if _handles[handle] then
    _handles[handle] = nil
    return true
  end
  return false
end

-- Example: async.db_query(conn, query, callback)
function async.db_query(conn, query, cb)
  local h = new_handle()
  _handles[h] = true
  engine.schedule(0.01, function()
    if not _handles[h] then return end
    local ok, res_or_err = pcall(function()
      return engine.db_query_sync(conn, query)
    end)
    if ok then cb(nil, res_or_err) else cb(res_or_err, nil) end
    _handles[h] = nil
  end)
  return h
end

-- Example resource tracking: a module that opens resources should register them
local my_resources = {}

function async.open_resource(path)
  local r = engine.resource_open(path)
  table.insert(my_resources, r)
  return r
end

-- OnScriptUnload hook: cancel outstanding handles and close resources.
function OnScriptUnload()
  -- cancel async handles
  for h, _ in pairs(_handles) do
    async.cancel(h)
  end
  -- close resources
  for _, r in ipairs(my_resources) do
    pcall(function() engine.resource_close(r) end)
  end
  my_resources = {}
end

return async
