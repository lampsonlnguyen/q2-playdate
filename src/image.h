/* Playdate image / imagetable / tilemap / font / geometry support.
 *
 * PDI/PDT decoding is implemented from the file format as produced by
 * Panic's pdc (verified against pdc output): a file is 12 bytes of magic
 * ("Playdate IMG" / "Playdate IMT"), u32 LE 0x80000000, u32 LE decompressed
 * payload length, u32 LE width, u32 LE height, u32 LE count (0 for PDI,
 * cell count for PDT), then a zlib stream. The payload starts with a
 * 16-byte header: u16 width, u16 height, u16 rowbytes, u16 0, u16 xoff,
 * u16 yoff, u16 0, u16 flags; then the MSB-first 1-bit bitmap
 * (height*rowbytes bytes, 1=white), then a mask of the same size only when
 * flags==7 (mask bit 1=opaque). A PDT payload is u16 count, u16 columns
 * (ignored), then `count` u32 LE cumulative end offsets into the cell
 * section; each cell is a PDI payload, possibly trimmed (its header
 * width/height may be smaller than the table's logical cell size, with
 * xoff/yoff giving the trimmed position). Payloads are padded with zeros
 * to a 16-byte multiple; trailing bytes are ignored.
 *
 * Images are 1-bit with an optional mask (transparent pixels). Drawing
 * uses the draw-mode and flip constants from the Playdate SDK
 * (LCDBitmapDrawMode / LCDBitmapFlip / LCDSolidColor). This is a fresh
 * implementation written for q2-playdate; no Panic code is used.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "lua.h"

typedef struct Image {
    int width, height;      /* logical size (from the file/table header) */
    int bw, bh;             /* bitmap size (may be smaller: trimmed) */
    int xoff, yoff;         /* trimmed bitmap position inside the logical rect */
    int rowbytes;
    uint8_t *bitmap;        /* bh*rowbytes, MSB-first, 1=white */
    uint8_t *mask;          /* NULL = fully opaque; bh*rowbytes, 1=opaque */
    int inverted;           /* setInverted flag: flip bits when drawing */
} Image;

typedef struct Imagetable {
    int width, height;      /* logical cell size */
    int count;
    Image *cells;           /* count decoded cells */
} Imagetable;

typedef struct Tilemap {
    int width, height;      /* tiles; height derived from #data/width */
    int *tiles;             /* width*height indices into the imagetable (0 = empty) */
    Imagetable *table;      /* imagetable set by setImageTable (borrowed) */
} Tilemap;

/* A drawing surface: either the byte-per-pixel logical framebuffer
 * (packed=0, mask=NULL) or an image's bit-packed bitmap (packed=1). */
typedef struct DrawTarget {
    uint8_t *bits;
    uint8_t *mask;          /* NULL for the framebuffer */
    int width, height;
    int stride;             /* bytes per row */
    int packed;             /* 1 = 1 bit per pixel, MSB-first */
    int screen;             /* 1 = the screen target (draw offset applies) */
} DrawTarget;

/* Pixel values: 0 black, 1 white, 2 clear (transparent). */
int image_pixel(const DrawTarget *target, int x, int y);
void image_set_pixel(DrawTarget *target, int x, int y, int value);
void draw_target_clear(DrawTarget *target, int color);

/* Decode a PDI payload (the bytes after the zlib stream of a .pdi file,
 * or a PDZ image entry). logical_w/h is the logical size from the file
 * header (0 = use the payload header's size). Returns 0 on success. */
int image_decode_payload(const uint8_t *payload, size_t length,
                         int logical_w, int logical_h, Image *out);
int imagetable_decode_payload(const uint8_t *payload, size_t length,
                              int logical_w, int logical_h, Imagetable *out);

/* Load a .pdi / .pdt file from disk. Returns 0 on success. */
int image_load_file(const char *path, Image **out);
int imagetable_load_file(const char *path, Imagetable **out);

Image *image_new_blank(int width, int height, int color);
Image *image_copy(const Image *src);
Image *image_inverted_copy(const Image *src);
Image *image_faded_copy(const Image *src, double alpha, int dither);
Image *image_scaled_copy(const Image *src, double xscale, double yscale);
Image *image_rotated_copy(const Image *src, double angle_deg,
                          double xscale, double yscale,
                          double centerx, double centery);
void image_clear(Image *image, int color);
void image_free(Image *image);
void imagetable_free(Imagetable *table);

/* Bundle directory used to resolve image paths (set by bundle_open). */
void image_set_bundle(const char *bundle);

/* Blit an image onto a target at (x, y) in target coordinates, applying
 * the image's trim offset, flip, the current draw mode, and (unless
 * ignore_offset) the given draw offset. Draw modes: 0 copy, 1 white
 * transparent, 2 black transparent, 3 fill white, 4 fill black, 5 XOR,
 * 6 NXOR, 7 inverted. Flip: 0 none, 1 X, 2 Y, 3 XY. The target's clip
 * rect (clip_x/y/w/h, or clip_w<0 for none) is honoured. */
void image_blit(DrawTarget *target, const Image *image, int x, int y,
                int flip, int draw_mode, int offset_x, int offset_y,
                int ignore_offset,
                int clip_x, int clip_y, int clip_w, int clip_h);
void image_blit_scaled(DrawTarget *target, const Image *image, int x, int y,
                       double xscale, double yscale, int draw_mode,
                       int offset_x, int offset_y, int ignore_offset,
                       int clip_x, int clip_y, int clip_w, int clip_h);
/* Blit an image centered at (x, y), rotated by angle degrees (positive =
 * clockwise on screen, y points down), optionally scaled. Nearest
 * sampling, same draw modes / clip / offset rules as image_blit. */
void image_blit_rotated(DrawTarget *target, const Image *image, int x, int y,
                        double angle_deg, double xscale, double yscale,
                        int draw_mode,
                        int offset_x, int offset_y, int ignore_offset,
                        int clip_x, int clip_y, int clip_w, int clip_h);
void image_blit_faded(DrawTarget *target, const Image *image, int x, int y,
                      double alpha, int dither, int draw_mode,
                      int offset_x, int offset_y, int ignore_offset,
                      int clip_x, int clip_y, int clip_w, int clip_h);

/* Dither threshold in [0,1) for pixel (x,y) under dither type:
 * 0 none, 1 diagonal line, 2 vertical line, 3 horizontal line, 4 screen,
 * 5 Bayer 2x2, 6 Bayer 4x4, 7 Bayer 8x8. */
double image_dither_value(int dither, int x, int y);

/* Estimated text width in pixels (8 px per character at the 16 px
 * font height); fonts are not rendered, so this is an approximation
 * that keeps text-sizing code from producing 0-width images. */
int text_width_estimate(const char *s);

/* Lua bindings. image_install(lua) creates playdate.graphics.image,
 * .imagetable, .tilemap and .font on the playdate table at the top of
 * the stack, plus playdate.geometry with .rect. It also registers the
 * tables in the registry under "q2.image.table" etc. so later C calls
 * can find them. */
void image_install(lua_State *lua);

/* Push an image / imagetable / tilemap userdata wrapping the given
 * object (the userdata takes ownership). */
void image_push(lua_State *lua, Image *image);
void imagetable_push(lua_State *lua, Imagetable *table);
void tilemap_push(lua_State *lua, Tilemap *tilemap);
/* Push a playdate.geometry.rect userdata. */
void rect_push(lua_State *lua, double x, double y, double w, double h);
Image *image_check(lua_State *lua, int index);
Imagetable *imagetable_check(lua_State *lua, int index);
Tilemap *tilemap_check(lua_State *lua, int index);
int image_is(lua_State *lua, int index);
int imagetable_is(lua_State *lua, int index);
int tilemap_is(lua_State *lua, int index);

int font_push_payload(lua_State *lua, const char *name, const uint8_t *data, size_t size);

int font_current_text(lua_State *lua, int draw);
