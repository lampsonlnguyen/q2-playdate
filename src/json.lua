-- Minimal JSON for q2-playdate: the SDK's global `json` library.
-- Chunk argument: pd, the playdate table (for playdate.file in
-- decodeFile/encodeToFile). Sets the global `json`.
-- Implemented from the JSON grammar (RFC 8259); no Panic code is used.
-- Supported: encode, encodePretty, decode, decodeFile, encodeToFile,
-- json.null. Object keys are sorted on encode, so output is deterministic.
local pd = ...

json = {}

json.null = setmetatable({}, {
  __tostring = function() return "null" end,
})

local escape_map = {
  ['"'] = '\\"', ["\\"] = "\\\\", ["\n"] = "\\n", ["\r"] = "\\r",
  ["\t"] = "\\t", ["\b"] = "\\b", ["\f"] = "\\f",
}

local function encode_string(s)
  return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
    return escape_map[c] or string.format("\\u%04x", c:byte())
  end) .. '"'
end

local encode_value
local function encode_table(t, pretty, level)
  local n = #t
  local is_array = n > 0
  if is_array then
    for k in pairs(t) do
      if type(k) ~= "number" or math.type(k) ~= "integer" or k < 1 or k > n then
        is_array = false
        break
      end
    end
  end
  local parts = {}
  if is_array then
    for i = 1, n do parts[#parts + 1] = encode_value(t[i], pretty, level + 1) end
    if #parts == 0 then return "[]" end
    if pretty then
      local pad = string.rep("  ", level + 1)
      local close_pad = string.rep("  ", level)
      return "[\n" .. pad .. table.concat(parts, ",\n" .. pad) ..
        "\n" .. close_pad .. "]"
    end
    return "[" .. table.concat(parts, ",") .. "]"
  end
  -- Object: string keys only, sorted for deterministic output.
  local keys = {}
  for k in pairs(t) do
    if type(k) == "string" then keys[#keys + 1] = k end
  end
  table.sort(keys)
  for _, k in ipairs(keys) do
    parts[#parts + 1] = encode_string(k) .. ":" .. (pretty and " " or "") ..
      encode_value(t[k], pretty, level + 1)
  end
  if #parts == 0 then return "{}" end
  if pretty then
    local pad = string.rep("  ", level + 1)
    local close_pad = string.rep("  ", level)
    return "{\n" .. pad .. table.concat(parts, ",\n" .. pad) ..
      "\n" .. close_pad .. "}"
  end
  return "{" .. table.concat(parts, ",") .. "}"
end

encode_value = function(v, pretty, level)
  local tv = type(v)
  if tv == "nil" or v == json.null then return "null" end
  if tv == "boolean" then return tostring(v) end
  if tv == "number" then
    if math.type(v) == "integer" then return tostring(v) end
    if v ~= v then error("json.encode: cannot encode NaN") end
    if v == math.huge or v == -math.huge then
      error("json.encode: cannot encode infinite number")
    end
    if v == math.floor(v) then return string.format("%.0f", v) end
    return string.format("%.14g", v)
  end
  if tv == "string" then return encode_string(v) end
  if tv == "table" then return encode_table(v, pretty, level or 0) end
  error("json.encode: cannot encode " .. tv)
end

function json.encode(value)
  return encode_value(value, false, 0)
end

function json.encodePretty(value)
  return encode_value(value, true, 0)
end

local function decode(str)
  if type(str) ~= "string" then error("json.decode: string expected") end
  local pos = 1
  local len = #str
  local function err(msg)
    error("json.decode: " .. msg .. " at position " .. pos)
  end
  local function skip_ws()
    while pos <= len do
      local b = str:byte(pos)
      if b == 0x20 or b == 0x09 or b == 0x0A or b == 0x0D then
        pos = pos + 1
      else
        break
      end
    end
  end
  local parse_value
  local function parse_string()
    -- str[pos] == '"'
    pos = pos + 1
    local out = {}
    while pos <= len do
      local c = str:sub(pos, pos)
      if c == '"' then
        pos = pos + 1
        return table.concat(out)
      end
      if c == "\\" then
        local e = str:sub(pos + 1, pos + 1)
        if e == "n" then out[#out + 1] = "\n"; pos = pos + 2
        elseif e == "r" then out[#out + 1] = "\r"; pos = pos + 2
        elseif e == "t" then out[#out + 1] = "\t"; pos = pos + 2
        elseif e == "b" then out[#out + 1] = "\b"; pos = pos + 2
        elseif e == "f" then out[#out + 1] = "\f"; pos = pos + 2
        elseif e == "/" then out[#out + 1] = "/"; pos = pos + 2
        elseif e == "\\" then out[#out + 1] = "\\"; pos = pos + 2
        elseif e == '"' then out[#out + 1] = '"'; pos = pos + 2
        elseif e == "u" then
          local hex = str:sub(pos + 2, pos + 5)
          local cp = tonumber(hex, 16)
          if not cp then err("bad \\u escape") end
          pos = pos + 6
          if cp >= 0xD800 and cp <= 0xDBFF then
            if str:sub(pos, pos + 1) == "\\u" then
              local lo = tonumber(str:sub(pos + 2, pos + 5), 16)
              if lo and lo >= 0xDC00 and lo <= 0xDFFF then
                cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)
                pos = pos + 6
              end
            end
          end
          out[#out + 1] = utf8.char(cp)
        else
          err("bad escape \\" .. e)
        end
      elseif c:byte() < 0x20 then
        err("unescaped control character in string")
      else
        out[#out + 1] = c
        pos = pos + 1
      end
    end
    err("unterminated string")
  end
  local function parse_number()
    local start = pos
    if str:sub(pos, pos) == "-" then pos = pos + 1 end
    while pos <= len and str:sub(pos, pos):match("%d") do pos = pos + 1 end
    if str:sub(pos, pos) == "." then
      pos = pos + 1
      while pos <= len and str:sub(pos, pos):match("%d") do pos = pos + 1 end
    end
    local e = str:sub(pos, pos)
    if e == "e" or e == "E" then
      pos = pos + 1
      local s = str:sub(pos, pos)
      if s == "+" or s == "-" then pos = pos + 1 end
      while pos <= len and str:sub(pos, pos):match("%d") do pos = pos + 1 end
    end
    local value = tonumber(str:sub(start, pos - 1))
    if value == nil then err("bad number") end
    return value
  end
  local function parse_array()
    pos = pos + 1 -- '['
    local arr = {}
    skip_ws()
    if str:sub(pos, pos) == "]" then pos = pos + 1; return arr end
    while true do
      arr[#arr + 1] = parse_value()
      skip_ws()
      local c = str:sub(pos, pos)
      if c == "," then pos = pos + 1
      elseif c == "]" then pos = pos + 1; return arr
      else err("expected ',' or ']' in array") end
    end
  end
  local function parse_object()
    pos = pos + 1 -- '{'
    local obj = {}
    skip_ws()
    if str:sub(pos, pos) == "}" then pos = pos + 1; return obj end
    while true do
      skip_ws()
      if str:sub(pos, pos) ~= '"' then err("expected string key") end
      local key = parse_string()
      skip_ws()
      if str:sub(pos, pos) ~= ":" then err("expected ':' in object") end
      pos = pos + 1
      obj[key] = parse_value()
      skip_ws()
      local c = str:sub(pos, pos)
      if c == "," then pos = pos + 1
      elseif c == "}" then pos = pos + 1; return obj
      else err("expected ',' or '}' in object") end
    end
  end
  parse_value = function()
    skip_ws()
    local c = str:sub(pos, pos)
    if c == "{" then return parse_object() end
    if c == "[" then return parse_array() end
    if c == '"' then return parse_string() end
    if c == "t" and str:sub(pos, pos + 3) == "true" then
      pos = pos + 4; return true
    end
    if c == "f" and str:sub(pos, pos + 4) == "false" then
      pos = pos + 5; return false
    end
    if c == "n" and str:sub(pos, pos + 3) == "null" then
      pos = pos + 4; return json.null
    end
    if c == "-" or c:match("%d") then return parse_number() end
    err("unexpected character '" .. c .. "'")
  end
  local value = parse_value()
  skip_ws()
  if pos <= len then err("trailing characters") end
  return value
end

function json.decode(str)
  return decode(str)
end

-- File forms: decodeFile reads the saves dir first, then the bundle
-- (playdate.file open-for-read order); encodeToFile writes to the saves
-- dir. A missing or invalid file decodes to nil.
-- SDK signature (verified against XTRIS's calls, which pass the path
-- first): encodeToFile(path, [pretty], table) — the pretty boolean is
-- optional, so encodeToFile(path, table) also works. The earlier
-- (table, path) order was wrong and made XTRIS fail at load with
-- "bad argument #1 to 'open' (string expected, got boolean)".
function json.decodeFile(path)
  local file = pd.file.open(path)
  if not file then return nil end
  local data = file:read()
  file:close()
  if data == nil then return nil end
  local ok, value = pcall(decode, data)
  if not ok then return nil end
  return value
end

function json.encodeToFile(path, pretty, value)
  if type(pretty) == "table" then
    value, pretty = pretty, false
  end
  local ok, encoded = pcall(pretty and json.encodePretty or json.encode, value)
  if not ok then return false end
  local file = pd.file.open(path, pd.file.kFileWrite)
  if not file then return false end
  file:write(encoded)
  file:close()
  return true
end
