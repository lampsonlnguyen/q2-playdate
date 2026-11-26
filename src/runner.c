#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "lauxlib.h"
#include "lualib.h"
#include "card.h"
#include "bundle.h"
#include "file.h"
#include "image.h"
#include "runtime.h"

enum { MAX_PDZ = 8 * 1024 * 1024, MAX_CHUNK = 2 * 1024 * 1024 };

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char *index_chunks(lua_State *lua, const unsigned char *pdz, size_t size)
{
    if (size < 16 || memcmp(pdz, "Playdate PDZ", 12) != 0)
        return "invalid PDZ header";
    if (read_le32(pdz + 12) & 0x40000000u)
        return "encrypted PDZ is unsupported";

    for (size_t pos = 16; pos < size;) {
        if (size - pos < 4)
            return "truncated PDZ entry header";
        uint32_t entry = read_le32(pdz + pos);
        pos += 4;
        const unsigned char *name = pdz + pos;
        const unsigned char *end = memchr(name, 0, size - pos);
        if (!end)
            return "unterminated PDZ entry name";
        pos = ((size_t)(end - pdz) + 4) & ~(size_t)3;
        size_t length = entry >> 8;
        if (pos > size || length > size - pos)
            return "truncated PDZ entry";
        if ((entry & 15) == 1 || (entry & 15) == 2 || (entry & 15) == 3 || (entry & 15) == 5 || (entry & 15) == 7) {
            lua_getfield(lua, -1, (const char *)name);
            int duplicate = !lua_isnil(lua, -1);
            lua_pop(lua, 1);
            if (duplicate)
                return "duplicate PDZ entry";
            lua_pushinteger(lua, (lua_Integer)(name - pdz - 4));
            lua_setfield(lua, -2, (const char *)name);
        }
        pos += length;
    }
    return NULL;
}

/* Locate a PDZ entry's data and decompress it if needed. On success
 * returns NULL and sets *data_out (pointing into the PDZ, or into a
 * malloc'd buffer owned by *decoded_out, which is NULL when the entry
 * was stored uncompressed), *length_out and *flag_out. */
static const char *entry_data(const unsigned char *pdz, size_t offset,
                              const unsigned char **data_out, size_t *length_out,
                              int *flag_out, unsigned char **decoded_out)
{
    uint32_t entry = read_le32(pdz + offset);
    size_t end = offset + 4 + strlen((const char *)pdz + offset + 4);
    const unsigned char *data = pdz + ((end + 4) & ~(size_t)3);
    size_t length = entry >> 8;
    *flag_out = entry & 15;
    *decoded_out = NULL;
    if (entry & 128) {
        /* Compressed PDA entries retain their four-byte audio header
         * before the decompressed-size field and zlib stream. */
        size_t prefix = *flag_out == 5 ? 4 : 0;
        if (length < prefix + 4)
            return "invalid compressed PDZ entry";
        unsigned long expected = read_le32(data + prefix);
        if (expected == 0 || expected > MAX_CHUNK)
            return "PDZ entry exceeds probe limit or is empty";
        unsigned char *decoded = malloc(expected + prefix);
        if (!decoded)
            return "out of memory";
        if (prefix)
            memcpy(decoded, data, prefix);
        unsigned long actual = expected;
        if (uncompress(decoded + prefix, &actual, data + prefix + 4, length - prefix - 4) != Z_OK ||
            actual != expected) {
            free(decoded);
            return "failed to decompress PDZ entry";
        }
        data = decoded;
        length = actual + prefix;
        *decoded_out = decoded;
    }
    if (length > MAX_CHUNK) {
        free(*decoded_out);
        *decoded_out = NULL;
        return "PDZ entry exceeds probe limit";
    }
    *data_out = data;
    *length_out = length;
    return NULL;
}

static const char *load_chunk(lua_State *lua, const unsigned char *pdz,
                              size_t offset, const char *name)
{
    const unsigned char *chunk;
    size_t chunk_size;
    int flag;
    unsigned char *decoded = NULL;
    const char *error = entry_data(pdz, offset, &chunk, &chunk_size, &flag, &decoded);
    if (error)
        return error;
    if (flag != 1) {
        free(decoded);
        return "PDZ entry is not a Lua chunk";
    }
    int status = luaL_loadbufferx(lua, (const char *)chunk, chunk_size, name, "b");
    free(decoded);
    if (status != LUA_OK)
        return lua_tostring(lua, -1);
    return NULL;
}

/* Look up a PDZ image/imagetable entry by name and return its raw
 * (decompressed) payload in a malloc'd buffer. Returns 0 on success,
 * -1 when there is no such entry or it is not an image entry. */
int bundle_image_payload(lua_State *lua, const char *name, int *is_table,
                         uint8_t **payload, size_t *payload_len)
{
    lua_getfield(lua, LUA_REGISTRYINDEX, "q2.pdz.index");
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        return -1;
    }
    lua_getfield(lua, -1, name);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 2);
        return -1;
    }
    size_t offset = (size_t)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, LUA_REGISTRYINDEX, "q2.pdz.data");
    const unsigned char *pdz = lua_touserdata(lua, -1);
    if (!pdz) {
        lua_pop(lua, 2);
        return -1;
    }
    const unsigned char *data;
    size_t length;
    int flag;
    unsigned char *decoded = NULL;
    const char *error = entry_data(pdz, offset, &data, &length, &flag, &decoded);
    lua_pop(lua, 2);
    if (error) {
        free(decoded);
        return -1;
    }
    if (flag != 2 && flag != 3) {
        free(decoded);
        return -1;
    }
    uint8_t *copy = malloc(length ? length : 1);
    if (!copy) {
        free(decoded);
        return -1;
    }
    memcpy(copy, data, length);
    free(decoded);
    *is_table = flag == 3;
    *payload = copy;
    *payload_len = length;
    return 0;
}

static int import_chunk(lua_State *lua)
{
    size_t length;
    const char *requested = luaL_checklstring(lua, 1, &length);
    char name[256];
    if (length >= sizeof name || strlen(requested) != length)
        return luaL_error(lua, "invalid import name");
    if (length > 4 && strcmp(requested + length - 4, ".lua") == 0)
        length -= 4;
    memcpy(name, requested, length);
    name[length] = 0;

    lua_getfield(lua, lua_upvalueindex(2), name);
    if (lua_isnil(lua, -1))
        return luaL_error(lua, "missing Lua import: %s", name);
    size_t offset = (size_t)lua_tointeger(lua, -1);
    lua_pop(lua, 1);

    const unsigned char *pdz = lua_touserdata(lua, lua_upvalueindex(1));
    int flag = read_le32(pdz + offset) & 15;
    lua_getfield(lua, lua_upvalueindex(3), name);
    if (lua_toboolean(lua, -1))
        return flag == 1 ? 0 : 1;
    lua_pop(lua, 1);
    lua_pushboolean(lua, 1);
    lua_setfield(lua, lua_upvalueindex(3), name);

    if (flag == 2 || flag == 3 || flag == 5 || flag == 7) {
        /* Image / imagetable entry: decode the raw PDI/PDT payload
         * and return the userdata (import's return value). */
        const unsigned char *data;
        size_t length;
        int actual_flag;
        unsigned char *decoded = NULL;
        const char *entry_error = entry_data(pdz, offset, &data, &length,
                                             &actual_flag, &decoded);
        if (entry_error) {
            free(decoded);
            lua_pushnil(lua);
            lua_setfield(lua, lua_upvalueindex(3), name);
            return luaL_error(lua, "%s: %s", name, entry_error);
        }
        if (actual_flag == 5) {
            /* Audio has a four-byte rate/format header before the sample data.
             * The uncompressed entry contains it directly. */
            int ok = runtime_push_sample(lua, data, length);
            free(decoded);
            if (!ok) {
                lua_pushnil(lua);
                lua_setfield(lua, lua_upvalueindex(3), name);
                return luaL_error(lua, "%s: invalid audio data", name);
            }
            lua_pushvalue(lua, -1);
            lua_setfield(lua, lua_upvalueindex(3), name);
            return 1;
        }
        if (actual_flag == 7) {
            int ok = font_push_payload(lua, name, data, length);
            free(decoded);
            if (!ok) {
                lua_pushnil(lua);
                lua_setfield(lua, lua_upvalueindex(3), name);
                return luaL_error(lua, "%s: invalid font data", name);
            }
            lua_pushvalue(lua, -1);
            lua_setfield(lua, lua_upvalueindex(3), name);
            return 1;
        }
        if (actual_flag == 2) {
            Image image;
            if (image_decode_payload(data, length, 0, 0, &image) != 0) {
                free(decoded);
                lua_pushnil(lua);
                lua_setfield(lua, lua_upvalueindex(3), name);
                return luaL_error(lua, "%s: invalid image data", name);
            }
            free(decoded);
            image_push(lua, &image);
            lua_pushvalue(lua, -1);
            lua_setfield(lua, lua_upvalueindex(3), name);
            return 1;
        }
        Imagetable table;
        if (imagetable_decode_payload(data, length, 0, 0, &table) != 0) {
            free(decoded);
            lua_pushnil(lua);
            lua_setfield(lua, lua_upvalueindex(3), name);
            return luaL_error(lua, "%s: invalid imagetable data", name);
        }
        free(decoded);
        imagetable_push(lua, &table);
        lua_pushvalue(lua, -1);
        lua_setfield(lua, lua_upvalueindex(3), name);
        return 1;
    }

    const char *error = load_chunk(lua, pdz, offset, name);
    if (!error && lua_pcall(lua, 0, 0, 0) == LUA_OK)
        return 0;

    lua_pushnil(lua);
    lua_setfield(lua, lua_upvalueindex(3), name);
    if (error)
        return luaL_error(lua, "%s: %s", name, error);
    return lua_error(lua);
}

static void open_game_libraries(lua_State *lua)
{
    luaL_requiref(lua, "_G", luaopen_base, 1);
    lua_pop(lua, 1);
    const char *restricted[] = {"dofile", "loadfile", "load", "collectgarbage"};
    for (size_t i = 0; i < sizeof restricted / sizeof *restricted; i++) {
        lua_pushnil(lua);
        lua_setglobal(lua, restricted[i]);
    }

    const luaL_Reg libraries[] = {
        {LUA_MATHLIBNAME, luaopen_math}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_UTF8LIBNAME, luaopen_utf8},
        /* The debug library: the real Playdate runtime ships it, and
         * official CoreLibs need it (CoreLibs/utilities/where calls
         * debug.getinfo; XTRIS imports where at load). */
        {LUA_DBLIBNAME, luaopen_debug},
    };
    for (size_t i = 0; i < sizeof libraries / sizeof *libraries; i++) {
        luaL_requiref(lua, libraries[i].name, libraries[i].func, 1);
        lua_pop(lua, 1);
    }
}

static void limit_instructions(lua_State *lua, lua_Debug *debug)
{
    (void)debug;
    luaL_error(lua, "probe instruction limit exceeded");
}

lua_State *bundle_open(const char *bundle, void (*setup)(lua_State *),
                       char *error, size_t error_size)
{
    image_set_bundle(bundle);
    file_set_bundle(bundle);
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/pdex.bin", bundle);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        snprintf(error, error_size, "Bundle path is too long");
        return NULL;
    }
    struct stat info;
    if (stat(path, &info) == 0) {
        snprintf(error, error_size, "native Playdate bundles are unsupported");
        return NULL;
    }

    snprintf(path, sizeof(path), "%s/main.pdz", bundle);
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(error, error_size, "Bundle has no readable main.pdz");
        return NULL;
    }
    if (fstat(fileno(file), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 16 || info.st_size > MAX_PDZ) {
        snprintf(error, error_size, "main.pdz must be a regular file between 16 bytes and 8 MiB");
        fclose(file);
        return NULL;
    }
    size_t size = (size_t)info.st_size;
    lua_State *lua = luaL_newstate();
    if (!lua) {
        fclose(file);
        snprintf(error, error_size, "Could not create the Lua VM");
        return NULL;
    }

    unsigned char *pdz = lua_newuserdatauv(lua, size, 0);
    int read_ok = fread(pdz, 1, size, file) == size;
    fclose(file);
    lua_newtable(lua);
    const char *problem = read_ok ? index_chunks(lua, pdz, size) : "Could not read main.pdz";
    if (!problem) {
        /* Keep the PDZ data and index reachable for image.new() lookups. */
        lua_pushvalue(lua, 1);
        lua_setfield(lua, LUA_REGISTRYINDEX, "q2.pdz.data");
        lua_pushvalue(lua, 2);
        lua_setfield(lua, LUA_REGISTRYINDEX, "q2.pdz.index");
        lua_getfield(lua, 2, "main");
        if (lua_isnil(lua, -1))
            problem = "PDZ has no main Lua chunk";
        size_t main_offset = (size_t)lua_tointeger(lua, -1);
        lua_pop(lua, 1);
        if (!problem) {
            open_game_libraries(lua);
            lua_newtable(lua);
            lua_pushboolean(lua, 1);
            lua_setfield(lua, -2, "main");
            lua_pushcclosure(lua, import_chunk, 3);
            lua_setglobal(lua, "import");
            lua_newtable(lua);
            if (setup)
                setup(lua);
            lua_setglobal(lua, "playdate");
            /* Bound the load only: Celeste's load is ~150-250k
             * instructions and a frame is ~100-200k, so a lifetime
             * limit would kill any real game after a few frames.
             * The hook is cleared once the main chunk has run. */
            lua_sethook(lua, limit_instructions, LUA_MASKCOUNT, 2000000);
            problem = load_chunk(lua, pdz, main_offset, "@main");
            if (!problem && lua_pcall(lua, 0, 0, 0) != LUA_OK) {
                problem = lua_tostring(lua, -1);
                if (!problem)
                    problem = "Lua initialization failed";
            }
            lua_sethook(lua, NULL, 0, 0);
        }
    }

    if (problem) {
        snprintf(error, error_size, "%s", problem);
        if (lua)
            lua_close(lua);
        return NULL;
    }
    return lua;
}

int main(int argc, char **argv)
{
    /* Q2 Pod supplies an audio device and, on some routes, a volume. */
    if (argc == 1 || (argc <= 3 &&
        (strcmp(argv[1], "plughw:0,0") == 0 || strcmp(argv[1], "usb") == 0 ||
         strcmp(argv[1], "plug:bluealsa") == 0))) {
        runtime_set_output(argc > 1 ? argv[1] : NULL);
        return card_run();
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s path/to/game.pdx\n", argv[0]);
        return EXIT_FAILURE;
    }
    char error[256];
    lua_State *lua = bundle_open(argv[1], card_lua_init, error, sizeof error);
    if (!lua) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    /* Headless smoke test: run a few frames through playdate.update,
     * advancing the game clock each frame like the card runner does. */
    int frames = 0;
    for (; frames < 5; frames++) {
        lua_getglobal(lua, "playdate");
        lua_getfield(lua, -1, "update");
        if (!lua_isfunction(lua, -1)) {
            lua_pop(lua, 2);
            break;
        }
        lua_remove(lua, -2);
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
            fprintf(stderr, "frame %d: %s\n", frames, lua_tostring(lua, -1));
            lua_pop(lua, 1);
            lua_close(lua);
            return EXIT_FAILURE;
        }
        runtime_tick();
    }
    lua_close(lua);
    printf("Executed main Lua chunk (%d frames)\n", frames);
    return EXIT_SUCCESS;
}
