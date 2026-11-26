#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "card.h"
#include "file.h"
#include "image.h"
#include "json_lua.h"
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
#include <time.h>
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
static unsigned elapsed_base;

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
    elapsed_base = 0;
    image_set_bundle(bundle);
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

static int datastore_delete(lua_State *lua)
{
    char path[600];
    lua_pushboolean(lua, save_path(lua, 1, path, sizeof path) && unlink(path) == 0);
    return 1;
}

static int elapsed_ms(lua_State *lua)
{
    lua_pushinteger(lua, (lua_Integer)game_ms);
    return 1;
}

/* --- Sound stubs: no audio is decoded or played. The objects accept
 * the calls games make and stay silent. --- */

static int sound_noop(lua_State *lua)
{
    (void)lua;
    return 0;
}

static int sound_false(lua_State *lua)
{
    lua_pushboolean(lua, 0);
    return 1;
}

static int sound_zero(lua_State *lua)
{
    lua_pushnumber(lua, 0);
    return 1;
}

static int sound_one(lua_State *lua)
{
    lua_pushnumber(lua, 1);
    return 1;
}

static void sound_method(lua_State *lua, const char *name, lua_CFunction fn)
{
    lua_pushcfunction(lua, fn);
    lua_setfield(lua, -2, name);
}

static int fileplayer_new(lua_State *lua)
{
    (void)luaL_checkstring(lua, 1);
    lua_newtable(lua);
    sound_method(lua, "play", sound_noop);
    sound_method(lua, "stop", sound_noop);
    sound_method(lua, "pause", sound_noop);
    sound_method(lua, "isPlaying", sound_false);
    sound_method(lua, "load", sound_noop);
    sound_method(lua, "setStopOnUnderrun", sound_noop);
    sound_method(lua, "setVolume", sound_noop);
    sound_method(lua, "getVolume", sound_one);
    sound_method(lua, "setLoop", sound_noop);
    sound_method(lua, "isLooping", sound_false);
    sound_method(lua, "getLength", sound_zero);
    sound_method(lua, "setOffset", sound_noop);
    sound_method(lua, "getOffset", sound_zero);
    sound_method(lua, "setFinishCallback", sound_noop);
    sound_method(lua, "setRate", sound_noop);
    sound_method(lua, "getRate", sound_one);
    return 1;
}

static int player_set_sample(lua_State *lua)
{
    lua_pushvalue(lua, 2);
    lua_setfield(lua, 1, "_sample");
    return 0;
}
static int player_get_sample(lua_State *lua)
{
    lua_getfield(lua, 1, "_sample");
    return 1;
}
static int sampleplayer_new(lua_State *lua)
{
    lua_newtable(lua);
    sound_method(lua, "play", sound_noop);
    sound_method(lua, "stop", sound_noop);
    sound_method(lua, "isPlaying", sound_false);
    sound_method(lua, "setVolume", sound_noop);
    sound_method(lua, "getVolume", sound_one);
    sound_method(lua, "setSample", player_set_sample);
    sound_method(lua, "getSample", player_get_sample);
    if (lua_istable(lua, 1)) {
        lua_pushvalue(lua, 1);
        lua_setfield(lua, -2, "_sample");
    }
    sound_method(lua, "setLoop", sound_noop);
    sound_method(lua, "setRate", sound_noop);
    sound_method(lua, "getRate", sound_one);
    sound_method(lua, "setFinishCallback", sound_noop);
    return 1;
}

/* SDK: playdate.sound.channel.new() — XTRIS creates three channels
 * at load (voice-overs, SFX, BGM) and calls addSource / setVolume
 * on them. Silent stub, like the fileplayer/sampleplayer ones: no
 * audio is decoded or played. */
static int channel_new(lua_State *lua)
{
    lua_newtable(lua);
    sound_method(lua, "addSource", sound_noop);
    sound_method(lua, "removeSource", sound_noop);
    sound_method(lua, "setVolume", sound_noop);
    sound_method(lua, "getVolume", sound_one);
    sound_method(lua, "setPan", sound_noop);
    sound_method(lua, "getPan", sound_zero);
    sound_method(lua, "addEffect", sound_noop);
    sound_method(lua, "removeEffect", sound_noop);
    sound_method(lua, "setVolumeModulator", sound_noop);
    sound_method(lua, "setPanModulator", sound_noop);
    return 1;
}

static int sample_rate(lua_State *lua)
{
    lua_getfield(lua, 1, "_rate");
    return 1;
}

static int sample_length(lua_State *lua)
{
    lua_getfield(lua, 1, "_length");
    return 1;
}

static void sample_fields(lua_State *lua, unsigned rate, double length)
{
    lua_newtable(lua);
    lua_pushinteger(lua, rate);
    lua_setfield(lua, -2, "_rate");
    lua_pushnumber(lua, length);
    lua_setfield(lua, -2, "_length");
    sound_method(lua, "getLength", sample_length);
    sound_method(lua, "getSampleRate", sample_rate);
}

int runtime_push_sample(lua_State *lua, const unsigned char *data, size_t length)
{
    if (length < 4) return 0;
    unsigned rate = data[0] | (unsigned)data[1] << 8 | (unsigned)data[2] << 16;
    unsigned format = data[3];
    if (!rate || rate > 192000 || format > 5) return 0;
    size_t bytes = length - 4;
    unsigned channels = (format & 1) ? 2 : 1;
    double frames = 0;
    if (format < 4) {
        frames = bytes / (channels * (format >= 2 ? 2 : 1));
    } else if (bytes >= 2) {
        unsigned block = data[4] | (unsigned)data[5] << 8;
        if (block <= 4 * channels) return 0;
        size_t rest = bytes - 2;
        size_t full = rest / block, tail = rest % block;
        frames = full * (1 + (block - 4 * channels) * 2 / channels);
        if (tail >= 4 * channels) frames += 1 + (tail - 4 * channels) * 2 / channels;
    }
    sample_fields(lua, rate, frames / rate);
    return 1;
}

static int sample_new(lua_State *lua)
{
    sample_fields(lua, 0, 0);
    return 1;
}

/* --- playdate-level API stubs --- */

static int menu_noop(lua_State *lua)
{
    (void)lua;
    return 0;
}

static int menu_get_items(lua_State *lua)
{
    /* Items are accepted and ignored (see get_system_menu), so the
     * list is always empty. */
    lua_newtable(lua);
    return 1;
}

static int menu_item(lua_State *lua)
{
    lua_newtable(lua);
    sound_method(lua, "setTitle", sound_noop);
    sound_method(lua, "setCallback", sound_noop);
    sound_method(lua, "setValue", sound_noop);
    sound_method(lua, "getValue", sound_false);
    return 1;
}

static int get_system_menu(lua_State *lua)
{
    /* The Q2 has no Playdate system menu; the menu button opens the
     * runner's own menu instead. Items are accepted and ignored. */
    lua_newtable(lua);
    lua_pushcfunction(lua, menu_item);
    lua_setfield(lua, -2, "addMenuItem");
    lua_pushcfunction(lua, menu_item);
    lua_setfield(lua, -2, "addCheckmarkMenuItem");
    lua_pushcfunction(lua, menu_item);
    lua_setfield(lua, -2, "addOptionsMenuItem");
    lua_pushcfunction(lua, menu_noop);
    lua_setfield(lua, -2, "removeMenuItem");
    lua_pushcfunction(lua, menu_noop);
    lua_setfield(lua, -2, "removeAllMenuItems");
    lua_pushcfunction(lua, menu_get_items);
    lua_setfield(lua, -2, "getMenuItems");
    return 1;
}

static int get_time(lua_State *lua)
{
    time_t now = time(NULL);
    struct tm broken;
    localtime_r(&now, &broken);
    struct timespec precise;
    clock_gettime(CLOCK_REALTIME, &precise);
    lua_newtable(lua);
    lua_pushinteger(lua, broken.tm_year + 1900);
    lua_setfield(lua, -2, "year");
    lua_pushinteger(lua, broken.tm_mon + 1);
    lua_setfield(lua, -2, "month");
    lua_pushinteger(lua, broken.tm_mday);
    lua_setfield(lua, -2, "day");
    lua_pushinteger(lua, broken.tm_hour);
    lua_setfield(lua, -2, "hour");
    lua_pushinteger(lua, broken.tm_min);
    lua_setfield(lua, -2, "minute");
    lua_pushinteger(lua, broken.tm_sec);
    lua_setfield(lua, -2, "second");
    /* SDK: getTime() also has millisecond. XTRIS's log timestamp
     * concatenates it, so a missing field crashes the game at load. */
    lua_pushinteger(lua, precise.tv_nsec / 1000000);
    lua_setfield(lua, -2, "millisecond");
    lua_pushinteger(lua, broken.tm_wday + 1);
    lua_setfield(lua, -2, "weekday");
    lua_pushinteger(lua, broken.tm_yday + 1);
    lua_setfield(lua, -2, "yearday");
    return 1;
}

static int epoch_from_time(lua_State *lua)
{
    luaL_checktype(lua, 1, LUA_TTABLE);
    struct tm broken;
    memset(&broken, 0, sizeof broken);
    lua_getfield(lua, 1, "year");
    broken.tm_year = (int)lua_tointeger(lua, -1) - 1900;
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "month");
    broken.tm_mon = (int)lua_tointeger(lua, -1) - 1;
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "day");
    broken.tm_mday = (int)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "hour");
    broken.tm_hour = (int)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "minute");
    broken.tm_min = (int)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "second");
    broken.tm_sec = (int)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    time_t value = mktime(&broken);
    /* Playdate epoch: 2000-01-01 00:00:00 UTC. */
    lua_pushinteger(lua, (lua_Integer)(value - 946684800));
    return 1;
}

static int seconds_since_epoch(lua_State *lua)
{
    lua_pushinteger(lua, (lua_Integer)(time(NULL) - 946684800));
    return 1;
}

static int get_elapsed_time(lua_State *lua)
{
    lua_pushnumber(lua, (lua_Number)(game_ms - elapsed_base) / 1000.0);
    return 1;
}

static int reset_elapsed_time(lua_State *lua)
{
    (void)lua;
    elapsed_base = game_ms;
    return 0;
}

static int get_fps(lua_State *lua)
{
    lua_pushinteger(lua, 30);
    return 1;
}

static int draw_fps_top(lua_State *lua)
{
    (void)lua;
    return 0;
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
    "-- Sprites. Methods live on the shared pd.graphics.sprite table so\n"
    "-- class(\"X\").extends(playdate.graphics.sprite) inherits them, and\n"
    "-- sprite.new() instances get the same table as their metatable.\n"
    "-- Collision is a stub: checkCollisions reports overlaps only, and\n"
    "-- moveWithCollisions just moves (no slide/freeze/bounce). The real\n"
    "-- per-frame query, querySpritesInRect, is implemented for real\n"
    "-- because Celeste's physics (is_solid/is_ice) uses it.\n"
    "local sprites = {}\n"
    "local seq = 0\n"
    "pd.graphics.sprite = {}\n"
    "pd.graphics.sprite.__index = pd.graphics.sprite\n"
    "local spritelib = pd.graphics.sprite\n"
    "function spritelib.new(imageOrTilemap)\n"
    "  local sprite = {x = 0, y = 0, width = 0, height = 0,\n"
    "    centerX = 0.5, centerY = 0.5, zIndex = 0, visible = true,\n"
    "    updatesEnabled = true, ignoresDrawOffset = false,\n"
    "    image = nil, imagetable = nil, tilemap = nil, imageIndex = 1,\n"
    "    flip = 0, drawMode = 0, collideRect = nil,\n"
    "    groups = nil, collidesWith = nil, collisionsEnabled = true}\n"
    "  setmetatable(sprite, {__index = spritelib})\n"
    "  sprite:init(imageOrTilemap)\n"
    "  return sprite\n"
    "end\n"
    "function spritelib:init(imageOrTilemap)\n"
    "  if imageOrTilemap ~= nil then\n"
    "    if getmetatable(imageOrTilemap) == playdate.graphics.image then\n"
    "      self:setImage(imageOrTilemap)\n"
    "    elseif getmetatable(imageOrTilemap) == playdate.graphics.tilemap then\n"
    "      self:setTilemap(imageOrTilemap)\n"
    "    end\n"
    "  end\n"
    "  self:moveTo(0, 0)\n"
    "end\n"
    "function spritelib:moveTo(x, y) self.x, self.y = x, y end\n"
    "function spritelib:moveBy(dx, dy) self.x, self.y = self.x + dx, self.y + dy end\n"
    "function spritelib:getPosition() return self.x, self.y end\n"
    "function spritelib:setSize(w, h)\n"
    "  if self.image == nil and self.imagetable == nil and self.tilemap == nil then\n"
    "    self.width, self.height = w, h\n"
    "  end\n"
    "end\n"
    "function spritelib:getSize() return self.width, self.height end\n"
    "function spritelib:setBounds(x, y, w, h)\n"
    "  if self.image == nil and self.imagetable == nil and self.tilemap == nil then\n"
    "    self.width, self.height = w, h\n"
    "  end\n"
    "  self:moveTo(x + self.centerX * self.width, y + self.centerY * self.height)\n"
    "end\n"
    "function spritelib:getBounds()\n"
    "  return self.x - self.centerX * self.width,\n"
    "    self.y - self.centerY * self.height, self.width, self.height\n"
    "end\n"
    "function spritelib:getBoundsRect()\n"
    "  return playdate.geometry.rect.new(self:getBounds())\n"
    "end\n"
    "function spritelib:setCenter(x, y) self.centerX, self.centerY = x, y end\n"
    "function spritelib:getCenter() return self.centerX, self.centerY end\n"
    "function spritelib:setZIndex(z) self.zIndex = z end\n"
    "function spritelib:getZIndex() return self.zIndex end\n"
    "function spritelib:setVisible(v) self.visible = v end\n"
    "function spritelib:isVisible() return self.visible end\n"
    "function spritelib:hide() self.visible = false end\n"
    "function spritelib:show() self.visible = true end\n"
    "function spritelib:setUpdatesEnabled(v) self.updatesEnabled = v end\n"
    "function spritelib:updatesEnabled() return self.updatesEnabled end\n"
    "function spritelib:setIgnoresDrawOffset(v) self.ignoresDrawOffset = v end\n"
    "function spritelib:getIgnoresDrawOffset() return self.ignoresDrawOffset end\n"
    "function spritelib:setImage(image, flip, scale, yscale)\n"
    "  self.image = image\n"
    "  self.imagetable = nil\n"
    "  self.tilemap = nil\n"
    "  self.flip = flip or 0\n"
    "  if image ~= nil then\n"
    "    local w, h = image:getSize()\n"
    "    if scale ~= nil then\n"
    "      w = w * scale\n"
    "      h = h * (yscale or scale)\n"
    "    end\n"
    "    self.width, self.height = w, h\n"
    "  end\n"
    "end\n"
    "function spritelib:getImage() return self.image end\n"
    "function spritelib:setImageTable(table, imageIndex)\n"
    "  self.imagetable = table\n"
    "  self.image = nil\n"
    "  self.tilemap = nil\n"
    "  self.imageIndex = imageIndex or self.imageIndex or 1\n"
    "  if table ~= nil then\n"
    "    local first = table:getImage(1)\n"
    "    if first ~= nil then\n"
    "      local w, h = first:getSize()\n"
    "      self.width, self.height = w, h\n"
    "    end\n"
    "  end\n"
    "end\n"
    "function spritelib:getImageTable() return self.imagetable end\n"
    "function spritelib:setImageIndex(index)\n"
    "  self.imageIndex = index\n"
    "  if self.imagetable ~= nil then\n"
    "    self.image = self.imagetable:getImage(index)\n"
    "  end\n"
    "end\n"
    "function spritelib:getImageIndex() return self.imageIndex end\n"
    "function spritelib:setTilemap(tilemap)\n"
    "  self.tilemap = tilemap\n"
    "  self.image = nil\n"
    "  self.imagetable = nil\n"
    "  if tilemap ~= nil then\n"
    "    local w, h = tilemap:getPixelSize()\n"
    "    self.width, self.height = w, h\n"
    "  end\n"
    "end\n"
    "function spritelib:getTilemap() return self.tilemap end\n"
    "function spritelib:setImageDrawMode(mode) self.drawMode = mode end\n"
    "function spritelib:getImageDrawMode() return self.drawMode end\n"
    "function spritelib:setCollideRect(x, y, w, h)\n"
    "  if type(x) == \"table\" or type(x) == \"userdata\" then\n"
    "    -- The SDK also accepts a rect (or a plain {x,y,width,height}\n"
    "    -- table); Celeste passes hitbox rects directly. Normalize to\n"
    "    -- a plain table so the collision helpers only see numbers.\n"
    "    self.collideRect = {x = x.x, y = x.y, width = x.width, height = x.height}\n"
    "  else\n"
    "    self.collideRect = {x = x, y = y, width = w, height = h}\n"
    "  end\n"
    "end\n"
    "function spritelib:getCollideRect()\n"
    "  local r = self.collideRect\n"
    "  if r == nil then return 0, 0, self.width, self.height end\n"
    "  return r.x, r.y, r.width, r.height\n"
    "end\n"
    "function spritelib:clearCollideRect() self.collideRect = nil end\n"
    "function spritelib:setGroups(groups) self.groups = groups end\n"
    "function spritelib:getGroups() return self.groups end\n"
    "function spritelib:setCollidesWithGroups(groups) self.collidesWith = groups end\n"
    "function spritelib:getCollidesWithGroups() return self.collidesWith end\n"
    "function spritelib:setCollisionsEnabled(v) self.collisionsEnabled = v end\n"
    "function spritelib:collisionsEnabled() return self.collisionsEnabled end\n"
    "function spritelib:markDirty() end\n"
    "-- addDirtyRect is a no-op here: the player redraws the whole frame\n"
    "-- every update, so there is no dirty-rect list to maintain. XTRIS's\n"
    "-- title page calls self:addDirtyRect(34, 190, 17, 17) from update()\n"
    "-- and fails without it. Since spritelib IS pd.graphics.sprite, this\n"
    "-- also covers the module form sprite.addDirtyRect(x, y, w, h).\n"
    "function spritelib:addDirtyRect(...) end\n"
    "function spritelib:setRedrawOnImageChange(v) end\n"
    "function spritelib:setOpaque(v) end\n"
    "function spritelib:setAnimator(animator, moveWithCollisions, removeOnCollision) end\n"
    "function spritelib:removeAnimator() end\n"
    "function spritelib:copy()\n"
    "  local copy = spritelib.new()\n"
    "  for k, v in pairs(self) do copy[k] = v end\n"
    "  setmetatable(copy, getmetatable(self))\n"
    "  return copy\n"
    "end\n"
    "function spritelib:add()\n"
    "  if not sprites[self] then\n"
    "    seq = seq + 1\n"
    "    self._seq = seq\n"
    "    sprites[self] = true\n"
    "  end\n"
    "end\n"
    "function spritelib:remove() sprites[self] = nil end\n"
    "function spritelib.addSprite(sprite) sprite:add() end\n"
    "function spritelib.removeSprite(sprite) sprite:remove() end\n"
    "function spritelib.removeAll()\n"
    "  for sprite in iter, sprites do sprites[sprite] = nil end\n"
    "end\n"
    "function spritelib.getAllSprites()\n"
    "  local all = {}\n"
    "  for sprite in iter, sprites do all[#all + 1] = sprite end\n"
    "  return all\n"
    "end\n"
    "function spritelib.performOnAllSprites(func)\n"
    "  for _, sprite in ipairs(spritelib.getAllSprites()) do func(sprite) end\n"
    "end\n"
    "local function collideRectOf(sprite)\n"
    "  local bx, by, bw, bh = sprite:getBounds()\n"
    "  local r = sprite.collideRect\n"
    "  if r == nil then return bx, by, bw, bh end\n"
    "  return bx + r.x, by + r.y, r.width, r.height\n"
    "end\n"
    "local function overlaps(a, b)\n"
    "  local ax, ay, aw, ah = collideRectOf(a)\n"
    "  local bx, by, bw, bh = collideRectOf(b)\n"
    "  return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah\n"
    "end\n"
    "local function groupMatch(a, b)\n"
    "  if a.collisionsEnabled == false or b.collisionsEnabled == false then return false end\n"
    "  local function has(list, group)\n"
    "    if list == nil then return true end\n"
    "    for _, g in ipairs(list) do if g == group then return true end end\n"
    "    return false\n"
    "  end\n"
    "  local function anyCommon(listA, listB)\n"
    "    if listA == nil or listB == nil then return true end\n"
    "    for _, g in ipairs(listA) do if has(listB, g) then return true end end\n"
    "    return false\n"
    "  end\n"
    "  return anyCommon(a.collidesWith, b.groups) and anyCommon(b.collidesWith, a.groups)\n"
    "end\n"
    "function spritelib.querySpritesInRect(x, y, w, h)\n"
    "  if type(x) == \"table\" or type(x) == \"userdata\" then\n"
    "    -- The SDK also accepts a rect; Celeste passes one.\n"
    "    local r = x\n"
    "    x, y, w, h = r.x, r.y, r.width, r.height\n"
    "  end\n"
    "  local found = {}\n"
    "  for sprite in iter, sprites do\n"
    "    if sprite.visible ~= false then\n"
    "      local sx, sy, sw, sh = collideRectOf(sprite)\n"
    "      if sx < x + w and x < sx + sw and sy < y + h and y < sy + sh then\n"
    "        found[#found + 1] = sprite\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return found\n"
    "end\n"
    "function spritelib.querySpritesAlongLine(x1, y1, x2, y2)\n"
    "  local found = {}\n"
    "  for sprite in iter, sprites do\n"
    "    if sprite.visible ~= false then\n"
    "      local sx, sy, sw, sh = collideRectOf(sprite)\n"
    "      local minx, maxx = math.min(x1, x2), math.max(x1, x2)\n"
    "      local miny, maxy = math.min(y1, y2), math.max(y1, y2)\n"
    "      if sx < maxx and minx < sx + sw and sy < maxy and miny < sy + sh then\n"
    "        found[#found + 1] = sprite\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return found\n"
    "end\n"
    "function spritelib:checkCollisions(x, y)\n"
    "  local collisions = {}\n"
    "  local oldX, oldY = self.x, self.y\n"
    "  self.x, self.y = x, y\n"
    "  for other in iter, sprites do\n"
    "    if other ~= self and other.visible ~= false and groupMatch(self, other)\n"
    "      and overlaps(self, other) then\n"
    "      local ax, ay, aw, ah = collideRectOf(self)\n"
    "      local bx, by, bw, bh = collideRectOf(other)\n"
    "      collisions[#collisions + 1] = {sprite = self, other = other,\n"
    "        type = \"overlap\", overlaps = true, ti = 0,\n"
    "        spriteRect = playdate.geometry.rect.new(ax, ay, aw, ah),\n"
    "        otherRect = playdate.geometry.rect.new(bx, by, bw, bh)}\n"
    "    end\n"
    "  end\n"
    "  self.x, self.y = oldX, oldY\n"
    "  return x, y, collisions, #collisions\n"
    "end\n"
    "function spritelib:moveWithCollisions(x, y)\n"
    "  local actualX, actualY, collisions, count = self:checkCollisions(x, y)\n"
    "  self:moveTo(actualX, actualY)\n"
    "  return actualX, actualY, collisions, count\n"
    "end\n"
    "-- Default draw: blit the sprite's image (or its imagetable's current\n"
    "-- image, or its tilemap) at the bounds origin. A game that defines\n"
    "-- its own draw on a subclass overrides this.\n"
    "function spritelib:draw(x, y, w, h)\n"
    "  local gfx = playdate.graphics\n"
    "  local image = self.image\n"
    "  if image == nil and self.imagetable ~= nil then\n"
    "    image = self.imagetable:getImage(self.imageIndex or 1)\n"
    "  end\n"
    "  if image ~= nil then\n"
    "    local mode = gfx.getImageDrawMode()\n"
    "    if self.drawMode ~= nil and self.drawMode ~= 0 then\n"
    "      gfx.setImageDrawMode(self.drawMode)\n"
    "    end\n"
    "    if self.ignoresDrawOffset then\n"
    "      image:drawIgnoringOffset(x, y, self.flip or 0)\n"
    "    else\n"
    "      image:draw(x, y, self.flip or 0)\n"
    "    end\n"
    "    if self.drawMode ~= nil and self.drawMode ~= 0 then\n"
    "      gfx.setImageDrawMode(mode)\n"
    "    end\n"
    "  elseif self.tilemap ~= nil then\n"
    "    self.tilemap:draw(x, y)\n"
    "  end\n"
    "end\n"
    "function spritelib:update() end\n"
    "local updating = false\n"
    "function spritelib.update()\n"
    "  if updating then return end\n"
    "  updating = true\n"
    "  local current = {}\n"
    "  for sprite in iter, sprites do current[#current + 1] = sprite end\n"
    "  table.sort(current, function(a, b)\n"
    "    if a.zIndex ~= b.zIndex then return a.zIndex < b.zIndex end\n"
    "    return (a._seq or 0) < (b._seq or 0)\n"
    "  end)\n"
    "  for i = 1, #current do\n"
    "    local sprite = current[i]\n"
    "    if sprites[sprite] and sprite.updatesEnabled ~= false and sprite.update then\n"
    "      sprite:update()\n"
    "    end\n"
    "  end\n"
    "  for i = 1, #current do\n"
    "    local sprite = current[i]\n"
    "    if sprites[sprite] and sprite.visible ~= false and sprite.draw then\n"
    "      local bx, by, bw, bh = sprite:getBounds()\n"
    "      sprite:draw(bx, by, bw, bh)\n"
    "    end\n"
    "  end\n"
    "  updating = false\n"
    "end\n";

void runtime_install(lua_State *lua)
{
    static const luaL_Reg datastore[] = {
        {"read", datastore_read}, {"write", datastore_write},
        {"delete", datastore_delete}, {NULL, NULL}
    };
    luaL_newlib(lua, datastore);
    lua_setfield(lua, -2, "datastore");

    file_install(lua);

    lua_newtable(lua);
    lua_pushinteger(lua, 0);
    lua_setfield(lua, -2, "kWaveSquare");
    lua_newtable(lua);
    lua_pushcfunction(lua, synth_new);
    lua_setfield(lua, -2, "new");
    lua_setfield(lua, -2, "synth");
    lua_newtable(lua);
    lua_pushcfunction(lua, fileplayer_new);
    lua_setfield(lua, -2, "new");
    lua_setfield(lua, -2, "fileplayer");
    lua_newtable(lua);
    lua_pushcfunction(lua, sampleplayer_new);
    lua_setfield(lua, -2, "new");
    lua_setfield(lua, -2, "sampleplayer");
    lua_newtable(lua);
    lua_pushcfunction(lua, channel_new);
    lua_setfield(lua, -2, "new");
    lua_setfield(lua, -2, "channel");
    lua_newtable(lua);
    lua_pushcfunction(lua, sample_new);
    lua_setfield(lua, -2, "new");
    lua_setfield(lua, -2, "sample");
    lua_setfield(lua, -2, "sound");

    /* playdate-level API used by games at load time. */
    lua_pushcfunction(lua, get_system_menu);
    lua_setfield(lua, -2, "getSystemMenu");
    lua_pushcfunction(lua, get_time);
    lua_setfield(lua, -2, "getTime");
    lua_pushcfunction(lua, epoch_from_time);
    lua_setfield(lua, -2, "epochFromTime");
    lua_pushcfunction(lua, elapsed_ms);
    lua_setfield(lua, -2, "getCurrentTimeMilliseconds");
    lua_pushcfunction(lua, seconds_since_epoch);
    lua_setfield(lua, -2, "getSecondsSinceEpoch");
    lua_pushcfunction(lua, sound_noop);
    lua_setfield(lua, -2, "setCrankSoundsDisabled");
    lua_pushcfunction(lua, sound_zero);
    lua_setfield(lua, -2, "getCrankPosition");
    lua_pushcfunction(lua, sound_false);
    lua_setfield(lua, -2, "isCrankDocked");
    lua_pushcfunction(lua, sound_false);
    lua_setfield(lua, -2, "getReduceFlashing");
    lua_pushstring(lua, "en");
    lua_setfield(lua, -2, "getSystemLanguage");
    lua_pushcfunction(lua, get_elapsed_time);
    lua_setfield(lua, -2, "getElapsedTime");
    lua_pushcfunction(lua, reset_elapsed_time);
    lua_setfield(lua, -2, "resetElapsedTime");
    lua_pushcfunction(lua, get_fps);
    lua_setfield(lua, -2, "getFPS");
    lua_pushcfunction(lua, sound_false);
    lua_setfield(lua, -2, "getFlipped");
    lua_pushboolean(lua, 0);
    lua_setfield(lua, -2, "isSimulator");
    lua_pushcfunction(lua, sound_noop);
    lua_setfield(lua, -2, "setMenuImage");
    lua_pushcfunction(lua, draw_fps_top);
    lua_setfield(lua, -2, "drawFPS");
    lua_newtable(lua);
    lua_pushcfunction(lua, sound_noop);
    lua_setfield(lua, -2, "push");
    lua_pushcfunction(lua, sound_noop);
    lua_setfield(lua, -2, "pop");
    lua_setfield(lua, -2, "inputHandlers");

    if (luaL_loadbuffer(lua, runtime_lua, sizeof runtime_lua - 1, "=q2-runtime") == LUA_OK) {
        lua_pushvalue(lua, -2);
        lua_pushcfunction(lua, elapsed_ms);
        lua_pushcfunction(lua, next_sprite);
        if (lua_pcall(lua, 3, 0, 0) != LUA_OK) {
            fprintf(stderr, "Q2 Lua runtime: %s\n", lua_tostring(lua, -1));
            lua_pop(lua, 1);
        }
    } else {
        fprintf(stderr, "Q2 Lua runtime: %s\n", lua_tostring(lua, -1));
        lua_pop(lua, 1);
    }

    /* json: the SDK's global json library (src/json.lua). XTRIS's
     * achievements module loads/saves Achievements.json with
     * decodeFile/encodeToFile, which read/write through playdate.file. */
    if (luaL_loadbuffer(lua, json_lua, sizeof json_lua - 1, "=q2-json") == LUA_OK) {
        lua_pushvalue(lua, -2);
        if (lua_pcall(lua, 1, 0, 0) != LUA_OK) {
            fprintf(stderr, "Q2 Lua json: %s\n", lua_tostring(lua, -1));
            lua_pop(lua, 1);
        }
    } else {
        fprintf(stderr, "Q2 Lua json: %s\n", lua_tostring(lua, -1));
        lua_pop(lua, 1);
    }
}
