#define _POSIX_C_SOURCE 200809L

#include "card.h"
#include "bundle.h"
#include "picker.h"
#include "itch.h"
#include "runtime.h"
#include "lauxlib.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
    /* Coordinates within the centered game-width region. */
    MENU_X = 39 - GAME_LEFT, MENU_Y = GAME_TOP / 2 - 12,
    B_X = 62 - GAME_LEFT, B_Y = GAME_TOP + GAME_HEIGHT + GAME_TOP / 2 - 8,
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

static int draw_rect(lua_State *lua)
{
    int64_t x = luaL_checkinteger(lua, 1), y = luaL_checkinteger(lua, 2);
    int64_t w = luaL_checkinteger(lua, 3), h = luaL_checkinteger(lua, 4);
    if (w <= 0 || h <= 0)
        return 0;
    int left = clamp(x, SOURCE_WIDTH), right = clamp(x + w, SOURCE_WIDTH);
    int top = clamp(y, SOURCE_HEIGHT), bottom = clamp(y + h, SOURCE_HEIGHT);
    for (int row = top; row < bottom; row++) {
        if (row == y || row == y + h - 1)
            memset(frame[row] + left, color, right - left);
        else {
            if (x >= 0 && x < SOURCE_WIDTH)
                frame[row][x] = color;
            if (x + w - 1 >= 0 && x + w - 1 < SOURCE_WIDTH)
                frame[row][x + w - 1] = color;
        }
    }
    return 0;
}

static int draw_line(lua_State *lua)
{
    int xy[4];
    for (int i = 0; i < 4; i++) {
        lua_Integer value = luaL_checkinteger(lua, i + 1);
        if (value < -4096 || value > 4096)
            return luaL_argerror(lua, i + 1, "line coordinate out of range");
        xy[i] = (int)value;
    }
    int x = xy[0], y = xy[1], end_x = xy[2], end_y = xy[3];
    int dx = abs(end_x - x), dy = -abs(end_y - y);
    int sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        if ((unsigned)x < SOURCE_WIDTH && (unsigned)y < SOURCE_HEIGHT)
            frame[y][x] = color;
        if (x == end_x && y == end_y)
            break;
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x += sx;
        }
        if (twice <= dx) {
            error += dx;
            y += sy;
        }
    }
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
        {"drawLine", draw_line}, {"drawRect", draw_rect},
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

typedef struct {
    int fd;
    uint32_t *mapping;
    size_t stride;
    struct fb_var_screeninfo var;
    unsigned visible;
    unsigned page_rows;
    int portrait;
    int can_pan;
} Display;

static uint32_t screen_color(uint8_t picture[SOURCE_HEIGHT][SOURCE_WIDTH],
                             int x, int y, int game)
{
    x -= GAME_LEFT;
    if (x >= 0 && x < GAME_WIDTH && y >= GAME_TOP && y < GAME_TOP + GAME_HEIGHT)
        return picture[(y - GAME_TOP) * 5 / 4][x * 5 / 4] ? 0 : 0xffffff;
    if (game && x >= MENU_X && x < MENU_X + 44 && y >= MENU_Y && y < MENU_Y + 24 && (y - MENU_Y) % 9 < 3)
        return 0xffffff;
    if (game && x >= B_X && x < B_X + 12 && y >= B_Y && y < B_Y + 20) {
        static const unsigned glyph[] = {6, 5, 6, 5, 6};
        if (glyph[(y - B_Y) / 4] & (4u >> ((x - B_X) / 4)))
            return 0xffffff;
    }
    return 0;
}

static void display_frame(Display *display, uint8_t picture[SOURCE_HEIGHT][SOURCE_WIDTH], int game)
{
    size_t stride = display->stride;
    unsigned page = display->can_pan ? display->page_rows - display->visible : display->visible;
    uint32_t *pixels = display->mapping + page * stride;
    /* Portrait panel mapping: logical (x,y) -> (row=x,col=319-y). */
    unsigned columns = display->var.xres;
    for (unsigned row = 0; row < display->page_rows; row++) {
        uint32_t *line = pixels + row * stride;
        for (unsigned col = 0; col < columns; col++)
            line[col] = 0xff000000u | screen_color(picture,
                display->portrait ? row : col, display->portrait ? SCREEN_HEIGHT - 1 - col : row, game);
    }
    if (!display->can_pan)
        return;

    struct fb_var_screeninfo next = display->var;
    next.yoffset = page;
    if (ioctl(display->fd, FBIOPAN_DISPLAY, &next) == 0) {
        display->visible = page;
    } else {
        display->can_pan = 0;
        uint32_t *visible = display->mapping + display->visible * stride;
        for (unsigned row = 0; row < display->page_rows; row++)
            memcpy(visible + row * stride, pixels + row * stride, columns * sizeof *pixels);
    }
}

static void present_browser(void *context, uint8_t picture[PICKER_HEIGHT][PICKER_WIDTH])
{
    display_frame(context, picture, 0);
}

static int update_game(lua_State *lua)
{
    lua_getglobal(lua, "playdate");
    if (!lua_istable(lua, -1))
        return luaL_error(lua, "game replaced the playdate table");
    lua_getfield(lua, -1, "update");
    if (!lua_isfunction(lua, -1))
        return luaL_error(lua, "game did not define playdate.update");
    lua_call(lua, 0, 0);
    return 0;
}

static int run_library(int fd, Display *display)
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

    for (;;) {
        int64_t start = now_ms();
        if (back_since >= 0 && start - back_since >= 700)
            break;
        if (lua && !menu) {
            lua_pushcfunction(lua, update_game);
            if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
                const char *message = lua_tostring(lua, -1);
                snprintf(error, sizeof error, "%s", message ? message : "Lua update failed");
                report_run(frames, work, peak);
                runtime_silence();
                lua_close(lua);
                lua = NULL;
            } else {
                runtime_tick();
                display_frame(display, frame, 1);
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
            display_frame(display, menu_frame, 0);
        }

        int launch = 0, library = 0, quit = 0, get_games = 0;
        while (now_ms() < start + 33 && !launch && !library && !quit && !get_games) {
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
                    if (value == PLAY_KEY)
                        get_games = 1;
                    if (bit == LEFT || bit == RIGHT)
                        move_selection(&picker, bit == LEFT ? -1 : 1);
                }
                continue;
            }

            if (sscanf(message, "t %c %d %d", &type, &value, &axis) != 3 ||
                value < 0 || value >= SCREEN_WIDTH || axis < 0 || axis >= SCREEN_HEIGHT)
                continue;
            /* Firmware touch coordinates are landscape column and row. */
            int x = value - GAME_LEFT, y = axis;
            if (lua && !menu) {
                set_buttons(&touch, type != 'u' && x >= B_X - 10 && x < B_X + 22 && y >= B_Y - 10 && y < B_Y + 30 ? B : 0);
                if (type == 'u' && x >= MENU_X - 10 && x < MENU_X + 54 && y >= MENU_Y - 10 && y < MENU_Y + 34) {
                    menu = 1;
                    runtime_silence();
                    physical = touch = pressed = released = 0;
                    crank_change = 0;
                }
            } else if (type == 'd') {
                touch_start = x >= 0 && x < GAME_WIDTH ? x : -1;
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
                } else if (!error[0] && logical_y >= 205 && logical_y < 240 && x < 145) {
                    get_games = 1;
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
        }
        if (get_games) {
            itch_browse(fd, present_browser, display);
            picker_scan(&picker);
            if (picker.selected >= picker.count)
                picker.selected = picker.count - 1;
            wheel = 0;
            back_since = -1;
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
        ioctl(fb, FBIOGET_FSCREENINFO, &fix) < 0) {
        perror("read Q2 framebuffer");
        close(fb);
        return 1;
    }
    int portrait = var.xres == SCREEN_HEIGHT && var.yres == SCREEN_WIDTH;
    int landscape = var.xres == SCREEN_WIDTH && var.yres == SCREEN_HEIGHT;
    if ((!portrait && !landscape) || var.bits_per_pixel != 32 || var.xoffset != 0 ||
        fix.line_length % sizeof(uint32_t) ||
        fix.line_length < var.xres * sizeof(uint32_t) ||
        (uint64_t)var.yoffset + var.yres > fix.smem_len / fix.line_length) {
        fprintf(stderr, "Unsupported Q2 framebuffer layout (%ux%u, %u bpp, stride %u)\n",
                var.xres, var.yres, var.bits_per_pixel, fix.line_length);
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
        unsigned rows = fix.smem_len / fix.line_length;
        if (rows > var.yres_virtual)
            rows = var.yres_virtual;
        Display display = {
            .fd = fb, .mapping = mapping, .stride = fix.line_length / sizeof(uint32_t),
            .var = var, .visible = var.yoffset, .page_rows = var.yres, .portrait = portrait,
            .can_pan = rows >= 2 * var.yres &&
                       (var.yoffset == 0 || var.yoffset == var.yres),
        };
        result = run_library(input, &display);
        if (display.visible != var.yoffset)
            ioctl(fb, FBIOPAN_DISPLAY, &display.var);
        close(input);
        unlink(INPUT_PATH);
    } else {
        perror(INPUT_PATH);
    }

    munmap(mapping, fix.smem_len);
    close(fb);
    return result;
}
