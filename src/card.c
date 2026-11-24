#define _POSIX_C_SOURCE 200809L

#include "card.h"
#include "lauxlib.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define INPUT_PATH "/tmp/q2-playdate.input.sock"

enum {
    SOURCE_WIDTH = 400, SOURCE_HEIGHT = 240,
    SCREEN_WIDTH = 320, SCREEN_HEIGHT = 375,
    GAME_HEIGHT = 192, GAME_TOP = (SCREEN_HEIGHT - GAME_HEIGHT) / 2,
    COLOR_WHITE = 0, COLOR_BLACK = 1, BACK_KEY = 170
};

static uint8_t frame[SOURCE_HEIGHT][SOURCE_WIDTH];
static int color = COLOR_BLACK;
static double crank_change;

static int clear_frame(lua_State *lua)
{
    memset(frame, luaL_optinteger(lua, 1, COLOR_WHITE) == COLOR_BLACK, sizeof frame);
    return 0;
}

static int set_color(lua_State *lua)
{
    color = luaL_checkinteger(lua, 1) == COLOR_BLACK;
    return 0;
}

static int clamp(int64_t value, int limit)
{
    if (value < 0)
        return 0;
    return value > limit ? limit : (int)value;
}

static int fill_rect(lua_State *lua)
{
    int64_t x = luaL_checkinteger(lua, 1);
    int64_t y = luaL_checkinteger(lua, 2);
    int64_t width = luaL_checkinteger(lua, 3);
    int64_t height = luaL_checkinteger(lua, 4);
    int right = clamp(x + width, SOURCE_WIDTH);
    int bottom = clamp(y + height, SOURCE_HEIGHT);
    int left = clamp(x, SOURCE_WIDTH);
    int top = clamp(y, SOURCE_HEIGHT);
    if (right > left)
        for (int row = top; row < bottom; row++)
            memset(&frame[row][left], color, right - left);
    return 0;
}

static int get_crank_change(lua_State *lua)
{
    lua_pushnumber(lua, crank_change);
    crank_change = 0;
    return 1;
}

void card_lua_init(lua_State *lua)
{
    static const luaL_Reg graphics[] = {
        {"clear", clear_frame}, {"setColor", set_color}, {"fillRect", fill_rect},
        {NULL, NULL}
    };
    lua_pushcfunction(lua, get_crank_change);
    lua_setfield(lua, -2, "getCrankChange");
    luaL_newlib(lua, graphics);
    lua_pushinteger(lua, COLOR_WHITE);
    lua_setfield(lua, -2, "kColorWhite");
    lua_pushinteger(lua, COLOR_BLACK);
    lua_setfield(lua, -2, "kColorBlack");
    lua_setfield(lua, -2, "graphics");
}

static void present(uint32_t *pixels, size_t stride)
{
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        uint32_t *row = pixels + x * stride;
        for (int y = 0; y < GAME_HEIGHT; y++) {
            row[GAME_TOP + y] = frame[y * 5 / 4][x * 5 / 4] ? 0 : 0xffffff;
        }
    }
}

static int open_input(void)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = INPUT_PATH};
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    unlink(INPUT_PATH);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) == 0)
        return fd;
    close(fd);
    return -1;
}

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int frame_loop(lua_State *lua, int fd, uint32_t *pixels, size_t stride)
{
    lua_getglobal(lua, "playdate");
    unsigned frames = 0;
    int64_t work = 0, peak_work = 0;
    for (;;) {
        int64_t start = now_ms();
        int64_t deadline = start + 33;
        lua_getfield(lua, -1, "update");
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
            const char *error = lua_tostring(lua, -1);
            fprintf(stderr, "Playdate update failed: %s\n", error ? error : "Lua error");
            return 1;
        }
        present(pixels, stride);
        int64_t elapsed = now_ms() - start;
        work += elapsed;
        if (elapsed > peak_work)
            peak_work = elapsed;
        frames++;

        for (;;) {
            int64_t remaining = deadline - now_ms();
            if (remaining <= 0)
                break;
            struct pollfd input = {.fd = fd, .events = POLLIN};
            int ready = poll(&input, 1, (int)remaining);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || (ready > 0 && !(input.revents & POLLIN)))
                return 1;
            if (!ready)
                break;

            char message[64];
            ssize_t length = recv(fd, message, sizeof message - 1, 0);
            if (length <= 0)
                continue;
            message[length] = '\0';

            int value;
            if (sscanf(message, "k u %d", &value) == 1 && value == BACK_KEY) {
                FILE *log = fopen("/mnt/mmc/.playdate/last-run.txt", "w");
                if (log) {
                    struct rusage usage = {0};
                    getrusage(RUSAGE_SELF, &usage);
                    fprintf(log, "Frames: %u\nMean update and render: %.2f ms\n"
                            "Peak update and render: %lld ms\nPeak RSS: %ld KiB\n",
                            frames, (double)work / frames, (long long)peak_work, usage.ru_maxrss);
                    fclose(log);
                }
                return 0;
            }
            if (sscanf(message, "w %d", &value) == 1 && value >= -2000 && value <= 2000)
                crank_change -= value * 1.8;
        }
    }
}

int card_run(lua_State *lua)
{
    int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0) {
        perror("/dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fb, FBIOGET_FSCREENINFO, &fix) < 0 ||
        var.xres != SCREEN_HEIGHT || var.yres != SCREEN_WIDTH ||
        var.bits_per_pixel != 32 || var.xoffset != 0 ||
        fix.line_length % sizeof(uint32_t) ||
        fix.line_length < SCREEN_HEIGHT * sizeof(uint32_t) ||
        (uint64_t)var.yoffset + SCREEN_WIDTH > fix.smem_len / fix.line_length) {
        fprintf(stderr, "Unsupported Q2 framebuffer layout\n");
        close(fb);
        return 1;
    }

    uint32_t *mapping = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (mapping == MAP_FAILED) {
        perror("map Q2 framebuffer");
        close(fb);
        return 1;
    }

    int result = 1;
    int input = open_input();
    if (input >= 0) {
        size_t stride = fix.line_length / sizeof(uint32_t);
        uint32_t *pixels = mapping + var.yoffset * stride;
        for (int x = 0; x < SCREEN_WIDTH; x++)
            memset(pixels + x * stride, 0, SCREEN_HEIGHT * sizeof(uint32_t));
        result = frame_loop(lua, input, pixels, stride);
        close(input);
        unlink(INPUT_PATH);
    } else {
        perror(INPUT_PATH);
    }

    munmap(mapping, fix.smem_len);
    close(fb);
    return result;
}
