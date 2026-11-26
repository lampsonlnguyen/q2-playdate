#define _POSIX_C_SOURCE 200809L

#include "card.h"
#include "bundle.h"
#include "image.h"
#include "picker.h"
#include "itch.h"
#include "runtime.h"
#include "lauxlib.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
/* Logical framebuffer: the surface games draw into. Its size is
 * 400/scale x 240/scale (400x240 at scale 1); after each update it is
 * upscaled nearest-neighbour into `frame`, inverted to the frame's
 * 1=black convention. Byte values here follow the Playdate convention:
 * 1=white, 0=black. */
static uint8_t logical[SOURCE_HEIGHT][SOURCE_WIDTH];
static int logical_w = SOURCE_WIDTH, logical_h = SOURCE_HEIGHT;
static int display_scale = 1;
static int display_inverted;
static int refresh_rate = 30;

typedef struct GfxSaved {
    DrawTarget target;
    int color, draw_mode, offset_x, offset_y;
    int clip_x, clip_y, clip_w, clip_h;
    int line_width, stroke_location;
    int focus_ref;
    int full;
    /* Current draw pattern (see GfxContext). */
    uint8_t pattern[8], pattern_mask[8];
    int pattern_fg, pattern_bg_paints, pattern_on;
    int line_cap_style;
} GfxSaved;

typedef struct GfxContext {
    DrawTarget target;
    int color, draw_mode, offset_x, offset_y;
    int clip_x, clip_y, clip_w, clip_h;
    int line_width, stroke_location;
    int focus_ref;
    int background_color;
    int stencil_ref;
    /* Current draw pattern. setColor(), setPattern() and
     * setDitherPattern() are mutually exclusive (Inside Playdate 7.20):
     * setting one replaces the other, so only one of `color` and the
     * pattern is live at a time; `pattern_on` says which.
     * pattern[] holds the 8 row bytes of the 8x8 pattern (MSB-first,
     * bit 1 = foreground pixel); pattern_mask[] holds an 8-row alpha
     * mask (bit 1 = paint this pixel, 0 = leave it alone);
     * pattern_fg is the colour painted where the pattern bit is 1
     * (0 black / 1 white); pattern_bg_paints says whether a 0 bit
     * paints the opposite colour (setPattern: fills paint both black
     * and white pixels) or is transparent (setDitherPattern: only the
     * dithered pixels are drawn). */
    uint8_t pattern[8], pattern_mask[8];
    int pattern_fg, pattern_bg_paints, pattern_on;
    /* Line cap style for drawLine endpoints: 0 butt (flat), 1 square
     * (extend by half the line width), 2 round (semicircle caps).
     * SDK constants kLineCapStyleButt/Square/Round. */
    int line_cap_style;
    GfxSaved stack[8];
    int stack_top;
} GfxContext;

static GfxContext gfx;
static double crank_change;
static unsigned physical, touch, pressed, released;

/* The screen drawing target, rebuilt whenever the scale changes. */
static void gfx_use_screen(void)
{
    gfx.target.bits = &logical[0][0];
    gfx.target.mask = NULL;
    gfx.target.width = logical_w;
    gfx.target.height = logical_h;
    gfx.target.stride = SOURCE_WIDTH;
    gfx.target.packed = 0;
    gfx.target.screen = 1;
    gfx.focus_ref = LUA_NOREF;
}

/* Reset the graphics state (called on launch, when the previous Lua
 * state has been closed, so any stored registry refs are already dead). */
static void gfx_reset(void)
{
    gfx.stack_top = 0;
    gfx_use_screen();
    gfx.color = 1;
    gfx.draw_mode = 0;
    gfx.offset_x = gfx.offset_y = 0;
    gfx.clip_x = gfx.clip_y = 0;
    gfx.clip_w = gfx.clip_h = -1;
    gfx.line_width = 1;
    gfx.stroke_location = 1;
    gfx.background_color = 1;
    gfx.stencil_ref = LUA_NOREF;
    gfx.pattern_on = 0;
    gfx.line_cap_style = 0;
}

/* Current graphics state, for image.c's image/tilemap drawing. */
DrawTarget *gfx_target(void) { return &gfx.target; }
int gfx_draw_mode(void) { return gfx.draw_mode; }
void gfx_offset(int *x, int *y) { *x = gfx.offset_x; *y = gfx.offset_y; }
void gfx_clip(int *x, int *y, int *w, int *h)
{
    *x = gfx.clip_x;
    *y = gfx.clip_y;
    *w = gfx.clip_w;
    *h = gfx.clip_h;
}

/* Upscale the logical buffer into the physical frame (nearest neighbour,
 * inverted to the frame's 1=black convention). */
static void gfx_upscale(void)
{
    for (int fy = 0; fy < SOURCE_HEIGHT; fy++) {
        int ly = fy * logical_h / SOURCE_HEIGHT;
        for (int fx = 0; fx < SOURCE_WIDTH; fx++) {
            int lx = fx * logical_w / SOURCE_WIDTH;
            int white = logical[ly][lx] != 0;
            if (display_inverted)
                white = !white;
            frame[fy][fx] = (uint8_t)(white ? 0 : 1);
        }
    }
}

/* Plot one pixel in the current colour, applying the draw offset and
 * the clip rect. Coordinates are in the current target's space. */
static void gfx_put(int x, int y)
{
    DrawTarget *target = &gfx.target;
    if (target->screen) {
        x -= gfx.offset_x;
        y -= gfx.offset_y;
    }
    if (gfx.clip_w >= 0 && (x < gfx.clip_x || x >= gfx.clip_x + gfx.clip_w ||
                            y < gfx.clip_y || y >= gfx.clip_y + gfx.clip_h))
        return;
    if (x < 0 || y < 0 || x >= target->width || y >= target->height)
        return;
    if (gfx.pattern_on) {
        /* The pattern tiles every 8 pixels, anchored to the current
         * target's origin (x, y are already post-offset here). */
        int bit = 1 << (7 - (x & 7));
        if (!(gfx.pattern_mask[y & 7] & bit))
            return; /* transparent under the pattern */
        if (gfx.pattern[y & 7] & bit) {
            image_set_pixel(target, x, y, gfx.pattern_fg);
        } else if (gfx.pattern_bg_paints) {
            image_set_pixel(target, x, y, 1 - gfx.pattern_fg);
        }
        return;
    }
    if (gfx.color == 3) {
        int current = image_pixel(target, x, y);
        image_set_pixel(target, x, y, current < 0 ? 0 : 1 - current);
    } else {
        image_set_pixel(target, x, y, gfx.color);
    }
}

static int clamp(int64_t value, int limit)
{
    if (value < 0)
        return 0;
    return value > limit ? limit : (int)value;
}

static void set_buttons(unsigned *source, unsigned state)
{
    unsigned before = physical | touch;
    *source = state;
    unsigned after = physical | touch;
    pressed |= after & ~before;
    released |= before & ~after;
}

/* SDK: buttonIsPressed / buttonJustPressed / buttonJustReleased take a
 * kButton* constant or one of the strings "a", "b", "up", "down", "left",
 * "right" (Inside Playdate 6.9). Match case-insensitively: Frogger
 * passes "A" on the title screen but "UP" / "LEFT" / "RIGHT" / "DOWN"
 * (all caps) for movement, so a lower/Titlecase-only match silently
 * drops every direction press. An unknown string matches nothing (the
 * SDK fails silently on a typo'd string). */
static unsigned check_button(lua_State *lua, int index)
{
    /* NB: lua_isstring() is also true for numbers, so test the type
     * directly: a kButton* constant (an integer) must not fall into
     * the string branch, where lua_tostring() would turn 32 into "32"
     * and match nothing. That broke every game that passes constants
     * (Celeste reads buttons only via kButtonA etc., so its title
     * stopped responding to A entirely). */
    if (lua_type(lua, index) == LUA_TSTRING) {
        const char *name = lua_tostring(lua, index);
        static const struct {
            const char *name;
            unsigned mask;
        } buttons[] = {
            {"a", 32}, {"b", 16}, {"up", 4},
            {"down", 8}, {"left", 1}, {"right", 2},
        };
        for (size_t i = 0; i < sizeof buttons / sizeof *buttons; i++) {
            if (strcasecmp(name, buttons[i].name) == 0)
                return buttons[i].mask;
        }
        return 0;
    }
    return (unsigned)luaL_checkinteger(lua, index);
}

static int button_pressed(lua_State *lua)
{
    unsigned mask = check_button(lua, 1);
    lua_pushboolean(lua, ((physical | touch) & mask) != 0);
    return 1;
}

static int button_just_pressed(lua_State *lua)
{
    lua_pushboolean(lua, (pressed & check_button(lua, 1)) != 0);
    return 1;
}

static int button_just_released(lua_State *lua)
{
    lua_pushboolean(lua, (released & check_button(lua, 1)) != 0);
    return 1;
}

static int clear_frame(lua_State *lua)
{
    draw_target_clear(&gfx.target, luaL_optinteger(lua, 1, gfx.background_color));
    return 0;
}

static int set_color(lua_State *lua)
{
    int color = luaL_checkinteger(lua, 1);
    if (color < 0 || color > 3)
        return luaL_argerror(lua, 1, "invalid color");
    gfx.color = color;
    gfx.pattern_on = 0; /* setColor() replaces any pattern */
    return 0;
}

static int set_background_color(lua_State *lua)
{
    int color = luaL_checkinteger(lua, 1);
    if (color < 0 || color > 2)
        return luaL_argerror(lua, 1, "invalid color");
    gfx.background_color = color;
    return 0;
}

static int get_background_color(lua_State *lua)
{
    lua_pushinteger(lua, gfx.background_color);
    return 1;
}

/* One pixel of an image, as lua_image_get_pixel reports it: 0 black,
 * 1 white, 2 clear (outside the image, outside the trimmed bitmap, or
 * masked out). Used by the setPattern(image) form. */
static int pattern_image_pixel(const Image *image, int x, int y)
{
    if (x < 0 || y < 0 || x >= image->width || y >= image->height)
        return 2;
    int sx = x - image->xoff, sy = y - image->yoff;
    if (sx < 0 || sy < 0 || sx >= image->bw || sy >= image->bh)
        return 2;
    if (image->mask &&
        !((image->mask[sy * image->rowbytes + sx / 8] >> (7 - sx % 8)) & 1))
        return 2;
    int value = (image->bitmap[sy * image->rowbytes + sx / 8] >> (7 - sx % 8)) & 1;
    return image->inverted ? 1 - value : value;
}

/* SDK: playdate.graphics.setPattern(pattern) / setPattern(image, [x, y]).
 * The table form is 8 numbers, one row byte each (bit set = white, MSB
 * first); an optional second 8 numbers form an alpha mask. The image
 * form takes the 8x8 block at (x, y) of the image (white pixel = set
 * bit, anything else = clear bit, fully opaque mask). A set pattern
 * replaces the current colour. Frogger calls the table form twice per
 * frame (background band, timer bar). */
static int set_pattern(lua_State *lua)
{
    if (image_is(lua, 1)) {
        Image *image = image_check(lua, 1);
        int ox = (int)luaL_optinteger(lua, 2, 0);
        int oy = (int)luaL_optinteger(lua, 3, 0);
        for (int row = 0; row < 8; row++) {
            uint8_t bits = 0;
            for (int col = 0; col < 8; col++)
                if (pattern_image_pixel(image, ox + col, oy + row) == 1)
                    bits |= (uint8_t)(1 << (7 - col));
            gfx.pattern[row] = bits;
            gfx.pattern_mask[row] = 0xff;
        }
    } else {
        luaL_checktype(lua, 1, LUA_TTABLE);
        lua_Integer len = lua_rawlen(lua, 1);
        if (len < 8)
            return luaL_argerror(lua, 1, "pattern needs 8 numbers");
        for (int row = 0; row < 8; row++) {
            lua_rawgeti(lua, 1, row + 1);
            if (!lua_isinteger(lua, -1))
                return luaL_argerror(lua, 1, "pattern entries must be integers");
            gfx.pattern[row] = (uint8_t)(lua_tointeger(lua, -1) & 0xff);
            lua_pop(lua, 1);
            if (len >= 16) {
                lua_rawgeti(lua, 1, row + 9);
                if (!lua_isinteger(lua, -1))
                    return luaL_argerror(lua, 1, "pattern mask entries must be integers");
                gfx.pattern_mask[row] = (uint8_t)(lua_tointeger(lua, -1) & 0xff);
                lua_pop(lua, 1);
            } else {
                gfx.pattern_mask[row] = 0xff;
            }
        }
    }
    gfx.pattern_fg = 1; /* bit set = white, bit clear = black */
    gfx.pattern_bg_paints = 1;
    gfx.pattern_on = 1;
    return 0;
}

/* SDK: playdate.graphics.getPattern() — the current pattern as a table
 * of 8 row bytes, or nil when a solid colour is set. For a dither
 * pattern this is the computed 8x8 dither mask (bit 1 = drawn pixel). */
static int get_pattern(lua_State *lua)
{
    if (!gfx.pattern_on) {
        lua_pushnil(lua);
        return 1;
    }
    lua_newtable(lua);
    for (int row = 0; row < 8; row++) {
        lua_pushinteger(lua, gfx.pattern[row]);
        lua_rawseti(lua, -2, row + 1);
    }
    return 1;
}

/* SDK: playdate.graphics.setDitherPattern(alpha, [ditherType]).
 * The dither type is one of playdate.graphics.image.kDitherType*
 * (0 none, 1 diagonal line, 2 vertical line, 3 horizontal line,
 * 4 screen, 5 Bayer 2x2, 6 Bayer 4x4, 7 Bayer 8x8; the error-diffusion
 * types 8-10 are treated as Bayer 8x8, which the SDK itself calls
 * "very unpredictable" here). The default type is Bayer 8x8.
 * The pattern is computed from image_dither_value() (same thresholds
 * as drawFaded): a pixel is drawn when dither_value < alpha for a
 * black pattern, and when dither_value >= alpha for a white one —
 * the SDK inverts alpha when the current colour is white (a known
 * bug, kept: 1.0 transparent, 0 opaque). Only the dithered pixels
 * are painted; the rest stay transparent. XTRIS uses this for its
 * background (0.8), toast drop shadows (0.25) and menu pages (0.5),
 * all Bayer 8x8. */
static int set_dither_pattern(lua_State *lua)
{
    double alpha = luaL_checknumber(lua, 1);
    int type = (int)luaL_optinteger(lua, 2, 7);
    if (type < 0 || type > 10)
        return luaL_argerror(lua, 2, "invalid dither type");
    if (type >= 8)
        type = 7; /* error-diffusion dithers: fall back to Bayer 8x8 */
    if (alpha < 0)
        alpha = 0;
    if (alpha > 1)
        alpha = 1;
    int white = gfx.color == 1; /* current colour decides the dither colour */
    for (int y = 0; y < 8; y++) {
        uint8_t bits = 0;
        for (int x = 0; x < 8; x++) {
            double value = image_dither_value(type, x, y);
            int drawn = white ? value >= alpha : value < alpha;
            if (drawn)
                bits |= (uint8_t)(1 << (7 - x));
        }
        gfx.pattern[y] = bits;
        gfx.pattern_mask[y] = bits; /* only the dithered pixels paint */
    }
    gfx.pattern_fg = white ? 1 : 0;
    gfx.pattern_bg_paints = 0;
    gfx.pattern_on = 1;
    return 0;
}

/* Fill a rectangle in the current target, applying the draw offset and
 * the clip rect. */
/* Coordinates arrive as floats from games; the SDK truncates them. */
static int check_coord(lua_State *lua, int index)
{
    return (int)luaL_checknumber(lua, index);
}

static int fill_rect(lua_State *lua)
{
    int64_t x = check_coord(lua, 1);
    int64_t y = check_coord(lua, 2);
    int64_t width = check_coord(lua, 3);
    int64_t height = check_coord(lua, 4);
    DrawTarget *target = &gfx.target;
    if (target->screen) {
        x -= gfx.offset_x;
        y -= gfx.offset_y;
    }
    if (gfx.clip_w >= 0) {
        int64_t cx = gfx.clip_x, cy = gfx.clip_y;
        int64_t cr = cx + gfx.clip_w, cb = cy + gfx.clip_h;
        if (x < cx) { width -= cx - x; x = cx; }
        if (y < cy) { height -= cy - y; y = cy; }
        if (x + width > cr) width = cr - x;
        if (y + height > cb) height = cb - y;
    }
    int left = clamp(x, target->width);
    int top = clamp(y, target->height);
    int right = clamp(x + width, target->width);
    int bottom = clamp(y + height, target->height);
    if (right <= left || bottom <= top)
        return 0;
    /* The memset fast path paints a solid colour; a pattern must go
     * through gfx_put so each pixel is looked up in the pattern. */
    if (!gfx.pattern_on && !target->packed && (gfx.color == 0 || gfx.color == 1)) {
        for (int row = top; row < bottom; row++)
            memset(&target->bits[row * target->stride + left],
                   gfx.color, (size_t)(right - left));
        return 0;
    }
    for (int row = top; row < bottom; row++)
        for (int col = left; col < right; col++)
            gfx_put(col, row);
    return 0;
}

static int draw_rect(lua_State *lua)
{
    int64_t x = check_coord(lua, 1), y = check_coord(lua, 2);
    int64_t w = check_coord(lua, 3), h = check_coord(lua, 4);
    if (w <= 0 || h <= 0)
        return 0;
    for (int64_t col = x; col < x + w; col++) {
        gfx_put((int)col, (int)y);
        gfx_put((int)col, (int)(y + h - 1));
    }
    for (int64_t row = y + 1; row < y + h - 1; row++) {
        gfx_put((int)x, (int)row);
        gfx_put((int)(x + w - 1), (int)row);
    }
    return 0;
}

/* Plot one point of a stroked line, applying the line width as a
 * square brush (the SDK's wide lines are square-ish; the brush is
 * centred on the point). */
static void fill_ellipse(int x, int y, int w, int h);

static void draw_line_point(int x, int y)
{
    if (gfx.line_width <= 1) {
        gfx_put(x, y);
        return;
    }
    int half = gfx.line_width / 2;
    for (int oy = -half; oy <= half; oy++)
        for (int ox = -half; ox <= half; ox++)
            gfx_put(x + ox, y + oy);
}

static void draw_line_points(int x, int y, int end_x, int end_y)
{
    int start_x = x, start_y = y;
    /* Square caps extend the line by half the width at each end;
     * round caps draw a disc of the line width at each end. */
    if (gfx.line_cap_style == 1 && gfx.line_width > 1) {
        double len = sqrt((double)(end_x - x) * (end_x - x) +
                          (double)(end_y - y) * (end_y - y));
        if (len > 0) {
            int half = gfx.line_width / 2;
            int ex = (int)llround((end_x - x) / len * half);
            int ey = (int)llround((end_y - y) / len * half);
            x -= ex;
            y -= ey;
            end_x += ex;
            end_y += ey;
        }
    }
    int dx = abs(end_x - x), dy = -abs(end_y - y);
    int sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        draw_line_point(x, y);
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
    if (gfx.line_cap_style == 2 && gfx.line_width > 1) {
        int half = gfx.line_width / 2;
        fill_ellipse(start_x - half, start_y - half, 2 * half + 1, 2 * half + 1);
        fill_ellipse(end_x - half, end_y - half, 2 * half + 1, 2 * half + 1);
    }
}

/* SDK: playdate.graphics.setLineCapStyle(style) / getLineCapStyle().
 * 0 butt, 1 square, 2 round (kLineCapStyleButt/Square/Round). */
static int set_line_cap_style(lua_State *lua)
{
    int style = luaL_checkinteger(lua, 1);
    if (style < 0 || style > 2)
        return luaL_argerror(lua, 1, "invalid line cap style");
    gfx.line_cap_style = style;
    return 0;
}

static int get_line_cap_style(lua_State *lua)
{
    lua_pushinteger(lua, gfx.line_cap_style);
    return 1;
}

/* Read a rect argument: either 4 numbers (x, y, w, h) starting at
 * `index`, or a playdate.geometry.rect at `index` with `radius` as
 * the next argument. Returns the radius. */
static double check_round_rect(lua_State *lua, int index,
                               int *x, int *y, int *w, int *h)
{
    if (lua_isnumber(lua, index)) {
        *x = check_coord(lua, index);
        *y = check_coord(lua, index + 1);
        *w = check_coord(lua, index + 2);
        *h = check_coord(lua, index + 3);
        return luaL_checknumber(lua, index + 4);
    }
    lua_getfield(lua, index, "x");
    lua_getfield(lua, index, "y");
    lua_getfield(lua, index, "w");
    lua_getfield(lua, index, "h");
    *x = (int)luaL_checknumber(lua, -4);
    *y = (int)luaL_checknumber(lua, -3);
    *w = (int)luaL_checknumber(lua, -2);
    *h = (int)luaL_checknumber(lua, -1);
    lua_pop(lua, 4);
    return luaL_checknumber(lua, index + 1);
}

/* SDK: playdate.graphics.fillRoundRect(x, y, w, h, radius) /
 * fillRoundRect(rect, radius). Row-scan fill: the middle rows span
 * the full width; the corner rows shrink by the corner-circle
 * half-chord. Same half-pixel convention as fill_ellipse. */
static int fill_round_rect(lua_State *lua)
{
    int x, y, w, h;
    double radius = check_round_rect(lua, 1, &x, &y, &w, &h);
    if (w <= 0 || h <= 0)
        return 0;
    double r = radius;
    if (r < 0)
        r = 0;
    if (r > w / 2.0)
        r = w / 2.0;
    if (r > h / 2.0)
        r = h / 2.0;
    for (int row = y; row < y + h; row++) {
        double cy = row + 0.5;
        int left = x, right = x + w - 1;
        if (r > 0) {
            double top_c = y + r, bot_c = y + h - r;
            double dy = 0;
            if (cy < top_c)
                dy = top_c - cy;
            else if (cy > bot_c)
                dy = cy - bot_c;
            if (dy > 0) {
                double half = sqrt(r * r - dy * dy);
                left = (int)ceil(x + r - half);
                right = (int)floor(x + w - r + half);
            }
        }
        for (int col = left; col <= right; col++)
            gfx_put(col, row);
    }
    return 0;
}

/* SDK: playdate.graphics.drawRoundRect(x, y, w, h, radius) /
 * drawRoundRect(rect, radius). Four straight edges plus four
 * quarter-ellipse arcs sampled by angle. */
static int draw_round_rect(lua_State *lua)
{
    int x, y, w, h;
    double radius = check_round_rect(lua, 1, &x, &y, &w, &h);
    if (w <= 0 || h <= 0)
        return 0;
    double r = radius;
    if (r < 0)
        r = 0;
    if (r > w / 2.0)
        r = w / 2.0;
    if (r > h / 2.0)
        r = h / 2.0;
    int ir = (int)llround(r);
    if (ir <= 0) {
        /* No rounding: a plain rect outline. */
        for (int col = x; col < x + w; col++) {
            gfx_put(col, y);
            gfx_put(col, y + h - 1);
        }
        for (int row = y + 1; row < y + h - 1; row++) {
            gfx_put(x, row);
            gfx_put(x + w - 1, row);
        }
        return 0;
    }
    /* Straight edges, inset by the radius. */
    draw_line_points(x + ir, y, x + w - ir, y);
    draw_line_points(x + ir, y + h - 1, x + w - ir, y + h - 1);
    draw_line_points(x, y + ir, x, y + h - ir);
    draw_line_points(x + w - 1, y + ir, x + w - 1, y + h - ir);
    /* Quarter arcs: sample every ~4 degrees, more for large radii. */
    int steps = ir * 2 + 8;
    if (steps < 12)
        steps = 12;
    if (steps > 64)
        steps = 64;
    double cx[4] = {x + r, x + w - r, x + w - r, x + r};
    double cy[4] = {y + r, y + r, y + h - r, y + h - r};
    double start[4] = {180, 270, 0, 90};
    for (int corner = 0; corner < 4; corner++) {
        for (int i = 0; i <= steps; i++) {
            double angle = (start[corner] + 90.0 * i / steps) * 3.141592653589793 / 180.0;
            int px = (int)llround(cx[corner] + r * cos(angle));
            int py = (int)llround(cy[corner] + r * sin(angle));
            draw_line_point(px, py);
        }
    }
    return 0;
}

static int draw_line(lua_State *lua)
{
    int xy[4];
    for (int i = 0; i < 4; i++) {
        lua_Number value = luaL_checknumber(lua, i + 1);
        if (value < -4096 || value > 4096)
            return luaL_argerror(lua, i + 1, "line coordinate out of range");
        xy[i] = (int)value;
    }
    draw_line_points(xy[0], xy[1], xy[2], xy[3]);
    return 0;
}

/* Ellipse helpers: the ellipse inscribed in the rect (x, y, w, h). */
static void fill_ellipse(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    double cx = x + w / 2.0, cy = y + h / 2.0;
    double rx = w / 2.0, ry = h / 2.0;
    for (int row = y; row < y + h; row++) {
        double ny = (row + 0.5 - cy) / ry;
        if (ny < -1 || ny > 1)
            continue;
        double half = rx * sqrt(1 - ny * ny);
        int left = (int)ceil(cx - half);
        int right = (int)floor(cx + half);
        for (int col = left; col <= right; col++)
            gfx_put(col, row);
    }
}

static void draw_ellipse(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    double cx = x + w / 2.0, cy = y + h / 2.0;
    double rx = w / 2.0, ry = h / 2.0;
    for (int row = y; row < y + h; row++) {
        double ny = (row + 0.5 - cy) / ry;
        if (ny < -1 || ny > 1)
            continue;
        double half = rx * sqrt(1 - ny * ny);
        gfx_put((int)ceil(cx - half), row);
        gfx_put((int)floor(cx + half), row);
    }
}

static int draw_circle_at_point(lua_State *lua)
{
    int x = check_coord(lua, 1);
    int y = check_coord(lua, 2);
    int radius = check_coord(lua, 3);
    draw_ellipse(x - radius, y - radius, 2 * radius + 1, 2 * radius + 1);
    return 0;
}

static int fill_circle_at_point(lua_State *lua)
{
    int x = check_coord(lua, 1);
    int y = check_coord(lua, 2);
    int radius = check_coord(lua, 3);
    fill_ellipse(x - radius, y - radius, 2 * radius + 1, 2 * radius + 1);
    return 0;
}

static int draw_ellipse_in_rect(lua_State *lua)
{
    draw_ellipse(check_coord(lua, 1), check_coord(lua, 2),
                 check_coord(lua, 3), check_coord(lua, 4));
    return 0;
}

static int fill_ellipse_in_rect(lua_State *lua)
{
    fill_ellipse(check_coord(lua, 1), check_coord(lua, 2),
                 check_coord(lua, 3), check_coord(lua, 4));
    return 0;
}

static int set_line_width(lua_State *lua)
{
    gfx.line_width = check_coord(lua, 1);
    if (gfx.line_width < 1)
        gfx.line_width = 1;
    return 0;
}

static int get_line_width(lua_State *lua)
{
    lua_pushinteger(lua, gfx.line_width);
    return 1;
}

static int set_stroke_location(lua_State *lua)
{
    gfx.stroke_location = luaL_checkinteger(lua, 1);
    return 0;
}

static int get_stroke_location(lua_State *lua)
{
    lua_pushinteger(lua, gfx.stroke_location);
    return 1;
}

static int set_clip_rect(lua_State *lua)
{
    gfx.clip_x = check_coord(lua, 1);
    gfx.clip_y = check_coord(lua, 2);
    gfx.clip_w = check_coord(lua, 3);
    gfx.clip_h = check_coord(lua, 4);
    return 0;
}

static int clear_clip_rect(lua_State *lua)
{
    (void)lua;
    gfx.clip_w = gfx.clip_h = -1;
    return 0;
}

static int get_clip_rect(lua_State *lua)
{
    if (gfx.clip_w < 0) {
        /* No clip rect set: the SDK reports the full display. */
        lua_pushinteger(lua, 0);
        lua_pushinteger(lua, 0);
        lua_pushinteger(lua, logical_w);
        lua_pushinteger(lua, logical_h);
        return 4;
    }
    lua_pushinteger(lua, gfx.clip_x);
    lua_pushinteger(lua, gfx.clip_y);
    lua_pushinteger(lua, gfx.clip_w);
    lua_pushinteger(lua, gfx.clip_h);
    return 4;
}

static int set_image_draw_mode(lua_State *lua)
{
    gfx.draw_mode = luaL_checkinteger(lua, 1);
    if (gfx.draw_mode < 0 || gfx.draw_mode > 7)
        return luaL_argerror(lua, 1, "invalid draw mode");
    return 0;
}

static int get_image_draw_mode(lua_State *lua)
{
    lua_pushinteger(lua, gfx.draw_mode);
    return 1;
}

static int set_draw_offset(lua_State *lua)
{
    gfx.offset_x = check_coord(lua, 1);
    gfx.offset_y = check_coord(lua, 2);
    return 0;
}

static int get_draw_offset(lua_State *lua)
{
    lua_pushinteger(lua, gfx.offset_x);
    lua_pushinteger(lua, gfx.offset_y);
    return 2;
}

/* Install an image as the current draw target. The image's mask is
 * allocated here if it has none, so the target's mask pointer stays in
 * sync with the image for the whole focus session. */
static void gfx_focus_image(lua_State *lua, Image *image, int index)
{
    if (!image->mask) {
        size_t bytes = (size_t)image->bh * image->rowbytes;
        image->mask = malloc(bytes);
        if (image->mask)
            memset(image->mask, 0xff, bytes);
    }
    gfx.target.bits = image->bitmap;
    gfx.target.mask = image->mask;
    gfx.target.width = image->width;
    gfx.target.height = image->height;
    gfx.target.stride = image->rowbytes;
    gfx.target.packed = 1;
    gfx.target.screen = 0;
    lua_pushvalue(lua, index);
    gfx.focus_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
}

static int push_context(lua_State *lua, int full)
{
    /* SDK: pushContext([image]) — with no image (or nil) the target
     * stays the display framebuffer; the call only saves the graphics
     * state (and, for pushContext, resets it). XTRIS's background()
     * does exactly this: pushContext() with no argument, dither-fill
     * the screen, popContext(). Previously image_check() rejected the
     * missing argument with "image expected". */
    Image *image = NULL;
    if (!lua_isnoneornil(lua, 1))
        image = image_check(lua, 1);
    if (gfx.stack_top >= (int)(sizeof gfx.stack / sizeof *gfx.stack))
        return luaL_error(lua, "graphics context stack overflow");
    GfxSaved *saved = &gfx.stack[gfx.stack_top++];
    saved->target = gfx.target;
    saved->color = gfx.color;
    saved->draw_mode = gfx.draw_mode;
    saved->offset_x = gfx.offset_x;
    saved->offset_y = gfx.offset_y;
    saved->clip_x = gfx.clip_x;
    saved->clip_y = gfx.clip_y;
    saved->clip_w = gfx.clip_w;
    saved->clip_h = gfx.clip_h;
    saved->line_width = gfx.line_width;
    saved->stroke_location = gfx.stroke_location;
    saved->focus_ref = gfx.focus_ref;
    saved->full = full;
    memcpy(saved->pattern, gfx.pattern, sizeof gfx.pattern);
    memcpy(saved->pattern_mask, gfx.pattern_mask, sizeof gfx.pattern_mask);
    saved->pattern_fg = gfx.pattern_fg;
    saved->pattern_bg_paints = gfx.pattern_bg_paints;
    saved->pattern_on = gfx.pattern_on;
    saved->line_cap_style = gfx.line_cap_style;
    if (image) {
        gfx_focus_image(lua, image, 1);
    } else {
        /* No new target: leave gfx.target alone and clear the focus
         * ref so pop_context() does not unref the outer context's
         * image ref (which the saved entry still holds). */
        gfx.focus_ref = LUA_NOREF;
    }
    if (full) {
        gfx.color = 1;
        gfx.draw_mode = 0;
        gfx.offset_x = gfx.offset_y = 0;
        gfx.clip_x = gfx.clip_y = 0;
        gfx.clip_w = gfx.clip_h = -1;
        gfx.line_width = 1;
        gfx.stroke_location = 1;
        gfx.pattern_on = 0;
        gfx.line_cap_style = 0;
    }
    return 0;
}

static int lua_push_context(lua_State *lua) { return push_context(lua, 1); }
static int lua_lock_focus(lua_State *lua) { return push_context(lua, 0); }

static int pop_context(lua_State *lua)
{
    if (gfx.stack_top <= 0)
        return luaL_error(lua, "graphics context stack underflow");
    GfxSaved *saved = &gfx.stack[--gfx.stack_top];
    if (gfx.focus_ref != LUA_NOREF)
        luaL_unref(lua, LUA_REGISTRYINDEX, gfx.focus_ref);
    gfx.target = saved->target;
    gfx.color = saved->color;
    gfx.draw_mode = saved->draw_mode;
    gfx.offset_x = saved->offset_x;
    gfx.offset_y = saved->offset_y;
    gfx.clip_x = saved->clip_x;
    gfx.clip_y = saved->clip_y;
    gfx.clip_w = saved->clip_w;
    gfx.clip_h = saved->clip_h;
    gfx.line_width = saved->line_width;
    gfx.stroke_location = saved->stroke_location;
    gfx.focus_ref = saved->focus_ref;
    memcpy(gfx.pattern, saved->pattern, sizeof gfx.pattern);
    memcpy(gfx.pattern_mask, saved->pattern_mask, sizeof gfx.pattern_mask);
    gfx.pattern_fg = saved->pattern_fg;
    gfx.pattern_bg_paints = saved->pattern_bg_paints;
    gfx.pattern_on = saved->pattern_on;
    gfx.line_cap_style = saved->line_cap_style;
    return 0;
}

static int set_stencil_image(lua_State *lua)
{
    /* Stencil masking is not implemented; the image is stored and
     * ignored, so games that set one still draw (unmasked). */
    if (gfx.stencil_ref != LUA_NOREF)
        luaL_unref(lua, LUA_REGISTRYINDEX, gfx.stencil_ref);
    lua_pushvalue(lua, 1);
    gfx.stencil_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    return 0;
}

static int clear_stencil(lua_State *lua)
{
    if (gfx.stencil_ref != LUA_NOREF) {
        luaL_unref(lua, LUA_REGISTRYINDEX, gfx.stencil_ref);
        gfx.stencil_ref = LUA_NOREF;
    }
    return 0;
}

static int draw_text_stub(lua_State *lua)
{
    /* No font renderer: text is not drawn. CoreLibs/graphics.lua
     * replaces drawTextInRect with a Lua implementation that still
     * needs a working font, so text stays invisible; this is a known
     * gap, logged once. */
    static int warned;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "q2-playdate: font drawing is a stub; text is not rendered\n");
    }
    (void)lua;
    lua_pushinteger(lua, 0);
    return 1;
}

static int get_text_size(lua_State *lua)
{
    const char *text = luaL_checkstring(lua, 1);
    lua_pushinteger(lua, text_width_estimate(text));
    lua_pushinteger(lua, 16);
    return 2;
}

static int get_font(lua_State *lua)
{
    lua_getfield(lua, LUA_REGISTRYINDEX, "q2.current.font");
    return 1;
}

static int set_font(lua_State *lua)
{
    if (lua_isnil(lua, 1)) {
        /* Reset to the default font. */
        lua_getfield(lua, LUA_REGISTRYINDEX, "q2.default.font");
        lua_setfield(lua, LUA_REGISTRYINDEX, "q2.current.font");
        return 0;
    }
    lua_pushvalue(lua, 1);
    lua_setfield(lua, LUA_REGISTRYINDEX, "q2.current.font");
    return 0;
}

static int draw_fps(lua_State *lua)
{
    (void)lua;
    return 0;
}

/* --- playdate.display -------------------------------------------------- */

static int display_set_scale(lua_State *lua)
{
    int scale = luaL_checkinteger(lua, 1);
    if (scale < 1 || scale > 8)
        return luaL_argerror(lua, 1, "scale must be 1..8");
    display_scale = scale;
    logical_w = SOURCE_WIDTH / scale;
    logical_h = SOURCE_HEIGHT / scale;
    memset(logical, 1, sizeof logical);
    if (gfx.target.screen)
        gfx_use_screen();
    return 0;
}

static int display_get_scale(lua_State *lua)
{
    lua_pushinteger(lua, display_scale);
    return 1;
}

static int display_get_width(lua_State *lua)
{
    lua_pushinteger(lua, logical_w);
    return 1;
}

static int display_get_height(lua_State *lua)
{
    lua_pushinteger(lua, logical_h);
    return 1;
}

static int display_get_size(lua_State *lua)
{
    lua_pushinteger(lua, logical_w);
    lua_pushinteger(lua, logical_h);
    return 2;
}

static int display_get_rect(lua_State *lua)
{
    rect_push(lua, 0, 0, logical_w, logical_h);
    return 1;
}

static int display_set_offset(lua_State *lua)
{
    return set_draw_offset(lua);
}

static int display_get_offset(lua_State *lua)
{
    return get_draw_offset(lua);
}

static int display_set_refresh_rate(lua_State *lua)
{
    /* The runner ticks at 30 fps regardless; the rate is stored only. */
    refresh_rate = luaL_checkinteger(lua, 1);
    return 0;
}

static int display_get_refresh_rate(lua_State *lua)
{
    lua_pushinteger(lua, refresh_rate);
    return 1;
}

static int display_set_inverted(lua_State *lua)
{
    display_inverted = lua_toboolean(lua, 1);
    return 0;
}

static int display_get_inverted(lua_State *lua)
{
    lua_pushboolean(lua, display_inverted);
    return 1;
}

static int display_flush(lua_State *lua)
{
    (void)lua;
    return 0;
}

static int display_set_mosaic(lua_State *lua)
{
    (void)lua;
    return 0;
}

static int display_get_mosaic(lua_State *lua)
{
    lua_pushinteger(lua, 0);
    lua_pushinteger(lua, 0);
    return 2;
}

static int display_set_flipped(lua_State *lua)
{
    (void)lua;
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
        {"setPattern", set_pattern}, {"getPattern", get_pattern},
        {"setDitherPattern", set_dither_pattern},
        {"drawLine", draw_line}, {"drawRect", draw_rect},
        {"drawCircleAtPoint", draw_circle_at_point},
        {"fillCircleAtPoint", fill_circle_at_point},
        {"drawEllipseInRect", draw_ellipse_in_rect},
        {"fillEllipseInRect", fill_ellipse_in_rect},
        {"drawCircleInRect", draw_ellipse_in_rect},
        {"fillCircleInRect", fill_ellipse_in_rect},
        {"setLineWidth", set_line_width}, {"getLineWidth", get_line_width},
        {"setLineCapStyle", set_line_cap_style},
        {"getLineCapStyle", get_line_cap_style},
        {"fillRoundRect", fill_round_rect},
        {"drawRoundRect", draw_round_rect},
        {"setStrokeLocation", set_stroke_location},
        {"getStrokeLocation", get_stroke_location},
        {"setBackgroundColor", set_background_color},
        {"getBackgroundColor", get_background_color},
        {"setClipRect", set_clip_rect}, {"clearClipRect", clear_clip_rect},
        {"getClipRect", get_clip_rect},
        {"setImageDrawMode", set_image_draw_mode},
        {"getImageDrawMode", get_image_draw_mode},
        {"setDrawOffset", set_draw_offset}, {"getDrawOffset", get_draw_offset},
        {"pushContext", lua_push_context}, {"popContext", pop_context},
        {"lockFocus", lua_lock_focus}, {"unlockFocus", pop_context},
        {"setStencilImage", set_stencil_image}, {"clearStencil", clear_stencil},
        {"drawText", draw_text_stub}, {"drawTextInRect", draw_text_stub},
        {"getTextSize", get_text_size},
        {"getFont", get_font}, {"setFont", set_font},
        {"drawFPS", draw_fps},
        {NULL, NULL}
    };
    static const luaL_Reg display[] = {
        {"setScale", display_set_scale}, {"getScale", display_get_scale},
        {"getWidth", display_get_width}, {"getHeight", display_get_height},
        {"getSize", display_get_size}, {"getRect", display_get_rect},
        {"setOffset", display_set_offset}, {"getOffset", display_get_offset},
        {"setRefreshRate", display_set_refresh_rate},
        {"getRefreshRate", display_get_refresh_rate},
        {"setInverted", display_set_inverted},
        {"getInverted", display_get_inverted},
        {"flush", display_flush},
        {"setMosaic", display_set_mosaic},
        {"getMosaic", display_get_mosaic},
        {"setFlipped", display_set_flipped},
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
    lua_pushinteger(lua, 2);
    lua_setfield(lua, -2, "kColorClear");
    lua_pushinteger(lua, 3);
    lua_setfield(lua, -2, "kColorXOR");
    lua_pushinteger(lua, 0);
    lua_setfield(lua, -2, "kDrawModeCopy");
    lua_pushinteger(lua, 1);
    lua_setfield(lua, -2, "kDrawModeWhiteTransparent");
    lua_pushinteger(lua, 2);
    lua_setfield(lua, -2, "kDrawModeBlackTransparent");
    lua_pushinteger(lua, 3);
    lua_setfield(lua, -2, "kDrawModeFillWhite");
    lua_pushinteger(lua, 4);
    lua_setfield(lua, -2, "kDrawModeFillBlack");
    lua_pushinteger(lua, 5);
    lua_setfield(lua, -2, "kDrawModeXOR");
    lua_pushinteger(lua, 6);
    lua_setfield(lua, -2, "kDrawModeNXOR");
    lua_pushinteger(lua, 7);
    lua_setfield(lua, -2, "kDrawModeInverted");
    lua_pushinteger(lua, 0);
    lua_setfield(lua, -2, "kStrokeInside");
    lua_pushinteger(lua, 1);
    lua_setfield(lua, -2, "kStrokeCentered");
    lua_pushinteger(lua, 2);
    lua_setfield(lua, -2, "kStrokeOutside");
    lua_pushinteger(lua, 0);
    lua_setfield(lua, -2, "kLineCapStyleButt");
    lua_pushinteger(lua, 1);
    lua_setfield(lua, -2, "kLineCapStyleSquare");
    lua_pushinteger(lua, 2);
    lua_setfield(lua, -2, "kLineCapStyleRound");
    lua_setfield(lua, -2, "graphics");
    luaL_newlib(lua, display);
    lua_setfield(lua, -2, "display");
    image_install(lua);
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

static int traceback_handler(lua_State *lua)
{
    const char *message = lua_tostring(lua, 1);
    luaL_traceback(lua, lua, message, 1);
    return 1;
}

static int update_game(lua_State *lua)
{
    lua_getglobal(lua, "playdate");
    if (!lua_istable(lua, -1))
        return luaL_error(lua, "game replaced the playdate table");
    /* The SDK clears the clip rect at the start of each update. */
    gfx.clip_w = gfx.clip_h = -1;
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
            lua_pushcfunction(lua, traceback_handler);
            lua_pushcfunction(lua, update_game);
            if (lua_pcall(lua, 0, 0, -2) != LUA_OK) {
                const char *message = lua_tostring(lua, -1);
                fprintf(stderr, "q2-playdate update error:\n%s\n",
                        message ? message : "Lua update failed");
                snprintf(error, sizeof error, "%s", message ? message : "Lua update failed");
                report_run(frames, work, peak);
                runtime_silence();
                lua_close(lua);
                lua = NULL;
            } else {
                runtime_tick();
                gfx_upscale();
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
        /* do-while, not while: the poll must run at least once per frame
         * even when the update overran the 33 ms budget. With a plain
         * while, a slow game (Frogger's title took ~57 ms/frame before
         * drawRotated was fixed-point) never entered the poll at all, so
         * no key or touch ever reached it. An overrun frame still polls
         * once with timeout 0 and drains one pending message. */
        do {
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
        } while (now_ms() < start + 33 && !launch && !library && !quit && !get_games);
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
            frames = 0;
            work = peak = 0;
            display_scale = 1;
            logical_w = SOURCE_WIDTH;
            logical_h = SOURCE_HEIGHT;
            display_inverted = 0;
            memset(logical, 1, sizeof logical);
            memset(frame, 0, sizeof frame);
            gfx_reset();
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
