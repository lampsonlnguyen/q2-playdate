#pragma once

#include "lua.h"

#define CARD_BUNDLE "/mnt/mmc/.playdate/demo.pdx"

void card_lua_init(lua_State *lua);
int card_run(lua_State *lua);
