#pragma once

#include <stddef.h>
#include "lua.h"

lua_State *bundle_open(const char *path, void (*setup)(lua_State *),
                       char *error, size_t error_size);
