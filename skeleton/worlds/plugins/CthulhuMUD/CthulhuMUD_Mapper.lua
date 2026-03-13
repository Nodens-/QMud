--[[
Adapted for CthulhuMUD and implemented:

-Sqlite3 storage backend.

-Map Window mouse resizing.

-Excluded look from processing new room. This fixes the problem of losing map location on look
while in duplicate marked unique UID rooms.

-To be filled.

-Need to also add extra documentation

by Tanthul.


CthulhuMUD Mapper

Author: Tanthul
Based on the original work of: Nick Gammon but almost entirely rewritten.
Date:   24th January 2020

 PERMISSION TO DISTRIBUTE

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 and associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.


 LIMITATION OF LIABILITY

 The software is provided "as is", without warranty of any kind, express or implied,
 including but not limited to the warranties of merchantability, fitness for a particular
 purpose and noninfringement. In no event shall the authors or copyright holders be liable
 for any claim, damages or other liability, whether in an action of contract,
 tort or otherwise, arising from, out of or in connection with the software
 or the use or other dealings in the software.

 -------------------------------------------------------------------------

 EXPOSED FUNCTIONS 

  set_line_type (linetype, contents) --> set this current line to be definitely linetype with option contents
  set_line_type_contents (linetype, contents)  --> sets the content for <linetype> to be <contents>
                                                   (for example, if you get a room name on a prompt line)
  set_not_line_type (linetype)       --> set this current line to be definitely not linetype (can call for multiple line types)
  set_area_name (name)               --> sets the name of the area you are in
  set_uid (uid)                      --> sets a string to be hashed as the UID for this room
  do_not_deduce_line_type (linetype) --> do not deduce (do Bayesian analysis) on this type of line - has to be set by set_line_type
  deduce_line_type (linetype)        --> deduce this line type (cancels do_not_deduce_line_type)
  get_last_line_type ()              --> get the previous line type as deduced or set by set_line_type
  get_this_line_type ()              --> get the current overridden line type (from set_line_type)
  set_config_option (name, value)    --> set a mapper configuration value of <name> to <value>
  get_config_option (name)           --> get the current configuration value of <name>
  get_corpus ()                      --> get the corpus (serialized table)
  get_stats ()                       --> get the training stats (serialized table)
  get_database ()                    --> get the mapper database (rooms table) (serialized table)
  get_config ()                      --> get the configuration options (serialized table)
  get_current_room ()                --> gets the current room's UID and room information (serialized table)
  set_room_extras (uid, extras)      --> sets extra information for the room (user-supplied)
                                          extras must be a string which serializes into a variable including a table
                                          eg. " { a = 42, b = 666, c = 'Kobold' } "


  eg. config = CallPlugin ("6edbf5450bf820be31d0c434", "get_config")
               CallPlugin ("6edbf5450bf820be31d0c434", "set_line_type", "exits")
               CallPlugin ("6edbf5450bf820be31d0c434", "do_not_deduce_line_type", "exits")

  Note: The plugin ID is fixed as it is set in the CthulhuMUD_Mapper.xml file near the top:
       id="6edbf5450bf820be31d0c434"

--]]

CTHULHUMUD_MAPPER_LUA_VERSION = 4.0 -- version must agree with plugin version

-- The probability (in the range 0.0 to 1.0) that a line has to meet to be considered a certain line type.
-- The higher, the stricter the requirement.
-- Default of 0.7 seems to work OK, but you could tweak that.

PROBABILITY_CUTOFF = 0.7

-- other modules needed by this plugin
require "mapper"
require "serialize"
require "sqlite3"


-- -----------------------------------------------------------------
-- Zone / Area ID mappings
-- -----------------------------------------------------------------

ZONES = {
  [0]  = "Default",
  [1]  = "North America, 1920",
  [2]  = "Arabia, 1920",
  [3]  = "South America, 1920",
  [4]  = "Europe, 1920",
  [5]  = "Asia, 1920",
  [6]  = "Africa, 1920",
  [7]  = "Antartica, 1920",
  [8]  = "Space",
  [9]  = "Aldebaran",
  [10] = "Dreamlands I",
  [11] = "Dreamlands II",
  [12] = "High Seas",
  [13] = "Immortal Areas",
}

AREAS = {
  [0]   = "[ 25- 65] Arkham Asylum",
  [1]   = "[  All  ] Archam I",
  [2]   = "[  All  ] Archam II",
  [3]   = "[  All  ] Archam III",
  [4]   = "[ 05- 15] Archam Sewers",
  [5]   = "[  ***  ] Arctic Wasteland",
  [6]   = "[ 10- 20] Arkham Sewers South",
  [7]   = "[  All  ] Archam IV",
  [9]   = "[  ***  ] Battleship",
  [14]  = "[  ALL  ] Red Hook",
  [15]  = "[  All  ] Dreamlands",
  [16]  = "[  All  ] Dream Gardens",
  [17]  = "[  All  ] Dreamland Boat Passages",
  [18]  = "[  ***  ] DuBois",
  [19]  = "[ 10- 25] Dunwich",
  [20]  = "[ 20- 50] Sentinel Hill",
  [21]  = "[  5- 55] Dylath-Leen",
  [22]  = "[35 - 50] Oriab II",
  [25]  = "[  ***  ] Special Events",
  [26]  = "[70 - 95] Dark Temple",
  [27]  = "[ 45- 75] Heaven",
  [28]  = "[ 90-***] Kadath - Palace of the Gods",
  [29]  = "[ 30- 90] Y'ha-nthlei",
  [30]  = "[ 20- 45] Oriab Island",
  [32]  = "[  Mare ] The Sub",
  [33]  = "[  ***  ] Migo City",
  [34]  = "[250-***] Nkai",
  [35]  = "[ 10- 35] Aylesbury Pike",
  [38]  = "[ 65- 90] Buried Tomb",
  [39]  = "[  ***  ] Red Hook II",
  [40]  = "[100-200] VonRicknor",
  [41]  = "[  0- 20] Miskatonic University",
  [42]  = "[  ***  ] High Seas",
  [43]  = "[ 20- 30] Road to Innsmouth",
  [44]  = "[  5- 30] Gardener Farm",
  [45]  = "[ 80-150] Dunwich Dream",
  [46]  = "[  ***  ] Waystation",
  [48]  = "[  ***  ] Tcho Tcho Village",
  [49]  = "[  ***  ] Longhand",
  [50]  = "[ 10- 75] The Tomb",
  [52]  = "[ 10- 50] Ulthar",
  [54]  = "[ 25- 45] Witches Hollow",
  [55]  = "[ 15- 75] AEgypt",
  [56]  = "[ 80-***] Serpent Den",
  [57]  = "[  0- 30] Yithian Library",
  [58]  = "[ 30- 80] Yuggoth",
  [59]  = "[ 60-110] Caverns of Zin",
  [60]  = "[160-200] Ebon Lake",
  [61]  = "[  ALL  ] Nyarlathotep",
  [62]  = "[  0- 20] DarKith",
  [64]  = "[ 30- 90] Innsmouth",
  [65]  = "[  ALL  ] Anu-Ka'Ra",
  [66]  = "[ 50-100] Valley of the Ancient Kings",
  [67]  = "[ 100+  ] Tunnels",
  [68]  = "[ 50- 90] Zyneth",
  [71]  = "[060-100] Five points",
  [73]  = "[  ALL  ] Isle of Blood",
  [74]  = "[  ???  ] New Zoogville",
  [75]  = "[ Remort] Yig's Riddle",
  [78]  = "[30 - 60] Castle Rock",
  [80]  = "[  ***  ] Temple of the Fox",
  [83]  = "[  ???  ] New area",
  [86]  = "[ 70 - 100 ] Eternal Darkness",
  [88]  = "[  ???  ] Wisteria Garden Estates",
  [89]  = "[  ???  ] Curled Vines Industrial Park",
  [91]  = "[  ???  ] [CLAN] The Coven",
  [92]  = "[  ???  ] [CLAN] Redhook Municipal",
  [95]  = "[  ***  ] The Conservatory Clan Hall",
  [96]  = "[  ???  ] Abraham's House",
  [97]  = "[  ???  ] Twisting Vines Quarry",
  [98]  = "[  ???  ] Conservatory",
  [100] = "[145-180] Pawtuxet Village",
  [101] = "[  ???  ] The Great Abyss",
}

-- SQLite backend (rooms + nomap + duplicates + corpus only)
local _db = nil
local _pending_room_writes = {}
local _flush_scheduled = false
local _db_flush_token = 0
local _corpus_loaded_from_db = false
local _rooms_loaded_from_db = false
local _duplicates_loaded_from_db = false

local _DUMMY_UID = string.rep('F', 25)  -- dummy UID used to clear/redraw

local function trim (s) return (s:gsub("^%s+",""):gsub("%s+$","")) end
local function fixsql (s)
  if s == nil then return "NULL" end
  s = tostring(s)
  return "'" .. s:gsub("'", "''") .. "'"
end

local function db_path ()
  -- Mushclient root directory (where the plugin state DB lives for this world)
  -- Keep filename convention unchanged.
  local base = GetInfo (66) or ""
  local addr = trim (WorldAddress () or "")
  local port = tostring (WorldPort () or "")
  local plug = string.lower (GetPluginName () or "")
  return base .. addr .. "_" .. port .. "_" .. plug .. "_state.db"
end

local function dbcheck (rc, err)
  if rc == sqlite3.BUSY or (type(err) == "string" and err:find("locked", 1, true)) then
    return false, err, true
  end
  if rc ~= sqlite3.OK and rc ~= sqlite3.ROW and rc ~= sqlite3.DONE and err then
    error (string.format ("SQLite error: %s", tostring(err)))
  end
  return true, err, false
end

local function db_open ()
  if _db then return _db end
  local filename = db_path ()
  _db = sqlite3.open (filename)
  -- keep pragmas minimal to avoid I/O issues in Mushclient root
  -- busy timeout may not exist in older builds, so use PRAGMA
  pcall(function () _db:exec ("PRAGMA busy_timeout = 2000") end)
  pcall(function () _db:exec ("PRAGMA journal_mode = WAL") end)
  pcall(function () _db:exec ("PRAGMA synchronous = NORMAL") end)

 local rc, err = _db:exec ([[
    CREATE TABLE IF NOT EXISTS kv (
      k TEXT PRIMARY KEY,
      v TEXT
    );
    CREATE TABLE IF NOT EXISTS rooms (
      uid TEXT PRIMARY KEY,
      v   TEXT
    );
    CREATE TABLE IF NOT EXISTS nomap (
      id  TEXT PRIMARY KEY,
      v   TEXT
    );
  ]])
  local ok = dbcheck (rc, err)
  return _db
end

local function db_kv_get (k)
  db_open ()
  for row in _db:nrows ("SELECT v FROM kv WHERE k = " .. fixsql(k) .. " LIMIT 1") do
    return row.v
  end
  return nil
end

local function db_kv_set (k, v)
  db_open ()
  local sql =
    "INSERT INTO kv (k, v) VALUES (" .. fixsql(k) .. ", " .. fixsql(v) .. ") " ..
    "ON CONFLICT(k) DO UPDATE SET v=excluded.v " ..
    "WHERE kv.v IS NOT excluded.v"
  local rc, err = _db:exec (sql)
  local ok, _, busy = dbcheck (rc, err)
  if busy then
    -- kv writes are small; drop on busy and retry next save
    return false
  end
  return ok
end

local function db_rooms_load ()
  db_open ()
  rooms = rooms or {}
  for row in _db:nrows ("SELECT uid, v FROM rooms") do
    local chunk = "return " .. row.v
    local f = loadstring (chunk)
    if f then
      local ok, t = pcall (f)
      if ok and type(t) == "table" then
        rooms[(row.uid or ''):upper()] = t
      end
    end
  end
end

local function db_room_upsert (uid, room)
  if not uid or not room then return end
  uid = tostring(uid):upper()
  db_open ()

  if not room._dirty then
    return true
  end

  local v = serialize.save_simple (room)

  local sql =
    "INSERT INTO rooms (uid, v) VALUES (" .. fixsql(uid) .. ", " .. fixsql(v) .. ") " ..
    "ON CONFLICT(uid) DO UPDATE SET v=excluded.v " ..
    "WHERE rooms.v IS NOT excluded.v"

  local rc, err = _db:exec (sql)
  local ok, _, busy = dbcheck (rc, err)
  if busy then
    _pending_room_writes[uid] = v
    if not _flush_scheduled then
      _flush_scheduled = true
      local tok = _db_flush_token
      DoAfterSpecial (0.2, string.format("db_flush_pending(%d)", tok), 12)
    end
    return false
  end
  if ok then
    room._dirty = nil
  end
  return ok
end

-- Empty map splash to override mapper.lua's with useful information.

local function draw_empty_map_splash ()
  if not (mapper and mapper.win) then return end
  if not WindowInfo (mapper.win, 5) then return end

  local w = config.WINDOW.width
  local h = config.WINDOW.height

  local bg = (config.BACKGROUND_COLOUR and config.BACKGROUND_COLOUR.colour) or ColourNameToRGB("black")

  -- clear + frame
  WindowRectOp (mapper.win, miniwin.rect_fill, 0, 0, w, h, bg)
  draw_3d_box (mapper.win, 0, 0, w, h)

  -- ensure a font we control exists (don't depend on mapper.lua's font ids)
  if not draw_empty_map_splash._font_ok then
    local f = get_preferred_font { "DejaVu Sans Mono", "Dina", "Lucida Console", "Fixedsys", "Courier", "Sylfaen"}
    WindowFont (mapper.win, "splash",
      f, 9, true, false, false, false,
      miniwin.font_charset_ansi, miniwin.font_family_any
    )
    draw_empty_map_splash._font_ok = true
  end

  local lines = {
    string.format("CthulhuMUD Mapper %s", tostring(GetPluginInfo(GetPluginID(), 19) or "")),
    "Written by Tanthul",
  }

  local fh = WindowFontInfo (mapper.win, "splash", 1) or 12
  local total_h = #lines * fh
  local y = math.floor((h - total_h) / 2)

  for _, s in ipairs(lines) do
    local tw = WindowTextWidth (mapper.win, "splash", s, true)
    local x = math.floor((w - tw) / 2)
    WindowText (mapper.win, "splash", s, x, y, 0, 0, ColourNameToRGB("white"), true)
    y = y + fh
  end

  Repaint ()
end

---------------------------------------------------------------------
-- Resizable mapper window (no mapper.lua changes)
---------------------------------------------------------------------

local _map_resizing = false
local _map_resize_start = nil
local _map_resize_pending_redraw = false
local _map_resize_prev_w = nil
local _map_resize_prev_h = nil

local function map_resize_install_hotspot ()
  if not (mapper and mapper.win) then return end
  if not WindowInfo (mapper.win, 5) then return end -- not visible / not created

  local w = config.WINDOW.width
  local h = config.WINDOW.height

  -- 16x16 grip in bottom-right corner
  local g = 16
  local x1, y1, x2, y2 = w - g, h - g, w, h

  -- If hotspot already exists, just move it (do NOT delete/re-add; that kills drag)
  local move_ok = false
  local ok, rc = pcall(WindowMoveHotspot, mapper.win, "HS_map_resize", x1, y1, x2, y2)

  -- MUSHclient returns 0 (eOK) on success
  if ok and rc == 0 then
    move_ok = true
  end

  if not move_ok then
    WindowAddHotspot (mapper.win, "HS_map_resize",
      x1, y1, x2, y2,
      "", "", "map_resize_mousedown", "map_resize_cancel_mousedown",
      "", "",  -- mouseup handled by drag handler, no tooltip
      miniwin.cursor_nw_se_arrow, 0
    )
  end

  -- only makes sense if hotspot exists now
  WindowDragHandler (mapper.win, "HS_map_resize",
    "map_resize_mousemove", "map_resize_mouseup", 0
  )

  -- optional: tiny diagonal lines as a visual grip
  WindowLine (mapper.win, x1 + 4, y2 - 4, x2 - 4, y1 + 4, ColourNameToRGB("silver"), 1, 0)
  WindowLine (mapper.win, x1 + 8, y2 - 4, x2 - 4, y1 + 8, ColourNameToRGB("silver"), 1, 0)
end

local function map_resize_draw_grip ()
  if not (mapper and mapper.win) then return end
  if not WindowInfo (mapper.win, 5) then return end

  local w = config.WINDOW.width
  local h = config.WINDOW.height
  local g = 16
  local x1, y1, x2, y2 = w - g, h - g, w, h

  -- clear the grip square first so it doesn't "ink" trails while dragging
  local bg = (config.BACKGROUND_COLOUR and config.BACKGROUND_COLOUR.colour) or ColourNameToRGB("black")
  WindowRectOp (mapper.win, miniwin.rect_fill,
    x1, y1, x2, y2,
    bg
  )

  WindowLine (mapper.win, x1 + 4, y2 - 4, x2 - 4, y1 + 4, ColourNameToRGB("silver"), 1, 0)
  WindowLine (mapper.win, x1 + 8, y2 - 4, x2 - 4, y1 + 8, ColourNameToRGB("silver"), 1, 0)
end

function map_resize_mousedown (flags, hotspot_id)
  if not (mapper and mapper.win) then return end

  _map_resizing = true
  _map_resize_pending_redraw = true

  -- x/y in output-window client coords
  local x = WindowInfo (mapper.win, 17) or 0
  local y = WindowInfo (mapper.win, 18) or 0

  _map_resize_start = {
    x0 = x,
    y0 = y,
    w0 = config.WINDOW.width,
    h0 = config.WINDOW.height,
  }
  _map_resize_prev_w = config.WINDOW.width
  _map_resize_prev_h = config.WINDOW.height
end

function map_resize_cancel_mousedown (flags, hotspot_id)
  _map_resizing = false
  _map_resize_start = nil
end

function map_resize_mousemove (flags, hotspot_id)
  if not _map_resizing or not _map_resize_start then return end
  if not (mapper and mapper.win) then return end

  local x = WindowInfo (mapper.win, 17) or 0
  local y = WindowInfo (mapper.win, 18) or 0

  local dx = x - _map_resize_start.x0
  local dy = y - _map_resize_start.y0

  local new_w = _map_resize_start.w0 + dx
  local new_h = _map_resize_start.h0 + dy

  if new_w < 200 then new_w = 200 end
  if new_w > 1000 then new_w = 1000 end
  if new_h < 200 then new_h = 200 end
  if new_h > 1000 then new_h = 1000 end

  if new_w == config.WINDOW.width and new_h == config.WINDOW.height then
    return
  end

  config.WINDOW.width  = new_w
  config.WINDOW.height = new_h

  local old_w = _map_resize_prev_w or config.WINDOW.width
  local old_h = _map_resize_prev_h or config.WINDOW.height

  local bg = (config.BACKGROUND_COLOUR and config.BACKGROUND_COLOUR.colour) or ColourNameToRGB("black")
  local g = 16

  -- clear previous grip square (prevents trails)
  WindowRectOp (mapper.win, miniwin.rect_fill,
    old_w - g, old_h - g, old_w, old_h,
    bg
  )

  -- clear previous frame strips (draw_3d_box bevel is thicker than 2px)
  local bw = 6  -- border wipe thickness
  WindowRectOp (mapper.win, miniwin.rect_fill,
    old_w - bw, 0, old_w, old_h,
    bg
  )
  WindowRectOp (mapper.win, miniwin.rect_fill,
    0, old_h - bw, old_w, old_h,
    bg
  )

  WindowResize (mapper.win, new_w, new_h, ColourNameToRGB ("black"))

  -- paint the newly-exposed area when expanding so growth is visible immediately
  local bg = (config.BACKGROUND_COLOUR and config.BACKGROUND_COLOUR.colour) or ColourNameToRGB("black")

  if new_w > old_w then
    WindowRectOp (mapper.win, miniwin.rect_fill,
      old_w, 0, new_w, math.min(old_h, new_h),
      bg
    )
  end

  if new_h > old_h then
    WindowRectOp (mapper.win, miniwin.rect_fill,
      0, old_h, new_w, new_h,
      bg
    )
  end

  _map_resize_prev_w = new_w
  _map_resize_prev_h = new_h

  -- redraw window frame live (right/bottom borders otherwise "snap" on mouseup)
  draw_3d_box (mapper.win, 0, 0, new_w, new_h)

  map_resize_draw_grip ()
  Repaint ()
end

function map_resize_mouseup (flags, hotspot_id)
  _map_resizing = false
  _map_resize_start = nil

  if _map_resize_pending_redraw and last_drawn_id then
    _map_resize_pending_redraw = false
    mapper.draw (last_drawn_id)
  else
    map_resize_install_hotspot ()
    Repaint ()
  end
end

-- -------------------------------
-- nomap (unmappable regions)
-- -------------------------------

nomap = nomap or {}  -- id -> table

local function db_nomap_load ()
  db_open ()
  local t = {}
  for row in _db:nrows ("SELECT id, v FROM nomap") do
    if row.id and row.v and row.v ~= "" then
      local env = {}
      local f = assert (loadstring ("return " .. row.v))
      setfenv (f, env)
      local ok, v = pcall (f)
      if ok and type (v) == "table" then
        t [tostring(row.id)] = v
      end
    end
  end
  nomap = t
  return nomap
end

local function db_nomap_upsert (id, v)
  db_open ()
  id = tostring(id)
  local s = serialize.save_simple (v)

  local sql =
    "INSERT INTO nomap (id, v) VALUES (" .. fixsql(id) .. ", " .. fixsql(s) .. ") " ..
    "ON CONFLICT(id) DO UPDATE SET v=excluded.v " ..
    "WHERE nomap.v IS NOT excluded.v"

  local rc, err = _db:exec (sql)
  local ok, _, busy = dbcheck (rc, err)
  if busy then
    return false
  end
  return ok
end

local function db_nomap_delete (id)
  db_open ()
  id = tostring(id)
  local rc, err = _db:exec ("DELETE FROM nomap WHERE id = " .. fixsql(id))
  local ok, _, busy = dbcheck (rc, err)
  if busy then
    return false
  end
  return ok
end

local function nomap_next_id ()
  -- stored in kv so it survives restarts; NOT in plugin-state.
  local s = db_kv_get ("nomap_seq") or ""
  local n = tonumber (s:match("(%d+)")) or 0
  n = n + 1
  db_kv_set ("nomap_seq", tostring(n))
  return string.format ("NM%06d", n)
end

local function nomap_find_by_entrance (from_uid, dir)
  if not from_uid or not dir then return nil end
  for id, t in pairs (nomap or {}) do
    if type(t) == "table" and type(t.links) == "table" then
      for _, e in ipairs (t.links) do
        if e.kind == "in" and e.from == from_uid and e.dir == dir then
          return id
        end
      end
    end
  end
  return nil
end

local function nomap_find_by_out_boundary (real_uid, enter_dir, area_id)
  if not real_uid or not enter_dir then return nil end
  local opp = inverse_direction[enter_dir]
  if not opp then return nil end

  for id, t in pairs (nomap or {}) do
    if type(t) == "table" and t.area_id == area_id and type(t.links) == "table" then
      for _, e in ipairs (t.links) do
        if e.kind == "out" and e.to == real_uid and e.dir == opp then
          return id
        end
      end
    end
  end
  return nil
end

local function nomap_out_exists (id, dir, to)
  local t = nomap[id]
  if not t or type(t.links) ~= "table" then return false end
  for _, e in ipairs (t.links) do
    if e.kind == "out" and e.dir == dir and e.to == to then
      return true
    end
  end
  return false
end

function mapper_redraw_now ()
  if not mapper or not mapper.draw then return end
  local first_uid = next (rooms or {})
  if first_uid then
    current_room = first_uid
    mapper.draw (first_uid)
  else
    current_room = nil
    -- draw an 'unknown' placeholder to clear/redraw the window
    mapper.draw (_DUMMY_UID)
  end
end

function db_flush_pending (tok)
  if tok ~= _db_flush_token then
    return -- stale timer from previous plugin instance
  end

  _flush_scheduled = false
  if not _db then return end

  local n = 0
  for uid, v in pairs (_pending_room_writes) do
    uid = tostring(uid):upper()
    local sql = "REPLACE INTO rooms (uid, v) VALUES (" .. fixsql(uid) .. ", " .. fixsql(v) .. ")"
    local rc, err = _db:exec (sql)
    local ok, _, busy = dbcheck (rc, err)
    if busy then
      if not _flush_scheduled then
        _flush_scheduled = true
        local tok2 = _db_flush_token
        DoAfterSpecial (0.3, string.format("db_flush_pending(%d)", tok2), 12)
      end
      return
    end
    _pending_room_writes[uid] = nil
    n = n + 1
    if n >= 50 then break end
  end

  if next(_pending_room_writes) and not _flush_scheduled then
    _flush_scheduled = true
    local tok3 = _db_flush_token
    DoAfterSpecial (0.1, string.format("db_flush_pending(%d)", tok3), 12)
  end
end

local function db_rooms_replace_all ()
  db_open ()
  local rc, err = _db:exec ("BEGIN IMMEDIATE TRANSACTION")
  local ok, _, busy = dbcheck (rc, err)
  if busy then
    -- fall back to deferred writes; don't block UI
    for uid, room in pairs (rooms or {}) do
      db_room_upsert (uid, room)
    end
    return
  end

  _db:exec ("DELETE FROM rooms")

  for uid, room in pairs (rooms or {}) do
    uid = tostring(uid):upper()
    local v = serialize.save_simple (room)
    _db:exec ("INSERT INTO rooms (uid, v) VALUES (" .. fixsql(uid) .. ", " .. fixsql(v) .. ")")
  end

  _db:exec ("COMMIT")
end

require "copytable"
require "commas"
require "tprint"
require "pairsbykeys"

-- our two windows
win = "window_type_info_" .. GetPluginID ()
learn_window = "learn_dialog_" .. GetPluginID ()

-- -----------------------------------------------------------------
-- Handlers for when a line-type changes
-- -----------------------------------------------------------------

description_styles = { }
exits_styles = { }
room_name_styles = { }

UNKNOWN_DUPLICATE_ROOM = string.rep ("F", 25)  -- dummy UID

-- Unmappable display node (NOT stored in rooms table)
local NOMAP_DRAW_UID = string.rep("X", 25)  -- stable 25-char pseudo room id used only for drawing
current_nomap_id = nil                      -- persisted in plugin state (per-character)

nomap = nomap or {}                         -- stored in SQLite table 'nomap'

-- We temporarily overlay real-room exits to point into NOMAP_DRAW_UID so the map can draw links.
-- We never write these overlays to SQLite; we always restore them when leaving nomap.
local _nomap_overlay = {}   -- array of { from_uid=, dir=, old_dest= }
local function nomap_overlay_clear ()
  for _, o in ipairs (_nomap_overlay) do
    local r = rooms[o.from_uid]
    if r and r.exits then
      if o.old_dest == nil then
        r.exits[o.dir] = nil
      else
        r.exits[o.dir] = o.old_dest
      end
    end
  end
  _nomap_overlay = {}
end

local function nomap_link_exists (id, kind, from, dir, to)
  local t = nomap[id]
  if not t or type(t.links) ~= "table" then return false end
  for _, e in ipairs (t.links) do
    if e.kind == kind and e.from == from and e.dir == dir and e.to == to then
      return true
    end
  end
  return false
end

-- Called by the XML alias: "mapper reset database".
-- Clears mapping state (rooms/duplicates/nomap) from BOTH memory and SQLite.
-- Does NOT touch corpus/training data.
function mapper_reset_database ()
  -- clear in-memory state
  rooms = { }
  duplicates = { }
  nomap = { }

  highest_uid = 0
  inverse_ids = { }
  inverse_desc_hash = { }

  current_room = nil
  last_drawn_id = nil
  last_direction_moved = nil
  from_room = nil

  current_nomap_id = nil
  _pending_nomap_exit = nil

  -- restore any temporary nomap overlays immediately
  nomap_overlay_clear ()

  -- clear deferred room writes
  _pending_room_writes = { }
  _flush_scheduled = false

  -- clear SQLite mapping tables/keys
  pcall (function ()
    db_open ()
    if not _db then return end
    local rc, err = _db:exec ("BEGIN IMMEDIATE TRANSACTION")
    local ok, _, busy = dbcheck (rc, err)
    if busy then
      -- best-effort; leave DB as-is if locked
      return
    end
    _db:exec ("DELETE FROM rooms")
    _db:exec ("DELETE FROM nomap")
    _db:exec ("DELETE FROM kv WHERE k = 'duplicates'")
    _db:exec ("DELETE FROM kv WHERE k = 'nomap_seq'")
    _db:exec ("COMMIT")
  end)

  -- redraw mapper window (clears it)
  pcall (mapper_redraw_now)
end

local function reset_room_tag_state ()
  room_tag_uid = nil
  room_zone_id = nil
  room_area_id = nil
  room_unmappable = false
end

local function nomap_find_by_room (room_uid, exclude_id)
  if not room_uid then return nil end
  for id, t in pairs (nomap or {}) do
    if id ~= exclude_id and type(t) == "table" and type(t.links) == "table" then
      for _, e in ipairs (t.links) do
        if (e.kind == "in" and e.from == room_uid) or (e.kind == "out" and e.to == room_uid) then
          return id
        end
      end
    end
  end
  return nil
end

local function nomap_merge_into (target_id, src_id)
  if not target_id or not src_id then return end
  if target_id == src_id then return end
  if not nomap[target_id] or not nomap[src_id] then return end

  nomap[target_id].links = nomap[target_id].links or {}
  nomap[src_id].links = nomap[src_id].links or {}

  for _, e in ipairs (nomap[src_id].links) do
    if not nomap_link_exists (target_id, e.kind, e.from, e.dir, e.to) then
      table.insert (nomap[target_id].links, e)
    end
  end

  db_nomap_upsert (target_id, nomap[target_id])
  db_nomap_delete (src_id)
  nomap[src_id] = nil
end


-- Nomap link deduper.
local function nomap_link_exists (id, kind, from, dir, to)
  local t = nomap[id]
  if not t or type(t.links) ~= "table" then return false end
  for _, e in ipairs (t.links) do
    if e.kind == kind and e.from == from and e.dir == dir and e.to == to then
      return true
    end
  end
  return false
end



room_tag_uid = nil
room_unmappable = false

DEBUGGING = false

function set_last_direction_moved (where)
  last_direction_moved = where
  DEBUG ("SET: last_direction_moved: " .. tostring (where))
end  -- set_last_direction_moved
function get_last_direction_moved ()
  DEBUG ("get: last_direction_moved: " .. tostring (last_direction_moved))
  return last_direction_moved
end  -- get_last_direction_moved
function set_from_room (where)
  from_room = where
  DEBUG ("SET: from_room: " .. fixuid (tostring (where)))
end  -- set_from_room
function get_from_room (f)
  if f then
    DEBUG ("get: from_room: " .. fixuid (tostring (from_room)) .. " (" .. f .. ")")
  else
    DEBUG ("get: from_room: " .. fixuid (tostring (from_room)))
  end -- if
  return from_room
end  -- get_from_room
function set_current_room (where)
  current_room = where
  DEBUG ("SET: current_room: " .. fixuid (tostring (where)))

  -- NEVER persist the draw-only nomap node
  if where == NOMAP_DRAW_UID then
    return
  end

  if current_room and rooms and rooms[current_room] then
    db_room_upsert (current_room, rooms [current_room])
  end
end
function get_current_room_ (f)
  if f then
    DEBUG ("get: current_room: " .. fixuid (tostring (current_room)) .. " (" .. f .. ")")
  else
    DEBUG ("get: current_room: " .. fixuid (tostring (current_room)))
  end -- if
  return current_room
end  -- get_current_room_

-- -----------------------------------------------------------------
-- description
-- -----------------------------------------------------------------
local _last_desc_fp = nil
function f_handle_description (saved_lines)

  if description and ignore_received then
    return
  end -- if

  -- if the description follows the exits, then ignore descriptions that don't follow exits
  if config.ACTIVATE_DESCRIPTION_AFTER_EXITS then
    if not exits_str then
      return
    end -- if
  end -- if

  -- if the description follows the room name, then ignore descriptions that don't follow the room name
  if config.ACTIVATE_DESCRIPTION_AFTER_ROOM_NAME then
    if not room_name then
      return
    end -- if
  end -- if

  local n = #saved_lines
  if n == 0 then return end

  -- fingerprint without building the big description string
  local first = saved_lines[1].line or ""
  local last  = saved_lines[n].line or ""
  local tot   = 0
  for i = 1, n do
    local s = saved_lines[i].line or ""
    tot = tot + #s
  end
  local fp = n .. "\0" .. tot .. "\0" .. first .. "\0" .. last

  if fp == _last_desc_fp and description then
    return
  end
  _last_desc_fp = fp

  local lines = {}
  description_styles = {}
  for i = 1, n do
    local li = saved_lines[i]
    lines[i] = li.line
    description_styles[i] = li.styles[1]
  end
  description = table.concat(lines, "\n")

  if config.WHEN_TO_DRAW_MAP == DRAW_MAP_ON_DESCRIPTION then
    process_new_room ()
  end -- if
end -- f_handle_description

-- -----------------------------------------------------------------
-- exits
-- -----------------------------------------------------------------
function f_handle_exits ()
  local lines = { }
  exits_styles = { }
  for _, line_info in ipairs (saved_lines) do
    table.insert (lines, line_info.line) -- get text of line
    table.insert (exits_styles, line_info.styles [1])  -- remember first style run
  end -- for each line

  -- Normal player format typically ends up as one logical line like:
  --   "[Exits: north |#|south]"  (and process_new_room gmatch("%w+") works)
  --
  -- Immortal xinfo format is multi-line like:
  --   "Exits:  [ flags ]"
  --   "    south   [ closed pickproof]"
  --   "     down   [ invisible]"
  --
  -- For xinfo we must collapse it to just: "south down"
  local first = lines[1] or ""
  if first:match("^%s*Exits:%s*%[") then
    local t = {}
    for i = 2, #lines do
      local dir = lines[i]:match("^%s*(%w+)%s*%[")
      if dir and valid_direction[dir:lower()] then
        table.insert(t, dir:lower())
      end
    end
    exits_str = table.concat(t, " ")
  else
    exits_str = table.concat (lines, " "):lower ()
  end

  if (config.WHEN_TO_DRAW_MAP == DRAW_MAP_ON_EXITS) then
    process_new_room ()
  end -- if
end -- f_handle_exits

-- -----------------------------------------------------------------
-- room name
-- -----------------------------------------------------------------
function f_handle_name ()
  local lines = { }
  room_name_styles = { }
  for _, line_info in ipairs (saved_lines) do
    table.insert (lines, line_info.line) -- get text of line
    table.insert (room_name_styles, line_info.styles [1])  -- remember first style run
  end -- for each line
  room_name = table.concat (lines, " ")

  -- Extract new mappability tag at end: "[x]" or "[zone-area-subarea-vnum]"
  room_tag_uid = nil
  room_zone_id = nil
  room_area_id = nil
  room_unmappable = false

  -- Strip immortal room vnum prefix: "Room [ 3833]"
  room_name = room_name:gsub("^Room%s*%[%s*%d+%s*%]%s*", "")

  local tag = room_name:match("%s%[([^%]]+)%]%s*$")
  if tag then
    local z, a, s, v = tag:match("^(%d+)%-(%d+)%-(x)%-(x)$")
    if not z then z, a, s, v = tag:match("^(%d+)%-(%d+)%-(x)%-(%d+)$") end
    if not z then z, a, s, v = tag:match("^(%d+)%-(%d+)%-(%d+)%-(x)$") end
    if not z then z, a, s, v = tag:match("^(%d+)%-(%d+)%-(%d+)%-(%d+)$") end

    if z and a and s and v then
      room_zone_id = tonumber(z)
      room_area_id = tonumber(a)
      if v == "x" then
        room_unmappable = true
        room_tag_uid = nil
      else
        room_tag_uid = v
      end
    end
  end

  -- Prepare area string.
  room_area_string = nil

  if room_zone_id and room_area_id then
    local area = AREAS[room_area_id] or "[  ???  ] Unknown Area"
    local zone = ZONES[room_zone_id] or "Unknown Zone"
    set_area_name(string.format("%s (%s)", area, zone))
  end

  -- Strip new mapper suffix: " [zone-area-subarea-vnum]" (subarea/vnum can be digits or x)
  room_name = room_name:gsub("%s%[%d+%-%d+%-%w+%-%w+%]%s*$", "")


  -- Strip optional weather/surface suffix from the sector tag:
  -- "(sector-weather-surface)" -> "(sector)"
  room_name = room_name:gsub("%s%(([^()%-]+)%-.+%-.+%)%s*$", " (%1)")


  -- a bit of a hack, but look for: Room name [N, S, W]
  if config.EXITS_ON_ROOM_NAME then
    local name, exits = string.match (room_name, "^([^%[]+)(%[.*%])%s*$")
    if name then
      room_name = name
      exits_str = exits:lower ()
    end -- if that sort of line found
  end -- if exits on room name wanted

  if config.WHEN_TO_DRAW_MAP == DRAW_MAP_ON_ROOM_NAME then
    process_new_room ()
  end -- if
end -- f_handle_name

-- -----------------------------------------------------------------
-- prompt
-- -----------------------------------------------------------------
function f_handle_prompt ()
  local lines = { }
  for _, line_info in ipairs (saved_lines) do
    table.insert (lines, line_info.line) -- get text of line
  end -- for each line
  prompt = table.concat (lines, " ")
  if config.WHEN_TO_DRAW_MAP == DRAW_MAP_ON_PROMPT then
    if override_contents ['description'] then
      description = override_contents ['description']
    end -- if
    if override_contents ['exits'] then
      exits_str = override_contents ['exits']:lower ()
    end -- if
    if override_contents ['room_name'] then
      room_name = override_contents ['room_name']
    end -- if
    if description and exits_str then
      process_new_room ()
    end -- if
  end -- if time to draw the map
end -- f_handle_prompt

-- -----------------------------------------------------------------
-- ignore this line type
-- -----------------------------------------------------------------
function f_handle_ignore ()
  ignore_received = true
end -- f_handle_ignore

-- -----------------------------------------------------------------
-- cannot move - cancel speedwalk
-- -----------------------------------------------------------------
function f_cannot_move ()
  mapper.cancel_speedwalk ()
  set_last_direction_moved (nil)  -- therefore we haven't moved anywhere
end -- f_cannot_move

-- -----------------------------------------------------------------
-- Handlers for getting the wanted value for a marker for the nominated line
-- -----------------------------------------------------------------

-- these are the types of lines we are trying to classify as a certain line IS or IS NOT that type
line_types = {
  room_name   = { short = "Room name",    handler = f_handle_name,        seq = 1 },
  description = { short = "Description",  handler = f_handle_description, seq = 2 },
  exits       = { short = "Exits",        handler = f_handle_exits,       seq = 3 },
  prompt      = { short = "Prompt",       handler = f_handle_prompt,      seq = 4 },
  ignore      = { short = "Ignore",       handler = f_handle_ignore,      seq = 5 },
  cannot_move = { short = "Can't move",   handler = f_cannot_move,        seq = 6 },
}  -- end of line_types table

function f_first_style_run_foreground (line)
  return { GetStyleInfo(line, 1, 14) or -1 }
end -- f_first_style_run_foreground

function f_show_colour (which, value)
  mapper.mapprint (string.format ("    %20s %5d %5d %7.2f", RGBColourToName (which), value.black, value.red, value.score))
end -- f_show_colour

function f_show_word (which, value)
  if #which > 20 then
    mapper.mapprint (string.format ("%s\n    %20s %5d %5d %7.2f", which, '', value.black, value.red, value.score))
  else
    mapper.mapprint (string.format ("    %20s %5d %5d %7.2f", which, value.black, value.red, value.score))
  end -- if
end -- f_show_colour

function f_first_word (line)
  if not GetLineInfo(line, 1) then
    return {}
  end -- no line available
  return { (string.match (GetLineInfo(line, 1), "^%s*(%a+)") or ""):lower () }
end -- f_first_word

function f_exact_line (line)
  if not GetLineInfo(line, 1) then
    return {}
  end -- no line available
  return { GetLineInfo(line, 1) }
end -- f_exact_line

function f_first_two_words (line)
  if not GetLineInfo(line, 1) then
    return {}
  end -- no line available
  return { (string.match (GetLineInfo(line, 1), "^%s*(%a+%s+%a+)") or ""):lower () }
end -- f_first_two_words

function f_first_three_words (line)
  if not GetLineInfo(line, 1) then
    return {}
  end -- no line available
  return { (string.match (GetLineInfo(line, 1), "^%s*(%a+%s+%a+%s+%a+)") or ""):lower () }
end -- f_first_three_words

function f_all_words (line)
  if not GetLineInfo(line, 1) then
    return {}
  end -- no line available
  local words = { }
  for w in string.gmatch (GetLineInfo(line, 1), "%a+") do
    table.insert (words, w:lower ())
  end -- for
  return words
end -- f_all_words

function f_first_character (line)
  if not GetLineInfo(line, 1) then
    return {}
  end -- no line available
  return { string.match (GetLineInfo(line, 1), "^.") or "" }
end -- f_first_character

-- -----------------------------------------------------------------
-- markers: things we are looking for, like colour of first style run
-- You could add others, for example:
--   * colour of the last style run
--   * number of words on the line
--   * number of style runs on the line
--  Whether that would help or not remains to be seen.

-- The functions above return the value(s) for the corresponding marker, for the nominated line.
-- -----------------------------------------------------------------
markers = {

  {
  desc = "Foreground colour of first style run",
  func = f_first_style_run_foreground,
  marker = "first_style_run_foreground",
  show = f_show_colour,
  accessing_function = pairs,
  },

  {
  desc = "First word in the line",
  func = f_first_word,
  marker = "first_word",
  show = f_show_word,
  accessing_function = pairsByKeys,

  },

 {
  desc = "First two words in the line",
  func = f_first_two_words,
  marker = "first_two_words",
  show = f_show_word,
  accessing_function = pairsByKeys,

  },

 {
  desc = "First three words in the line",
  func = f_first_three_words,
  marker = "first_three_words",
  show = f_show_word,
  accessing_function = pairsByKeys,

  },

  {
  desc = "All words in the line",
  func = f_all_words,
  marker = "all_words",
  show = f_show_word,
  accessing_function = pairsByKeys,

  },

 {
  desc = "Exact line",
  func = f_exact_line,
  marker = "exact_line",
  show = f_show_word,
  accessing_function = pairsByKeys,
  },

--[[

 {
  desc = "First character in the line",
  func = f_first_character,
  marker = "first_character",
  show = f_show_word,

  },

--]]

  } -- end of markers

inverse_markers = { }
for k, v in ipairs (markers) do
  inverse_markers [v.marker] = v
end -- for

local MAX_NAME_LENGTH = 60

-- when to update the map
DRAW_MAP_ON_ROOM_NAME = 1
DRAW_MAP_ON_DESCRIPTION = 2
DRAW_MAP_ON_EXITS = 3
DRAW_MAP_ON_PROMPT = 4


default_config = {
  -- assorted colours
  BACKGROUND_COLOUR       = { name = "Background",        colour =  ColourNameToRGB "#663399", },
  ROOM_COLOUR             = { name = "Room",              colour =  ColourNameToRGB "cyan", },
  EXIT_COLOUR             = { name = "Exit",              colour =  ColourNameToRGB "darkgreen", },
  EXIT_COLOUR_UP_DOWN     = { name = "Exit up/down",      colour =  ColourNameToRGB "orange", },
  EXIT_COLOUR_IN_OUT      = { name = "Exit in/out",       colour =  ColourNameToRGB "#3775E8", },
  OUR_ROOM_COLOUR         = { name = "Our room",          colour =  ColourNameToRGB "black", },
  UNKNOWN_ROOM_COLOUR     = { name = "Unknown room",      colour =  ColourNameToRGB "#00CACA", },
  DIFFERENT_AREA_COLOUR   = { name = "Another area",      colour =  ColourNameToRGB "#009393", },
  SHOP_FILL_COLOUR        = { name = "Shop",              colour =  ColourNameToRGB "darkolivegreen", },
  TRAINER_FILL_COLOUR     = { name = "Trainer",           colour =  ColourNameToRGB "yellowgreen", },
  BANK_FILL_COLOUR        = { name = "Bank",              colour =  ColourNameToRGB "gold", },
  DUPLICATE_FILL_COLOUR   = { name = "Duplicate",         colour =  ColourNameToRGB "lightgoldenrodyellow", },
  BOOKMARK_FILL_COLOUR    = { name = "Notes",             colour =  ColourNameToRGB "lightskyblue", },
  MAPPER_NOTE_COLOUR      = { name = "Messages",          colour =  ColourNameToRGB "lightgreen" },

  ROOM_NAME_TEXT          = { name = "Room name text",    colour = ColourNameToRGB "#BEF3F1", },
  ROOM_NAME_FILL          = { name = "Room name fill",    colour = ColourNameToRGB "#105653", },
  ROOM_NAME_BORDER        = { name = "Room name box",     colour = ColourNameToRGB "black", },

  AREA_NAME_TEXT          = { name = "Area name text",    colour = ColourNameToRGB "#BEF3F1",},
  AREA_NAME_FILL          = { name = "Area name fill",    colour = ColourNameToRGB "#105653", },
  AREA_NAME_BORDER        = { name = "Area name box",     colour = ColourNameToRGB "black", },

  FONT = { name = "DejaVu Sans Mono", size = 8, },

  -- size of map window
  WINDOW = { width = 400, height = 400 },

  -- how far from where we are standing to draw (rooms)
  SCAN = { depth = 30 },

  -- speedwalk delay
  DELAY = { time = 0.3 },

  -- how many seconds to show "recent visit" lines (default 3 minutes)
  LAST_VISIT_TIME = { time = 60 * 3 },

  -- config for learning mapper

  STATUS_BACKGROUND_COLOUR  = "black",       -- the background colour of the status window
  STATUS_FRAME_COLOUR       = "#1B1B1B",     -- the frame colour of the status window
  STATUS_TEXT_COLOUR        = "lightgreen",  -- palegreen is more visible

  UID_SIZE = 4,  -- how many characters of the UID to show

  -- learning configuration
  WHEN_TO_DRAW_MAP = DRAW_MAP_ON_EXITS,        	-- we need to have name/description/exits to draw the map
  ACTIVATE_DESCRIPTION_AFTER_EXITS = false,		  -- descriptions are activated *after* an exit line (used for MUDs with exits then descriptions)
  ACTIVATE_DESCRIPTION_AFTER_ROOM_NAME = true,	-- descriptions are activated *after* a room name line
  BLANK_LINE_TERMINATES_LINE_TYPE = false,     	-- if true, a blank line terminates the previous line type
  ADD_NEWLINE_TO_PROMPT = false,               	-- if true, attempts to add a newline to a prompt at the end of a packet
  SHOW_LEARNING_WINDOW = false,                 -- if true, show the learning status and training windows on startup
  EXITS_ON_ROOM_NAME = false,                  	-- if true, exits are listed on the room name line (eg. Starter Inventory and Shops [E, U])
  INCLUDE_EXITS_IN_HASH = true,                 -- if true, exits are included in the description hash (UID)
  INCLUDE_ROOM_NAME_IN_HASH = true,           	-- if true, the room name is included in the description hash (UID)
  EXITS_IS_SINGLE_LINE = true,                	-- if true, exits are assumed to be only a single line
  PROMPT_IS_SINGLE_LINE = true,                	-- if true, prompts are assumed to be only a single line
  EXIT_LINES_START_WITH_DIRECTION = false,     	-- if true, exit lines must start with a direction (north, south, etc.)
  SORT_EXITS = false,                          	-- if true, exit lines are extracted into words and sorted, excluding any other characters on the line
  LINK_TO_SELF = true,							            -- if true, a room exit can link back to the same room. This supports maze rooms who use this trick often
  SAVE_LINE_INFORMATION = true,                	-- if true, we save to the database the colour of the first style run for name/description/exits

  -- other stuff

  SHOW_INFO = false,              -- if true, information messages are displayed
  SHOW_WARNINGS = true,           -- if true, warning messages are displayed
  SHOW_ROOM_AND_EXITS = false,    -- if true, exact deduced room name and exits are shown (needs SHOW_INFO)

  }

-- -----------------------------------------------------------------
-- Handlers for validating configuration values (eg. colour, boolean)
-- -----------------------------------------------------------------

function config_validate_colour (which)
  local colour = ColourNameToRGB (which)
  if colour == -1 then
    mapper.maperror (string.format ('Colour name "%s" not a valid HTML colour name or code.', which))
    mapper.mapprint ("  You can use HTML colour codes such as '#ab34cd' or names such as 'green'.")
    mapper.mapprint ("  See the Colour Picker (Edit menu -> Colour Picker: Ctrl+Alt+P).")
    return nil, nil
  end -- if bad
  return which, colour
end -- config_validate_colour

function config_validate_uid_size (which)
  local size = tonumber (which)
  if not size then
    mapper.maperror ("Bad UID size: " .. which)
    return nil
  end -- if

  if size < 3 or size > 25 then
    mapper.maperror ("UID size must be in the range 3 to 25")
    return nil
  end -- if

  return size
end -- config_validate_uid_size

-- -----------------------------------------------------------------
-- when we draw the map (after what sort of line)
-- -----------------------------------------------------------------
local when_types = {
    ["room name"]   = DRAW_MAP_ON_ROOM_NAME,
    ["description"] = DRAW_MAP_ON_DESCRIPTION,
    ["exits"]       = DRAW_MAP_ON_EXITS,
    ["prompt"]      = DRAW_MAP_ON_PROMPT,
    } -- end of table

function config_validate_when_to_draw (which)
  local when = which:lower ()

  local w = when_types [when]
  if not w then
    mapper.maperror ("Unknown time to draw the map: " .. which)
    mapper.mapprint ("Valid times are:")
    local t = { }
    for k, v in ipairs (when_types) do
      table.insert (t, k)
    end
    mapper.mapprint ("    " .. table.concat (t, ", "))
    return nil
  end -- if type not found

  return w
end -- when_to_draw

function convert_when_to_draw_to_name (which)
  local when = "Unknown"
  for k, v in pairs (when_types) do
    if which == v then
      when = k
      break
    end -- if
  end -- for
  return when
end -- convert_when_to_draw_to_name

local bools = {
  yes = true,
  y = true,
  no = false,
  n = false
} -- end of bools

function config_validate_boolean (which)
  local which = which:lower ()
  local yesno = bools [which]
  if yesno == nil then
    mapper.maperror ("Invalid option: must be YES or NO")
    return
  end -- not in bools table
  return yesno
end -- config_validate_boolean

-- -----------------------------------------------------------------
-- Handlers for displaying configuration values (eg. colour, boolean)
-- -----------------------------------------------------------------

function config_display_colour (which)
  return which
end -- config_display_colour

function config_display_number (which)
  return tostring (which)
end -- config_display_number

function config_display_when_to_draw (which)
  return convert_when_to_draw_to_name (which)
end -- config_display_when_to_draw

function config_display_boolean (which)
  if which then
    return "Yes"
  else
    return "No"
  end -- if
end -- config_display_boolean

-- -----------------------------------------------------------------
-- Configuration options (ie. mapper config <option>) and their handlers and internal option name
-- -----------------------------------------------------------------

config_control = {
  { option = 'WHEN_TO_DRAW_MAP',                  		name = 'when_to_draw',                     		validate = config_validate_when_to_draw, show = config_display_when_to_draw },
  { option = 'ACTIVATE_DESCRIPTION_AFTER_EXITS',  		name = 'activate_description_after_exits', 		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'ACTIVATE_DESCRIPTION_AFTER_ROOM_NAME',	name = 'activate_description_after_room_name',	validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'ADD_NEWLINE_TO_PROMPT',             		name = 'add_newline_to_prompt',            		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'BLANK_LINE_TERMINATES_LINE_TYPE',   		name = 'blank_line_terminates_line_type',  		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'EXITS_ON_ROOM_NAME',                		name = 'exits_on_room_name',               		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'INCLUDE_EXITS_IN_HASH',             		name = 'include_exits_in_hash',            		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'INCLUDE_ROOM_NAME_IN_HASH',         		name = 'include_room_name_in_hash',        		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'EXITS_IS_SINGLE_LINE',              		name = 'exits_is_single_line',             		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'PROMPT_IS_SINGLE_LINE',             		name = 'prompt_is_single_line',            		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'EXIT_LINES_START_WITH_DIRECTION',   		name = 'exit_lines_start_with_direction',  		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'SORT_EXITS',                        		name = 'sort_exits',                       		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'AUTOMAP_AREA',              				    name = 'automap_area',		             		    validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'STATUS_BACKGROUND_COLOUR',          		name = 'status_background',                		validate = config_validate_colour,       show = config_display_colour },
  { option = 'STATUS_FRAME_COLOUR',               		name = 'status_border',                    		validate = config_validate_colour,       show = config_display_colour },
  { option = 'STATUS_TEXT_COLOUR',                		name = 'status_text',                      		validate = config_validate_colour,       show = config_display_colour },
  { option = 'UID_SIZE',                          		name = 'uid_size',                         		validate = config_validate_uid_size,     show = config_display_number },
  { option = 'SAVE_LINE_INFORMATION',             		name = 'save_line_info',                   		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'SHOW_INFO',                         		name = 'show_info',                        		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'SHOW_WARNINGS',                     		name = 'show_warnings',                    		validate = config_validate_boolean,      show = config_display_boolean },
  { option = 'SHOW_ROOM_AND_EXITS',               		name = 'show_room_and_exits',              		validate = config_validate_boolean,      show = config_display_boolean },

}

-- make a table keyed on the name the user uses
config_control_names = { }
for k, v in ipairs (config_control) do
  config_control_names [v.name] = v
end -- for

-- -----------------------------------------------------------------
-- valid_direction - for detecting movement between rooms, and validating exit lines
-- -----------------------------------------------------------------

valid_direction = {
  n = "n",
  s = "s",
  e = "e",
  w = "w",
  u = "u",
  d = "d",
  ne = "ne",
  sw = "sw",
  nw = "nw",
  se = "se",
  north = "n",
  south = "s",
  east = "e",
  west = "w",
  up = "u",
  down = "d",
  northeast = "ne",
  northwest = "nw",
  southeast = "se",
  southwest = "sw",
  ['in'] = "in",
  out = "out",
  }  -- end of valid_direction

-- -----------------------------------------------------------------
-- inverse_direction - if we go north then the inverse direction is south, and so on.
-- -----------------------------------------------------------------

inverse_direction = {
  n = "s",
  s = "n",
  e = "w",
  w = "e",
  u = "d",
  d = "u",
  ne = "sw",
  sw = "ne",
  nw = "se",
  se = "nw",
  ['in'] = "out",
  out = "in",
  }  -- end of inverse_direction

-- -----------------------------------------------------------------
-- OnPluginDrawOutputWindow
--  Update our line information info
-- -----------------------------------------------------------------
function OnPluginDrawOutputWindow (firstline, offset, notused)

  if not WindowInfo (win, 5) then
    return
  end -- if

  -- cache colours once per redraw
  local background_colour = ColourNameToRGB (config.STATUS_BACKGROUND_COLOUR)
  local frame_colour      = ColourNameToRGB (config.STATUS_FRAME_COLOUR)
  local text_colour       = ColourNameToRGB (config.STATUS_TEXT_COLOUR)
  local gray_colour       = ColourNameToRGB ("darkgray")

  local main_height = GetInfo (280)
  local font_height = GetInfo (212)

  WindowRectOp (win, miniwin.rect_fill, 0, 0, 0, 0, background_colour)
  WindowRectOp (win, miniwin.rect_frame, 0, 0, 0, 0, frame_colour)

  local top =  (((firstline - 1) * font_height) - offset) - 2
  local lastline = firstline + (main_height / font_height)

  for line = firstline, lastline do
    if line >= 1 then
      local exists = GetLineInfo (line, 1)
      if exists then
        -- cache all GetLineInfo results we touch (no repeats)
        local is_note  = GetLineInfo (line, 4)
        local is_input = GetLineInfo (line, 5)

        if not is_note and not is_input then
          local style = GetLineInfo (line, 10)
          local ded = deduced_line_types [style]

          if ded and ded.lt then
            if ded.ov then
              line_type_info = string.format ("<- %s (certain)", line_types [ded.lt].short)
            else
              line_type_info = string.format ("<- %s (%0.0f%%)", line_types [ded.lt].short, (ded.con or 0) * 100)
            end -- if overridden or not

            local x_offset = WindowText (win, font_id, line_type_info, 1, top, 0, 0, text_colour)

            local full = GetLineInfo (line, 3)
            if (not full) and (line >= lastline - 1) then
              x_offset = x_offset + WindowText (win, font_id, " (partial line)", 1 + x_offset, top, 0, 0, gray_colour)
            end -- if

            if ded.draw then
              x_offset = x_offset + WindowText (win, font_id,
                        string.format (" (draw room %s)", fixuid (ded.uid)), 1 + x_offset, top, 0, 0, gray_colour)
            end -- if
          end -- if in deduced_line_types table
        end -- if output line

        top = top + font_height
      end -- if line exists
    end -- if line >= 1
  end -- for each line

end -- OnPluginDrawOutputWindow

-- -----------------------------------------------------------------
-- OnPluginWorldOutputResized
--  On world window resize, remake the miniwindow to fit the size correctly
-- -----------------------------------------------------------------
function OnPluginWorldOutputResized ()

  font_name = GetInfo (20) -- output window font
  font_size = GetOption "output_font_height"

  local output_width  = GetInfo (240)  -- average width of pixels per character
  local wrap_column   = GetOption ('wrap_column')
  local pixel_offset  = GetOption ('pixel_offset')

  -- make window so I can grab the font info
  WindowCreate (win,
                (output_width * wrap_column) + pixel_offset + 10, -- left
                0,  -- top
                400, -- width
                GetInfo (263),   -- world window client height
                miniwin.pos_top_left,   -- position (irrelevant)
                miniwin.create_absolute_location,   -- flags
                ColourNameToRGB (config.STATUS_BACKGROUND_COLOUR))   -- background colour

  -- add font
  WindowFont (win, font_id, font_name, font_size,
              false, false, false, false,  -- normal
              miniwin.font_charset_ansi, miniwin.font_family_any)

  -- find height of font for future calculations
  font_height = WindowFontInfo (win, font_id, 1)  -- height

  WindowSetZOrder(win, -5)

   if WindowInfo (learn_window, 5) then
     WindowShow (win)
   end -- if

end -- OnPluginWorldOutputResized

-- -----------------------------------------------------------------
-- INFO helper function for debugging the plugin (information messages)
-- -----------------------------------------------------------------
function INFO (...)
  if config.SHOW_INFO then
    ColourNote ("orange", "", table.concat ( { ... }, " "))
  end -- if
end -- INFO

-- -----------------------------------------------------------------
-- WARNING helper function for debugging the plugin (warning/error messages)
-- -----------------------------------------------------------------
function WARNING (...)
  if config.SHOW_WARNINGS then
    ColourNote ("red", "", table.concat ( { ... }, " "))
  end -- if
end -- WARNING

-- -----------------------------------------------------------------
-- DEBUG helper function for debugging the plugin
-- -----------------------------------------------------------------
function DEBUG (...)
  if DEBUGGING then
    ColourNote ("cornflowerblue", "", table.concat ( { ... }, " "))
  end -- if
end -- DEBUG


-- -----------------------------------------------------------------
-- corpus_reset - throw away the learned corpus
-- -----------------------------------------------------------------
function corpus_reset (empty)
  if empty then
    corpus = { }
    stats  = { }
  end -- if

  -- make sure each line type is in the corpus

  for k, v in pairs (line_types) do
    if not corpus [k] then
      corpus [k] = {}
    end -- not there yet

    if not stats [k] then
      stats [k] = { is = 0, isnot = 0 }
    end -- not there yet

    for k2, v2 in ipairs (markers) do
      if not corpus [k] [v2.marker] then  -- if that marker not there, add it
         corpus [k] [v2.marker] = { } -- table of values for this marker
      end -- marker not there yet

    end -- for each marker type
  end -- for each line type

end -- corpus_reset

LEARN_WINDOW_WIDTH = 300
LEARN_WINDOW_HEIGHT = 270
LEARN_BUTTON_WIDTH = 80
LEARN_BUTTON_HEIGHT = 30

hotspots = { }
button_down = false

-- -----------------------------------------------------------------
-- button_mouse_down - generic mouse-down handler
-- -----------------------------------------------------------------
function button_mouse_down (flags, hotspot_id)
  local hotspot_info = hotspots [hotspot_id]
  if not hotspot_info then
    WARNING ("No info found for hotspot", hotspot_id)
    return
  end

  -- no button state change if no selection
  if GetSelectionStartLine () == 0 then
    return
  end -- if

  button_down = true
  WindowRectOp (hotspot_info.window, miniwin.rect_draw_edge,
                hotspot_info.x1, hotspot_info.y1, hotspot_info.x2, hotspot_info.y2,
                miniwin.rect_edge_sunken,
                miniwin.rect_edge_at_all + miniwin.rect_option_fill_middle)  -- sunken, filled
  WindowText   (hotspot_info.window, hotspot_info.font, hotspot_info.text, hotspot_info.text_x + 1, hotspot_info.y1 + 8 + 1, 0, 0, ColourNameToRGB "black", true)
  Redraw ()

end -- button_mouse_down

-- -----------------------------------------------------------------
-- button_cancel_mouse_down - generic cancel-mouse-down handler
-- -----------------------------------------------------------------
function button_cancel_mouse_down (flags, hotspot_id)
  local hotspot_info = hotspots [hotspot_id]
  if not hotspot_info then
    WARNING ("No info found for hotspot", hotspot_id)
    return
  end

  button_down = false
  buttons_active = nil

  WindowRectOp (hotspot_info.window, miniwin.rect_draw_edge,
                hotspot_info.x1, hotspot_info.y1, hotspot_info.x2, hotspot_info.y2,
                miniwin.rect_edge_raised,
                miniwin.rect_edge_at_all + miniwin.rect_option_fill_middle)  -- raised, filled
  WindowText   (hotspot_info.window, hotspot_info.font, hotspot_info.text, hotspot_info.text_x, hotspot_info.y1 + 8, 0, 0, ColourNameToRGB "black", true)

  Redraw ()
end -- button_cancel_mouse_down

-- -----------------------------------------------------------------
-- button_mouse_up - generic mouse-up handler
-- -----------------------------------------------------------------
function button_mouse_up (flags, hotspot_id)
  local hotspot_info = hotspots [hotspot_id]
  if not hotspot_info then
    WARNING ("No info found for hotspot", hotspot_id)
    return
  end

  button_down = false
  buttons_active = nil

  -- call the handler
  hotspot_info.handler ()

  WindowRectOp (hotspot_info.window, miniwin.rect_draw_edge,
                hotspot_info.x1, hotspot_info.y1, hotspot_info.x2, hotspot_info.y2,
                miniwin.rect_edge_raised,
                miniwin.rect_edge_at_all + miniwin.rect_option_fill_middle)  -- raised, filled
  WindowText   (hotspot_info.window, hotspot_info.font, hotspot_info.text, hotspot_info.text_x, hotspot_info.y1 + 8, 0, 0, ColourNameToRGB "black", true)

  Redraw ()
end -- button_mouse_up

-- -----------------------------------------------------------------
-- make_button - make a button for the dialog window and remember its handler
-- -----------------------------------------------------------------
function make_button (window, font, x, y, text, tooltip, handler)

  WindowRectOp (window, miniwin.rect_draw_edge, x, y, x + LEARN_BUTTON_WIDTH, y + LEARN_BUTTON_HEIGHT,
            miniwin.rect_edge_raised,
            miniwin.rect_edge_at_all + miniwin.rect_option_fill_middle)  -- raised, filled

  local width = WindowTextWidth (window, font, text, true)
  local text_x = x + (LEARN_BUTTON_WIDTH - width) / 2

  WindowText   (window, font, text, text_x, y + 8, 0, 0, ColourNameToRGB "black", true)

  local hotspot_id = string.format ("HS_learn_%d,%d", x, y)
  -- remember handler function
  hotspots [hotspot_id] = { handler = handler,
                            window = window,
                            x1 = x, y1 = y,
                            x2 = x + LEARN_BUTTON_WIDTH, y2 = y + LEARN_BUTTON_HEIGHT,
                            font = font,
                            text = text,
                            text_x = text_x }

  WindowAddHotspot(window,
                  hotspot_id,
                   x, y, x + LEARN_BUTTON_WIDTH, y + LEARN_BUTTON_HEIGHT,
                   "",                          -- MouseOver
                   "",                          -- CancelMouseOver
                   "button_mouse_down",         -- MouseDown
                   "button_cancel_mouse_down",  -- CancelMouseDown
                   "button_mouse_up",           -- MouseUp
                   tooltip,                     -- tooltip text
                   miniwin.cursor_hand,         -- mouse cursor shape
                   0)                           -- flags


end -- make_button

-- -----------------------------------------------------------------
-- update_buttons - grey-out buttons if nothing selected
-- -----------------------------------------------------------------

buttons_active = nil

function update_buttons (name)

  -- throttle: timer can fire extremely frequently; avoid pegging UI
  _last_update_buttons_clock = _last_update_buttons_clock or 0
  local _now_clock = os.clock ()
  if (_now_clock - _last_update_buttons_clock) < 0.25 then
    return
  end
  _last_update_buttons_clock = _now_clock

  -- if the learning window isn't visible, there's nothing useful to update and this can be expensive
  if learn_window then
    local vis = WindowInfo (learn_window, 5)
    if not vis or vis == 0 then
      return
    end
  else
    return
  end


  -- to save memory, throw away info for lines more than 1000 further back in the buffer
  local this_line = GetLinesInBufferCount()         -- which line in the output buffer
  local line_number = GetLineInfo (this_line, 10)   -- which line this was overall
  local wanted_line_number = line_number - 1000     -- keep info for 1000 lines

  if line_number then
    for k in pairs (deduced_line_types) do
       if k < wanted_line_number then
         deduced_line_types [k] = nil
        end -- for
    end -- for
  end -- if we have any lines

  -- do nothing if button pressed
  if button_down then
    return
  end -- if

  local have_selection = GetSelectionStartLine () ~= 0

  -- do nothing if the state hasn't changed
  if have_selection == buttons_active then
    return
  end -- if

  buttons_active = have_selection

  for hotspot_id, hotspot_info in pairs (hotspots) do
    if string.match (hotspot_id, "^HS_learn_") then
      local wanted_colour = ColourNameToRGB "black"
      if not buttons_active then
        wanted_colour = ColourNameToRGB "silver"
      end -- if
      WindowText   (hotspot_info.window, hotspot_info.font, hotspot_info.text, hotspot_info.text_x, hotspot_info.y1 + 8, 0, 0, wanted_colour, true)
    end -- if a learning button
  end -- for

  Redraw ()

end -- update_buttons

-- -----------------------------------------------------------------
-- mouseup_close_configure - they hit the close box in the learning window
-- -----------------------------------------------------------------
function mouseup_close_configure  (flags, hotspot_id)
  WindowShow (learn_window, false)
  WindowShow (win, false)
  mapper.mapprint ('Type: "mapper learn" to show the training window again')
  config.SHOW_LEARNING_WINDOW = false
end -- mouseup_close_configure

-- -----------------------------------------------------------------
-- toggle_learn_window - toggle the window: called from "mapper learn"
-- -----------------------------------------------------------------
function toggle_learn_window (name, line, wildcards)
  if WindowInfo (learn_window, 5) then
    WindowShow (win, false)
    WindowShow (learn_window, false)
    config.SHOW_LEARNING_WINDOW = false
  else
    WindowShow (win, true)
    WindowShow (learn_window, true)
    config.SHOW_LEARNING_WINDOW = true
  end -- if
end -- toggle_learn_window

-- -----------------------------------------------------------------
-- Plugin Install
-- -----------------------------------------------------------------
function OnPluginInstall ()
font_id = "f"

  -- this table has the counters
  corpus = { }

  -- stats
  stats = { }

  _db_flush_token = os.time()
  _flush_scheduled = false

  -- load corpus (SQLite)
  local corpus_s = db_kv_get ("corpus")
  if corpus_s and corpus_s ~= "" then
    assert (loadstring (corpus_s)) ()
  end
  _corpus_loaded_from_db = (corpus_s and corpus_s ~= "") and type(corpus)=="table" and next(corpus) ~= nil or false
  -- load stats
  assert (loadstring (GetVariable ("stats") or "")) ()
  -- start with default configuration (deep copy so we don't mutate default_config)
  local function _deep_copy (t)
    if type (t) ~= "table" then return t end
    local r = { }
    for k, v in pairs (t) do
      r [_deep_copy (k)] = _deep_copy (v)
    end
    return r
  end

  local function _deep_merge (dst, src)
    if type (src) ~= "table" then return dst end
    for k, v in pairs (src) do
      if type (v) == "table" and type (dst [k]) == "table" then
        _deep_merge (dst [k], v)
      else
        dst [k] = v
      end
    end
    return dst
  end


-- normalize colour config entries so they always stay as { name=..., colour=... }
-- (older saved configs sometimes store colours as plain numbers/strings)
local function _normalize_colour_entries (cfg, defaults)
  for k, dv in pairs (defaults) do
    if type (dv) == "table" and dv.name and dv.colour ~= nil then
      local cv = cfg [k]
      if type (cv) == "number" then
        cfg [k] = { name = dv.name, colour = cv }
      elseif type (cv) == "string" then
        local rgb = ColourNameToRGB (cv)
        if rgb == -1 then
          rgb = dv.colour
        end
        cfg [k] = { name = dv.name, colour = rgb }
      elseif type (cv) == "table" then
        if cv.name == nil then cv.name = dv.name end
        if cv.colour == nil then cv.colour = dv.colour end
      else
        -- missing: keep default
        cfg [k] = { name = dv.name, colour = dv.colour }
      end
    elseif type (dv) == "table" and type (cfg [k]) == "table" then
      _normalize_colour_entries (cfg [k], dv)
    end
  end
end

  config = _deep_copy (default_config)

  -- get saved configuration (if any) and merge it over defaults
  local saved_config = nil
  local cfg_chunk = GetVariable ("config") or ""
  if cfg_chunk ~= "" then
    local ok = pcall (function () assert (loadstring (cfg_chunk)) () end)
    if ok and type (config) == "table" then
      saved_config = config
    end
  end

  config = _deep_copy (default_config)
  if type (saved_config) == "table" then
    _deep_merge (config, saved_config)
  else
    -- no saved config: persist defaults to the plugin state file
    SetVariable ("config", "config = " .. serialize.save_simple (config))
  end


-- ensure all colour entries are present and in the expected table format
_normalize_colour_entries (config, default_config)

-- if we loaded a saved config, write back the normalized merged config (keeps future loads consistent)
if type (saved_config) == "table" then
  SetVariable ("config", "config = " .. serialize.save_simple (config))
end

  -- Initialize the proper trigger if we loaded saved configuration -- Tanthul
  if config.AUTOMAP_AREA then
    EnableTrigger ("Area_line", false)
	EnableTriggerGroup ("Area_scan", true)
  else
    EnableTriggerGroup ("Area_scan", false)
	EnableTrigger ("Area_line", true)
  end -- if

  corpus_reset ()
  -- rooms/duplicates are loaded from SQLite before mapper init (rooms are keyed by vnum UID)
rooms = { }
duplicates = { }

db_rooms_load ()
_rooms_loaded_from_db = true

local dup_s = db_kv_get ("duplicates")
if dup_s and dup_s ~= "" then
  pcall (function () assert (loadstring (dup_s)) () end)
end
_duplicates_loaded_from_db = true
  
  -- load nomap table (SQLite)
  db_nomap_load ()

  -- load current nomap id (plugin state, per-character)
  current_nomap_id = nil
  local nm_chunk = GetVariable ("current_nomap_id") or ""
  if nm_chunk ~= "" then
    pcall (function () assert(loadstring(nm_chunk))() end)
  end

  -- initialize mapper
  mapper.init {
              config = config,            -- our configuration table
              get_room = get_room,        -- get info about a room
              room_click = room_click,    -- called on RH click on room square
              show_other_areas = true,    -- show all areas
--              show_help = OnHelp,         -- to show help
  }

  map_resize_install_hotspot ()

  local _orig_mapper_draw = mapper.draw
  mapper.draw = function (...)
    if _map_resizing then
      _map_resize_pending_redraw = true
      return
    end

     -- suppress mapper.lua's splash screen for dummy draws; show our own splash instead
    if uid == _DUMMY_UID then
      draw_empty_map_splash ()
      map_resize_install_hotspot ()
      return
    end

    local r = _orig_mapper_draw (...)
    map_resize_install_hotspot ()
    return r
  end


  -- ensure mapper window shows loaded rooms immediately
  pcall (mapper_redraw_now)

  mapper.mapprint (string.format ("[%s version %0.1f]", GetPluginName(), GetPluginInfo (GetPluginID (), 19)))
  mapper.mapprint (string.format ("MUSHclient mapper installed, version %0.1f", mapper.VERSION))

  local rooms_count = 0
  local explored_exits_count = 0
  local unexplored_exits_count = 0

  for uid, room in pairs (rooms) do
    rooms_count = rooms_count + 1
    for dir, exit_uid in pairs (room.exits) do
      if exit_uid == '0' then
        unexplored_exits_count = unexplored_exits_count + 1
      else
        explored_exits_count = explored_exits_count + 1
      end -- if
    end -- for each exit
  end -- for each room

  mapper.mapprint (string.format (
        "Mapper database loaded: %d rooms, %d explored exits, %d unexplored exits.",
         rooms_count, explored_exits_count, unexplored_exits_count))

  OnPluginWorldOutputResized ()

 -- find where window was last time

  windowinfo = movewindow.install (learn_window, miniwin.pos_center_right)

  learnFontName = get_preferred_font {"Dina",  "Lucida Console",  "Fixedsys", "Courier", "Sylfaen",}
  learnFontId = "f"
  learnFontSize = 9

  WindowCreate (learn_window,
                 windowinfo.window_left,
                 windowinfo.window_top,
                 LEARN_WINDOW_WIDTH,
                 LEARN_WINDOW_HEIGHT,
                 windowinfo.window_mode,   -- top right
                 windowinfo.window_flags,
                 ColourNameToRGB "lightcyan")

  WindowFont (learn_window, learnFontId, learnFontName, learnFontSize,
              true, false, false, false,  -- bold
              miniwin.font_charset_ansi, miniwin.font_family_any)

  -- find height of font for future calculations
  learn_font_height = WindowFontInfo (learn_window, font_id, 1)  -- height

  -- let them move it around
  movewindow.add_drag_handler (learn_window, 0, 0, 0, learn_font_height + 5)
  WindowRectOp (learn_window, miniwin.rect_fill, 0, 0, 0, learn_font_height + 5, ColourNameToRGB "darkblue", 0)
  draw_3d_box  (learn_window, 0, 0, LEARN_WINDOW_WIDTH, LEARN_WINDOW_HEIGHT)
  DIALOG_TITLE = "Learn line type"
  local width = WindowTextWidth (learn_window, learnFontId, DIALOG_TITLE, true)
  local x = (LEARN_WINDOW_WIDTH - width) / 2
  WindowText   (learn_window, learnFontId, DIALOG_TITLE, x, 3, 0, 0, ColourNameToRGB "white", true)

 -- close box
  local box_size = learn_font_height - 2
  local GAP = 5
  local y = 3
  local x = 1

  WindowRectOp (learn_window,
                miniwin.rect_frame,
                x + LEARN_WINDOW_WIDTH - box_size - GAP * 2,
                y + 1,
                x + LEARN_WINDOW_WIDTH - GAP * 2,
                y + 1 + box_size,
                0x808080)
  WindowLine (learn_window,
              x + LEARN_WINDOW_WIDTH - box_size - GAP * 2 + 3,
              y + 4,
              x + LEARN_WINDOW_WIDTH - GAP * 2 - 3,
              y - 2 + box_size,
              0x808080,
              miniwin.pen_solid, 1)
  WindowLine (learn_window,
              x - 4 + LEARN_WINDOW_WIDTH - GAP * 2,
              y + 4,
              x - 1 + LEARN_WINDOW_WIDTH - box_size - GAP * 2 + 3,
              y - 2 + box_size,
              0x808080,
              miniwin.pen_solid, 1)

  -- close configuration hotspot
  WindowAddHotspot(learn_window, "close_learn_dialog",
                   x + LEARN_WINDOW_WIDTH - box_size - GAP * 2,
                   y + 1,
                   x + LEARN_WINDOW_WIDTH - GAP * 2,
                   y + 1 + box_size,   -- rectangle
                   "", "", "", "", "mouseup_close_configure",  -- mouseup
                   "Click to close",
                   miniwin.cursor_hand, 0)  -- hand cursor


  -- the buttons for learning
  local LABEL_LEFT = 10
  local YES_BUTTON_LEFT = 100
  local NO_BUTTON_LEFT = YES_BUTTON_LEFT + LEARN_BUTTON_WIDTH + 20

  -- get the line types into my preferred order
  local sorted_line_types = { }
  for type_name in pairs (line_types) do
    table.insert (sorted_line_types, type_name)
  end -- for
  table.sort (sorted_line_types, function (a, b) return line_types [a].seq < line_types [b].seq end)

  local y = learn_font_height + 10
  for _, type_name in ipairs (sorted_line_types) do
    local type_info = line_types [type_name]
    WindowText   (learn_window, learnFontId, type_info.short, LABEL_LEFT, y + 8, 0, 0, ColourNameToRGB "black", true)

    make_button (learn_window, learnFontId, YES_BUTTON_LEFT, y, "Yes", "Learn selection IS " .. type_info.short,
                  function () learn_line_type (type_name, true) end)
    make_button (learn_window, learnFontId, NO_BUTTON_LEFT,  y, "No",  "Learn selection is NOT " .. type_info.short,
                  function () learn_line_type (type_name, false) end)

    y = y + LEARN_BUTTON_HEIGHT + 10

  end -- for

  WindowShow (learn_window, config.SHOW_LEARNING_WINDOW)
  WindowShow (win, config.SHOW_LEARNING_WINDOW)
  
  override_noprocess = true
  SendNoEcho ("look")

end -- OnPluginInstall

-- -----------------------------------------------------------------
-- OnPluginClose
-- -----------------------------------------------------------------
function OnPluginClose ()
  WindowShow (learn_window, false)
  WindowShow (win, false)
  mapper.hide ()  -- hide the map

  -- stop any deferred DB write storms
  _pending_room_writes = {}
  _flush_scheduled = false

  -- close SQLite handle so reinstall / reload does not leave the DB locked
  if _db then
    pcall (function () _db:close () end)
    _db = nil
  end
end -- OnPluginClose

-- -----------------------------------------------------------------
-- Plugin Save State
-- -----------------------------------------------------------------
function OnPluginSaveState ()
mapper.save_state ()
  db_kv_set ("corpus", "corpus = " .. serialize.save_simple (corpus))
  db_kv_set ("duplicates", "duplicates = " .. serialize.save_simple (duplicates))
  SetVariable ("stats", "stats = " .. serialize.save_simple (stats))
  SetVariable ("config", "config = " .. serialize.save_simple (config))
  SetVariable ("current_nomap_id", "current_nomap_id = " .. (current_nomap_id and serialize.save_simple (current_nomap_id) or "nil"))

  movewindow.save_state (learn_window)

end -- OnPluginSaveState

local C1 = 2   -- weightings
local C2 = 1
local weight = 1
local MAX_WEIGHT = 2.0

-- calculate the probability one word is red or black
function CalcProbability (red, black)
 local pResult = ( (black - red) * weight )
                 / (C1 * (black + red + C2) * MAX_WEIGHT)
  return 0.5 + pResult
end -- CalcProbability


-- -----------------------------------------------------------------
-- update_corpus
--  add one to red or black for a certain value, for a certain type of line, for a certain marker type
-- -----------------------------------------------------------------
function update_corpus (which, marker, value, black)
  local which_corpus = corpus [which] [marker]
  -- make new one for this value if necessary
  if not which_corpus [value] then
     which_corpus [value] = { red = 0, black = 0, score = 0 }
  end -- end of this value not there yet
  if black then
     which_corpus [value].black = which_corpus [value].black + 1
  else
     which_corpus [value].red = which_corpus [value].red + 1
  end -- if
  which_corpus [value].score = assert (CalcProbability (which_corpus [value].red, which_corpus [value].black))
end -- update_corpus


-- -----------------------------------------------------------------
-- learn_line_type
--  The user is training a line type. Update the corpus for each line type to show that this set of
--  markers is/isn't in it.
-- -----------------------------------------------------------------
function learn_line_type (which, black)

  start_line = GetSelectionStartLine ()
  end_line = GetSelectionEndLine ()

  if start_line == 0 then
     WARNING ("No line(s) selected - select one or more lines (or part lines)")
     return
  end -- if

  if black then
    stats [which].is = stats [which].is + 1
  else
    stats [which].isnot = stats [which].isnot + 1
  end -- if

  -- do all lines in the selection
  for line = start_line, end_line do
    -- process all the marker types, and add 1 to the red/black counter for that particular marker
    for k, v in ipairs (markers) do
      local values = v.func (line) -- call handler to get values
      for _, value in ipairs (values) do
        update_corpus (which, v.marker, value, black)
      end -- for each value

    end -- for each type of marker
  end -- for each line

  -- INFO (string.format ("Selection is from %d to %d", start_line, end_line))

  local s = ":"
  if not black then
    s = ": NOT"
  end -- if

  -- INFO ("Selected lines " .. s .. " " .. which)

  -- tprint (corpus)

  Pause (false)

end -- learn_line_type

--   See:
--     http://www.paulgraham.com/naivebayes.html
--   For a good explanation of the background, see:
--     http://www.mathpages.com/home/kmath267.htm.

-- -----------------------------------------------------------------
-- SetProbability
-- calculate the probability a bunch of markers are ham (black)
--  using an array of probabilities, get an overall one
-- -----------------------------------------------------------------
function SetProbability (probs)
  local n, inv = 1, 1
  local i = 0
  for k, v in pairs (probs) do
    n = n * v
    inv = inv * (1 - v)
    i = i + 1
  end
  return  n / (n + inv)
end -- SetProbability

-- DO NOT DEBUG TO THE OUTPUT WINDOW IN THIS FUNCTION!
-- -----------------------------------------------------------------
-- analyse_line
-- work out type of line by comparing its markers to the corpus
-- -----------------------------------------------------------------
function analyse_line (line)
  local result = {}
  local line_type_probs = {}
  local marker_values = { }

  if Trim (GetLineInfo (line, 1)) == "" then
    return nil
  end -- if blank line

  -- get the values first, they will stay the same for all line types
  for _, m in ipairs (markers) do
    marker_values [m.marker] = m.func (line) -- call handler to get values
  end -- for each type of marker

  for line_type, line_type_info in pairs (line_types) do
     -- don't if they don't want Bayesian deduction for this type
    if not do_not_deduce_linetypes [line_type] and not line_is_not_line_type [line_type] then
      local probs = { }
      for _, m in ipairs (markers) do
        marker_probs = { }  -- probability for this marker
        local values = marker_values [m.marker] -- get previously-retrieved values
        for _, value in ipairs (values) do
          local corpus_value = corpus [line_type] [m.marker] [value]
          if corpus_value then
            assert (type (corpus_value) == 'table', 'corpus_value not a table')
            --table.insert (probs, corpus_value.score)
            table.insert (marker_probs, corpus_value.score)
          end -- of having a value
        end -- for each value
        table.insert (probs, SetProbability (marker_probs))
      end -- for each type of marker
      local score = SetProbability (probs)
      table.insert (result, string.format ("%s: %3.2f", line_type_info.short, score))
      local first_word = (string.match (GetLineInfo(line, 1), "^%s*(%a+)") or ""):lower ()

      if line_type ~= 'exits' or
        (not config.EXIT_LINES_START_WITH_DIRECTION) or
        valid_direction [first_word] then
          table.insert (line_type_probs, { line_type = line_type, score = score } )
      end -- if
    end -- allowed to deduce this line type
  end -- for each line type
  table.sort (line_type_probs, function (a, b) return a.score > b.score end)
  if line_type_probs [1].score > PROBABILITY_CUTOFF then
    return line_type_probs [1].line_type, line_type_probs [1].score
  else
    return nil
  end -- if
end -- analyse_line

-- -----------------------------------------------------------------
-- fixuid
-- shorten a UID for display purposes
-- -----------------------------------------------------------------
function fixuid (uid)
  if not uid then
    return "NO_UID"
  end -- if nil
  return uid:sub (1, config.UID_SIZE)
end -- fixuid

function get_unique_styles (styles)
  local t = { }
  for k, v in ipairs (styles) do
    local s = string.format ("%d/%d/%d", v.textcolour, v.backcolour, v.style)
    if not t[s] then
      t [s] = v
    end -- if not there
  end -- for each supplied style

  local result = { }
  for k, v in pairs (t) do
    if v.textcolour == nil then
      tprint (v)
    end -- if
    table.insert (result, { fore = v.textcolour, back = v.backcolour, style = v.style } )
  end -- for each unique style
  return result
end -- get_unique_styles

-- -----------------------------------------------------------------
-- process_new_room
-- we have an exit line - work out where we are and what the exits are
-- -----------------------------------------------------------------
function process_new_room ()
  if noprocess then
    description = nil
    description_styles = { }
    return
  end -- if

  local from_room = get_from_room ("process_new_room")

  if override_contents ['description'] then
    description = override_contents ['description']
    description_styles = { }
  end -- if
  if override_contents ['exits'] then
    exits_str = override_contents ['exits']:lower ()
    exits_styles = { }
  end -- if
  if override_contents ['room_name'] then
    room_name = override_contents ['room_name']
    room_name_styles = { }
  end -- if

  if not description then
    if override_uid then
      description = "(none)"
    else
      WARNING "No description for this room"
      return
    end -- if
  end -- if no description

  if not exits_str then
    WARNING "No exits for this room"
    return
  end -- if no exits string

  -- handle unmappable ([x]) via nomap table + draw-only node
  if room_unmappable then

    -- If we are already in nomap, do NOT record another "in" edge and do NOT create new ids.
    if get_current_room_ ("process_new_room") == NOMAP_DRAW_UID or last_drawn_id == NOMAP_DRAW_UID then
      set_current_room (NOMAP_DRAW_UID)
      mapper.draw (NOMAP_DRAW_UID)
      last_drawn_id = NOMAP_DRAW_UID
      reset_room_tag_state ()
      return
    end

    local fr  = get_from_room ("process_new_room")
    local dir = get_last_direction_moved ()

    -- entering nomap without a normal move (portal/etc):
    -- if we already have an EMPTY TEMP session (teleport-in), reuse it instead of making stales.
    if not fr or not dir then

      local reuse =
        current_nomap_id
        and nomap[current_nomap_id]
        and nomap[current_nomap_id].temp
        and type(nomap[current_nomap_id].links) == "table"
        and #nomap[current_nomap_id].links == 0

      if not reuse then
        current_nomap_id = nomap_next_id ()
        nomap[current_nomap_id] = {
          links = {},
          area_id = room_area_id,
          temp = true
        }
        db_nomap_upsert (current_nomap_id, nomap[current_nomap_id])
      else
        -- keep area_id up to date while we stay in an unlinked temp session
        nomap[current_nomap_id].area_id = room_area_id
      end

      mapper.draw (_DUMMY_UID)
      set_current_room (NOMAP_DRAW_UID)
      mapper.draw (NOMAP_DRAW_UID)
      last_drawn_id = NOMAP_DRAW_UID
      reset_room_tag_state ()
      return
    end

    -- entering via normal movement: reuse existing nomap if possible
    if not current_nomap_id or not nomap[current_nomap_id] then
      -- first: exact entrance match
      current_nomap_id = nomap_find_by_entrance (fr, dir)

      -- second: if we previously exited this nomap into fr via the opposite direction, it's the same cluster
      if not current_nomap_id then
        current_nomap_id = nomap_find_by_out_boundary (fr, dir, room_area_id)
      end

      -- otherwise: create a new nomap
      if not current_nomap_id then
        current_nomap_id = nomap_next_id ()
        nomap[current_nomap_id] = {
          links = {},
          area_id = room_area_id,
        }
        db_nomap_upsert (current_nomap_id, nomap[current_nomap_id])
      end
    end

    -- overlay the real room exit so the map draws fr --dir--> [x]
    do
      local r = rooms[fr]
      if r and r.exits then
        local old = r.exits[dir]
        table.insert (_nomap_overlay, { from_uid = fr, dir = dir, old_dest = old })
        r.exits[dir] = NOMAP_DRAW_UID
      end
    end

    -- record the entrance edge ONCE
    if not nomap_link_exists (current_nomap_id, "in", fr, dir, nil) then
      table.insert (nomap[current_nomap_id].links, { kind="in", from=fr, dir=dir })
      db_nomap_upsert (current_nomap_id, nomap[current_nomap_id])
    end

    -- prevent "stuck from_room" from being reused while inside nomap
    set_from_room (nil)
    set_last_direction_moved (nil)

    -- draw
    set_current_room (NOMAP_DRAW_UID)
    mapper.draw (NOMAP_DRAW_UID)
    last_drawn_id = NOMAP_DRAW_UID
    reset_room_tag_state ()
    return
  end

  -- If we were in [x] (NOMAP_DRAW_UID) and we arrive in a real room WITHOUT a normal move
  -- (portal/teleport/reconnect), we must drop nomap state and restore any temporary exit overlays,
  -- otherwise the map graph gets broken.
  local was_in_nomap = (get_current_room_ ("process_new_room") == NOMAP_DRAW_UID) or (last_drawn_id == NOMAP_DRAW_UID)
  if was_in_nomap then
    if (not get_from_room ("process_new_room")) and (not get_last_direction_moved ()) then
      -- If this nomap id was created by portal/teleport and never gained any links,
      -- it is unmergeable junk: delete it.
      if current_nomap_id and nomap[current_nomap_id] and nomap[current_nomap_id].temp then
        local links = nomap[current_nomap_id].links
        if type(links) ~= "table" or #links == 0 then
          db_nomap_delete (current_nomap_id)
          nomap[current_nomap_id] = nil
        end
      end
      nomap_overlay_clear ()
      current_nomap_id = nil
      _pending_nomap_exit = nil
      set_from_room (nil)
      set_last_direction_moved (nil)
    end
  end

  -- if we were previously in nomap and we are now in a real room via normal movement,
  -- record the exit out of nomap and then clear overlays.
  --
  -- IMPORTANT: do NOT rely on from_room == NOMAP_DRAW_UID here, because when current_room is NOMAP_DRAW_UID
  -- OnPluginSent may not be able to set from_room (rooms[NOMAP_DRAW_UID] doesn't exist).
  if was_in_nomap and current_nomap_id and nomap[current_nomap_id] then
    local dir = get_last_direction_moved ()
    if dir then
      _pending_nomap_exit = { dir = dir }
    end
  end


  if from_room and get_last_direction_moved () then
    local last_desc = "Unknown"
    if rooms [from_room] then
      desc = rooms [from_room].desc
    end -- if
--    if last_desc == description then
--      mapper.mapprint ("Warning: You have moved from a room to one with an identical description - the mapper may get confused.")
--    end -- if

  end -- if moved from somewhere

  if config.SORT_EXITS then
    -- get all the exit words, exclude other crap, put them in a table, and sort it
    -- this is for MUDs that put markers after exit words to show if you have explored that way or not
    -- it is also to deal with MUDs that might sort the exits into different orders for some reason
    local t_exits = { }
    for exit in string.gmatch (exits_str, "%w+") do
      local ex = valid_direction [exit]
      if ex then
        table.insert (t_exits, ex)
      end -- if
    end -- for
    table.sort (t_exits)
    exits_str = table.concat (t_exits, " ")
  end -- if

  local uid
  local hashed_uid

  if room_tag_uid then
    uid = room_tag_uid
    current_room_hash = nil
  else
    -- keep hash generation for future use / diagnostics
    if override_uid then
      hashed_uid = utils.tohex (utils.md5 (override_uid))
    -- description / name / exits
    elseif config.INCLUDE_EXITS_IN_HASH and config.INCLUDE_ROOM_NAME_IN_HASH and room_name then
      hashed_uid = utils.tohex (utils.md5 (description .. exits_str .. room_name))
    -- description / exits
    elseif config.INCLUDE_EXITS_IN_HASH then
      hashed_uid = utils.tohex (utils.md5 (description .. exits_str))
    -- description / name
    elseif config.INCLUDE_ROOM_NAME_IN_HASH and room_name then
      hashed_uid = utils.tohex (utils.md5 (description .. room_name))
    else
      -- description only
      hashed_uid = utils.tohex (utils.md5 (description))
    end -- if

    hashed_uid = hashed_uid:sub (1, 25)
    current_room_hash = hashed_uid   -- keep existing meaning
    uid = hashed_uid
  end

  local duplicate = nil

  -- finalize "exit out of nomap" now that uid is known
  if _pending_nomap_exit and current_nomap_id and nomap[current_nomap_id] then
    local odir = _pending_nomap_exit.dir
    local changed = false

    if odir and not nomap_out_exists (current_nomap_id, odir, uid) then
      table.insert (nomap[current_nomap_id].links, { kind="out", dir=odir, to=uid })
      changed = true
    end

    if nomap[current_nomap_id].temp then
      nomap[current_nomap_id].temp = nil
      changed = true
    end

    if changed then
      db_nomap_upsert (current_nomap_id, nomap[current_nomap_id])
    end

    -- If this destination room is already associated with some other nomap,
    -- merge this nomap into that one (auto-merge heuristic).
    local target = nomap_find_by_room (uid, current_nomap_id)
    if target
      and nomap[target]
      and nomap[current_nomap_id]
      and nomap[target].area_id == nomap[current_nomap_id].area_id
    then
      nomap_merge_into (target, current_nomap_id)
      current_nomap_id = target
    end

    -- leaving nomap: overlays must be removed and state cleared
    nomap_overlay_clear ()
    current_nomap_id = nil
    _pending_nomap_exit = nil
  end

  -- Duplicate-room logic is ONLY valid for hash-based UID mode.
  -- When room_tag_uid is present (new [..] UID mode), bypass all of this.
  if not room_tag_uid then

    -- is this a known duplicate room?
    if duplicates [current_room_hash] then
      INFO ("<<This is a duplicate room identified by hash " .. fixuid (uid) .. ">>")
      -- yes, so disregard the uid and work out where we came from

      if not (from_room and get_last_direction_moved ()) then
        -- make up some non-existent UID and give up
        uid = UNKNOWN_DUPLICATE_ROOM
        INFO ("Hit a duplicate room, but don't know where we came from, giving up.")
      else
        -- the UID is known to the room that led to this
        uid = rooms [from_room].exits [get_last_direction_moved ()]
        if not uid or uid == "0" or uid == from_room then
          uid = UNKNOWN_DUPLICATE_ROOM
          INFO ("No exit known going " .. get_last_direction_moved ()  .. " from " .. fixuid (from_room))
        elseif duplicates [uid] then
          uid = UNKNOWN_DUPLICATE_ROOM
          INFO ("Hit a duplicate room, disregarding " .. get_last_direction_moved ()  .. " exit from " .. fixuid (from_room))
        end -- if
      end -- if we don't know where we came from

    end -- if

    if uid == UNKNOWN_DUPLICATE_ROOM and from_room and get_last_direction_moved () then
      INFO ("Entering unknown duplicate room - querying whether to make a new room here")
      local a, b, c = get_last_direction_moved (), fixuid (from_room), fixuid (current_room_hash)
      local answer = utils.msgbox (string.format ("Entering duplicate room %s, make new room leading %s from %s?",
                      fixuid (current_room_hash), get_last_direction_moved (), fixuid (from_room)),
              "Duplicate room", "yesno", "?", 1)
      INFO (string.format ("Response to query: Make a new room leading %s from %s to %s (duplicate)?: %s",
            a, b, c, answer))
      if answer == "yes" then
        uid = utils.tohex (utils.md5 (string.format ("%0.9f/%0.9f/%0.9f/%d",
                                math.random (), math.random (), math.random (), os.time ()))):sub (1, 25)
        rooms [from_room].exits [get_last_direction_moved ()] = uid
        INFO (string.format ("Adding new room with random UID (%s) leading %s from %s", fixuid (uid), get_last_direction_moved (), fixuid (from_room)))
        duplicate = current_room_hash
        create_unique_room = false
      end -- if

    end -- if

    if uid == UNKNOWN_DUPLICATE_ROOM then
      set_from_room (nil)
      from_room = nil  -- local copy
    end -- if we don't know where we are
  end -- if not room_tag_uid

  if config.SHOW_ROOM_AND_EXITS then
    INFO (string.format ("Description:\n'%s'\nExits: '%s'\nHash: %s", description, exits_str, fixuid (uid)))
  end -- if config.SHOW_ROOM_AND_EXITS

  if not room_tag_uid then
    if uid == UNKNOWN_DUPLICATE_ROOM then
      desc = "Unknown duplicate room"
      exits_str = ""
      room_name = "Unknown duplicate of " .. fixuid (current_room_hash)
    end -- if
  end -- if not room_tag_uid

  -- break up exits into individual directions
  local exits = {}

  -- for each word in the exits line, which happens to be an exit name (eg. "north") add to the table
  for exit in string.gmatch (exits_str, "%w+") do
    local ex = valid_direction [exit]
    if ex then
      exits [ex] = "0"  -- don't know where it goes yet
    end -- if
  end -- for


  -- add room to rooms table if not already known
  if not rooms [uid] then
    INFO ("Mapper adding room " .. fixuid (uid))
    rooms [uid] = {
        desc = description,
        exits = exits,
        area = area_name or WorldName (),
        name = room_name or fixuid (uid),
        duplicate = duplicate,   -- which UID, if any, this is a duplicate of
        _dirty = true,
        } -- end of new room table

    if config.SAVE_LINE_INFORMATION then
      rooms [uid].styles = {
                           name   = get_unique_styles (room_name_styles),
                           exits  = get_unique_styles (exits_styles),
                           desc   = get_unique_styles (description_styles),
                           } -- end of styles
    end -- if
  else
    -- room there - add exits not known
    for dir in pairs (exits) do
      if not rooms [uid].exits [dir] then
        rooms [uid].exits [dir]  = "0"
        rooms [uid]._dirty = true
        INFO ("Adding exit", dir)
      end -- if exit not there
    end -- for each exit we now about *now*

    -- remove exits that don't exist
--    for dir in pairs (rooms [uid].exits) do
--      if not exits [dir] then
--        INFO ("Removing exit", dir)
--        rooms [uid].exits [dir] = nil
--      end -- if
--    end -- for each exit in the existing room
  end -- if

  -- update room description if it changed
  if description and rooms[uid].desc ~= description then
    rooms[uid].desc = description
    rooms[uid]._dirty = true
  end

  -- update room name if possible
  if room_name and rooms[uid].name ~= room_name then
    rooms[uid].name = room_name
    rooms[uid]._dirty = true
  end -- if

  -- update room area if it changed
  if area_name and rooms[uid].area ~= area_name then
    rooms[uid].area = area_name
    rooms[uid]._dirty = true
  end

  INFO ("We are now in room " .. fixuid (uid))
  -- INFO ("Description: ", description)

  -- save so we know current room later on
  set_current_room (uid)

  -- show what we believe the current exits to be
  if config.SHOW_INFO then
    local probable_map = nil
    for dir, dest in pairs (rooms[uid].exits) do
      local probable = ''
      if dest == '0' then
        if not probable_map then
          probable_map = find_probable_exits(uid, rooms[uid].exits)
          _probable_exit_cache_uid = uid
          _probable_exit_cache_map = probable_map
        end
        local exit_uid = probable_map[dir]
        if exit_uid then
          probable = ' (Probably ' .. fixuid(exit_uid) .. ')'
        end
      end
      INFO ("Exit: " .. dir .. " -> " .. fixuid(dest) .. probable)
    end
  end

  -- try to work out where previous room's exit led
  if get_last_direction_moved () and
     expected_exit ~= uid and
     from_room and uid ~= UNKNOWN_DUPLICATE_ROOM then
    fix_up_exit ()
  end -- exit was wrong

  -- warn them to explore nearby rooms
  if rooms [uid].duplicate then
    local unexplored = { }
    for dir, dest in pairs (rooms [uid].exits) do
      if dest == "0" then
        table.insert (unexplored, dir:upper ())
      end -- not explored
    end -- for each exit
    if #unexplored > 0 then
      local s = 's'
      local u = ' until none left'
      if #unexplored == 1 then
        s = ''
        u = ''
      end -- if
      mapper.mapprint ("Inside duplicated room. Please explore exit" .. s .. ": " ..
                      table.concat (unexplored, ", ") ..
                      " and then return here" .. u)
    end -- if
  end -- if

 -- portals/etc into a real room: if we were in nomap, do not create any nomap links; just clear state
  if (not from_room) or (not get_last_direction_moved ()) then
    if current_nomap_id then
      nomap_overlay_clear ()
      current_nomap_id = nil
      _pending_nomap_exit = nil
    end

    -- teleport/portal/reconnect: force mapper to rebuild its drawn set without inventing exits
    mapper.draw (_DUMMY_UID)
  end

  mapper.draw (uid)
  last_drawn_id = uid    -- in case they change the window size

  -- emergency fallback to stop lots of errors
  if not deduced_line_types [line_number] then
    deduced_line_types [line_number] = { }
  end -- if

  deduced_line_types [line_number].draw = true
  deduced_line_types [line_number].uid = get_current_room_ ("process_new_room")
  
  DEBUG "Finished processing new room"

  room_name = nil
  exits_str = nil
  description = nil
  set_last_direction_moved (nil)
  ignore_received = false
  override_line_type = nil
  override_line_contents = nil
  override_uid = nil
  line_is_not_line_type = { }
  override_contents = { }
  description_styles = { }
  exits_styles = { }
  room_name_styles = { }

  reset_room_tag_state ()

end -- process_new_room


-- -----------------------------------------------------------------
-- mapper 'get_room' callback - it wants to know about room uid
-- -----------------------------------------------------------------

function get_room (uid)

  -- synthetic draw-only room for unmappable ([x])
  if uid == NOMAP_DRAW_UID then
    local r = {
      name = "No Bearings",  -- Room name. 
      desc = "",
      area = nomap_entry_area or area_name or WorldName (),
      exits = {},
    }

    -- drawable exits out of nomap
    if current_nomap_id and nomap[current_nomap_id] and type(nomap[current_nomap_id].links) == "table" then
      for _, e in ipairs (nomap[current_nomap_id].links) do
        if e.kind == "out" and e.dir and e.to then
          r.exits[e.dir] = e.to
        end
      end
    end

    r.hovermessage = "No bearing"

    r.borderpenwidth = 1
    if uid == current_room then
      r.bordercolour = config.OUR_ROOM_COLOUR.colour
      r.borderpenwidth = 2
    end

    -- NOT filled, but shows a red X pattern inside the square
    r.fillcolour = ColourNameToRGB("orange")
    r.fillbrush  = miniwin.brush_hatch_cross_diagonal

    return r
  end

  if not rooms [uid] then
   return nil
  end -- if

  local room = copytable.deep (rooms [uid])

  local texits = {}
  for dir,dest in pairs (room.exits) do
    table.insert (texits, dir .. " -> " .. fixuid (dest))
  end -- for
  table.sort (texits)

  -- get first sentence of description
  local desc = room.desc
  if desc:sub (1, #room.name) == room.name then
    desc = desc:sub (#room.name + 1)
  end -- if
  desc = Trim ((string.match (desc, "^[^.]+") or desc) .. ".")
  if room.name and not string.match (room.name, "^%x+$") then
    -- desc = room.name
  end -- if

  local textras = { }
  if room.Bank then
    table.insert (textras, "Bank")
  end -- if
  if room.Shop then
    table.insert (textras, "Shop")
  end -- if
  if room.Trainer then
    table.insert (textras, "Trainer")
  end -- if
  local extras = ""
  if #textras then
    extras = "\n" .. table.concat (textras, ", ")
  end -- if extras

  local notes = ""
  if room.notes then
    notes = "\nNotes: " .. room.notes
  end -- if notes

  room.hovermessage = string.format (
       "%s\tExits: %s\nRoom: %s%s\n%s\n%s",
        room.name or "unknown",
        table.concat (texits, ", "),
        fixuid (uid),
        extras,
        desc,
        notes
      )

  room.borderpenwidth = 1 -- default

  if uid == current_room then
    room.bordercolour = config.OUR_ROOM_COLOUR.colour
    room.borderpenwidth = 2
  end -- not in this area

  room.fillbrush = miniwin.brush_null -- no fill

  -- special room fill colours

  if room.notes and room.notes ~= "" then
    room.fillcolour = config.BOOKMARK_FILL_COLOUR.colour
    room.fillbrush = miniwin.brush_solid
  elseif room.Shop then
    room.fillcolour = config.SHOP_FILL_COLOUR.colour
    room.fillbrush = miniwin.brush_fine_pattern
  elseif room.Trainer then
    room.fillcolour = config.TRAINER_FILL_COLOUR.colour
    room.fillbrush = miniwin.brush_fine_pattern
  elseif room.Bank then
    room.fillcolour = config.BANK_FILL_COLOUR.colour
    room.fillbrush = miniwin.brush_fine_pattern
  end -- if

  if room.duplicate then
    room.fillcolour = config.DUPLICATE_FILL_COLOUR.colour
    room.fillbrush = miniwin.brush_fine_pattern
  end -- if

  return room
end -- get_room

-- -----------------------------------------------------------------
-- We have changed rooms - work out where the previous room led to
-- -----------------------------------------------------------------

function fix_up_exit ()

  local current_room = get_current_room_ ("fix_up_exit")
  local from_room = get_from_room ("fix_up_exit")
  local last_direction_moved = get_last_direction_moved ("fix_up_exit")

  -- where we were before
  local room = rooms [from_room]

  INFO ("Exit from " .. fixuid (from_room) .. " in the direction " .. last_direction_moved .. " was previously " .. (fixuid (room.exits [last_direction_moved]) or "nowhere"))
  -- leads to here

  if from_room == current_room then
    WARNING ("Declining to set the exit " .. last_direction_moved .. " from this room to be itself")
  else
    room.exits [last_direction_moved] = current_room
    INFO ("Exit from " .. fixuid (from_room) .. " in the direction " .. last_direction_moved .. " is now " .. fixuid (current_room))
  end -- if

  -- do inverse direction as a guess
  local inverse = inverse_direction [last_direction_moved]
  if inverse and current_room then
    if duplicates [rooms [current_room].exits [inverse]] then
      rooms [current_room].exits [inverse] = '0'
    end -- if
    if rooms [current_room].exits [inverse] == '0' then
      rooms [current_room].exits [inverse] = from_room
      INFO ("Added inverse direction from " .. fixuid (current_room) .. " in the direction " .. inverse .. " to be " .. fixuid (from_room))
    end -- if
  end -- of having an inverse


  -- clear for next time
  set_last_direction_moved (nil)
  set_from_room (nil)

end -- fix_up_exit

-- -----------------------------------------------------------------
-- try to detect when we send a movement command
-- -----------------------------------------------------------------

function OnPluginSent (sText)
  --if sText == "look" and not override_noprocess then
  --  noprocess = true
  --else
  if override_noprocess then
    override_noprocess = false
  end -- if
  noprocess = false
  local current_room = get_current_room_ ("OnPluginSent")
  
  if valid_direction [sText] then
    set_last_direction_moved (valid_direction [sText])
    INFO ("current_room =", fixuid (current_room))
    INFO ("Just moving", get_last_direction_moved ())
    if current_room and rooms [current_room] then
      expected_exit = rooms [current_room].exits [get_last_direction_moved ()]
      if expected_exit then
        set_from_room (current_room)
      end -- if
      INFO ("Expected exit for this in direction " .. get_last_direction_moved () .. " is to room", fixuid (expected_exit))
    end -- if
  else
    -- any non-movement command (teleport/portal/etc) must not reuse stale movement state
    set_last_direction_moved (nil)
    set_from_room (nil)
  end -- if
  --end -- if
end -- function

-- -----------------------------------------------------------------
-- Plugin just connected to world
-- -----------------------------------------------------------------

function OnPluginConnect ()
  mapper.cancel_speedwalk ()
  set_from_room (nil)
  room_name = nil
  exits_str = nil
  description = nil
  set_last_direction_moved (nil)
  ignore_received = false
  override_line_type = nil
  override_line_contents = nil
  override_contents = { }
  line_is_not_line_type = { }
  set_current_room (nil)
end -- OnPluginConnect

-- -----------------------------------------------------------------
-- Plugin just disconnected from world
-- -----------------------------------------------------------------

function OnPluginDisconnect ()
  mapper.cancel_speedwalk ()
end -- OnPluginDisconnect

-- -----------------------------------------------------------------
-- Callback to show part of the room description/name/notes, used by map_find
-- -----------------------------------------------------------------

FIND_OFFSET = 33

function show_find_details (uid)
  local this_room = rooms [uid]
  local target = this_room.desc
  local label = "Description: "
  local st, en = string.find (target:lower (), wanted, 1, true)
  -- not in description, try the name
  if not st then
    target = this_room.name
    label = "Room name: "
    st, en = string.find (target:lower (), wanted, 1, true)
    if not st then
      target = this_room.notes
      label = "Notes: "
      if target then
        st, en = string.find (target:lower (), wanted, 1, true)
      end -- if any notes
    end -- not found in the name
  end -- can't find the wanted text anywhere, odd


  local first, last
  local first_dots = ""
  local last_dots = ""

  for i = 1, #target do

    -- find a space before the wanted match string, within the FIND_OFFSET range
    if not first and
       target:sub (i, i) == ' ' and
       i < st and
       st - i <= FIND_OFFSET then
      first = i
      first_dots = "... "
    end -- if

    -- find a space after the wanted match string, within the FIND_OFFSET range
    if not last and
      target:sub (i, i) == ' ' and
      i > en and
      i - en >= FIND_OFFSET then
      last = i
      last_dots = " ..."
    end -- if

  end -- for

  if not first then
    first = 1
  end -- if
  if not last then
    last = #target
  end -- if

  mapper.mapprint (label .. first_dots .. Trim (string.gsub (target:sub (first, last), "\n", " ")) .. last_dots)

end -- show_find_details

-- -----------------------------------------------------------------
-- Find a room
-- -----------------------------------------------------------------

function map_find (name, line, wildcards)

  local room_ids = {}
  local count = 0
  wanted = (wildcards [1]):lower ()     -- NOT local

  -- scan all rooms looking for a simple match
  for k, v in pairs (rooms) do
     local desc = v.desc:lower ()
     local name = v.name:lower ()
     local notes = ""
     if v.notes then
       notes = v.notes:lower ()
      end -- if notes
     if string.find (desc, wanted, 1, true) or
        string.find (name, wanted, 1, true) or
        string.find (notes, wanted, 1, true) then
       room_ids [k] = true
       count = count + 1
     end -- if
  end   -- finding room

  -- see if nearby
  mapper.find (
    function (uid)
      local room = room_ids [uid]
      if room then
        room_ids [uid] = nil
      end -- if
      return room, next (room_ids) == nil
    end,  -- function
    show_vnums,  -- show vnum?
    count,      -- how many to expect
    false,      -- don't auto-walk
    show_find_details -- callback function
    )

end -- map_find

-- -----------------------------------------------------------------
-- mapper_show_bookmarked_room - callback to show a bookmark
-- -----------------------------------------------------------------
function mapper_show_bookmarked_room (uid)
  local this_room = rooms [uid]
  mapper.mapprint (this_room.notes)
end -- mapper_show_bookarked_room

-- -----------------------------------------------------------------
-- Find bookmarked rooms
-- -----------------------------------------------------------------
function map_bookmarks (name, line, wildcards)

  local room_ids = {}
  local count = 0

  -- scan all rooms looking for a simple match
  for k, v in pairs (rooms) do
    if v.notes and v.notes ~= "" then
      room_ids [k] = true
      count = count + 1
    end -- if
  end   -- finding room

  -- find such places
  mapper.find (
    function (uid)
      local room = room_ids [uid]
      if room then
        room_ids [uid] = nil
      end -- if
      return room, next (rooms) == nil  -- room will be type of info (eg. shop)
    end,  -- function
    show_vnums,  -- show vnum?
    count,       -- how many to expect
    false,       -- don't auto-walk
    mapper_show_bookmarked_room  -- callback function to show the room bookmark
    )

end -- map_bookmarks

-- -----------------------------------------------------------------
-- Go to a room
-- -----------------------------------------------------------------

function map_goto (name, line, wildcards)

  if not mapper.check_we_can_find () then
    return
  end -- if

  local wanted = wildcards [1]

  if not string.match (wanted, "^%x+$") then
    mapper.mapprint ("Room IDs are hex strings (eg. FC758) - you can specify a partial string")
    return
  end -- if

  local goto_uid, room = find_room_partial_uid (wanted)
  if not goto_uid then
    return
  end -- if not found

  local current_room = get_current_room_ ("map_goto")
  if current_room and goto_uid == current_room then
    mapper.mapprint ("You are already in that room.")
    return
  end -- if

  -- find desired room
  mapper.find (
    function (uid)
      return uid == goto_uid, uid == goto_uid
    end,  -- function
    show_vnums,  -- show vnum?
    1,          -- how many to expect
    true        -- just walk there
    )

end -- map_goto

-- -----------------------------------------------------------------
-- line_received - called by a trigger on all lines
--   work out its line type, and then handle a line-type change
-- -----------------------------------------------------------------

last_deduced_type = nil
saved_lines = { }
deduced_line_types = { }
_xinfo_exits_block = false

function line_received (name, line, wildcards, styles)

  -- these need to be global, for use later on
-- one-shot: if plugin loaded before world/address/port were ready, corpus may not have loaded yet
if not _corpus_loaded_from_db then
  local corpus_s = db_kv_get ("corpus")
  if corpus_s and corpus_s ~= "" then
    local ok = pcall (function () assert (loadstring (corpus_s)) () end)
    if ok and type(corpus) == "table" and next(corpus) ~= nil then
      corpus_reset ()
      _corpus_loaded_from_db = true
    end
  end
end


  this_line = GetLinesInBufferCount()         -- which line in the output buffer
  line_number = GetLineInfo (this_line, 10)   -- which line this was overall

  -- Normalize incoming MUD text to UTF-8
  --line = line:gsub("%z", "")
  --if line:find("[\128-\255]") then
  --  line = utils.utf8convert(line)
  --end

  local deduced_type, probability

  -- xinfo exits support:
  --   Exits:  [ flags ]
  --       south   [ closed pickproof]
  --        down   [ invisible]
  --
  -- We must force these lines to be of type "exits" so f_handle_exits gets the whole block.
  local is_xinfo_header = line:match("^%s*Exits:%s*%[%s*flags%s*%]%s*$") ~= nil
  local is_xinfo_row    = line:match("^%s*%w+%s*%[") ~= nil

  if is_xinfo_header then
    _xinfo_exits_block = true
    override_line_type = "exits"
  elseif _xinfo_exits_block then
    if is_xinfo_row then
      override_line_type = "exits"
    else
      -- first non-row line ends the xinfo exits block
      _xinfo_exits_block = false
    end
  end

  -- see if a plugin has overriden the line type
  if override_line_type then
    deduced_type = override_line_type
    if override_line_contents then
      line = override_line_contents
    end -- if new contents wanted
  else
    if (not config.BLANK_LINE_TERMINATES_LINE_TYPE) and Trim (line) == "" then
      return
    end -- if empty line

    if config.BLANK_LINE_TERMINATES_LINE_TYPE and Trim (line) == "" then
      deduced_type = nil
    else
      deduced_type, probability = analyse_line (this_line)
    end -- if

  end -- if

  -- record for scrollback buffer
  if deduced_type then
    deduced_line_types [line_number] = {
        lt = deduced_type,  -- what type we assigned to it
        con = probability,  -- with what probability
        draw = false,       -- did we draw on this line?
        ov = override_line_type,  -- was it overridden?
        }
  end -- if not nil type

  -- INFO ("This line is", deduced_type, "last type was", last_deduced_type)

  if deduced_type ~= last_deduced_type then

    -- deal with previous line type
    -- INFO ("Now handling", last_deduced_type)

    if last_deduced_type then
      line_types [last_deduced_type].handler (saved_lines)  -- handle the line(s)
    end -- if we have a type

    last_deduced_type = deduced_type
    saved_lines = { }
  end -- if line type has changed


  table.insert (saved_lines, { line = line, styles = styles } )

  -- if exits are on a single line, then we can process them as soon as we get them
  if config.EXITS_IS_SINGLE_LINE and deduced_type == 'exits' and not _xinfo_exits_block then
      -- INFO ("Now handling", deduced_type)
      line_types.exits.handler (saved_lines)  -- handle the line
      saved_lines = { }
      last_deduced_type = nil
  end -- if

  -- if prompt are on a single line, then we can process it as soon as we get it
  if config.PROMPT_IS_SINGLE_LINE and deduced_type == 'prompt' then
      -- INFO ("Now handling", deduced_type)
      line_types.prompt.handler (saved_lines)  -- handle the line
      saved_lines = { }
      last_deduced_type = nil
  end -- if

  -- reset back ready for next line
  line_is_not_line_type = { }
  override_line_type = nil

end -- line_received

-- -----------------------------------------------------------------
-- corpus_info - show how many times we trained the corpus
-- -----------------------------------------------------------------

function corpus_info ()
  mapper.mapprint  (string.format ("%20s %5s %5s", "Line type", "is", "not"))
  mapper.mapprint  (string.format ("%20s %5s %5s", string.rep ("-", 15), string.rep ("-", 5), string.rep ("-", 5)))
  for k, v in pairs (stats) do
    mapper.mapprint  (string.format ("%20s %5d %5d", k, v.is, v.isnot))
  end -- for each line type
  mapper.mapprint ("There are " .. count_values (corpus) .. " entries in the corpus.")
end -- corpus_info

-- -----------------------------------------------------------------
-- OnHelp - show help
-- -----------------------------------------------------------------
function OnHelp ()
  local p = mapper.mapprint
	mapper.mapprint (string.format ("[MUSHclient mapper, version %0.1f]", mapper.VERSION))
  p (string.format ("[%s version %0.1f]", GetPluginName(), GetPluginInfo (GetPluginID (), 19)))
  p (string.rep ("-", 78))
	p (GetPluginInfo (GetPluginID (), 3))
  p (string.rep ("-", 78))

  -- Tell them where to get the latest mapper plugin
  local old_note_colour = GetNoteColourFore ()
  SetNoteColourFore(config.MAPPER_NOTE_COLOUR.colour)

  Tell ("Lastest mapper plugin: ")
  Hyperlink ("https://mushclient.cthulhumud.com/plugins/Mapper/Update/CthulhuMUD_Mapper.xml",
             "CthulhuMUD_Mapper.xml", "Click to see latest plugin", "darkorange", "", true)
  Tell (" and ")
  Hyperlink ("https://mushclient.cthulhumud.com/plugins/Mapper/Update/CthulhuMUD_Mapper.lua",
             "CthulhuMUD_Mapper.lua", "Click to see latest plugin", "darkorange", "", true)
  p ""
  p ('You need both files. RH-click the "Raw" button on those pages to download the files.')
  Tell ('Save them to the folder: ')
  ColourNote ("orange", "", GetPluginInfo (GetPluginID (), 20))
  p (string.rep ("-", 78))
  SetNoteColourFore (old_note_colour)

end

-- -----------------------------------------------------------------
-- map_where - where is the specified room? (by uid)
-- -----------------------------------------------------------------
function map_where (name, line, wildcards)

  if not mapper.check_we_can_find () then
    return
  end -- if

  local wanted = wildcards [1]:upper ()

  if not string.match (wanted, "^%x+$") then
    mapper.mapprint ("Room IDs are hex strings (eg. FC758) - you can specify a partial string")
    return
  end -- if

  local where_uid, room = find_room_partial_uid (wanted)
  if not where_uid then
    return
  end -- if not found

  local current_room = get_current_room_ ("map_where")

  if current_room and where_uid == current_room then
    mapper.mapprint ("You are already in that room.")
    return
  end -- if

  local paths = mapper.find_paths (current_room,
           function (uid)
            return uid == where_uid, uid == where_uid
            end)

  local uid, item = next (paths, nil) -- extract first (only) path

  -- nothing? room not found
  if not item then
    mapper.mapprint (string.format ("Room %s not found", wanted))
    return
  end -- if

  -- turn into speedwalk
  local path = mapper.build_speedwalk (item.path)

  -- display it
  mapper.mapprint (string.format ("Path to %s (%s) is: %s", wanted, rooms [where_uid].name, path))

end -- map_where

-- -----------------------------------------------------------------
-- OnPluginPacketReceived - try to add newlines to prompts if wanted
-- -----------------------------------------------------------------
function OnPluginPacketReceived (pkt)

  if not config.ADD_NEWLINE_TO_PROMPT then
    return pkt
  end -- if

  -- add a newline to the end of a packet if it appears to be a simple prompt
  -- (just a ">" sign at the end of a line optionally followed by one space)
  if GetInfo (104) then  -- if MXP enabled
    if string.match (pkt, "&gt; ?$") then
      return pkt .. "\n"
    end -- if
  else
    if string.match (pkt, "> ?$") then  -- > symbol at end of packet
      return pkt .. "\n"
    elseif string.match (pkt, "> ?\027%[0m ?$") then -- > symbol at end of packet followed by ESC [0m
      return pkt .. "\n"
    end -- if
  end -- if MXP or not

  return pkt
end -- OnPluginPacketReceived

-- -----------------------------------------------------------------
-- show_corpus - show all values in the corpus, printed nicely
-- -----------------------------------------------------------------
function show_corpus ()

  -- start with each line type (eg. exits, descriptions)
  for name, type_info in pairs (line_types) do
    mapper.mapprint (string.rep ("=", 72))
    mapper.mapprint (type_info.short)
    mapper.mapprint (string.rep ("=", 72))
    corpus_line_type = corpus [name]
    -- for each one show each marker type (eg. first word, all words, colour)
    for _, marker in ipairs (markers) do
      mapper.mapprint ("  " .. string.rep ("-", 70))
      mapper.mapprint ("  " .. marker.desc)
      mapper.mapprint ("  " .. string.rep ("-", 70))
      local f = marker.show
      local accessing_function  = marker.accessing_function  -- pairs for numbers or pairsByKeys for strings
      if f then
        mapper.mapprint (string.format ("    %20s %5s %5s %7s", "Value", "Yes", "No", "Score"))
        mapper.mapprint (string.format ("    %20s %5s %5s %7s", "-------", "---", "---", "-----"))
        -- for each marker show each value, along with its counts for and against, and its calculated score
        for k, v in accessing_function (corpus_line_type [marker.marker], function (a, b) return a:lower () < b:lower () end ) do
          f (k, v)
        end -- for each value
      end -- if function exists
    end -- for each marker type
  end -- for each line type

end -- show_corpus

-- -----------------------------------------------------------------
-- show_styles - show a set of style runs summary
-- -----------------------------------------------------------------
function show_styles (name, styles)
  local p = mapper.mapprint

  p ""
  p (string.format ("%s styles:", name))
  p (string.format ("%-20s %-20s %-30s %s", "Foreground", "Background", "Styles", "Count"))
  p (string.format ("%-20s %-20s %-30s %s", "----------", "----------", "------", "-----"))
  for k, v in pairs (styles) do
    local fore, back, style = string.match (k, "^(%d+)/(%d+)/(%d+)$")
    local t = { }
    if bit.band (style, 1) ~= 0 then
      table.insert (t, "bold")
    end
    if bit.band (style, 2) ~= 0 then
      table.insert (t, "underline")
    end
    if bit.band (style, 4) ~= 0 then
      table.insert (t, "italic")
    end

    p (string.format ("%-20s %-20s %-30s %5d", RGBColourToName (fore), RGBColourToName (back), table.concat (t, ","), v))
  end -- for

end -- show_styles


-- -----------------------------------------------------------------
-- mapper_analyse - analyse the map database
-- -----------------------------------------------------------------
function mapper_analyse (name, line, wildcards)
  local min_name_length = 1e20
  local max_name_length = 0
  local total_name_length = 0
  local room_count = 0
  local min_name = ""
  local max_name = ""
  local name_styles = { }
  local desc_styles = { }
  local exits_styles = { }

  local function get_styles (this_style, all)
    if this_style then
      for k, v in ipairs (this_style) do
        local s = string.format ("%d/%d/%d", v.fore, v.back, v.style)
        if all [s] then
          all [s] = all [s] + 1
        else
          all [s] = 1
        end -- if
      end -- for
    end -- if styles exits
  end -- get_styles

  for uid, room in pairs (rooms) do
    -- don't bother getting the length of hex uids - that doesn't prove much
    if not string.match (room.name, "^%x+$") then
      local len = #room.name
      room_count = room_count + 1
      min_name_length = math.min (min_name_length, len)
      max_name_length = math.max (max_name_length, len)
      if len == min_name_length then
        min_name = room.name
      end
      if len == max_name_length then
        max_name = room.name
      end
      total_name_length = total_name_length + len
      end -- if not a hex room name

    -- now get the colours
    if room.styles then
      get_styles (room.styles.name, name_styles)
      get_styles (room.styles.desc, desc_styles)
      get_styles (room.styles.exits, exits_styles)
    end -- if we have some styles

  end -- for each room

  local p = mapper.mapprint
  mapper.mapprint (string.rep ("-", 78))

  p (string.format ("%20s %4d (%s)", "Minimum room name length", min_name_length, min_name))
  p (string.format ("%20s %4d (%s)", "Maximum room name length", max_name_length, max_name))
  if room_count > 0 then  -- no divide by zero
    p (string.format ("%20s %4d",      "Average room name length", total_name_length / room_count))
  end -- if

  if not config.SAVE_LINE_INFORMATION then
    mapper.mapprint ("WARNING: Option 'save_line_info' is not set.")
  end -- if

  show_styles ("Room name",   name_styles)
  show_styles ("Description", desc_styles)
  show_styles ("Exits",       exits_styles)

  mapper.mapprint (string.rep ("-", 78))

end -- mapper_analyse

-- -----------------------------------------------------------------
-- mapper_delete - delete a room from the database
-- -----------------------------------------------------------------
function mapper_delete (name, line, wildcards)
  local uid = wildcards [1]
  if #uid < 4 then
    mapper.maperror ("UID length must be at least 4 characters for deleting rooms")
    return
  end -- if too short

  delete_uid, room = find_room_partial_uid (uid)
  if not delete_uid then
    return
  end -- if not found

  mapper_show (delete_uid)
  rooms [delete_uid] = nil  -- delete it
  mapper.mapprint ("Room", delete_uid, "deleted.")

  -- remove any exit references to this room
  for uid, room in pairs (rooms) do
    for dir, dest in pairs (room.exits) do
      if dest == delete_uid then
        mapper.mapprint (string.format ("Setting exit %s from room %s to be unknown",
                          dest, fixuid (uid)))
        room.exits [dir] = '0'
      end -- if that exit pointed to the deleted room
    end -- for each exit
  end -- for each room

  -- redraw map
  if last_drawn_id then
    mapper.draw (last_drawn_id)
  end -- if

end -- mapper_delete

-- -----------------------------------------------------------------
-- uid_hyperlink - displays (with no newline) a hyperlink to a UID
-- -----------------------------------------------------------------
function uid_hyperlink (uid)
  Hyperlink ("!!" .. GetPluginID () .. ":mapper_show(" .. uid .. ")",
            fixuid (uid), "Click to show room details for " .. fixuid (uid), RGBColourToName (config.MAPPER_NOTE_COLOUR.colour), "", false)
end -- uid_hyperlink

-- -----------------------------------------------------------------
-- list_rooms_table - displays a list of rooms from the supplied table of uid/room pairs
-- -----------------------------------------------------------------
function list_rooms_table (t)
  table.sort (t, function (a, b) return a.room.name:upper () < b.room.name:upper () end )

  for _, v in ipairs (t) do
    local room = v.room
    local uid = v.uid
    uid_hyperlink (uid)
    mapper.mapprint (" ", room.name)
  end -- for each room

  if #t == 0 then
    mapper.mapprint "No matches."
  else
    local s = ""
    if #t > 1 then
      s = "es"
    end -- if
    mapper.mapprint (string.format ("%d match%s.", #t, s))
  end -- if

end -- list_rooms_table

-- -----------------------------------------------------------------
-- find_orphans - finds all rooms nothing leads to
-- -----------------------------------------------------------------
function find_orphans ()
  local orphans = { }
  -- add all rooms to orphans table
  for uid, room in pairs (rooms) do
    orphans [uid] = room
  end -- for

  -- now eliminate rooms which others point to
  for uid, room in pairs (rooms) do
    for dir, exit_uid in pairs (room.exits) do
      if exit_uid ~= "0" then
        orphans [exit_uid] = nil  -- not an orphan
      end -- if
    end -- for each exit
  end -- for
  local t = { }
  for uid, room in pairs (orphans) do
    table.insert (t, { uid = uid, room = room })
  end -- for
  list_rooms_table (t)
end -- find_orphans

-- -----------------------------------------------------------------
-- find_connect - finds all rooms that this one eventually leads to
-- -----------------------------------------------------------------
function find_connect (uid, room)
  local found = {  }
  local unexplored = { [uid] = true }

  while next (unexplored) do
    local new_ones= { }
    for uid in pairs (unexplored) do
      if not found [uid] then
        local room = rooms [uid]
        if room then
          found [uid] = true
          unexplored [uid] = nil  -- remove from unexplored list
          for dir, exit_uid in pairs (room.exits) do
            if not found [exit_uid] and not unexplored [exit_uid] then
              table.insert (new_ones, exit_uid)
            end -- if not noticed yet
          end -- for
        else
          unexplored [uid] = nil  -- doesn't exist, remove from list
        end -- if room exists
      end -- if not yet found
    end -- for each unexplored room
    for _, uid in ipairs (new_ones) do
      unexplored [uid] = true
    end -- for
  end -- while still some to go

  local t = { }

  for uid in pairs (found) do
    table.insert (t, { uid = uid, room = rooms [uid] } )
  end -- for each found room

  list_rooms_table (t)
end -- find_connect

-- -----------------------------------------------------------------
-- colour_finder - generic finder for name/description/exits
--                 foreground or background colours
-- -----------------------------------------------------------------
function  colour_finder (wanted_colour, which_styles, fore_or_back)
  local colour, colourRGB = config_validate_colour (wanted_colour)
  if not colour then
    return
  end -- if
  mapper_list_finder (function (uid, room)
    if room [which_styles] then
      for _, style in ipairs (room [which_styles]) do
        if style [fore_or_back] == colourRGB then
          return true
        end -- if wanted colour
      end -- for each style
    end -- if any style
    return false
   end)
end -- colour_finder

-- -----------------------------------------------------------------
-- find_room_partial_uid - find a room by partial ID
-- if multiples found, advises what they are
-- if none, gives an error and returns nil
-- otherwise returns the UID and the room info for that room
-- -----------------------------------------------------------------
function find_room_partial_uid (which)
  local t = { }
  which = which:upper ()
  for uid, room in pairs (rooms) do
    if string.match (uid, "^" .. which) then
      table.insert (t, { uid = uid, room = room })
    end -- if a partial match
  end -- for
  if #t == 0 then
    mapper.maperror ("Room UID", which, "is not known.")
    return nil, nil
  end -- if

  if #t > 1 then
    mapper.maperror ("Multiple matches for UID", which .. ":")
    list_rooms_table (t)
    return nil, nil
  end -- if

  return t [1].uid, t [1].room
end -- find_room_partial_uid

-- -----------------------------------------------------------------
-- mapper_list_finder - generic finder which adds a room if f() returns true
-- -----------------------------------------------------------------
function mapper_list_finder (f)
  local t = { }

  for uid, room in pairs (rooms) do
    if not f or f (uid, room) then
      table.insert (t, { uid = uid, room = room } )
    end -- if room wanted
  end -- for

  list_rooms_table (t)
end -- mapper_list_finder

-- -----------------------------------------------------------------
-- mapper_list - show or search the map database
-- -----------------------------------------------------------------
function mapper_list (name, line, wildcards)
  local first_wildcard = Trim (wildcards [1]):lower ()
  local name     = string.match (first_wildcard, "^name%s+(.+)$")
  local desc     = string.match (first_wildcard, "^desc%s+(.+)$")
  local notes    = string.match (first_wildcard, "^notes?%s+(.+)$")
  local area     = string.match (first_wildcard, "^areas?%s+(.+)$")
  local any_notes= string.match (first_wildcard, "^notes?$")
  local orphans  = string.match (first_wildcard, "^orphans?$")
  local lead_to  = string.match (first_wildcard, "^dest%s+(%x+)$")
  local connect  = string.match (first_wildcard, "^connect%s+(%x+)$")
  local shops    = string.match (first_wildcard, "^shops?$")
  local banks    = string.match (first_wildcard, "^banks?$")
  local trainers = string.match (first_wildcard, "^trainers?$")
  local dups     = string.match (first_wildcard, "^duplicates?$")
  local colour_name_fore  = string.match (first_wildcard, "^colou?r%s+name%s+fore%s+(.*)$")
  local colour_name_back  = string.match (first_wildcard, "^colou?r%s+name%s+back%s+(.*)$")
  local colour_desc_fore  = string.match (first_wildcard, "^colou?r%s+desc%s+fore%s+(.*)$")
  local colour_desc_back  = string.match (first_wildcard, "^colou?r%s+desc%s+back%s+(.*)$")
  local colour_exits_fore = string.match (first_wildcard, "^colou?r%s+exits?%s+fore%s+(.*)$")
  local colour_exits_back = string.match (first_wildcard, "^colou?r%s+exits?%s+back%s+(.*)$")
  local p = mapper.mapprint

  -- no wildcard? list all rooms
  if first_wildcard == "" then
    p ("All rooms")
    mapper_list_finder ()

  -- wildcard consists of hex digits and spaces? must be a uid list
  elseif string.match (first_wildcard, "^[%x ]+$") then
    first_wildcard = first_wildcard:upper ()  -- UIDs are in upper case
    if not string.match (first_wildcard, ' ') then
      local uid, room = find_room_partial_uid (first_wildcard)
      if not uid then
        return
      end -- if not found

      mapper_show (uid)
    else
      -- hex strings? room uids
      mapper_list_finder (function (uid, room)
        for w in string.gmatch (first_wildcard, "%x+") do
          if string.match (uid, "^" .. w) then
            return true
          end -- if
        end -- for each uid they wanted
        return false
      end )  -- function
    end -- if more than one uid

  -- wildcard is: name xxx
  elseif name then
    p (string.format ('Rooms whose name match "%s"', name))
    mapper_list_finder (function (uid, room) return string.find (room.name:lower (), name, 1, true) end)

  -- wildcard is: desc xxx
  elseif desc then
    p (string.format ('Rooms whose description matches "%s"', desc))
    mapper_list_finder (function (uid, room) return string.find (room.desc:lower (), desc, 1, true) end)

  -- wildcard is: notes xxx
  elseif notes then
    p (string.format ('Rooms whose notes match "%s"', notes))
    mapper_list_finder (function (uid, room) return room.notes and string.find (room.notes:lower (), notes, 1, true) end)

  -- wildcard is: area xxx
  elseif area then
    p (string.format ('Rooms whose area matches "%s"', area))
    mapper_list_finder (function (uid, room) return room.area and string.find (room.area:lower (), area, 1, true) end)

  -- wildcard is: notes
  -- (show all rooms with notes)
  elseif any_notes then
    p ("Rooms with notes")
    mapper_list_finder (function (uid, room) return room.notes end)

  -- wildcard is: shops
  elseif shops then
    p ("Rooms with shops")
    mapper_list_finder (function (uid, room) return room.Shop end)

  -- wildcard is: trainers
  elseif trainers then
    p ("Rooms with trainers")
    mapper_list_finder (function (uid, room) return room.Trainer end)

  -- wildcard is: banks
  elseif banks then
    p ("Rooms with banks")
    mapper_list_finder (function (uid, room) return room.Bank end)
  elseif dups then
    p ("Duplicate rooms")
    for duplicate_uid in pairs (duplicates) do
      p (string.rep ("-", 10))
      p ("Marked duplicate: " .. fixuid (duplicate_uid))
      mapper_list_finder (function (uid, room) return room.duplicate == duplicate_uid end)
    end -- for

  -- find orphan rooms (rooms no other room leads to)
  elseif orphans then
    p ("Orphaned rooms")
    find_orphans ()

  -- find rooms which this one eventually connects to
  elseif connect then
    local connect_uid, room = find_room_partial_uid (connect)
    if not connect_uid then
      return
    end -- if not found
    p (string.format ('Rooms which can be reached from "%s" (%s)', connect, room.name))
    t = find_connect (connect_uid, room)

  -- find rooms which have an exit leading to this one
  elseif lead_to then
    local lead_to_uid, room = find_room_partial_uid (lead_to)
    if not lead_to_uid then
      return
    end -- if not found
    p (string.format ("Rooms which have an exit to %s:", lead_to))
    mapper_list_finder (function (uid, room)
      for dir, exit_uid in pairs (room.exits) do
        if exit_uid == lead_to_uid then
          return true
        end -- if exit leads to this room
      end -- for each exit
      return false
      end)  -- finding function

  -- colour finding
  elseif colour_name_fore then
    p (string.format ("Rooms who have a name foreground style of %s:", colour_name_fore))
    colour_finder (colour_name_fore, 'name_styles', 'fore')
  elseif colour_name_back then
    p (string.format ("Rooms who have a name background style of %s:", colour_name_back))
    colour_finder (colour_name_back, 'name_styles', 'back')
  elseif colour_desc_fore then
    p (string.format ("Rooms who have a description foreground style of %s:", colour_desc_fore))
    colour_finder (colour_desc_fore, 'desc_styles', 'fore')
  elseif colour_desc_back then
    p (string.format ("Rooms who have a description background style of %s:", colour_desc_back))
    colour_finder (colour_desc_back, 'desc_styles', 'back')
  elseif colour_exits_fore then
    p (string.format ("Rooms who have a exits foreground style of %s:", colour_exits_fore))
    colour_finder (colour_exits_fore, 'exits_styles', 'fore')
  elseif colour_exits_back then
    p (string.format ("Rooms who have a exits background style of %s:", colour_exits_back))
    colour_finder (colour_exits_back, 'exits_styles', 'back')

  else
    mapper.maperror ("Do not understand 'list' command:", first_wildcard)
    p ("Options are:")
    p ("  mapper list")
    p ("  mapper list uid ...")
    p ("  mapper list name <name>")
    p ("  mapper list desc <description>")
    p ("  mapper list note <note>")
    p ("  mapper list area <area>")
    p ("  mapper list notes")
    p ("  mapper list orphans")
    p ("  mapper list dest <uid>")
    p ("  mapper list connect <uid>")
    p ("  mapper list shop")
    p ("  mapper list trainer")
    p ("  mapper list bank")
    p ("  mapper list colour name  fore <colour>")
    p ("  mapper list colour name  back <colour>")
    p ("  mapper list colour desc  fore <colour>")
    p ("  mapper list colour desc  back <colour>")
    p ("  mapper list colour exits fore <colour>")
    p ("  mapper list colour exits back <colour>")
    return
  end -- if

end -- mapper_list

-- -----------------------------------------------------------------
-- mapper_show - show one room
-- -----------------------------------------------------------------
function mapper_show (uid)
  local room = rooms [uid]
  if not room then
    mapper.maperror ("Room", uid, "is not known.")
    return
  end -- if

  local old_note_colour = GetNoteColourFore ()
  SetNoteColourFore(config.MAPPER_NOTE_COLOUR.colour)

  Note (string.rep ("-", 78))
  Note (string.format ("Room:  %s -> %s.", fixuid (uid), room.name))
  if room.duplicate then
    Note (string.format ("(Duplicate of %s)", fixuid (room.duplicate)))
  end -- if a duplicate
  Note (string.rep ("-", 78))
  Note (room.desc)
  Note (string.rep ("-", 78))
  Tell ("Exits: ")
  for dir,dest in pairs (room.exits) do
    Tell (dir:upper () .. " -> ")
    if dest == "0" then
      Tell "(Not explored)"
    else
      local dest_room = rooms [dest]
      if dest_room then
        uid_hyperlink (dest)
        Tell (" (" .. dest_room.name .. ")")
      else
        Tell (fixuid (dest) .. " (unknown)")
      end -- if dest_room
    end -- if
    Tell " "
  end -- for
  Note "" -- finish line
  if room.notes then
    Note (string.rep ("-", 78))
    Note (room.notes)
  end -- of having notes
  if room.Trainer or room.Shop or room.Bank then
    Note (string.rep ("-", 78))
    local t = { }
    if room.Bank then
      table.insert (t, "Bank")
    end -- if
    if room.Shop then
      table.insert (t, "Shop")
    end -- if
    if room.Trainer then
      table.insert (t, "Trainer")
    end -- if
    Note (table.concat (t, ", "))
  end -- of having notes
  Note (string.rep ("-", 78))

  SetNoteColourFore (old_note_colour)
end -- mapper_show

-- -----------------------------------------------------------------
-- mapper_config - display or change configuration options
-- Format is: mapper config <name> <value>  <-- change option <name> to <value>
--            mapper config                 <-- show all options
--            mapper config <name>          <-- show setting for one option
-- -----------------------------------------------------------------
function mapper_config (name, line, wildcards)
  local name = Trim (wildcards.name:lower ())
  local value = Trim (wildcards.value)

  -- no config item - show all existing ones
  if name == "" then
    mapper.mapprint ("All mapper configuration options")
    mapper.mapprint (string.rep ("-", 60))
    mapper.mapprint ("")
    for k, v in ipairs (config_control) do
      mapper.mapprint (string.format ("mapper config %-40s %s", v.name, v.show (config [v.option])))
    end
    mapper.mapprint ("")
    mapper.mapprint (string.rep ("-", 60))
    mapper.mapprint ('Type "mapper help" for more information about the above options.')

    -- training counts
    local count = 0
    for k, v in pairs (stats) do
      count = count + v.is + v.isnot
    end -- for each line type
    mapper.mapprint (string.format ("%s: %s.", "Number of times line types trained", count))

    -- hints on corpus info
    mapper.mapprint ('Type "mapper corpus info" for more information about line training.')
    mapper.mapprint ('Type "mapper learn" to toggle the training windows.')
    return false
  end -- no item given

  -- config name given - look it up in the list
  local config_item = validate_option (name, 'mapper config')
  if not config_item then
    return false
  end -- if no such option

  -- no value given - display the current setting of this option
  if value == "" then
    mapper.mapprint ("Current value for " .. name .. ":")
    mapper.mapprint ("")
    mapper.mapprint (string.format ("mapper config %s %s", config_item.name, config_item.show (config [config_item.option])))
    mapper.mapprint ("")
    return false
  end -- no value given

  -- validate new option value
  local new_value = config_item.validate (value)
  if new_value == nil then    -- it might be false, so we have to test for nil
    mapper.maperror ("Configuration option not changed.")
    return false
  end -- bad value

  -- set the new value and confirm it was set
  config [config_item.option] = new_value
  
  -- Set the proper area trigger -- Tanthul
  if config_item == "AUTOMAP_AREA" and config.AUTOMAP_AREA then
     EnableTrigger ("Area_line", false)
	 EnableTriggerGroup ("Area_scan", true)
  else
    EnableTriggerGroup ("Area_scan", false)
	EnableTrigger ("Area_line", true)
  end -- if
  
  mapper.mapprint ("Configuration option changed. New value is:")
  mapper.mapprint (string.format ("mapper config %s %s", config_item.name, config_item.show (config [config_item.option])))
  return true
end -- mapper_config

-- -----------------------------------------------------------------
-- count_rooms - count how many rooms are in the database
-- -----------------------------------------------------------------
function count_rooms ()
  local count = 0
  for k, v in pairs (rooms) do
    count = count + 1
  end -- for
  return count
end -- count_rooms

-- =========================================================
-- EXPORT / IMPORT (rooms + duplicates + nomap + full kv)
-- =========================================================

local function db_kv_get_all ()
  db_open ()
  local t = {}
  for row in _db:nrows ("SELECT k, v FROM kv") do
    if row.k ~= nil then
      t[tostring(row.k)] = row.v
    end
  end
  return t
end

local function db_clear_table (name)
  db_open ()
  local rc, err = _db:exec ("DELETE FROM " .. name)
  dbcheck (rc, err)
end

-- ---------------------------------------------------------
-- Export: writes ONE Lua file that returns { rooms=..., duplicates=..., nomap=..., kv=... }
-- ---------------------------------------------------------
function mapper_export (name, line, wildcards)

  local filename = wildcards[1]
  if not filename or filename == "" then
    mapper.mapprint ("Export: missing filename.")
    return
  end

  db_open ()
  db_rooms_load ()
  if not _duplicates_loaded_from_db then
    local s = db_kv_get ("duplicates")
    if s and s ~= "" then pcall(function () assert(loadstring(s))() end) end
    _duplicates_loaded_from_db = true
  end
  if not (nomap and next(nomap)) then
    db_nomap_load ()
  end

  local out = assert (io.open (filename, "w"))

  local payload = {
    rooms      = rooms or {},
    duplicates = duplicates or {},
    nomap      = nomap or {},
    kv         = db_kv_get_all (),
  }

  out:write ("return ")
  out:write (serialize.save_simple (payload))
  out:write ("\n")
  out:close ()

  mapper.mapprint (string.format ("Exported rooms/duplicates/nomap/kv to: %s", filename))
end

-- -----------------------------------------------------------------
-- set_window_width - sets the mapper window width
-- -----------------------------------------------------------------
function set_window_width (name, line, wildcards)
  local size = tonumber (wildcards [1])
  if not size then
    mapper.maperror ("Bad size: " .. size)
    return
  end -- if

  if size < 200 or size > 1000 then
    mapper.maperror ("Size must be in the range 200 to 1000 pixels")
    return
  end -- if

  config.WINDOW.width = size
  mapper.mapprint ("Map window width set to", size, "pixels")
  if last_drawn_id then
    mapper.draw (last_drawn_id)
  end -- if
end -- set_window_width

-- -----------------------------------------------------------------
-- set_window_height - sets the mapper window height
-- -----------------------------------------------------------------
function set_window_height (name, line, wildcards)
  local size = tonumber (wildcards [1])
  if not size then
    mapper.maperror ("Bad size: " .. size)
    return
  end -- if

  if size < 200 or size > 1000 then
    mapper.maperror ("Size must be in the range 200 to 1000 pixels")
    return
  end -- if

  config.WINDOW.height = size
  mapper.mapprint ("Map window height set to", size, "pixels")
  if last_drawn_id then
    mapper.draw (last_drawn_id)
  end -- if
end -- set_window_height

-- ---------------------------------------------------------
-- Import: loads that Lua file and REPLACES db tables
-- ---------------------------------------------------------
function mapper_import (name, line, wildcards)

  local filename = wildcards[1]
  if not filename or filename == "" then
    mapper.mapprint ("Import: missing filename.")
    return
  end

  local f, err = loadfile (filename)
  if not f then
    mapper.mapprint ("Import failed: " .. tostring(err))
    return
  end

  local ok, payload = pcall (f)
  if not ok or type(payload) ~= "table" then
    mapper.mapprint ("Import failed: file did not return a table.")
    return
  end

  local in_rooms      = payload.rooms      or {}
  local in_duplicates = payload.duplicates or {}
  local in_nomap      = payload.nomap      or {}
  local in_kv         = payload.kv         or {}

  db_open ()

  -- wipe existing DB state
  db_clear_table ("rooms")
  db_clear_table ("nomap")
  db_clear_table ("kv")

  -- restore kv FIRST (so nomap_seq etc is restored)
  for k, v in pairs (in_kv) do
    db_kv_set (tostring(k), v)
  end

  -- restore duplicates (authoritative table + kv mirror)
  duplicates = in_duplicates
  db_kv_set ("duplicates", serialize.save_simple (duplicates))

  -- restore rooms
  rooms = {}
  for uid, room in pairs (in_rooms) do
    local U = tostring(uid):upper()
    rooms[U] = room
    db_room_upsert (U, room)
  end

  -- restore nomap
  nomap = in_nomap
  for id, v in pairs (nomap) do
    db_nomap_upsert (tostring(id), v)
  end

  -- reset runtime caches
  _rooms_loaded_from_db      = true
  _duplicates_loaded_from_db = true
  _corpus_loaded_from_db     = false  -- will lazy-load from kv("corpus") when needed

  mapper.mapprint (string.format (
    "Imported: %d rooms, %d nomap, %d kv keys.",
    (function() local n=0; for _ in pairs(rooms) do n=n+1 end; return n end)(),
    (function() local n=0; for _ in pairs(nomap) do n=n+1 end; return n end)(),
    (function() local n=0; for _ in pairs(in_kv) do n=n+1 end; return n end)()
  ))

  mapper_redraw_now ()
end

-- -----------------------------------------------------------------
-- count_values - count how many values are in the database
-- -----------------------------------------------------------------
function count_values (t, done)
  local count = count or 0
  done = done or {}
  for key, value in pairs (t) do
    if type (value) == "table" and not done [value] then
      done [value] = true
      count = count + count_values (value, done)
    elseif key == 'score' then
      count = count + 1
    end
  end
  return count
end -- count_values

-- -----------------------------------------------------------------
-- corpus_export - writes the corpus table to a file
-- -----------------------------------------------------------------
function corpus_export (name, line, wildcards)
  local filter = { lua = "Lua files" }

  local filename = utils.filepicker ("Export map corpus", "Map_corpus " .. WorldName () .. ".lua", "lua", filter, true)
  if not filename then
    return
  end -- if cancelled
  local f, err = io.open (filename, "w")
  if not f then
    corpus.maperror ("Cannot open " .. filename .. " for output: " .. err)
    return
  end -- if not open

  local status, err = f:write ("corpus = "  .. serialize.save_simple (corpus) .. "\n")
  if not status then
    mapper.maperror ("Cannot write corpus to " .. filename .. ": " .. err)
  end -- if cannot write
  f:close ()
  mapper.mapprint ("Corpus exported, " .. count_values (corpus) .. " entries.")
end -- corpus_export


-- -----------------------------------------------------------------
-- corpus_import - imports the corpus table from a file
-- -----------------------------------------------------------------
function corpus_import (name, line, wildcards)

  if count_values (corpus) > 0 then
    mapper.maperror ("Corpus is not empty (there are " .. count_values (corpus) .. " entries in it)")
    mapper.maperror ("Before importing another corpus, clear this one out with: mapper reset corpus")
    return
  end -- if

  local filter = { lua = "Lua files" }

  local filename = utils.filepicker ("Import map corpus", "Map_corpus " .. WorldName () .. ".lua", "lua", filter, false)
  if not filename then
    return
  end -- if cancelled
  local f, err = io.open (filename, "r")
  if not f then
    mapper.maperror ("Cannot open " .. filename .. " for input: " .. err)
    return
  end -- if not open

  local s, err = f:read ("*a")
  if not s then
    mapper.maperror ("Cannot read corpus from " .. filename .. ": " .. err)
  end -- if cannot write
  f:close ()

  -- make a sandbox so they can't put Lua functions into the import file

  local t = {} -- empty environment table
  f = assert (loadstring (s))
  setfenv (f, t)
  -- load it
  f ()

  -- move the corpus table into our corpus table
  corpus = t.corpus
  mapper.mapprint ("Corpus imported, " .. count_values (corpus) .. " entries.")

  db_kv_set ("corpus", "corpus = " .. serialize.save_simple (corpus))
end -- corpus_import

-- -----------------------------------------------------------------
-- room_toggle_trainer /  room_toggle_shop / room_toggle_bank
-- menu handlers to toggle trainers, shops, banks
-- -----------------------------------------------------------------
function room_toggle_trainer (room, uid)
  room.Trainer = not room.Trainer
  mapper.mapprint ("Trainer here: " .. config_display_boolean (room.Trainer))
end -- room_toggle_trainer

function room_toggle_shop (room, uid)
  room.Shop = not room.Shop
  mapper.mapprint ("Shop here: " .. config_display_boolean (room.Shop))
end -- room_toggle_shop

function room_toggle_bank (room, uid)
  room.Bank = not room.Bank
  mapper.mapprint ("Bank here: " .. config_display_boolean (room.Bank))
end -- room_toggle_bank

-- -----------------------------------------------------------------
-- room_edit_bookmark - menu handler to add, edit or remove a note
-- -----------------------------------------------------------------
function room_edit_bookmark (room, uid)

  local notes = room.notes
  local found = room.notes and room.notes ~= ""


  if found then
    newnotes = utils.inputbox ("Modify room comment (clear it to delete it)", room.name, notes)
  else
    newnotes = utils.inputbox ("Enter room comment (creates a note for this room)", room.name, notes)
  end -- if

  if not newnotes then
    return
  end -- if cancelled

  if newnotes == "" then
    if not found then
      mapper.mapprint ("Nothing, note not saved.")
      return
    else
      mapper.mapprint ("Note for room", uid, "deleted. Was previously:", notes)
      rooms [uid].notes = nil
      return
    end -- if
  end -- if

  if notes == newnotes then
    return -- no change made
  end -- if

  if found then
     mapper.mapprint ("Note for room", uid, "changed to:", newnotes)
   else
     mapper.mapprint ("Note added to room", uid, ":", newnotes)
   end -- if

   rooms [uid].notes = newnotes

end -- room_edit_bookmark

-- -----------------------------------------------------------------
-- room_edit_area - menu handler to edit the room area
-- -----------------------------------------------------------------
function room_edit_area (room, uid)

  local area = room.area


  newarea = utils.inputbox ("Modify room area name", room.name, area)

  if not newarea then
    return
  end -- if cancelled

  if newarea == "" then
    mapper.mapprint ("Area not changed.")
    return
  end -- if

  if area == newarea then
    return -- no change made
  end -- if

   mapper.mapprint ("Area name for room", uid, "changed to:", newarea)

   rooms [uid].area = newarea

end -- room_edit_area


-- -----------------------------------------------------------------
-- room_delete_exit - menu handler to delete an exit
-- -----------------------------------------------------------------
function room_delete_exit (room, uid)

local available =  {
  n = "North",
  s = "South",
  e = "East",
  w = "West",
  u = "Up",
  d = "Down",
  ne = "Northeast",
  sw = "Southwest",
  nw = "Northwest",
  se = "Southeast",
  ['in'] = "In",
  out = "Out",
  }  -- end of available

  -- remove non-existent exits
  for k in pairs (available) do
    if room.exits [k] then
      available [k] = available [k] .. " --> " .. fixuid (room.exits [k])
    else
      available [k] = nil
    end -- if not a room exit
  end -- for

  if next (available) == nil then
    utils.msgbox ("There are no exits from this room.", "No exits!", "ok", "!", 1)
    return
  end -- not known

  local chosen_exit = utils.listbox ("Choose exit to delete", "Exits ...", available )
  if not chosen_exit then
    return
  end

  mapper.mapprint ("Deleted exit", available [chosen_exit], "from room", uid, "from mapper.")

  -- update in-memory table
  rooms [uid].exits [chosen_exit] = nil

  local current_room = get_current_room_ ("room_delete_exit")

  mapper.draw (current_room)
  last_drawn_id = current_room    -- in case they change the window size

end -- room_delete_exit

-- -----------------------------------------------------------------
-- room_show_description - menu handler to show the room description
-- -----------------------------------------------------------------
function room_show_description (room, uid)

  local font_name = GetInfo (20) -- output window font
  local font_size = GetOption "output_font_height"
  local output_width  = GetInfo (240)  -- average width of pixels per character
  local wrap_column   = GetOption ('wrap_column')
  local _, lines = string.gsub (room.desc, "\n", "x") -- count lines

  local font_height = WindowFontInfo (win, font_id, 1)  -- height

  utils.editbox ("", "Description of " .. room.name, string.gsub (room.desc, "\n", "\r\n"), font_name, font_size,
                { read_only = true,
                box_width  = output_width * wrap_column + 100,
                box_height  = font_height * (lines + 1) + 120,
                reply_width = output_width * wrap_column + 10,
                -- cancel_button_width = 1,
                prompt_height = 1,
                 } )

end -- room_show_description

-- -----------------------------------------------------------------
-- room_connect_exit - menu handler to connect an exit to another room
-- -----------------------------------------------------------------
function room_connect_exit  (room, uid, dir)
  -- find what room they want to connect to
  local response = utils.inputbox ("Enter UID of room which is " .. dir:upper () .. " of here", "Connect exit")
  if not response then
    return
  end -- cancelled

  -- get rid of spaces, make upper-case
  response = Trim (response):upper ()

  -- validate
  if not string.match (response, "^%x%x%x+$") then
    mapper.maperror ("Destination room UID must be a hexadecimal string")
    return
  end -- if

  -- check it exists
  dest_uid = find_room_partial_uid (response)

  if not dest_uid then
    return  -- doesn't exist
  end -- if

  if duplicates [dest_uid] then
    mapper.maperror ("Cannot connect to a room marked as a duplicate")
    return
  end -- if

  if uid == dest_uid then
    mapper.maperror ("Cannot connect to a room to itself")
    return
  end -- if

  room.exits [dir] = dest_uid

  INFO (string.format ("Exit %s from %s now connects to %s",
      dir:upper (), fixuid (uid), fixuid (dest_uid)))

  local inverse = inverse_direction [dir]
  if inverse then
    rooms [dest_uid].exits [inverse] = uid
    INFO ("Added inverse direction from " .. fixuid (dest_uid) .. " in the direction " .. inverse .. " to be " .. fixuid (uid))
  end -- of having an inverse


end -- room_connect_exit

function room_copy_uid (room, uid)
  SetClipboard (uid)
  mapper.mapprint (string.format ("UID %s (in full: %s) now on the clipboard", fixuid (uid), uid))
end -- room_copy_uid

-- -----------------------------------------------------------------
-- room_click - RH-click on a room
-- -----------------------------------------------------------------
function room_click (uid, flags)

  -- check we got room at all
  if not uid then
    return nil
  end -- if

  -- look it up
  local room = rooms [uid]

  if not room then
    return
  end -- if still not there

  local notes_desc = "Add note"
  if room.notes then
    notes_desc = "Edit note"
  end -- if

  local handlers = {
      { name = notes_desc, func = room_edit_bookmark} ,
      { name = "Edit area", func = room_edit_area } ,
      { name = "Show description", func = room_show_description} ,
      { name = "-", } ,
      { name = "Trainer", func = room_toggle_trainer, check_item = true} ,
      { name = "Shop",    func = room_toggle_shop,    check_item = true} ,
      { name = "Bank",    func = room_toggle_bank,    check_item = true} ,
      { name = "-", } ,
      { name = "Copy UID", func = room_copy_uid } ,
      { name = "-", } ,
	  { name = "Mark Duplicate", func = mark_duplicate_room } ,
      { name = "-", } ,
      } -- handlers

  local count = 0
  for dir, dest in pairs (room.exits) do
    if dest == '0' then
      table.insert (handlers, { name = "Connect " .. dir:upper () .. " exit",
        func = function ()
          return room_connect_exit (room, uid, dir)
        end -- factory function
      } )
      count = count + 1
    end -- if not known
  end -- for
  if count > 0 then
    table.insert (handlers, { name = "-" })
  end --if we had any

  table.insert (handlers, { name = "Delete an exit", func = room_delete_exit} )

  local t, tf = {}, {}
  for _, v in pairs (handlers) do
    local name = v.name
    if v.check_item then
      if room [name] then
        name = "+" .. name
      end -- if
    end -- if need to add a checkmark
    table.insert (t, name)
    tf [v.name] = v.func
  end -- for

  local choice = WindowMenu (mapper.win,
                            WindowInfo (mapper.win, 14),
                            WindowInfo (mapper.win, 15),
                            table.concat (t, "|"))

  -- find their choice, if any (empty string if cancelled)
  local f = tf [choice]

  if f then
    f (room, uid)
    current_room = get_current_room_ ("room_click")
    mapper.draw (current_room)
    last_drawn_id = current_room    -- in case they change the window size
  end -- if handler found


end -- room_click

-- -----------------------------------------------------------------
-- Find a with a special attribute which f(room) will return true if it exists
-- -----------------------------------------------------------------

function map_find_special (f)

  local room_ids = {}
  local count = 0

  -- scan all rooms looking for a match
  for uid, room in pairs (rooms) do
     if f (room) then
       room_ids [uid] = true
       count = count + 1
     end -- if
  end   -- finding room

  -- see if nearby
  mapper.find (
    function (uid)
      local room = room_ids [uid]
      if room then
        room_ids [uid] = nil
      end -- if
      return room, next (room_ids) == nil
    end,  -- function
    show_vnums,  -- show vnum?
    count,      -- how many to expect
    false       -- don't auto-walk
    )

end -- map_find_special

-- -----------------------------------------------------------------
-- mapper_peek - draw the map as if you were at uid
-- -----------------------------------------------------------------
function mapper_peek (name, line, wildcards)
  local uid = wildcards [1]

  peek_uid, room = find_room_partial_uid (uid)
  if not peek_uid then
    return
  end -- if not found

  mapper.draw (peek_uid)
  last_drawn_id = peek_uid    -- in case they change the window size

end -- mapper_peek

-- -----------------------------------------------------------------
-- map_shops - find nearby shops
-- -----------------------------------------------------------------
function map_shops (name, line, wildcards)
  map_find_special (function (room) return room.Shop end)
end -- map_shops

-- -----------------------------------------------------------------
-- map_trainers - find nearby trainers
-- -----------------------------------------------------------------
function map_trainers (name, line, wildcards)
  map_find_special (function (room) return room.Trainer end)
end -- map_trainers


-- -----------------------------------------------------------------
-- map_banks - find nearby banks
-- -----------------------------------------------------------------
function map_banks (name, line, wildcards)
  map_find_special (function (room) return room.Bank end)
end -- map_banks

-- -----------------------------------------------------------------
-- mark_duplicate_room - mark the current room as a duplicate UID
-- -----------------------------------------------------------------
function mark_duplicate_room (name, line, wildcards)
  if not current_room_hash then
   mapper.maperror ("We don't know where we are, try LOOK")
   return
  end -- if

  if rooms [current_room_hash] then
    rooms [current_room_hash] = nil  -- delete it
    mapper.mapprint ("Room", current_room_hash, "deleted.")
  end -- if

  duplicates [current_room_hash] = true -- this is a duplicated room
  mapper.mapprint (string.format ("Room %s now marked as being a duplicated one", fixuid (current_room_hash)))
  set_current_room (nil)  -- throw away incorrect UID
  set_from_room (nil)
  set_last_direction_moved (nil)

  mapper.draw (UNKNOWN_DUPLICATE_ROOM)  -- draw with unknown room

end -- mark_duplicate_room

-- -----------------------------------------------------------------
-- mapper_integrity - integrity check of exits
-- -----------------------------------------------------------------
function mapper_integrity (name, line, wildcards)
  local p = mapper.mapprint
  local repair = Trim (wildcards [1]) == 'repair'

  p("Mapper integrity check")
  local t = { }  -- table of rooms that have connections
  for uid, room in pairs (rooms) do
    for dir, dest in pairs (room.exits) do
      if rooms [dest] then
        -- check for inverse exits
        local dest_room = rooms [dest]
        local inverse = inverse_direction [dir]
        if inverse then
          if dest_room.exits [inverse] ~= uid then
            p(string.format ("Room %s exit %s leads to %s, however room %s exit %s leads to %s",
                fixuid (uid), dir, fixuid (dest), fixuid (dest), inverse, fixuid (dest_room.exits [inverse])))
            if repair then
              dest_room.exits [inverse] = uid
              p (string.format ("Changed %s exit %s to be %s", fixuid (dest), inverse, fixuid (uid)))
            end -- repair
          end -- if

        end -- if inverse exists

        -- add to table for exits that lead to this room
        if not t [dest] then
         t [dest] = { }
        end -- if destination not in table
        local dest_room = t [dest]
        if not dest_room [dir] then
          dest_room [dir] = { }
        end -- if not one from this direction yet
        table.insert (dest_room [dir], uid)
      end -- of the exit existing
    end -- for each exit

    -- see if duplicate UID not recorded
    if duplicates [uid] and not room.duplicate then
      p ("Room " .. fixuid (uid) .. " in duplicates list but we don't know what it is a duplicate of.")
    end -- if

  end -- for each room

  for dest, exits in pairs (t) do
    for dir, from in pairs (exits) do
      if #from > 1 then
        p(string.format ("Problem with exit %s leading to %s",
                         dir:upper (), fixuid (dest)))
        for _, v in ipairs (from) do
          p(string.format ("Room %s leads there",
                          fixuid (v)))
        end -- for each one
      end -- if more than one leads here
    end -- for each direction
  end -- for each destination room

  p "Done."

end -- mapper_integrity


-- based on mapper.find_paths
-- the intention of this is to find a room which is probably the one we want
-- for example, a room directly north of where we are
-- Tanthul rewrite.

-- cache to avoid BFS-per-exit
local _probable_exit_cache_uid = nil
local _probable_exit_cache_map = nil

local function find_probable_exits (uid, exits)
  local relative_location = {
    n  = { x =  0, y = -1 },
    s  = { x =  0, y =  1 },
    e  = { x =  1, y =  0 },
    w  = { x = -1, y =  0 },
    ne = { x =  1, y = -1 },
    se = { x =  1, y =  1 },
    nw = { x = -1, y = -1 },
    sw = { x = -1, y =  1 },
  }

  -- build target set only for unknown exits
  local targets = {}          -- ["x,y"] = dir
  local remaining = 0
  for dir, dest in pairs(exits) do
    if dest == "0" then
      local o = relative_location[dir]
      if o then
        local k = o.x .. "," .. o.y
        if not targets[k] then
          targets[k] = dir
          remaining = remaining + 1
        end
      end
    end
  end
  if remaining == 0 then
    return {}
  end

  local rooms_t = rooms
  local get_room_f = get_room
  local max_depth = config.SCAN.depth

  -- per-BFS room cache
  local rc = {}
  local function room(id)
    local r = rc[id]
    if r == nil then
      r = rooms_t[id] or get_room_f(id)
      rc[id] = r
      rooms_t[id] = rooms_t[id] or r
    end
    return r
  end

  -- O(1) queue of (room_id, x, y, depth)
  local q_room, q_x, q_y, q_d = {}, {}, {}, {}
  local head, tail = 1, 1

  local visited = {}
  visited[uid] = true

  q_room[tail], q_x[tail], q_y[tail], q_d[tail] = uid, 0, 0, 0
  tail = tail + 1

  local found = {}   -- dir -> room_id

  while head < tail do
    local cur  = q_room[head]
    local x    = q_x[head]
    local y    = q_y[head]
    local dep  = q_d[head]
    q_room[head], q_x[head], q_y[head], q_d[head] = nil, nil, nil, nil
    head = head + 1

    if dep < max_depth then
      local r = room(cur)
      if r and r.exits then
        for edir, dest in pairs(r.exits) do
          if not visited[dest] then
            local o = relative_location[edir]
            if o then
              visited[dest] = true
              local nx, ny = x + o.x, y + o.y

              local k = nx .. "," .. ny
              local want_dir = targets[k]
              if want_dir and not found[want_dir] then
                if room(dest) then
                  found[want_dir] = dest
                  remaining = remaining - 1
                  if remaining == 0 then
                    return found
                  end
                end
              end

              if room(dest) then
                q_room[tail], q_x[tail], q_y[tail], q_d[tail] = dest, nx, ny, dep + 1
                tail = tail + 1
              end
            end
          end
        end
      end
    end
  end

  return found
end

-- keep API; now uses the per-room BFS result
function find_probable_room (uid, dir)
  if _probable_exit_cache_uid ~= uid then
    local r = rooms[uid] or get_room(uid)
    _probable_exit_cache_map = r and r.exits and find_probable_exits(uid, r.exits) or {}
    _probable_exit_cache_uid = uid
  end
  return _probable_exit_cache_map and _probable_exit_cache_map[dir] or false
end

-- -----------------------------------------------------------------
-- validate_linetype and  validate_option
-- helper functions for validating line types and option names
-- -----------------------------------------------------------------

function validate_linetype (which, func_name)
  if not line_types [which] then
    mapper.maperror ("Invalid line type '" .. which .. "' given to '" .. func_name .. "'")
    mapper.mapprint ("  Line types are:")
    for k, v in pairs (line_types) do
      mapper.mapprint ("    " .. k)
    end
    return false
  end -- not valid
  return true
end -- validate_linetype

function validate_option (which, func_name)
  -- config name given - look it up in the list
  local config_item = config_control_names [which]
  if not config_item then
    mapper.maperror ("Invalid config item name '" .. which .. "' given to '" .. func_name .. "'")
    mapper.mapprint ("  Configuration items are:")
    for k, v in ipairs (config_control) do
      mapper.mapprint ("    " .. v.name)
    end
    return false
  end -- config item not found
  return config_item
end -- validate_option

-- =================================================================
-- EXPOSED FUNCTIONS FOR OTHER PLUGINS TO CALL
-- =================================================================

-- -----------------------------------------------------------------
-- set_line_type - the current line is of type: linetype
-- linetype is one of: description, exits, room_name, prompt, ignore
-- optional contents lets you change what the contents are (eg. from "You are standing in a field" to "in a field")
-- -----------------------------------------------------------------
override_line_type = nil
override_line_contents = nil
function set_line_type (linetype, contents)
  if not validate_linetype (linetype, 'set_line_type') then
    return nil
  end -- not valid
  override_line_type = linetype
  override_line_contents = contents
  this_line = GetLinesInBufferCount()         -- which line in the output buffer
  line_number = GetLineInfo (this_line, 10)   -- which line this was overall

  -- if line type not recorded for this line yet, record it
  if not deduced_line_types [line_number] then
    deduced_line_types [line_number] = {
        lt = override_line_type,  -- what type we assigned to it
        con = 100,  -- with what probability
        draw = false,       -- did we draw on this line?
        ov = override_line_type,  -- was it overridden? (yes)
        }
  end -- if

  return true
end -- set_line_type

-- -----------------------------------------------------------------
-- set_line_type_contents - set the contents of <linetype> to be <contents>
-- linetype is one of: description, exits, room_name, prompt, ignore
-- This lets you set something (like the room name) from another line (eg. the prompt)
-- -----------------------------------------------------------------
override_contents = { }
function set_line_type_contents (linetype, contents)
  if not validate_linetype (linetype, 'set_line_type_contents') then
    return nil
  end -- not valid
  override_contents [linetype] = contents
  return true
end -- set_line_type_contents

-- -----------------------------------------------------------------
-- set_not_line_type - the current line is NOT of type: linetype
-- linetype is one of: description, exits, room_name, prompt, ignore
-- -----------------------------------------------------------------
line_is_not_line_type = { }
function set_not_line_type (linetype)
  if not validate_linetype (linetype, 'set_not_line_type') then
    return nil
  end -- not valid
  line_is_not_line_type [linetype] = true
  return true
end -- set_not_line_type

-- -----------------------------------------------------------------
-- set_area_name - set the name of the current area (used at the bottom of the map)
-- -----------------------------------------------------------------
area_name = nil
function set_area_name (name)
  area_name = name
end -- set_area_name

-- -----------------------------------------------------------------
-- set_current_area_name - set the name of the current area (used at the bottom of the map)
-- -----------------------------------------------------------------
function set_current_area_name (name)
  if uid and name and rooms [uid] then
    rooms [uid].area = name
	mapper.draw (uid)
  end -- if
end -- set_area_name

-- -----------------------------------------------------------------
-- set_uid - set the uid for the current room
-- -----------------------------------------------------------------
override_uid = nil
function set_uid (uid)
  override_uid = uid
end -- set_uid

-- -----------------------------------------------------------------
-- do_not_deduce_line_type - do not use the Bayesian deduction on linetype
-- linetype is one of: description, exits, room_name, prompt, ignore

-- Used to make sure that lines which we have not explicitly set (eg. to an exit)
-- are never deduced to be an exit. Useful for making sure that set_line_type is
-- the only way we know a certain line is a certain type (eg. an exit line)
-- -----------------------------------------------------------------
do_not_deduce_linetypes = { }
function do_not_deduce_line_type (linetype)
  if not validate_linetype (linetype, 'do_not_deduce_line_type') then
    return nil
  end -- not valid
  do_not_deduce_linetypes [linetype] = true
  return true
end -- do_not_deduce_line_type

-- -----------------------------------------------------------------
-- deduce_line_type - use the Bayesian deduction on linetype
--   (undoes do_not_deduce_line_type for that type of line)
-- linetype is one of: description, exits, room_name, prompt, ignore
-- -----------------------------------------------------------------
function deduce_line_type (linetype)
  if not validate_linetype (linetype, 'deduce_line_type') then
    return nil
  end -- not valid
  do_not_deduce_linetypes [linetype] = nil
  return true
end -- do_not_deduce_line_type

-- -----------------------------------------------------------------
-- get the previous line type (deduced or not)
-- returns nil if no last deduced type
-- -----------------------------------------------------------------
function get_last_line_type ()
  return last_deduced_type
end -- get_last_line_type

-- -----------------------------------------------------------------
-- get the current overridden line type
-- returns nil if no last overridden type
-- -----------------------------------------------------------------
function get_this_line_type ()
  return override_line_type
end -- get_last_line_type

-- -----------------------------------------------------------------
-- set_config_option - set a configuration option
-- name: which option (eg. when_to_draw)
-- value: new setting (string) (eg. 'description')
-- equivalent in behaviour to: mapper config <name> <value>
-- returns nil if option name not given or invalid
-- returns true on success
-- -----------------------------------------------------------------
function set_config_option (name, value)
  if type (value) == 'boolean' then
    if value then
      value = 'yes'
    else
      value = 'no'
    end -- if
  end -- they supplied a boolean
  return mapper_config (name, 'mapper config whatever', { name = name or '', value = value or '' } )
end -- set_config_option

-- -----------------------------------------------------------------
-- get_config_option - get a configuration option
-- name: which option (eg. when_to_draw)
-- returns (string) (eg. 'description')
-- returns nil if option name not given or invalid
-- -----------------------------------------------------------------
function get_config_option (name)
  if not name or name == '' then
    mapper.mapprint ("No option name given to 'get_config_option'")
    return nil
  end -- if no name
  local config_item = validate_option (name, 'get_config_option')
  if not config_item then
    return nil
  end -- if not valid
  return config_item.show (config [config_item.option])
end -- get_config_option

-- -----------------------------------------------------------------
-- get_corpus - gets the corpus (serialized)
-- -----------------------------------------------------------------
function get_corpus ()
  return "corpus = " .. serialize.save_simple (corpus)
end -- get_corpus_count

-- -----------------------------------------------------------------
-- get_stats - gets the training stats (serialized)
-- -----------------------------------------------------------------
function get_stats ()
  return "stats = " .. serialize.save_simple (stats)
end -- get_stats

-- -----------------------------------------------------------------
-- get_database - gets the mapper database (rooms) (serialized)
-- -----------------------------------------------------------------
function get_database ()
  return "rooms = " .. serialize.save_simple (rooms)
end -- get_database

-- -----------------------------------------------------------------
-- get_config - gets the mapper database (rooms) (serialized)
-- -----------------------------------------------------------------
function get_config ()
  return "config = " .. serialize.save_simple (config)
end -- get_config

-- -----------------------------------------------------------------
-- get_current_room_ - gets the current room's data (serialized)
-- returns uid, room
-- -----------------------------------------------------------------
function get_current_room ()
  local current_room = get_current_room_ ("get_current_room")
  if not current_room or not rooms [current_room] then
    return nil
  end -- if
  return uid, "room = " .. serialize.save_simple (rooms [current_room])
end -- get_current_room

-- -----------------------------------------------------------------
-- set_room_extras - sets extra information for the nominated UID
-- returns true if UID exists, false if not
-- extras must be a serialized table. eg. "{a = 'nick', b = 666 }"
-- -----------------------------------------------------------------
function set_room_extras (uid, extras)
  if not uid  or not rooms [uid] or type (extras) ~= 'string' then
    return false
  end -- if

  -- make a sandbox so they can't put Lua functions into the import data
  local t = {} -- empty environment table
  f = assert (loadstring ('extras =  ' .. extras))
  setfenv (f, t)
  -- load it
  f ()

  -- move the rooms table into our rooms table
  rooms [uid].extras = t.extras
  return true
end -- set_room_extras

