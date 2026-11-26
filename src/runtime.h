#pragma once

#include "lua.h"
#include <stddef.h>

void runtime_set_output(const char *device);
void runtime_configure(const char *bundle);
void runtime_install(lua_State *lua);
void runtime_tick(void);
void runtime_silence(void);

int runtime_push_sample(lua_State *lua, const unsigned char *data, size_t length);
