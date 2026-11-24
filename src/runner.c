#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "lauxlib.h"
#include "card.h"

enum { MAX_PDZ = 8 * 1024 * 1024, MAX_CHUNK = 2 * 1024 * 1024 };

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char *load_main(lua_State *lua, const unsigned char *pdz, size_t size)
{
    if (size < 16 || memcmp(pdz, "Playdate PDZ", 12) != 0)
        return "invalid PDZ header";
    if (read_le32(pdz + 12) & 0x40000000u)
        return "encrypted PDZ is unsupported";

    const unsigned char *chunk = NULL;
    size_t chunk_size = 0;
    int compressed = 0;

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
        if ((entry & 15) == 1 && strcmp((const char *)name, "main") == 0) {
            if (chunk)
                return "duplicate main Lua chunk";
            chunk = pdz + pos;
            chunk_size = length;
            compressed = (entry & 128) != 0;
        }
        pos += length;
    }

    if (!chunk)
        return "PDZ has no main Lua chunk";

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

    int status = luaL_loadbufferx(lua, (const char *)chunk, chunk_size, "@main", "b");
    free(decoded);
    if (status != LUA_OK)
        return lua_tostring(lua, -1);
    return NULL;
}

static void limit_instructions(lua_State *lua, lua_Debug *debug)
{
    (void)debug;
    luaL_error(lua, "probe instruction limit exceeded");
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: %s path/to/game.pdx\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *bundle = argc == 1 ? CARD_BUNDLE : argv[1];
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/pdex.bin", bundle);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "bundle path is too long\n");
        return EXIT_FAILURE;
    }
    struct stat info;
    if (stat(path, &info) == 0) {
        fprintf(stderr, "native Playdate bundles are unsupported\n");
        return EXIT_FAILURE;
    }

    snprintf(path, sizeof(path), "%s/main.pdz", bundle);
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (fstat(fileno(file), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 16 || info.st_size > MAX_PDZ) {
        fprintf(stderr, "main.pdz must be a regular file between 16 bytes and 8 MiB\n");
        fclose(file);
        return EXIT_FAILURE;
    }
    size_t size = (size_t)info.st_size;
    unsigned char *pdz = malloc(size);
    if (!pdz || fread(pdz, 1, size, file) != size) {
        fprintf(stderr, "could not read main.pdz\n");
        fclose(file);
        free(pdz);
        return EXIT_FAILURE;
    }
    fclose(file);

    lua_State *lua = luaL_newstate();
    if (!lua) {
        fprintf(stderr, "could not create the Lua VM\n");
        free(pdz);
        return EXIT_FAILURE;
    }

    const char *error = load_main(lua, pdz, size);
    free(pdz);
    if (!error) {
        lua_newtable(lua);
        if (argc == 1)
            card_lua_init(lua);
        lua_setglobal(lua, "playdate");
        lua_sethook(lua, limit_instructions, LUA_MASKCOUNT, 100000);
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
            error = lua_tostring(lua, -1);
            if (!error)
                error = "Lua initialization failed";
        }
    }

    int result = EXIT_FAILURE;
    if (error) {
        fprintf(stderr, "%s\n", error);
    } else if (argc == 1) {
        result = card_run(lua);
    } else {
        puts("Executed main Lua chunk");
        result = EXIT_SUCCESS;
    }
    lua_close(lua);
    return result;
}
