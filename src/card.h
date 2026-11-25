#pragma once

#ifndef CARD_ROOT
#define CARD_ROOT "/mnt/mmc/.playdate"
#endif

enum {
    SCREEN_WIDTH = 375, SCREEN_HEIGHT = 320,
    GAME_WIDTH = 320, GAME_HEIGHT = 192,
    GAME_LEFT = (SCREEN_WIDTH - GAME_WIDTH) / 2,
    GAME_TOP = (SCREEN_HEIGHT - GAME_HEIGHT) / 2
};

int card_run(void);

#include "lua.h"
void card_lua_init(lua_State *lua);
void card_configure_bundle(const char *path);
