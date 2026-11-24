#define _POSIX_C_SOURCE 200809L

#include "card.h"
#include "bundle.h"
#include "picker.h"
#include "runtime.h"
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
    COLOR_BLACK = 0, COLOR_WHITE = 1,
    BACK_KEY = 170, PLAY_KEY = 171, CENTRE_KEY = 218,
    LEFT = 1, RIGHT = 2, UP = 4, DOWN = 8, B = 16, A = 32
};

static uint8_t frame[SOURCE_HEIGHT][SOURCE_WIDTH];
static int color = 1;
static double crank_change;
static unsigned physical, touch, pressed, released;

static void set_buttons(unsigned *source, unsigned state)
{
    unsigned before = physical | touch;
    *source = state;
    unsigned after = physical | touch;
    pressed |= after & ~before;
    released |= before & ~after;
}

static int button_pressed(lua_State *lua)
{
    unsigned mask = (unsigned)luaL_checkinteger(lua, 1);
    lua_pushboolean(lua, ((physical | touch) & mask) != 0);
    return 1;
}

static int button_just_pressed(lua_State *lua)
{
    lua_pushboolean(lua, (pressed & (unsigned)luaL_checkinteger(lua, 1)) != 0);
    return 1;
}

static int button_just_released(lua_State *lua)
{
    lua_pushboolean(lua, (released & (unsigned)luaL_checkinteger(lua, 1)) != 0);
    return 1;
}

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

static void card_lua_init(lua_State *lua)
{
    static const luaL_Reg graphics[] = {
        {"clear", clear_frame}, {"setColor", set_color}, {"fillRect", fill_rect},
        {NULL, NULL}
    };
    static const char *names[] = {
        "kButtonLeft", "kButtonRight", "kButtonUp", "kButtonDown", "kButtonB", "kButtonA"
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        lua_pushinteger(lua, 1u << i);
        lua_setfield(lua, -2, names[i]);
    }
    lua_pushcfunction(lua, button_pressed);
    lua_setfield(lua, -2, "buttonIsPressed");
    lua_pushcfunction(lua, button_just_pressed);
    lua_setfield(lua, -2, "buttonJustPressed");
    lua_pushcfunction(lua, button_just_released);
    lua_setfield(lua, -2, "buttonJustReleased");
    lua_pushcfunction(lua, get_crank_change);
    lua_setfield(lua, -2, "getCrankChange");
    luaL_newlib(lua, graphics);
    lua_pushinteger(lua, COLOR_WHITE);
    lua_setfield(lua, -2, "kColorWhite");
    lua_pushinteger(lua, COLOR_BLACK);
    lua_setfield(lua, -2, "kColorBlack");
    lua_setfield(lua, -2, "graphics");
    runtime_install(lua);
}

static void present(uint32_t *pixels, size_t stride, uint8_t picture[SOURCE_HEIGHT][SOURCE_WIDTH])
{
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        uint32_t *row = pixels + x * stride;
        for (int y = 0; y < GAME_HEIGHT; y++) {
            row[GAME_TOP + y] = picture[y * 5 / 4][x * 5 / 4] ? 0 : 0xffffff;
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

static void show_game_controls(uint32_t *pixels, size_t stride)
{
    /* Menu and B sit in the black bands outside the Playdate picture. */
    for (int x = 220; x < 298; x++)
        for (int y = 28; y < 72; y++) {
            int line = y - 39;
            if (x >= 237 && x < 281 && line >= 0 && line < 24 && line % 9 < 3)
                pixels[x * stride + y] = 0xffffff;
        }
    for (int y = 304; y < 349; y++)
        for (int x = 220; x < 298; x++) {
            int cx = (x - 246) / 4;
            int cy = (y - 313) / 4;
            static const unsigned glyph[] = {6, 5, 6, 5, 6};
            unsigned ink = x >= 246 && cx < 3 && y >= 313 && cy < 5 &&
                           (glyph[cy] & (4u >> cx));
            pixels[x * stride + y] = ink ? 0xffffff : 0;
        }
}

static unsigned key_mask(int key)
{
    switch (key) {
    case BACK_KEY: return UP;
    case PLAY_KEY: return DOWN;
    case 172: case 222: return LEFT;
    case 173: case 223: return RIGHT;
    case CENTRE_KEY: return A;
    }
    return 0;
}

static void move_selection(Picker *picker, int direction)
{
    if (direction > 0 && picker->selected + 1 < picker->count)
        picker->selected++;
    if (direction < 0 && picker->selected > 0)
        picker->selected--;
}

static void report_run(unsigned frames, int64_t work, int64_t peak)
{
    FILE *log = fopen(CARD_ROOT "/last-run.txt", "w");
    if (!log)
        return;
    struct rusage usage = {0};
    getrusage(RUSAGE_SELF, &usage);
    fprintf(log, "Frames: %u\nMean update and render: %.2f ms\n"
            "Peak update and render: %lld ms\nPeak RSS: %ld KiB\n",
            frames, frames ? (double)work / frames : 0, (long long)peak, usage.ru_maxrss);
    fclose(log);
}

static void clear_screen(uint32_t *pixels, size_t stride)
{
    for (int x = 0; x < SCREEN_WIDTH; x++)
        memset(pixels + x * stride, 0, SCREEN_HEIGHT * sizeof *pixels);
}

static int run_library(int fd, uint32_t *pixels, size_t stride)
{
    static uint8_t menu_frame[SOURCE_HEIGHT][SOURCE_WIDTH];
    Picker picker = {0};
    picker_scan(&picker);
    char error[256] = "";
    lua_State *lua = NULL;
    int menu = 0, wheel = 0, touch_start = -1, result = 0;
    int64_t back_since = -1;
    unsigned frames = 0;
    int64_t work = 0, peak = 0;
    clear_screen(pixels, stride);

    for (;;) {
        int64_t start = now_ms();
        if (back_since >= 0 && start - back_since >= 700)
            break;
        if (lua && !menu) {
            lua_getglobal(lua, "playdate");
            lua_getfield(lua, -1, "update");
            if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
                const char *message = lua_tostring(lua, -1);
                snprintf(error, sizeof error, "%s", message ? message : "Lua update failed");
                report_run(frames, work, peak);
                runtime_silence();
                lua_close(lua);
                lua = NULL;
                clear_screen(pixels, stride);
            } else {
                lua_pop(lua, 1);
                runtime_tick();
                present(pixels, stride, frame);
                show_game_controls(pixels, stride);
                int64_t elapsed = now_ms() - start;
                work += elapsed;
                if (elapsed > peak)
                    peak = elapsed;
                frames++;
            }
            pressed = released = 0;
        }
        if (!lua || menu) {
            picker_draw(&picker, menu_frame, error, menu);
            present(pixels, stride, menu_frame);
        }

        int launch = 0, library = 0, quit = 0;
        while (now_ms() < start + 33 && !launch && !library && !quit) {
            struct pollfd input = {.fd = fd, .events = POLLIN};
            int64_t remaining = start + 33 - now_ms();
            int ready = poll(&input, 1, remaining > 0 ? (int)remaining : 0);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || (ready > 0 && !(input.revents & POLLIN))) {
                result = 1;
                quit = 1;
                break;
            }
            if (!ready)
                break;

            char message[64], type;
            int value, axis;
            ssize_t length = recv(fd, message, sizeof message - 1, 0);
            if (length <= 0)
                continue;
            message[length] = '\0';
            if (sscanf(message, "w %d", &value) == 1 && value >= -2000 && value <= 2000) {
                if (lua && !menu)
                    crank_change -= value * 1.8;
                else if (!lua && !error[0]) {
                    wheel -= value;
                    while (wheel >= 26 || wheel <= -26) {
                        int direction = wheel > 0 ? 1 : -1;
                        move_selection(&picker, direction);
                        wheel -= direction * 26;
                    }
                }
                continue;
            }

            if (sscanf(message, "k %c %d", &type, &value) == 2 &&
                (type == 'd' || type == 'u')) {
                if (value == BACK_KEY) {
                    if (type == 'd' && back_since < 0)
                        back_since = now_ms();
                    if (type == 'u') {
                        if (back_since >= 0 && now_ms() - back_since >= 700)
                            quit = 1;
                        back_since = -1;
                    }
                }
                if (lua && !menu) {
                    unsigned bit = key_mask(value);
                    set_buttons(&physical, type == 'd' ? physical | bit : physical & ~bit);
                } else if (type == 'u' && error[0]) {
                    if (value == BACK_KEY || value == CENTRE_KEY)
                        error[0] = '\0';
                } else if (type == 'u' && menu) {
                    if (value == BACK_KEY)
                        menu = 0;
                    if (value == CENTRE_KEY)
                        library = 1;
                } else if (type == 'u') {
                    unsigned bit = key_mask(value);
                    if (value == BACK_KEY)
                        quit = 1;
                    if (value == CENTRE_KEY)
                        launch = 1;
                    if (bit == LEFT || bit == RIGHT)
                        move_selection(&picker, bit == LEFT ? -1 : 1);
                }
                continue;
            }

            if (sscanf(message, "t %c %d %d", &type, &value, &axis) != 3 ||
                value < 0 || value >= SCREEN_HEIGHT || axis < 0 || axis >= SCREEN_WIDTH)
                continue;
            int x = SCREEN_WIDTH - 1 - axis, y = value;
            if (lua && !menu) {
                set_buttons(&touch, type != 'u' && x >= 220 && y >= 304 ? B : 0);
                if (type == 'u' && x >= 220 && y >= 28 && y < 72) {
                    menu = 1;
                    runtime_silence();
                    physical = touch = pressed = released = 0;
                    crank_change = 0;
                    clear_screen(pixels, stride);
                }
            } else if (type == 'd') {
                touch_start = x;
            } else if (type == 'u' && touch_start >= 0) {
                int dx = x - touch_start;
                touch_start = -1;
                int logical_y = (y - GAME_TOP) * 5 / 4;
                if (menu) {
                    if (logical_y >= 64 && logical_y < 100)
                        menu = 0;
                    else if (logical_y >= 100 && logical_y < 140)
                        library = 1;
                    else if (logical_y >= 140 && logical_y < 180)
                        quit = 1;
                } else if (!error[0] && (dx < -30 || dx > 30)) {
                    move_selection(&picker, dx < 0 ? 1 : -1);
                } else if (!error[0] && logical_y >= 42 && logical_y < 170) {
                    if (x >= 107 && x < 214)
                        launch = 1;
                    else
                        move_selection(&picker, x < 107 ? -1 : 1);
                }
            }
        }
        if (quit)
            break;
        if (library) {
            report_run(frames, work, peak);
            lua_close(lua);
            lua = NULL;
            menu = 0;
            clear_screen(pixels, stride);
        }
        if (launch) {
            char path[512];
            picker_path(&picker, path, sizeof path);
            physical = touch = pressed = released = 0;
            crank_change = 0;
            color = 1;
            frames = 0;
            work = peak = 0;
            memset(frame, 0, sizeof frame);
            runtime_configure(path);
            lua = bundle_open(path, card_lua_init, error, sizeof error);
            clear_screen(pixels, stride);
        }
    }

    if (lua) {
        report_run(frames, work, peak);
        lua_close(lua);
    }
    runtime_silence();
    return result;
}

int card_run(void)
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
        result = run_library(input, pixels, stride);
        close(input);
        unlink(INPUT_PATH);
    } else {
        perror(INPUT_PATH);
    }

    munmap(mapping, fix.smem_len);
    close(fb);
    return result;
}
