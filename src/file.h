#pragma once

#include "lua.h"

/* Bundle directory used to resolve file paths (set by bundle_open,
 * alongside image_set_bundle). Also records the per-game saves
 * directory (CARD_ROOT/saves/<builtin|game>-<name>) used as the
 * writable data folder. */
void file_set_bundle(const char *bundle);

/* Install playdate.file and playdate.metadata on the playdate table
 * at the top of the Lua stack (same discipline as runtime_install). */
void file_install(lua_State *lua);
