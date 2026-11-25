#pragma once

#include <stddef.h>
#include <stdint.h>
#include "lua.h"

lua_State *bundle_open(const char *path, void (*setup)(lua_State *),
                       char *error, size_t error_size);

/* Look up a PDZ image/imagetable entry by name and return its raw
 * (decompressed) payload in a malloc'd buffer. Returns 0 on success,
 * -1 when there is no such entry or it is not an image entry. */
int bundle_image_payload(lua_State *lua, const char *name, int *is_table,
                         uint8_t **payload, size_t *payload_len);
