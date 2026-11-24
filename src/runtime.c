#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "card.h"
#include "lauxlib.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <tinyalsa/pcm.h>

enum { RATE = 48000, TICK_SAMPLES = RATE * 33 / 1000, SAVE_LIMIT = 65536 };
static char save_directory[512];
static struct pcm *output;
static int attempted;
static unsigned remaining;
static double phase, step;
static int amplitude;
static int jack_enabled = 1;
static unsigned game_ms;

static void dac(unsigned long request, int value)
{
    int fd = open("/dev/shanling_dac", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        ioctl(fd, request, &value);
        close(fd);
    }
}

void runtime_silence(void)
{
    remaining = 0;
    attempted = 0;
    if (output) {
        dac(0xc0044d1fu, 1);
        pcm_close(output);
        output = NULL;
    }
}

void runtime_set_output(const char *device)
{
    jack_enabled = !device || strcmp(device, "plughw:0,0") == 0;
}

void runtime_configure(const char *bundle)
{
    runtime_silence();
    game_ms = 0;
    const char *name = strrchr(bundle, '/');
    snprintf(save_directory, sizeof save_directory, CARD_ROOT "/saves/%s-%s",
             strcmp(bundle, CARD_ROOT "/demo.pdx") == 0 ? "builtin" : "game",
             name ? name + 1 : bundle);
}

void runtime_tick(void)
{
    game_ms += 33;
    if (jack_enabled && remaining && !attempted) {
        const struct pcm_config config = {
            .channels = 2, .rate = RATE, .period_size = 1024, .period_count = 4,
            .format = PCM_FORMAT_S16_LE
        };
        attempted = 1;
        output = pcm_open(0, 0, PCM_OUT | PCM_NONBLOCK, &config);
        if (!output || !pcm_is_ready(output)) {
            if (output)
                pcm_close(output);
            output = NULL;
        } else {
            dac(0xc0044d1bu, 0);
            dac(0xc0044d1fu, 0);
        }
    }
    if (!output)
        return;
    int16_t samples[TICK_SAMPLES][2];
    for (unsigned i = 0; i < TICK_SAMPLES; ++i) {
        int16_t value = 0;
        if (remaining) {
            value = phase < 0.5 ? amplitude : -amplitude;
            phase += step;
            if (phase >= 1)
                phase -= 1;
            remaining--;
        }
        samples[i][0] = samples[i][1] = value;
    }
    int written = pcm_writei(output, samples, TICK_SAMPLES);
    (void)written;
}

static int play_note(lua_State *lua)
{
    double frequency = luaL_checknumber(lua, 2);
    double volume = luaL_optnumber(lua, 3, 0.5);
    double seconds = luaL_optnumber(lua, 4, 0.2);
    luaL_argcheck(lua, frequency >= 1 && frequency <= 10000, 2, "frequency out of range");
    luaL_argcheck(lua, volume >= 0 && volume <= 1, 3, "volume out of range");
    luaL_argcheck(lua, seconds > 0 && seconds <= 10, 4, "duration out of range");
    phase = 0;
    step = frequency / RATE;
    amplitude = (int)(volume * 3500);
    remaining = (unsigned)(RATE * seconds);
    return 0;
}

static int synth_new(lua_State *lua)
{
    luaL_argcheck(lua, luaL_optinteger(lua, 1, 0) == 0, 1, "only square wave is supported");
    lua_newtable(lua);
    lua_pushcfunction(lua, play_note);
    lua_setfield(lua, -2, "playNote");
    return 1;
}

static int save_path(lua_State *lua, int arg, char *path, size_t size)
{
    size_t length;
    const char *name = luaL_optlstring(lua, arg, "data", &length);
    if (!length || length > 48)
        return 0;
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '-' && name[i] != '_')
            return 0;
    return snprintf(path, size, "%s/%s.q2s", save_directory, name) < (int)size;
}

/* The private format stores scalar values and nested tables. Length and depth
 * limits bound both writes and reads, including corrupt files. */
typedef struct {
    unsigned char bytes[SAVE_LIMIT];
    size_t size, position;
} Save;

static int append(Save *save, const void *bytes, size_t count)
{
    if (count > sizeof save->bytes - save->size)
        return 0;
    memcpy(save->bytes + save->size, bytes, count);
    save->size += count;
    return 1;
}

static int consume(Save *save, void *bytes, size_t count)
{
    if (count > save->size - save->position)
        return 0;
    memcpy(bytes, save->bytes + save->position, count);
    save->position += count;
    return 1;
}

static int encode(lua_State *lua, int index, Save *save, unsigned depth)
{
    if (depth > 12 || !lua_checkstack(lua, 4))
        return 0;
    index = lua_absindex(lua, index);
    unsigned char tag = lua_type(lua, index);
    if (tag == LUA_TNUMBER && lua_isinteger(lua, index))
        tag = 32;
    if (!append(save, &tag, 1))
        return 0;
    switch (tag) {
    case LUA_TBOOLEAN: {
        unsigned char value = lua_toboolean(lua, index);
        return append(save, &value, 1);
    }
    case 32: {
        lua_Integer value = lua_tointeger(lua, index);
        return append(save, &value, sizeof value);
    }
    case LUA_TNUMBER: {
        lua_Number number = lua_tonumber(lua, index);
        return isfinite(number) && append(save, &number, sizeof number);
    }
    case LUA_TSTRING: {
        size_t length;
        const char *value = lua_tolstring(lua, index, &length);
        uint32_t count = (uint32_t)length;
        return length <= SAVE_LIMIT && append(save, &count, sizeof count) &&
               append(save, value, length);
    }
    case LUA_TTABLE:
        lua_pushnil(lua);
        while (lua_next(lua, index)) {
            int key = lua_type(lua, -2);
            int ok = (key == LUA_TSTRING || key == LUA_TNUMBER || key == LUA_TBOOLEAN) &&
                     encode(lua, -2, save, depth + 1) && encode(lua, -1, save, depth + 1);
            lua_pop(lua, 1);
            if (!ok) {
                lua_pop(lua, 1);
                return 0;
            }
        }
        tag = 0;
        return append(save, &tag, 1);
    }
    return 0;
}

static int decode(lua_State *lua, Save *save, unsigned depth)
{
    unsigned char tag;
    if (depth > 12 || !lua_checkstack(lua, 4) || !consume(save, &tag, 1))
        return 0;
    switch (tag) {
    case LUA_TBOOLEAN: {
        unsigned char value;
        if (!consume(save, &value, 1))
            return 0;
        lua_pushboolean(lua, value != 0);
        return 1;
    }
    case 32: {
        lua_Integer value;
        if (!consume(save, &value, sizeof value))
            return 0;
        lua_pushinteger(lua, value);
        return 1;
    }
    case LUA_TNUMBER: {
        lua_Number value;
        if (!consume(save, &value, sizeof value) || !isfinite(value))
            return 0;
        lua_pushnumber(lua, value);
        return 1;
    }
    case LUA_TSTRING: {
        uint32_t count;
        if (!consume(save, &count, sizeof count) || count > save->size - save->position)
            return 0;
        lua_pushlstring(lua, (const char *)save->bytes + save->position, count);
        save->position += count;
        return 1;
    }
    case LUA_TTABLE:
        lua_newtable(lua);
        while (save->position < save->size) {
            unsigned char next = save->bytes[save->position];
            if (next == 0) {
                save->position++;
                return 1;
            }
            if ((next != LUA_TSTRING && next != LUA_TNUMBER && next != LUA_TBOOLEAN && next != 32) ||
                !decode(lua, save, depth + 1) || !decode(lua, save, depth + 1))
                return 0;
            lua_rawset(lua, -3);
        }
        return 0;
    }
    return 0;
}

static int datastore_write(lua_State *lua)
{
    char path[600], temporary[608];
    Save save = {0};
    int ok = lua_istable(lua, 1) && save_path(lua, 2, path, sizeof path) &&
             append(&save, "Q2S1", 4) && encode(lua, 1, &save, 0);
    if (ok) {
        mkdir(CARD_ROOT "/saves", 0755);
        mkdir(save_directory, 0755);
        snprintf(temporary, sizeof temporary, "%s.XXXXXX", path);
        int fd = mkstemp(temporary);
        ok = 0;
        if (fd >= 0) {
            FILE *file = fdopen(fd, "wb");
            if (file) {
                int persisted = fwrite(save.bytes, 1, save.size, file) == save.size &&
                                fflush(file) == 0 && fsync(fd) == 0;
                ok = fclose(file) == 0 && persisted && rename(temporary, path) == 0;
            } else {
                close(fd);
            }
            if (!ok)
                unlink(temporary);
        }
    }
    lua_pushboolean(lua, ok);
    return 1;
}

static int datastore_read(lua_State *lua)
{
    char path[600];
    FILE *file = save_path(lua, 1, path, sizeof path) ? fopen(path, "rb") : NULL;
    if (file) {
        Save save;
        save.size = fread(save.bytes, 1, sizeof save.bytes, file);
        int valid = !ferror(file) && fgetc(file) == EOF && !ferror(file);
        fclose(file);
        save.position = 4;
        if (valid && save.size > 4 && memcmp(save.bytes, "Q2S1", 4) == 0 &&
            decode(lua, &save, 0) && lua_istable(lua, -1) && save.position == save.size)
            return 1;
    }
    lua_pushnil(lua);
    return 1;
}

static int elapsed_ms(lua_State *lua)
{
    lua_pushinteger(lua, (lua_Integer)game_ms);
    return 1;
}

static int next_sprite(lua_State *lua)
{
    lua_settop(lua, 2);
    if (lua_next(lua, 1))
        return 2;
    return 0;
}

static const char runtime_lua[] =
    "local pd, clock, iter = ...\n"
    "local timers = {}\n"
    "local lastTick\n"
    "pd.timer = {}\n"
    "function pd.timer.new(duration, callback)\n"
    "  local timer = {duration = duration, remaining = duration, timerEndedCallback = callback}\n"
    "  function timer:remove() self.removed = true end\n"
    "  timers[#timers + 1] = timer\n"
    "  return timer\n"
    "end\n"
    "function pd.timer.updateTimers()\n"
    "  local now = clock()\n"
    "  local elapsed = lastTick and ((now - lastTick) & 0xFFFFFFFF) or 0\n"
    "  lastTick = now\n"
    "  local count = #timers\n"
    "  for i = 1, count do\n"
    "    local timer = timers[i]\n"
    "    if not timer.removed then\n"
    "      timer.remaining = timer.remaining - elapsed\n"
    "      if timer.remaining <= 0 then\n"
    "        timer.removed = not timer.repeats\n"
    "        if timer.repeats then timer.remaining = timer.duration end\n"
    "        if timer.timerEndedCallback then timer.timerEndedCallback(timer) end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  local next = 1\n"
    "  for i = 1, #timers do\n"
    "    if not timers[i].removed then timers[next] = timers[i]; next = next + 1 end\n"
    "  end\n"
    "  for i = next, #timers do timers[i] = nil end\n"
    "end\n"
    "local sprites = {}\n"
    "pd.graphics.sprite = {}\n"
    "function pd.graphics.sprite.new()\n"
    "  local sprite = {x = 0, y = 0, width = 0, height = 0}\n"
    "  function sprite:moveTo(x, y) self.x, self.y = x, y end\n"
    "  function sprite:setSize(w, h) self.width, self.height = w, h end\n"
    "  function sprite:add() if not sprites[self] then sprites[self] = true end end\n"
    "  function sprite:remove() sprites[self] = nil end\n"
    "  return sprite\n"
    "end\n"
    "function pd.graphics.sprite.update()\n"
    "  local current = {}\n"
    "  for sprite in iter, sprites do current[#current + 1] = sprite end\n"
    "  for i = 1, #current do\n"
    "    local sprite = current[i]\n"
    "    if sprites[sprite] and sprite.update then sprite:update() end\n"
    "    if sprites[sprite] and sprite.draw then\n"
    "      sprite:draw(sprite.x - sprite.width // 2, sprite.y - sprite.height // 2, sprite.width, sprite.height)\n"
    "    end\n"
    "  end\n"
    "end\n";

void runtime_install(lua_State *lua)
{
    static const luaL_Reg datastore[] = {
        {"read", datastore_read}, {"write", datastore_write}, {NULL, NULL}
    };
    luaL_newlib(lua, datastore);
    lua_setfield(lua, -2, "datastore");

    lua_newtable(lua);
    lua_pushinteger(lua, 0);
    lua_setfield(lua, -2, "kWaveSquare");
    lua_newtable(lua);
    lua_pushcfunction(lua, synth_new);
    lua_setfield(lua, -2, "new");
    lua_setfield(lua, -2, "synth");
    lua_setfield(lua, -2, "sound");

    if (luaL_loadbuffer(lua, runtime_lua, sizeof runtime_lua - 1, "=q2-runtime") == LUA_OK) {
        lua_pushvalue(lua, -2);
        lua_pushcfunction(lua, elapsed_ms);
        lua_pushcfunction(lua, next_sprite);
        if (lua_pcall(lua, 3, 0, 0) == LUA_OK)
            return;
    }
    fprintf(stderr, "Q2 Lua runtime: %s\n", lua_tostring(lua, -1));
    lua_pop(lua, 1);
}
