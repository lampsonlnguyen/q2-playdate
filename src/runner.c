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
        if ((entry & 15) == 1) {
            lua_getfield(lua, -1, (const char *)name);
            int duplicate = !lua_isnil(lua, -1);
            lua_pop(lua, 1);
            if (duplicate)
                return "duplicate Lua chunk";
            lua_pushinteger(lua, (lua_Integer)(name - pdz - 4));
            lua_setfield(lua, -2, (const char *)name);
        }
        pos += length;
    }
    return NULL;
}

static const char *load_chunk(lua_State *lua, const unsigned char *pdz,
                              size_t offset, const char *name)
{
    uint32_t entry = read_le32(pdz + offset);
    size_t end = offset + 4 + strlen((const char *)pdz + offset + 4);
    const unsigned char *chunk = pdz + ((end + 4) & ~(size_t)3);
    size_t chunk_size = entry >> 8;
    int compressed = (entry & 128) != 0;

    unsigned char *decoded = NULL;
    if (compressed) {
        if (chunk_size < 4)
            return "invalid compressed Lua chunk";
        unsigned long expected = read_le32(chunk);
        if (expected == 0 || expected > MAX_CHUNK)
            return "Lua chunk exceeds probe limit or is empty";
        decoded = malloc(expected);
        if (!decoded)
            return "out of memory";
        unsigned long actual = expected;
        if (uncompress(decoded, &actual, chunk + 4, chunk_size - 4) != Z_OK ||
            actual != expected) {
            free(decoded);
            return "failed to decompress Lua chunk";
        }
        chunk = decoded;
        chunk_size = actual;
    }
    if (chunk_size > MAX_CHUNK) {
        free(decoded);
        return "Lua chunk exceeds probe limit";
    }

    int status = luaL_loadbufferx(lua, (const char *)chunk, chunk_size, name, "b");
    free(decoded);
    if (status != LUA_OK)
        return lua_tostring(lua, -1);
    return NULL;
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

    lua_getfield(lua, lua_upvalueindex(3), name);
    if (lua_toboolean(lua, -1))
        return 0;
    lua_pop(lua, 1);
    lua_pushboolean(lua, 1);
    lua_setfield(lua, lua_upvalueindex(3), name);

    const char *error = load_chunk(lua, lua_touserdata(lua, lua_upvalueindex(1)), offset, name);
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
            lua_sethook(lua, limit_instructions, LUA_MASKCOUNT, 100000);
            problem = load_chunk(lua, pdz, main_offset, "@main");
            if (!problem && lua_pcall(lua, 0, 0, 0) != LUA_OK) {
                problem = lua_tostring(lua, -1);
                if (!problem)
                    problem = "Lua initialization failed";
            }
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
    lua_State *lua = bundle_open(argv[1], NULL, error, sizeof error);
    if (!lua) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    lua_close(lua);
    puts("Executed main Lua chunk");
    return EXIT_SUCCESS;
}
