#define _POSIX_C_SOURCE 200809L

#include "image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "bundle.h"
#include "lauxlib.h"

/* Provided by card.c: the current graphics state. */
extern DrawTarget *gfx_target(void);
extern int gfx_draw_mode(void);
extern void gfx_offset(int *x, int *y);
extern void gfx_clip(int *x, int *y, int *w, int *h);

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int rowbytes_for(int width)
{
    return (width + 7) / 8;
}

static char bundle_dir[1024] = "";

void image_set_bundle(const char *bundle)
{
    snprintf(bundle_dir, sizeof bundle_dir, "%s", bundle ? bundle : "");
}

/* Resolve a game-relative path to a file on disk, trying the path as
 * given and then with `ext` appended. Rejects absolute paths and any
 * ".." component. Returns 0 and writes the full path on success. */
static int resolve_path(const char *given, const char *ext, char *out, size_t size)
{
    if (!given || given[0] == '/' || strstr(given, ".."))
        return -1;
    for (size_t i = 0; given[i]; i++)
        if (given[i] == '.' && given[i + 1] == '.' &&
            (i == 0 || given[i - 1] == '/') &&
            (given[i + 2] == '/' || given[i + 2] == '\0'))
            return -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        int n = snprintf(out, size, "%s/%s%s", bundle_dir, given,
                         attempt == 0 ? "" : ext);
        if (n < 0 || (size_t)n >= size)
            return -1;
        FILE *file = fopen(out, "rb");
        if (file) {
            fclose(file);
            return 0;
        }
    }
    return -1;
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0 || size > 64 * 1024 * 1024) {
        fclose(file);
        return NULL;
    }
    uint8_t *data = malloc((size_t)size ? (size_t)size : 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (size > 0 && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (size_t)size;
    return data;
}

/* --- Decoding ---------------------------------------------------------- */

static int decode_cell(const uint8_t *payload, size_t length,
                       int logical_w, int logical_h, Image *out)
{
    if (length < 16)
        return -1;
    int w = read_le16(payload);
    int h = read_le16(payload + 2);
    int rb = read_le16(payload + 4);
    int xoff = read_le16(payload + 8);
    int yoff = read_le16(payload + 10);
    int flags = read_le16(payload + 14);
    if (w == 0 && h == 0) {
        /* A fully transparent cell: pdc trims it to 0x0 (the remaining
         * header fields carry the original cell size). It draws nothing;
         * keep the logical size from the caller. */
        memset(out, 0, sizeof *out);
        out->width = logical_w > 0 ? logical_w : 0;
        out->height = logical_h > 0 ? logical_h : 0;
        return 0;
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || rb < rowbytes_for(w) ||
        rb > 4096 || xoff < 0 || yoff < 0)
        return -1;
    size_t need = 16 + (size_t)h * rb;
    if (flags == 7)
        need += (size_t)h * rb;
    if (length < need)
        return -1;
    uint8_t *bitmap = malloc((size_t)h * rb);
    if (!bitmap)
        return -1;
    memcpy(bitmap, payload + 16, (size_t)h * rb);
    uint8_t *mask = NULL;
    if (flags == 7) {
        mask = malloc((size_t)h * rb);
        if (!mask) {
            free(bitmap);
            return -1;
        }
        memcpy(mask, payload + 16 + (size_t)h * rb, (size_t)h * rb);
    }
    memset(out, 0, sizeof *out);
    out->width = logical_w > 0 ? logical_w : w;
    out->height = logical_h > 0 ? logical_h : h;
    out->bw = w;
    out->bh = h;
    out->xoff = xoff;
    out->yoff = yoff;
    out->rowbytes = rb;
    out->bitmap = bitmap;
    out->mask = mask;
    return 0;
}

int image_decode_payload(const uint8_t *payload, size_t length,
                         int logical_w, int logical_h, Image *out)
{
    return decode_cell(payload, length, logical_w, logical_h, out);
}

int imagetable_decode_payload(const uint8_t *payload, size_t length,
                              int logical_w, int logical_h, Imagetable *out)
{
    if (length < 4)
        return -1;
    int count = read_le16(payload);
    if (count <= 0 || count > 65536)
        return -1;
    if (length < 4 + (size_t)count * 4)
        return -1;
    Image *cells = calloc((size_t)count, sizeof *cells);
    if (!cells)
        return -1;
    size_t start = 4 + (size_t)count * 4;
    for (int i = 0; i < count; i++) {
        uint32_t end = read_le32(payload + 4 + (size_t)i * 4);
        uint32_t begin = i == 0 ? 0 : read_le32(payload + 4 + (size_t)(i - 1) * 4);
        if (end < begin || start + end > length) {
            for (int j = 0; j < i; j++)
                image_free(&cells[j]);
            free(cells);
            return -1;
        }
        if (decode_cell(payload + start + begin, end - begin,
                        logical_w, logical_h, &cells[i]) != 0) {
            for (int j = 0; j < i; j++)
                image_free(&cells[j]);
            free(cells);
            return -1;
        }
    }
    if (logical_w <= 0 || logical_h <= 0) {
        /* No file header to give the logical cell size (a PDZ entry):
         * derive it from the largest cell extent. */
        int max_w = 0, max_h = 0;
        for (int i = 0; i < count; i++) {
            int extent_w = cells[i].xoff + cells[i].bw;
            int extent_h = cells[i].yoff + cells[i].bh;
            if (extent_w > max_w)
                max_w = extent_w;
            if (extent_h > max_h)
                max_h = extent_h;
        }
        if (logical_w <= 0)
            logical_w = max_w;
        if (logical_h <= 0)
            logical_h = max_h;
        for (int i = 0; i < count; i++) {
            cells[i].width = logical_w;
            cells[i].height = logical_h;
        }
    }
    memset(out, 0, sizeof *out);
    out->width = logical_w;
    out->height = logical_h;
    out->count = count;
    out->cells = cells;
    return 0;
}

/* Read a .pdi/.pdt file: verify the header, inflate the payload. */
static int load_payload(const char *path, const char *magic,
                        uint8_t **payload_out, size_t *payload_len,
                        int *width, int *height, int *count)
{
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data || size < 32 || memcmp(data, magic, 12) != 0) {
        free(data);
        return -1;
    }
    if (read_le32(data + 12) != 0x80000000u) {
        free(data);
        return -1;
    }
    uint32_t expected = read_le32(data + 16);
    *width = (int)read_le32(data + 20);
    *height = (int)read_le32(data + 24);
    *count = (int)read_le32(data + 28);
    if (*width <= 0 || *height <= 0 || *width > 4096 || *height > 4096 ||
        expected == 0 || expected > 64 * 1024 * 1024) {
        free(data);
        return -1;
    }
    uint8_t *payload = malloc(expected);
    if (!payload) {
        free(data);
        return -1;
    }
    unsigned long actual = expected;
    int status = uncompress(payload, &actual, data + 32, (uLong)(size - 32));
    free(data);
    if (status != Z_OK || actual != expected) {
        free(payload);
        return -1;
    }
    *payload_out = payload;
    *payload_len = expected;
    return 0;
}

int image_load_file(const char *path, Image **out)
{
    uint8_t *payload;
    size_t length;
    int width, height, count;
    if (load_payload(path, "Playdate IMG", &payload, &length,
                     &width, &height, &count) != 0)
        return -1;
    Image *image = calloc(1, sizeof *image);
    if (!image) {
        free(payload);
        return -1;
    }
    if (image_decode_payload(payload, length, width, height, image) != 0) {
        free(payload);
        free(image);
        return -1;
    }
    free(payload);
    *out = image;
    return 0;
}

int imagetable_load_file(const char *path, Imagetable **out)
{
    uint8_t *payload;
    size_t length;
    int width, height, count;
    if (load_payload(path, "Playdate IMT", &payload, &length,
                     &width, &height, &count) != 0)
        return -1;
    Imagetable *table = calloc(1, sizeof *table);
    if (!table) {
        free(payload);
        return -1;
    }
    if (imagetable_decode_payload(payload, length, width, height, table) != 0) {
        free(payload);
        free(table);
        return -1;
    }
    free(payload);
    *out = table;
    return 0;
}

/* --- Image operations -------------------------------------------------- */

void image_free(Image *image)
{
    if (!image)
        return;
    free(image->bitmap);
    free(image->mask);
    image->bitmap = image->mask = NULL;
}

void imagetable_free(Imagetable *table)
{
    if (!table)
        return;
    for (int i = 0; i < table->count; i++)
        image_free(&table->cells[i]);
    free(table->cells);
    table->cells = NULL;
}

Image *image_new_blank(int width, int height, int color)
{
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return NULL;
    Image *image = calloc(1, sizeof *image);
    if (!image)
        return NULL;
    int rb = rowbytes_for(width);
    image->bitmap = malloc((size_t)height * rb);
    if (!image->bitmap) {
        free(image);
        return NULL;
    }
    image->width = image->bw = width;
    image->height = image->bh = height;
    image->rowbytes = rb;
    image_clear(image, color);
    return image;
}

void image_clear(Image *image, int color)
{
    if (!image)
        return;
    size_t bytes = (size_t)image->bh * image->rowbytes;
    if (color == 2) {
        if (!image->mask) {
            image->mask = malloc(bytes);
            if (!image->mask)
                return;
        }
        memset(image->mask, 0, bytes);
        memset(image->bitmap, 0, bytes);
        return;
    }
    memset(image->bitmap, color == 0 ? 0 : 0xff, bytes);
    if (image->mask)
        memset(image->mask, 0xff, bytes);
}

Image *image_copy(const Image *src)
{
    if (!src)
        return NULL;
    Image *copy = calloc(1, sizeof *copy);
    if (!copy)
        return NULL;
    *copy = *src;
    size_t bytes = (size_t)src->bh * src->rowbytes;
    copy->bitmap = malloc(bytes);
    if (!copy->bitmap) {
        free(copy);
        return NULL;
    }
    memcpy(copy->bitmap, src->bitmap, bytes);
    copy->mask = NULL;
    if (src->mask) {
        copy->mask = malloc(bytes);
        if (!copy->mask) {
            free(copy->bitmap);
            free(copy);
            return NULL;
        }
        memcpy(copy->mask, src->mask, bytes);
    }
    return copy;
}

Image *image_inverted_copy(const Image *src)
{
    Image *copy = image_copy(src);
    if (!copy)
        return NULL;
    size_t bytes = (size_t)copy->bh * copy->rowbytes;
    for (size_t i = 0; i < bytes; i++)
        copy->bitmap[i] = (uint8_t)~copy->bitmap[i];
    copy->inverted = 0;
    return copy;
}

double image_dither_value(int dither, int x, int y)
{
    static const uint8_t bayer2[2][2] = {{0, 2}, {3, 1}};
    static const uint8_t bayer4[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    static const uint8_t bayer8[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};
    x &= 7;
    y &= 7;
    switch (dither) {
    case 1: return ((x + y) & 3) / 4.0;
    case 2: return (x & 1) ? 0.5 : 0.0;
    case 3: return (y & 1) ? 0.5 : 0.0;
    case 4: return ((x & 1) ^ (y & 1)) ? 0.75 : 0.25;
    case 5: return bayer2[y & 1][x & 1] / 4.0;
    case 6: return bayer4[y & 3][x & 3] / 16.0;
    case 7: return bayer8[y][x] / 64.0;
    default: return 0.5;
    }
}

Image *image_faded_copy(const Image *src, double alpha, int dither)
{
    Image *copy = image_copy(src);
    if (!copy)
        return NULL;
    size_t bytes = (size_t)copy->bh * copy->rowbytes;
    if (!copy->mask) {
        copy->mask = malloc(bytes);
        if (!copy->mask) {
            image_free(copy);
            free(copy);
            return NULL;
        }
        memset(copy->mask, 0xff, bytes);
    }
    for (int y = 0; y < copy->bh; y++) {
        for (int x = 0; x < copy->bw; x++) {
            if (image_dither_value(dither, x, y) >= alpha)
                copy->mask[y * copy->rowbytes + x / 8] &=
                    (uint8_t)~(0x80u >> (x % 8));
        }
    }
    return copy;
}

Image *image_scaled_copy(const Image *src, double xscale, double yscale)
{
    if (!src || xscale <= 0 || yscale <= 0)
        return NULL;
    int width = (int)(src->width * xscale);
    int height = (int)(src->height * yscale);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return NULL;
    Image *copy = image_new_blank(width, height, 1);
    if (!copy)
        return NULL;
    copy->xoff = (int)(src->xoff * xscale);
    copy->yoff = (int)(src->yoff * yscale);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int sx = (int)(x / xscale) - copy->xoff;
            int sy = (int)(y / yscale) - copy->yoff;
            int value = 1;
            if (sx >= 0 && sx < src->bw && sy >= 0 && sy < src->bh) {
                if (src->mask &&
                    !((src->mask[sy * src->rowbytes + sx / 8] >> (7 - sx % 8)) & 1)) {
                    if (!copy->mask) {
                        size_t bytes = (size_t)copy->bh * copy->rowbytes;
                        copy->mask = malloc(bytes);
                        if (copy->mask)
                            memset(copy->mask, 0xff, bytes);
                    }
                    if (copy->mask)
                        copy->mask[y * copy->rowbytes + x / 8] &=
                            (uint8_t)~(0x80u >> (x % 8));
                    continue;
                }
                value = (src->bitmap[sy * src->rowbytes + sx / 8] >> (7 - sx % 8)) & 1;
            }
            if (value)
                copy->bitmap[y * copy->rowbytes + x / 8] |= (uint8_t)(0x80u >> (x % 8));
            else
                copy->bitmap[y * copy->rowbytes + x / 8] &=
                    (uint8_t)~(0x80u >> (x % 8));
        }
    }
    return copy;
}

/* SDK: image:rotatedImage(rotation, [xscale, [yscale, [centerx,
 * [centery]]]]) — a new image holding the source rotated by rotation
 * degrees (positive = clockwise, y points down) about (centerx,
 * centery) in the source (default: the image center), optionally
 * scaled. Sized to the rotated bounding box. Implemented by rendering
 * through image_blit_rotated (the same Q16.16 sampler as drawRotated),
 * with the target centre computed so the source centre lands where
 * the rotation about (centerx, centery) puts it. XTRIS rotates its
 * title-page affordance icons by +/-90 degrees. */
Image *image_rotated_copy(const Image *src, double angle_deg,
                          double xscale, double yscale,
                          double centerx, double centery)
{
    if (!src || xscale <= 0 || yscale <= 0)
        return NULL;
    double angle = angle_deg * 3.14159265358979323846 / 180.0;
    double cos_a = cos(angle), sin_a = sin(angle);
    /* Scaled source rect, rotated about (cx, cy) in scaled coords. */
    double w = src->width * xscale, h = src->height * yscale;
    double cx = centerx * xscale, cy = centery * yscale;
    double corners[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    for (int i = 0; i < 4; i++) {
        double dx = corners[i][0] - cx, dy = corners[i][1] - cy;
        double rx = cx + dx * cos_a - dy * sin_a;
        double ry = cy + dx * sin_a + dy * cos_a;
        if (rx < minx) minx = rx;
        if (rx > maxx) maxx = rx;
        if (ry < miny) miny = ry;
        if (ry > maxy) maxy = ry;
    }
    int tw = (int)ceil(maxx - minx), th = (int)ceil(maxy - miny);
    if (tw <= 0 || th <= 0 || tw > 4096 || th > 4096)
        return NULL;
    Image *copy = image_new_blank(tw, th, 2);
    if (!copy)
        return NULL;
    /* The source centre maps to T = (-minx, -miny) + R(S - C). */
    double sdx = w / 2.0 - cx, sdy = h / 2.0 - cy;
    double tx = -minx + sdx * cos_a - sdy * sin_a;
    double ty = -miny + sdx * sin_a + sdy * cos_a;
    DrawTarget target;
    target.bits = copy->bitmap;
    target.mask = copy->mask;
    target.width = tw;
    target.height = th;
    target.stride = copy->rowbytes;
    target.packed = 1;
    target.screen = 0;
    image_blit_rotated(&target, src, (int)llround(tx), (int)llround(ty),
                       angle_deg, xscale, yscale, 0, 0, 0, 1, 0, 0, -1, -1);
    return copy;
}

/* --- Pixel access and blitting ----------------------------------------- */

int image_pixel(const DrawTarget *target, int x, int y)
{
    if (!target || x < 0 || y < 0 || x >= target->width || y >= target->height)
        return 1;
    if (target->packed) {
        if (target->mask &&
            !((target->mask[y * target->stride + x / 8] >> (7 - x % 8)) & 1))
            return -1;
        return (target->bits[y * target->stride + x / 8] >> (7 - x % 8)) & 1;
    }
    return target->bits[y * target->stride + x] != 0;
}

void image_set_pixel(DrawTarget *target, int x, int y, int value)
{
    if (!target || x < 0 || y < 0 || x >= target->width || y >= target->height)
        return;
    if (target->packed) {
        if (value == 2) {
            if (!target->mask) {
                size_t bytes = (size_t)target->height * target->stride;
                /* The mask belongs to the image; only allocate when the
                 * target is an image (packed targets always are). */
                target->mask = malloc(bytes);
                if (!target->mask)
                    return;
                memset(target->mask, 0xff, bytes);
            }
            target->mask[y * target->stride + x / 8] &=
                (uint8_t)~(0x80u >> (x % 8));
            return;
        }
        if (target->mask)
            target->mask[y * target->stride + x / 8] |= (uint8_t)(0x80u >> (x % 8));
        if (value)
            target->bits[y * target->stride + x / 8] |= (uint8_t)(0x80u >> (x % 8));
        else
            target->bits[y * target->stride + x / 8] &=
                (uint8_t)~(0x80u >> (x % 8));
        return;
    }
    target->bits[y * target->stride + x] = (uint8_t)(value == 2 ? 1 : value);
}

/* Fill an entire draw target with a colour (2 = clear/transparent).
 * For a packed target with no mask, a clear allocates one; callers that
 * keep a separate mask pointer for the same image (the graphics context)
 * must allocate the mask when the target is installed, so the pointer
 * here stays in sync. */
void draw_target_clear(DrawTarget *target, int color)
{
    if (!target)
        return;
    size_t bytes = (size_t)target->height * target->stride;
    if (target->packed) {
        if (color == 2) {
            if (!target->mask) {
                target->mask = malloc(bytes);
                if (!target->mask)
                    return;
            }
            memset(target->mask, 0, bytes);
            memset(target->bits, 0, bytes);
            return;
        }
        memset(target->bits, color == 0 ? 0 : 0xff, bytes);
        if (target->mask)
            memset(target->mask, 0xff, bytes);
        return;
    }
    /* Byte-per-pixel targets use the Playdate convention (1=white);
     * clear (2) means white. */
    memset(target->bits, color == 0 ? 0 : 1, bytes);
}

/* Apply one source pixel to the destination under the draw mode. */
static void put_pixel(DrawTarget *target, int dx, int dy, int value, int mode,
                      int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (dx < 0 || dy < 0 || dx >= target->width || dy >= target->height)
        return;
    if (clip_w >= 0 && (dx < clip_x || dx >= clip_x + clip_w ||
                        dy < clip_y || dy >= clip_y + clip_h))
        return;
    switch (mode) {
    case 1: /* white transparent */
        if (value == 0)
            image_set_pixel(target, dx, dy, 0);
        return;
    case 2: /* black transparent */
        if (value == 1)
            image_set_pixel(target, dx, dy, 1);
        return;
    case 3: /* fill white */
        if (value == 0)
            image_set_pixel(target, dx, dy, 1);
        return;
    case 4: /* fill black */
        if (value == 0)
            image_set_pixel(target, dx, dy, 0);
        return;
    case 5: /* XOR */
        image_set_pixel(target, dx, dy, image_pixel(target, dx, dy) < 0 ? 1 : (image_pixel(target, dx, dy) ^ value));
        return;
    case 6: /* NXOR */
        image_set_pixel(target, dx, dy, (image_pixel(target, dx, dy) < 0 ? 1 : image_pixel(target, dx, dy)) == value ? 1 : 0);
        return;
    case 7: /* inverted */
        image_set_pixel(target, dx, dy, 1 - value);
        return;
    default: /* copy */
        image_set_pixel(target, dx, dy, value);
        return;
    }
}

static int source_pixel(const Image *image, int sx, int sy)
{
    if (sx < 0 || sy < 0 || sx >= image->bw || sy >= image->bh)
        return -1;
    if (image->mask &&
        !((image->mask[sy * image->rowbytes + sx / 8] >> (7 - sx % 8)) & 1))
        return -1;
    int value = (image->bitmap[sy * image->rowbytes + sx / 8] >> (7 - sx % 8)) & 1;
    return image->inverted ? 1 - value : value;
}

/* Blit a sub-region of the image's logical rect (sx0, sy0, sw, sh are
 * logical coordinates). The full-image blit is the special case of the
 * whole logical rect. */
static void blit_source_rect(DrawTarget *target, const Image *image,
                             int x, int y, int flip, int draw_mode,
                             int offset_x, int offset_y, int ignore_offset,
                             int clip_x, int clip_y, int clip_w, int clip_h,
                             int sx0, int sy0, int sw, int sh)
{
    for (int ly = sy0; ly < sy0 + sh; ly++) {
        for (int lx = sx0; lx < sx0 + sw; lx++) {
            int value = source_pixel(image, lx - image->xoff, ly - image->yoff);
            if (value < 0)
                continue;
            int dx = x + ((flip & 1) ? (sx0 + sw - 1 - lx) : (lx - sx0));
            int dy = y + ((flip & 2) ? (sy0 + sh - 1 - ly) : (ly - sy0));
            if (target->screen && !ignore_offset) {
                dx -= offset_x;
                dy -= offset_y;
            }
            put_pixel(target, dx, dy, value, draw_mode,
                      clip_x, clip_y, clip_w, clip_h);
        }
    }
}

void image_blit(DrawTarget *target, const Image *image, int x, int y,
                int flip, int draw_mode, int offset_x, int offset_y,
                int ignore_offset,
                int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (!target || !image)
        return;
    blit_source_rect(target, image, x, y, flip, draw_mode, offset_x, offset_y,
                     ignore_offset, clip_x, clip_y, clip_w, clip_h,
                     0, 0, image->width, image->height);
}

void image_blit_scaled(DrawTarget *target, const Image *image, int x, int y,
                       double xscale, double yscale, int draw_mode,
                       int offset_x, int offset_y, int ignore_offset,
                       int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (!target || !image || xscale <= 0 || yscale <= 0)
        return;
    int out_w = (int)(image->width * xscale);
    int out_h = (int)(image->height * yscale);
    for (int oy = 0; oy < out_h; oy++) {
        for (int ox = 0; ox < out_w; ox++) {
            int sx = (int)(ox / xscale) - image->xoff;
            int sy = (int)(oy / yscale) - image->yoff;
            int value = source_pixel(image, sx, sy);
            if (value < 0)
                continue;
            int dx = x + ox, dy = y + oy;
            if (target->screen && !ignore_offset) {
                dx -= offset_x;
                dy -= offset_y;
            }
            put_pixel(target, dx, dy, value, draw_mode,
                      clip_x, clip_y, clip_w, clip_h);
        }
    }
}

/* Nearest Q16.16 fixed-point value, rounding half up (portable for
 * negative values, where >> is implementation-defined). */
static int fixed_nearest(int32_t value)
{
    if (value >= 0)
        return (int)((value + 32768) >> 16);
    return -((int)((-value + 32767) >> 16));
}

void image_blit_rotated(DrawTarget *target, const Image *image, int x, int y,
                        double angle_deg, double xscale, double yscale,
                        int draw_mode,
                        int offset_x, int offset_y, int ignore_offset,
                        int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (!target || !image || xscale <= 0 || yscale <= 0)
        return;
    /* SDK: drawRotated draws the image centered at (x, y), rotated by
     * angle degrees (positive = clockwise on screen, y points down).
     * Inverse-map each destination pixel to the nearest source pixel;
     * source_pixel handles the trim offset, mask and setInverted.
     * The inner loop is Q16.16 fixed-point (integer adds only): the
     * Q2's soft-float MIPS CPU made the per-pixel double division cost
     * ~57 ms/frame on Frogger's 355x155 logo, starving the input poll.
     * The bounds are the AABB of the rotated corners, much tighter than
     * the half-diagonal circle bound for small angles. */
    double angle = angle_deg * 3.14159265358979323846 / 180.0;
    double cos_a = cos(angle), sin_a = sin(angle);
    double hw = image->width / 2.0, hh = image->height / 2.0;
    int rx = (int)ceil((fabs(cos_a) * hw + fabs(sin_a) * hh) * xscale);
    int ry = (int)ceil((fabs(sin_a) * hw + fabs(cos_a) * hh) * yscale);
    /* Inverse rotation + scale as Q16.16 coefficients. */
    int32_t ca = (int32_t)(cos_a / xscale * 65536.0);
    int32_t sa = (int32_t)(sin_a / xscale * 65536.0);
    int32_t cb = (int32_t)(-sin_a / yscale * 65536.0);
    int32_t db = (int32_t)(cos_a / yscale * 65536.0);
    int32_t cx = (int32_t)(((image->width - 1) / 2.0) * 65536.0);
    int32_t cy = (int32_t)(((image->height - 1) / 2.0) * 65536.0);
    for (int oy = -ry; oy <= ry; oy++) {
        /* Source coords of the row's first pixel, then step by (ca, cb). */
        int32_t xacc = ca * (-rx) + sa * oy + cx;
        int32_t yacc = cb * (-rx) + db * oy + cy;
        for (int ox = -rx; ox <= rx; ox++) {
            int value = source_pixel(image, fixed_nearest(xacc) - image->xoff,
                                     fixed_nearest(yacc) - image->yoff);
            if (value >= 0) {
                int ddx = x + ox, ddy = y + oy;
                if (target->screen && !ignore_offset) {
                    ddx -= offset_x;
                    ddy -= offset_y;
                }
                put_pixel(target, ddx, ddy, value, draw_mode,
                          clip_x, clip_y, clip_w, clip_h);
            }
            xacc += ca;
            yacc += cb;
        }
    }
}

void image_blit_faded(DrawTarget *target, const Image *image, int x, int y,
                      double alpha, int dither, int draw_mode,
                      int offset_x, int offset_y, int ignore_offset,
                      int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (!target || !image)
        return;
    for (int sy = 0; sy < image->bh; sy++) {
        for (int sx = 0; sx < image->bw; sx++) {
            int value = source_pixel(image, sx, sy);
            if (value < 0)
                continue;
            int dx = x + image->xoff + sx;
            int dy = y + image->yoff + sy;
            if (target->screen && !ignore_offset) {
                dx -= offset_x;
                dy -= offset_y;
            }
            if (image_dither_value(dither, dx, dy) >= alpha)
                continue;
            put_pixel(target, dx, dy, value, draw_mode,
                      clip_x, clip_y, clip_w, clip_h);
        }
    }
}

/* --- Lua bindings -------------------------------------------------------- */

/* The image / imagetable / tilemap / font / rect tables double as the
 * metatables of their userdata, so that getmetatable(obj) == the table
 * (CoreLibs/sprites.lua asserts this for images and tilemaps). The
 * tables are kept in the registry under the keys below. */

#define REG_IMAGE "q2.graphics.image.table"
#define REG_IMAGETABLE "q2.graphics.imagetable.table"
#define REG_TILEMAP "q2.graphics.tilemap.table"
#define REG_FONT "q2.graphics.font.table"
#define REG_RECT "q2.geometry.rect.table"
#define REG_POINT "q2.geometry.point.table"

typedef struct Font {
    char path[256];
    int height, width, leading;
    uint8_t *payload;
    size_t payload_size;
    int tracking; /* setTracking value; text is not rendered, so this
                     is stored and returned but has no visual effect */
} Font;

typedef struct Rect {
    double x, y, w, h;
} Rect;

typedef struct Point {
    double x, y;
} Point;

static void *check_udata(lua_State *lua, int index, const char *regkey,
                         const char *what)
{
    if (!lua_isuserdata(lua, index))
        luaL_argerror(lua, index, what);
    lua_getmetatable(lua, index);
    lua_getfield(lua, LUA_REGISTRYINDEX, regkey);
    int ok = lua_rawequal(lua, -1, -2);
    lua_pop(lua, 2);
    if (!ok)
        luaL_argerror(lua, index, what);
    return lua_touserdata(lua, index);
}

Image *image_check(lua_State *lua, int index)
{
    return check_udata(lua, index, REG_IMAGE, "image expected");
}

Imagetable *imagetable_check(lua_State *lua, int index)
{
    return check_udata(lua, index, REG_IMAGETABLE, "imagetable expected");
}

Tilemap *tilemap_check(lua_State *lua, int index)
{
    return check_udata(lua, index, REG_TILEMAP, "tilemap expected");
}

/* Coordinates arrive as floats from games (sprite bounds, camera
 * offsets); the SDK truncates them. */
static int check_coord(lua_State *lua, int index)
{
    return (int)luaL_checknumber(lua, index);
}

static int is_udata(lua_State *lua, int index, const char *regkey)
{
    if (!lua_isuserdata(lua, index))
        return 0;
    lua_getmetatable(lua, index);
    lua_getfield(lua, LUA_REGISTRYINDEX, regkey);
    int ok = lua_rawequal(lua, -1, -2);
    lua_pop(lua, 2);
    return ok;
}

int image_is(lua_State *lua, int index) { return is_udata(lua, index, REG_IMAGE); }
int imagetable_is(lua_State *lua, int index) { return is_udata(lua, index, REG_IMAGETABLE); }
int tilemap_is(lua_State *lua, int index) { return is_udata(lua, index, REG_TILEMAP); }

static void push_udata(lua_State *lua, const char *regkey, void *object, size_t size)
{
    void *ud = lua_newuserdata(lua, size);
    memcpy(ud, object, size);
    lua_getfield(lua, LUA_REGISTRYINDEX, regkey);
    lua_setmetatable(lua, -2);
}

void image_push(lua_State *lua, Image *image)
{
    Image *ud = lua_newuserdata(lua, sizeof *ud);
    *ud = *image;
    lua_getfield(lua, LUA_REGISTRYINDEX, REG_IMAGE);
    lua_setmetatable(lua, -2);
}

void imagetable_push(lua_State *lua, Imagetable *table)
{
    Imagetable *ud = lua_newuserdata(lua, sizeof *ud);
    *ud = *table;
    lua_getfield(lua, LUA_REGISTRYINDEX, REG_IMAGETABLE);
    lua_setmetatable(lua, -2);
}

void tilemap_push(lua_State *lua, Tilemap *tilemap)
{
    Tilemap *ud = lua_newuserdata(lua, sizeof *ud);
    *ud = *tilemap;
    lua_getfield(lua, LUA_REGISTRYINDEX, REG_TILEMAP);
    lua_setmetatable(lua, -2);
}

/* Current graphics state, from card.c. */
static void current_state(DrawTarget **target, int *mode, int *ox, int *oy,
                          int *cx, int *cy, int *cw, int *ch)
{
    *target = gfx_target();
    *mode = gfx_draw_mode();
    gfx_offset(ox, oy);
    gfx_clip(cx, cy, cw, ch);
}

/* --- playdate.graphics.image ------------------------------------------- */

static int lua_image_gc(lua_State *lua)
{
    image_free(image_check(lua, 1));
    return 0;
}

static int lua_image_new(lua_State *lua)
{
    if (lua_type(lua, 1) == LUA_TSTRING) {
        const char *path = lua_tostring(lua, 1);
        char full[1024];
        if (resolve_path(path, ".pdi", full, sizeof full) == 0) {
            Image *image = NULL;
            if (image_load_file(full, &image) == 0) {
                image_push(lua, image);
                free(image);
                return 1;
            }
            lua_pushnil(lua);
            lua_pushfstring(lua, "could not load image: %s", path);
            return 2;
        }
        /* Fall back to an image embedded in the bundle's main.pdz. */
        int is_table = 0;
        uint8_t *payload = NULL;
        size_t length = 0;
        if (bundle_image_payload(lua, path, &is_table, &payload, &length) == 0 &&
            !is_table) {
            Image image;
            if (image_decode_payload(payload, length, 0, 0, &image) == 0) {
                free(payload);
                image_push(lua, &image);
                return 1;
            }
            free(payload);
        }
        /* The SDK returns nil plus an error, it does not throw. */
        lua_pushnil(lua);
        lua_pushfstring(lua, "image file not found: %s", path);
        return 2;
    }
    int width = luaL_checkinteger(lua, 1);
    int height = luaL_checkinteger(lua, 2);
    int color = luaL_optinteger(lua, 3, 2);
    /* A 0 dimension is clamped to 1: the SDK's simulator rejects a
     * 0-size bitmap, but the device tolerates it, and games size
     * scratch images from text widths that can be 0 (an empty label).
     * A 1x1 image draws nothing. Negative or >4096 sizes still error
     * in image_new_blank. */
    if (width == 0)
        width = 1;
    if (height == 0)
        height = 1;
    Image *image = image_new_blank(width, height, color);
    if (!image)
        return luaL_error(lua, "invalid image size %dx%d", width, height);
    image_push(lua, image);
    free(image);
    return 1;
}

/* One pixel of an image, as getPixel reports it: 0 black, 1 white,
 * 2 clear (outside the image, outside the trimmed bitmap, or masked
 * out). Shared by getPixel and setMaskImage. */
static int image_pixel_value(const Image *image, int x, int y)
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

static int lua_image_get_pixel(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    int x = luaL_checkinteger(lua, 2);
    int y = luaL_checkinteger(lua, 3);
    if (x < 0 || y < 0 || x >= image->width || y >= image->height) {
        lua_pushnil(lua); /* outside the image */
        return 1;
    }
    lua_pushinteger(lua, image_pixel_value(image, x, y));
    return 1;
}

/* --- Image masks (Inside Playdate, "Image masks") ---------------------- */
/* Transparency is a 1-bit mask plane beside the bitmap (Image.mask,
 * NULL = fully opaque). XTRIS's achievement toasts use addMask(true),
 * hasMask() and setMaskImage() on their icon images. */

static int lua_image_add_mask(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    /* SDK: addMask([opaque]) adds a mask if the image doesn't already
     * have one. opaque true (or omitted) = fully white/opaque mask;
     * false = fully black/transparent. */
    int opaque = lua_isnoneornil(lua, 2) ? 1 : lua_toboolean(lua, 2);
    if (image->mask)
        return 0;
    size_t bytes = (size_t)image->bh * image->rowbytes;
    image->mask = malloc(bytes);
    if (!image->mask)
        return luaL_error(lua, "out of memory");
    memset(image->mask, opaque ? 0xff : 0x00, bytes);
    return 0;
}

static int lua_image_remove_mask(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    free(image->mask);
    image->mask = NULL;
    return 0;
}

static int lua_image_has_mask(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    lua_pushboolean(lua, image->mask != NULL);
    return 1;
}

static int lua_image_clear_mask(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    /* SDK: clearMask([opaque]) erases the mask; opaque (default
     * false) = entirely opaque, otherwise entirely transparent. */
    int opaque = lua_isnoneornil(lua, 2) ? 0 : lua_toboolean(lua, 2);
    if (!image->mask)
        return 0;
    memset(image->mask, opaque ? 0xff : 0x00,
           (size_t)image->bh * image->rowbytes);
    return 0;
}

static int lua_image_set_mask_image(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    Image *mask = image_check(lua, 2);
    if (mask->width != image->width || mask->height != image->height)
        return luaL_error(lua, "mask must be the same size as the image");
    size_t bytes = (size_t)image->bh * image->rowbytes;
    if (!image->mask) {
        image->mask = malloc(bytes);
        if (!image->mask)
            return luaL_error(lua, "out of memory");
    }
    /* The mask plane lives in the target's bitmap space (bw x bh at
     * xoff/yoff); sample the mask image at the matching logical
     * coordinates. A white mask pixel is opaque; black or clear is
     * transparent. The mask image's own mask is honoured. */
    for (int sy = 0; sy < image->bh; sy++) {
        for (int sx = 0; sx < image->bw; sx++) {
            int bit = image_pixel_value(mask, sx + image->xoff,
                                        sy + image->yoff) == 1;
            uint8_t *byte = &image->mask[sy * image->rowbytes + sx / 8];
            if (bit)
                *byte |= (uint8_t)(1 << (7 - sx % 8));
            else
                *byte &= (uint8_t)~(1 << (7 - sx % 8));
        }
    }
    return 0;
}

static int lua_image_get_mask_image(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    if (!image->mask) {
        lua_pushnil(lua);
        return 1;
    }
    /* The SDK returns a view sharing the mask data, so drawing into
     * it alters the original's mask. Ours returns a copy (same size
     * and trim, bitmap = the mask plane); drawing into it does not
     * affect the original. No game in the test set uses it. */
    Image *copy = calloc(1, sizeof *copy);
    if (!copy)
        return luaL_error(lua, "out of memory");
    copy->width = image->width;
    copy->height = image->height;
    copy->bw = image->bw;
    copy->bh = image->bh;
    copy->xoff = image->xoff;
    copy->yoff = image->yoff;
    copy->rowbytes = image->rowbytes;
    size_t bytes = (size_t)image->bh * image->rowbytes;
    copy->bitmap = malloc(bytes);
    if (!copy->bitmap) {
        free(copy);
        return luaL_error(lua, "out of memory");
    }
    memcpy(copy->bitmap, image->mask, bytes);
    image_push(lua, copy);
    free(copy);
    return 1;
}

static int lua_image_get_size(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    lua_pushinteger(lua, image->width);
    lua_pushinteger(lua, image->height);
    return 2;
}

static int lua_image_draw(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    int x = check_coord(lua, 2);
    int y = check_coord(lua, 3);
    int flip = luaL_optinteger(lua, 4, 0);
    DrawTarget *target;
    int mode, ox, oy, cx, cy, cw, ch;
    current_state(&target, &mode, &ox, &oy, &cx, &cy, &cw, &ch);
    if (lua_isuserdata(lua, 5) && is_udata(lua, 5, REG_RECT)) {
        Rect *r = check_udata(lua, 5, REG_RECT, "rect expected");
        blit_source_rect(target, image, x, y, flip, mode, ox, oy, 0,
                         cx, cy, cw, ch, (int)r->x, (int)r->y,
                         (int)r->w, (int)r->h);
    } else {
        image_blit(target, image, x, y, flip, mode, ox, oy, 0,
                   cx, cy, cw, ch);
    }
    return 0;
}

static int lua_image_draw_ignoring_offset(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    int x = check_coord(lua, 2);
    int y = check_coord(lua, 3);
    int flip = luaL_optinteger(lua, 4, 0);
    DrawTarget *target;
    int mode, ox, oy, cx, cy, cw, ch;
    current_state(&target, &mode, &ox, &oy, &cx, &cy, &cw, &ch);
    image_blit(target, image, x, y, flip, mode, ox, oy, 1,
               cx, cy, cw, ch);
    return 0;
}

static int lua_image_draw_scaled(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    int x = check_coord(lua, 2);
    int y = check_coord(lua, 3);
    double xscale = luaL_checknumber(lua, 4);
    double yscale = luaL_optnumber(lua, 5, xscale);
    DrawTarget *target;
    int mode, ox, oy, cx, cy, cw, ch;
    current_state(&target, &mode, &ox, &oy, &cx, &cy, &cw, &ch);
    image_blit_scaled(target, image, x, y, xscale, yscale, mode, ox, oy, 0,
                      cx, cy, cw, ch);
    return 0;
}

static int lua_image_draw_rotated(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    int x = check_coord(lua, 2);
    int y = check_coord(lua, 3);
    double angle = luaL_checknumber(lua, 4);
    double xscale = luaL_optnumber(lua, 5, 1.0);
    double yscale = luaL_optnumber(lua, 6, xscale);
    DrawTarget *target;
    int mode, ox, oy, cx, cy, cw, ch;
    current_state(&target, &mode, &ox, &oy, &cx, &cy, &cw, &ch);
    image_blit_rotated(target, image, x, y, angle, xscale, yscale, mode,
                       ox, oy, 0, cx, cy, cw, ch);
    return 0;
}

static int lua_image_draw_faded(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    int x = check_coord(lua, 2);
    int y = check_coord(lua, 3);
    double alpha = luaL_checknumber(lua, 4);
    int dither = luaL_checkinteger(lua, 5);
    DrawTarget *target;
    int mode, ox, oy, cx, cy, cw, ch;
    current_state(&target, &mode, &ox, &oy, &cx, &cy, &cw, &ch);
    image_blit_faded(target, image, x, y, alpha, dither, mode, ox, oy, 0,
                     cx, cy, cw, ch);
    return 0;
}

static int lua_image_set_inverted(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    image->inverted = lua_toboolean(lua, 2);
    return 0;
}

static int lua_image_get_inverted(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    lua_pushboolean(lua, image->inverted);
    return 1;
}

static int lua_image_clear(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    image_clear(image, luaL_optinteger(lua, 2, 1));
    return 0;
}

static int lua_image_copy(lua_State *lua)
{
    Image *copy = image_copy(image_check(lua, 1));
    if (!copy)
        return luaL_error(lua, "out of memory");
    image_push(lua, copy);
    free(copy);
    return 1;
}

static int lua_image_inverted_image(lua_State *lua)
{
    Image *copy = image_inverted_copy(image_check(lua, 1));
    if (!copy)
        return luaL_error(lua, "out of memory");
    image_push(lua, copy);
    free(copy);
    return 1;
}

static int lua_image_faded_image(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    double alpha = luaL_checknumber(lua, 2);
    int dither = luaL_checkinteger(lua, 3);
    Image *copy = image_faded_copy(image, alpha, dither);
    if (!copy)
        return luaL_error(lua, "out of memory");
    image_push(lua, copy);
    free(copy);
    return 1;
}

static int lua_image_scaled_image(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    double xscale = luaL_checknumber(lua, 2);
    double yscale = luaL_optnumber(lua, 3, xscale);
    Image *copy = image_scaled_copy(image, xscale, yscale);
    if (!copy)
        return luaL_error(lua, "invalid scale");
    image_push(lua, copy);
    free(copy);
    return 1;
}

static int lua_image_rotated_image(lua_State *lua)
{
    Image *image = image_check(lua, 1);
    double angle = luaL_checknumber(lua, 2);
    double xscale = luaL_optnumber(lua, 3, 1);
    double yscale = luaL_optnumber(lua, 4, xscale);
    /* Default centre is the image centre in the same convention as
     * drawRotated: (width-1)/2, (height-1)/2. */
    double centerx = luaL_optnumber(lua, 5, (image->width - 1) / 2.0);
    double centery = luaL_optnumber(lua, 6, (image->height - 1) / 2.0);
    Image *copy = image_rotated_copy(image, angle, xscale, yscale,
                                     centerx, centery);
    if (!copy)
        return luaL_error(lua, "invalid rotation");
    image_push(lua, copy);
    free(copy);
    return 1;
}

static const luaL_Reg image_methods[] = {
    {"new", lua_image_new},
    {"getSize", lua_image_get_size},
    {"getPixel", lua_image_get_pixel},
    {"draw", lua_image_draw},
    {"drawIgnoringOffset", lua_image_draw_ignoring_offset},
    {"drawScaled", lua_image_draw_scaled},
    {"drawRotated", lua_image_draw_rotated},
    {"drawFaded", lua_image_draw_faded},
    {"setInverted", lua_image_set_inverted},
    {"getInverted", lua_image_get_inverted},
    {"clear", lua_image_clear},
    {"copy", lua_image_copy},
    {"addMask", lua_image_add_mask},
    {"removeMask", lua_image_remove_mask},
    {"hasMask", lua_image_has_mask},
    {"clearMask", lua_image_clear_mask},
    {"setMaskImage", lua_image_set_mask_image},
    {"getMaskImage", lua_image_get_mask_image},
    {"invertedImage", lua_image_inverted_image},
    {"fadedImage", lua_image_faded_image},
    {"scaledImage", lua_image_scaled_image},
    {"rotatedImage", lua_image_rotated_image},
    {NULL, NULL},
};

/* --- playdate.graphics.imagetable -------------------------------------- */

static int lua_imagetable_gc(lua_State *lua)
{
    imagetable_free(imagetable_check(lua, 1));
    return 0;
}

static int lua_imagetable_new(lua_State *lua)
{
    const char *path = luaL_checkstring(lua, 1);
    char full[1024];
    if (resolve_path(path, ".pdt", full, sizeof full) == 0) {
        Imagetable *table = NULL;
        if (imagetable_load_file(full, &table) == 0) {
            imagetable_push(lua, table);
            free(table);
            return 1;
        }
        lua_pushnil(lua);
        lua_pushfstring(lua, "could not load imagetable: %s", path);
        return 2;
    }
    /* Fall back to an imagetable embedded in the bundle's main.pdz. */
    int is_table = 0;
    uint8_t *payload = NULL;
    size_t length = 0;
    if (bundle_image_payload(lua, path, &is_table, &payload, &length) == 0 &&
        is_table) {
        Imagetable table;
        if (imagetable_decode_payload(payload, length, 0, 0, &table) == 0) {
            free(payload);
            imagetable_push(lua, &table);
            return 1;
        }
        free(payload);
    }
    /* The SDK returns nil plus an error, it does not throw. */
    lua_pushnil(lua);
    lua_pushfstring(lua, "imagetable file not found: %s", path);
    return 2;
}

static int lua_imagetable_get_image(lua_State *lua)
{
    Imagetable *table = imagetable_check(lua, 1);
    int index = luaL_checkinteger(lua, 2);
    if (index < 1 || index > table->count) {
        lua_pushnil(lua);
        return 1;
    }
    Image *copy = image_copy(&table->cells[index - 1]);
    if (!copy)
        return luaL_error(lua, "out of memory");
    image_push(lua, copy);
    free(copy);
    return 1;
}

static int lua_imagetable_get_length(lua_State *lua)
{
    Imagetable *table = imagetable_check(lua, 1);
    lua_pushinteger(lua, table->count);
    return 1;
}

/* SDK: imagetable:drawImage(index, x, y [, flip]) — draws one cell at
 * (x, y), applying the current draw mode / offset / clip like
 * image:draw. XTRIS's stats page calls g_arrow_icons:drawImage(1, 330,
 * 84) from draw() and fails without it. Out-of-range index is an error,
 * matching the SDK (unlike getImage, which returns nil). */
static int lua_imagetable_draw_image(lua_State *lua)
{
    Imagetable *table = imagetable_check(lua, 1);
    int index = luaL_checkinteger(lua, 2);
    int x = check_coord(lua, 3);
    int y = check_coord(lua, 4);
    int flip = luaL_optinteger(lua, 5, 0);
    if (index < 1 || index > table->count)
        return luaL_error(lua, "imagetable index out of range");
    DrawTarget *target;
    int mode, ox, oy, cx, cy, cw, ch;
    current_state(&target, &mode, &ox, &oy, &cx, &cy, &cw, &ch);
    image_blit(target, &table->cells[index - 1], x, y, flip, mode, ox, oy, 0,
               cx, cy, cw, ch);
    return 0;
}

/* SDK: #imagetable is the cell count. CoreLibs/animation.lua does
 * `endFrame = #imagetable`, so the length operator must work on the
 * userdata (via the __len metamethod on its methods table). */
static int lua_imagetable_len(lua_State *lua)
{
    return lua_imagetable_get_length(lua);
}

static const luaL_Reg imagetable_methods[] = {
    {"new", lua_imagetable_new},
    {"getImage", lua_imagetable_get_image},
    {"drawImage", lua_imagetable_draw_image},
    {"getLength", lua_imagetable_get_length},
    {"__len", lua_imagetable_len},
    {NULL, NULL},
};

/* --- playdate.graphics.tilemap ----------------------------------------- */

static int lua_tilemap_gc(lua_State *lua)
{
    Tilemap *map = tilemap_check(lua, 1);
    free(map->tiles);
    map->tiles = NULL;
    return 0;
}

static int lua_tilemap_new(lua_State *lua)
{
    Tilemap map;
    memset(&map, 0, sizeof map);
    tilemap_push(lua, &map);
    return 1;
}

static int lua_tilemap_set_tiles(lua_State *lua)
{
    Tilemap *map = tilemap_check(lua, 1);
    luaL_checktype(lua, 2, LUA_TTABLE);
    int width = luaL_checkinteger(lua, 3);
    if (width <= 0)
        return luaL_error(lua, "tilemap width must be positive");
    size_t count = lua_rawlen(lua, 2);
    int height = (int)((count + (size_t)width - 1) / (size_t)width);
    int *tiles = calloc((size_t)width * (height ? height : 1), sizeof *tiles);
    if (!tiles)
        return luaL_error(lua, "out of memory");
    for (size_t i = 0; i < count; i++) {
        lua_rawgeti(lua, 2, (int)i + 1);
        tiles[i] = (int)lua_tointeger(lua, -1);
        lua_pop(lua, 1);
    }
    free(map->tiles);
    map->tiles = tiles;
    map->width = width;
    map->height = height;
    return 0;
}

static int lua_tilemap_set_image_table(lua_State *lua)
{
    Tilemap *map = tilemap_check(lua, 1);
    Imagetable *table = imagetable_check(lua, 2);
    map->table = table;
    /* Keep the imagetable userdata alive in the tilemap's uservalue. */
    lua_newtable(lua);
    lua_pushvalue(lua, 2);
    lua_setfield(lua, -2, "imagetable");
    lua_setuservalue(lua, 1);
    return 0;
}

static int tilemap_draw(lua_State *lua, int ignore_offset)
{
    Tilemap *map = tilemap_check(lua, 1);
    int x = check_coord(lua, 2);
    int y = check_coord(lua, 3);
    if (!map->table || map->width <= 0 || map->height <= 0)
        return 0;
    DrawTarget *target;
    int mode, ox, oy, cx, cy, cw, ch;
    current_state(&target, &mode, &ox, &oy, &cx, &cy, &cw, &ch);
    int cell_w = map->table->width;
    int cell_h = map->table->height;
    for (int row = 0; row < map->height; row++) {
        for (int col = 0; col < map->width; col++) {
            int value = map->tiles[row * map->width + col];
            if (value <= 0 || value > map->table->count)
                continue;
            image_blit(target, &map->table->cells[value - 1],
                       x + col * cell_w, y + row * cell_h, 0, mode,
                       ox, oy, ignore_offset, cx, cy, cw, ch);
        }
    }
    return 0;
}

static int lua_tilemap_draw(lua_State *lua) { return tilemap_draw(lua, 0); }
static int lua_tilemap_draw_ignoring_offset(lua_State *lua) { return tilemap_draw(lua, 1); }

static int lua_tilemap_get_size(lua_State *lua)
{
    Tilemap *map = tilemap_check(lua, 1);
    lua_pushinteger(lua, map->width);
    lua_pushinteger(lua, map->height);
    return 2;
}

static int lua_tilemap_get_pixel_size(lua_State *lua)
{
    Tilemap *map = tilemap_check(lua, 1);
    int cell_w = map->table ? map->table->width : 0;
    int cell_h = map->table ? map->table->height : 0;
    lua_pushinteger(lua, map->width * cell_w);
    lua_pushinteger(lua, map->height * cell_h);
    return 2;
}

static int lua_tilemap_get_tile_size(lua_State *lua)
{
    Tilemap *map = tilemap_check(lua, 1);
    lua_pushinteger(lua, map->table ? map->table->width : 0);
    lua_pushinteger(lua, map->table ? map->table->height : 0);
    return 2;
}

static int lua_tilemap_get_tiles(lua_State *lua)
{
    Tilemap *map = tilemap_check(lua, 1);
    lua_newtable(lua);
    int count = map->width * map->height;
    for (int i = 0; i < count; i++) {
        lua_pushinteger(lua, map->tiles[i]);
        lua_rawseti(lua, -2, i + 1);
    }
    lua_pushinteger(lua, map->width);
    return 2;
}

void rect_push(lua_State *lua, double x, double y, double w, double h)
{
    Rect *rect = lua_newuserdata(lua, sizeof *rect);
    rect->x = x;
    rect->y = y;
    rect->w = w;
    rect->h = h;
    lua_getfield(lua, LUA_REGISTRYINDEX, REG_RECT);
    lua_setmetatable(lua, -2);
}

static void push_rect(lua_State *lua, double x, double y, double w, double h)
{
    rect_push(lua, x, y, w, h);
}

/* getCollisionRects(emptyIDs): rects in TILE coordinates, merged into
 * maximal horizontal runs per row. Tile value 0 (no tile) is always
 * empty; values listed in emptyIDs are empty too. */
static int lua_tilemap_get_collision_rects(lua_State *lua)
{
    Tilemap *map = tilemap_check(lua, 1);
    lua_newtable(lua);
    int out = 0;
    for (int row = 0; row < map->height; row++) {
        int col = 0;
        while (col < map->width) {
            int value = map->tiles[row * map->width + col];
            int empty = value == 0;
            if (!empty && lua_istable(lua, 2)) {
                size_t n = lua_rawlen(lua, 2);
                for (size_t i = 1; i <= n && !empty; i++) {
                    lua_rawgeti(lua, 2, (int)i);
                    if (lua_tointeger(lua, -1) == value)
                        empty = 1;
                    lua_pop(lua, 1);
                }
            }
            if (empty) {
                col++;
                continue;
            }
            int start = col;
            while (col < map->width) {
                int v = map->tiles[row * map->width + col];
                int e = v == 0;
                if (!e && lua_istable(lua, 2)) {
                    size_t n = lua_rawlen(lua, 2);
                    for (size_t i = 1; i <= n && !e; i++) {
                        lua_rawgeti(lua, 2, (int)i);
                        if (lua_tointeger(lua, -1) == v)
                            e = 1;
                        lua_pop(lua, 1);
                    }
                }
                if (e)
                    break;
                col++;
            }
            push_rect(lua, start, row, col - start, 1);
            lua_rawseti(lua, -2, ++out);
        }
    }
    return 1;
}

static const luaL_Reg tilemap_methods[] = {
    {"new", lua_tilemap_new},
    {"setTiles", lua_tilemap_set_tiles},
    {"setImageTable", lua_tilemap_set_image_table},
    {"draw", lua_tilemap_draw},
    {"drawIgnoringOffset", lua_tilemap_draw_ignoring_offset},
    {"getSize", lua_tilemap_get_size},
    {"getPixelSize", lua_tilemap_get_pixel_size},
    {"getTileSize", lua_tilemap_get_tile_size},
    {"getTiles", lua_tilemap_get_tiles},
    {"getCollisionRects", lua_tilemap_get_collision_rects},
    {NULL, NULL},
};

/* --- playdate.graphics.font (stub: no font renderer) ------------------- */

static int lua_font_gc(lua_State *lua)
{
    Font *font = check_udata(lua, 1, REG_FONT, "font expected");
    free(font->payload);
    font->payload = NULL;
    return 0;
}

/* Keep the packed font payload; decode glyphs on demand. */
int font_push_payload(lua_State *lua, const char *name, const uint8_t *data, size_t size)
{
    if (size < 68 || !data[0] || !data[1])
        return 0;
    Font font = {0};
    snprintf(font.path, sizeof font.path, "%s", name);
    font.width = data[0];
    font.height = data[1];
    font.tracking = (int16_t)read_le16(data + 2);
    font.payload = malloc(size);
    if (!font.payload) return 0;
    memcpy(font.payload, data, size);
    font.payload_size = size;
    push_udata(lua, REG_FONT, &font, sizeof font);
    return 1;
}

static int lua_font_new(lua_State *lua)
{
    const char *path = luaL_checkstring(lua, 1);
    char full[1024];
    /* Compiled Playdate fonts are .pft ("Playdate FNT"); .fnt is the
     * uncompiled source format. Try both. */
    if (resolve_path(path, ".pft", full, sizeof full) != 0 &&
        resolve_path(path, ".fnt", full, sizeof full) != 0) {
        uint8_t *payload = NULL;
        size_t length;
        char name[1024];
        size_t n = strlen(path);
        if (n < sizeof name) {
            memcpy(name, path, n + 1);
            if (n >= 4 && (!strcmp(name + n - 4, ".pft") ||
                           !strcmp(name + n - 4, ".fnt")))
                name[n - 4] = 0;
            if (bundle_font_payload(lua, name, &payload, &length) == 0) {
                int ok = font_push_payload(lua, name, payload, length);
                free(payload);
                if (ok)
                    return 1;
                lua_pushnil(lua);
                lua_pushfstring(lua, "invalid embedded font: %s", path);
                return 2;
            }
        }
        lua_pushnil(lua);
        lua_pushfstring(lua, "font file not found: %s", path);
        return 2;
    }
    size_t size;
    uint8_t *raw = read_file(full, &size);
    if (raw && size >= 16 && !memcmp(raw, "Playdate FNT", 12)) {
        int ok = 0;
        if (read_le32(raw + 12) & 0x80000000u) {
            if (size >= 28) {
                unsigned long len = read_le32(raw + 16), actual = len;
                uint8_t *data = len && len <= 8 * 1024 * 1024 ? malloc(len) : NULL;
                if (data && uncompress(data, &actual, raw + 28, size - 28) == Z_OK && actual == len)
                    ok = font_push_payload(lua, path, data, len);
                free(data);
            }
        } else {
            ok = font_push_payload(lua, path, raw + 16, size - 16);
        }
        free(raw);
        if (ok) return 1;
        lua_pushnil(lua);
        lua_pushliteral(lua, "invalid compiled font");
        return 2;
    }
    free(raw);
    Font font = {0};
    snprintf(font.path, sizeof font.path, "%s", path);
    push_udata(lua, REG_FONT, &font, sizeof font);
    return 1;
}

static int lua_font_get_height(lua_State *lua)
{
    Font *font = check_udata(lua, 1, REG_FONT, "font expected");
    lua_pushinteger(lua, font->height ? font->height : 16);
    return 1;
}

static int lua_font_get_leading(lua_State *lua)
{
    Font *font = check_udata(lua, 1, REG_FONT, "font expected");
    lua_pushinteger(lua, font->leading);
    return 1;
}

static int lua_font_set_leading(lua_State *lua)
{
    Font *font = check_udata(lua, 1, REG_FONT, "font expected");
    font->leading = luaL_checkinteger(lua, 2);
    return 0;
}

/* SDK: font:setTracking(tracking) / font:getTracking(). XTRIS's
 * achievement toasts call m.fonts.status:setTracking(1) at load.
 * Text is not rendered (drawText is a stub), so the value is stored
 * and returned but has no visual effect. */
static int lua_font_set_tracking(lua_State *lua)
{
    Font *font = check_udata(lua, 1, REG_FONT, "font expected");
    font->tracking = (int)luaL_checkinteger(lua, 2);
    return 0;
}

static int lua_font_get_tracking(lua_State *lua)
{
    Font *font = check_udata(lua, 1, REG_FONT, "font expected");
    lua_pushinteger(lua, font->tracking);
    return 1;
}

/* Text measurement is an estimate: fonts are not rendered, so there
 * are no real glyph metrics. 8 px per character at the 16 px font
 * height is close to the system font's average advance, and it keeps
 * text-sizing code (image.new(getTextWidth(t), h)) from producing a
 * 0-width image, which the SDK rejects. Counts UTF-8 code points,
 * not bytes. */
int text_width_estimate(const char *s)
{
    size_t chars = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xc0) != 0x80)
            chars++;
    return (int)(chars * 8);
}

/* PFT pages use cumulative 32-bit page offsets and 16-bit glyph offsets.
 * Both tables are relative to the end of their respective offset tables. */
static unsigned font_bit(const uint8_t *bits, unsigned n)
{
    return (bits[n / 8] >> (n % 8)) & 1u;
}
static const uint8_t *font_glyph(const Font *font, unsigned cp, size_t *length)
{
    const uint8_t *p = font->payload;
    size_t n = font->payload_size;
    unsigned page = cp >> 8, ch = cp & 255, count = 0, index = 0;
    if (!p || n < 68 || page >= 512 || !font_bit(p+4,page)) return NULL;
    for (unsigned j=0;j<512;j++) {
        if (j<page) index += font_bit(p+4,j);
        count += font_bit(p+4,j);
    }
    size_t start=68+4u*count;
    if (start>n) return NULL;
    size_t lo=index?read_le32(p+68+4u*(index-1)):0;
    size_t hi=read_le32(p+68+4u*index);
    if (lo>hi || hi>n-start || hi-lo<36) return NULL;
    p+=start+lo; n=hi-lo;
    if (!font_bit(p+4,ch)) return NULL;
    count=0; index=0;
    for(unsigned j=0;j<256;j++) {
        if(j<ch) index+=font_bit(p+4,j);
        count+=font_bit(p+4,j);
    }
    start=(36+2u*count+3)&~(size_t)3;
    if(start>n) return NULL;
    lo=index?read_le16(p+36+2u*(index-1)):0;
    hi=read_le16(p+36+2u*index);
    if(lo>hi || hi>n-start || hi-lo<4) return NULL;
    *length=hi-lo;
    return p+start+lo;
}
static unsigned font_next(const unsigned char **s)
{
    unsigned c=*(*s)++;
    if(c<128) return c;
    unsigned n=c>=240&&c<=244?3:c>=224&&c<=239?2:c>=194&&c<=223?1:0;
    if(!n) return '?';
    c &= (1u<<(6-n))-1;
    while(n--) {
        unsigned d=**s;
        if((d&192)!=128) return '?';
        ++*s; c=(c<<6)|(d&63);
    }
    return c;
}
static int font_layout(Font *font, const char *text, int x, int y, int draw, int *height)
{
    int line=0, widest=0, lines=1;
    int fh=font->height?font->height:16;
    const unsigned char *s=(const unsigned char *)text;
    for(unsigned limit=0;*s && limit<8192;limit++) {
        unsigned cp=font_next(&s);
        if(cp=='\n') { if(line>widest)widest=line; line=0; lines++; continue; }
        size_t len=0;
        const uint8_t *g=font_glyph(font,cp,&len);
        if(!g && cp!=' ')g=font_glyph(font,'?',&len);
        int advance=g?g[0]:(font->width?font->width/2:8);
        if(draw && g) {
            size_t off=(4+2u*g[1]+3)&~(size_t)3;
            off+=4u*read_le16(g+2);
            if(off<=len && len-off>=16) {
                const uint8_t *p=g+off;
                unsigned w=read_le16(p), h=read_le16(p+2), stride=read_le16(p+4);
                unsigned left=read_le16(p+6), top=read_le16(p+10), flags=read_le16(p+14);
                size_t bytes=(size_t)stride*h;
                if(w && h && w<=512 && h<=512 && stride>=(w+7)/8 &&
                   bytes<=len-off-16 && (!(flags&3) || bytes<=(len-off-16)/2)) {
                    Image glyph={0};
                    glyph.width=w+left+read_le16(p+8);
                    glyph.height=h+top+read_le16(p+12);
                    glyph.bw=w; glyph.bh=h; glyph.xoff=left; glyph.yoff=top;
                    glyph.rowbytes=stride; glyph.bitmap=(uint8_t *)p+16;
                    glyph.mask=(flags&3)?glyph.bitmap+bytes:NULL;
                    DrawTarget *target; int mode,ox,oy,cx,cy,cw,ch;
                    current_state(&target,&mode,&ox,&oy,&cx,&cy,&cw,&ch);
                    image_blit(target,&glyph,x+line,y+(lines-1)*(fh+font->leading),
                               0,mode,ox,oy,0,cx,cy,cw,ch);
                }
            }
        }
        line+=advance+font->tracking;
    }
    if(line>widest)widest=line;
    if(height)*height=lines*fh+(lines-1)*font->leading;
    return widest;
}
static int lua_font_get_text_width(lua_State *lua)
{
    Font *font=check_udata(lua,1,REG_FONT,"font expected");
    lua_pushinteger(lua,font_layout(font,luaL_checkstring(lua,2),0,0,0,NULL));
    return 1;
}
static int lua_font_get_text_size(lua_State *lua)
{
    Font *font=check_udata(lua,1,REG_FONT,"font expected");
    int height;
    int width=font_layout(font,luaL_checkstring(lua,2),0,0,0,&height);
    lua_pushinteger(lua,width); lua_pushinteger(lua,height); return 2;
}
static int lua_font_draw_text(lua_State *lua)
{
    Font *font=check_udata(lua,1,REG_FONT,"font expected");
    const char *s=luaL_checkstring(lua,2);
    int x=check_coord(lua,3), y=check_coord(lua,4);
    lua_pushinteger(lua,font_layout(font,s,x,y,1,NULL)); return 1;
}
int font_current_text(lua_State *lua, int draw)
{
    const char *s=luaL_checkstring(lua,1);
    int x=draw?check_coord(lua,2):0, y=draw?check_coord(lua,3):0;
    lua_getfield(lua,LUA_REGISTRYINDEX,"q2.current.font");
    Font *font=check_udata(lua,-1,REG_FONT,"font expected");
    int height;
    int width=font_layout(font,s,x,y,draw,&height);
    lua_pop(lua,1);
    lua_pushinteger(lua,width);
    if(draw)return 1;
    lua_pushinteger(lua,height); return 2;
}

static const luaL_Reg font_methods[] = {
    {"new", lua_font_new},
    {"getHeight", lua_font_get_height},
    {"getLeading", lua_font_get_leading},
    {"setLeading", lua_font_set_leading},
    {"setTracking", lua_font_set_tracking},
    {"getTracking", lua_font_get_tracking},
    {"getTextWidth", lua_font_get_text_width},
    {"getTextSize", lua_font_get_text_size},
    {"drawText", lua_font_draw_text},
    {NULL, NULL},
};

/* --- playdate.geometry.rect -------------------------------------------- */

static int lua_rect_gc(lua_State *lua)
{
    (void)lua;
    return 0;
}

static int lua_rect_new(lua_State *lua)
{
    double x = luaL_checknumber(lua, 1);
    double y = luaL_checknumber(lua, 2);
    double w = luaL_checknumber(lua, 3);
    double h = luaL_checknumber(lua, 4);
    push_rect(lua, x, y, w, h);
    return 1;
}

static int lua_rect_offset_by(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    double dx = luaL_checknumber(lua, 2);
    double dy = luaL_checknumber(lua, 3);
    push_rect(lua, rect->x + dx, rect->y + dy, rect->w, rect->h);
    return 1;
}

static int lua_rect_contains_point(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    double x, y;
    if (lua_isuserdata(lua, 2)) {
        /* A point is a table {x=, y=} in our stubs. */
        lua_pushstring(lua, "x");
        lua_gettable(lua, 2);
        x = lua_tonumber(lua, -1);
        lua_pop(lua, 1);
        lua_pushstring(lua, "y");
        lua_gettable(lua, 2);
        y = lua_tonumber(lua, -1);
        lua_pop(lua, 1);
    } else {
        x = luaL_checknumber(lua, 2);
        y = luaL_checknumber(lua, 3);
    }
    lua_pushboolean(lua, x >= rect->x && x < rect->x + rect->w &&
                         y >= rect->y && y < rect->y + rect->h);
    return 1;
}

static int lua_rect_contains_rect(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    double x, y, w, h;
    if (lua_isuserdata(lua, 2)) {
        Rect *other = check_udata(lua, 2, REG_RECT, "rect expected");
        x = other->x;
        y = other->y;
        w = other->w;
        h = other->h;
    } else {
        x = luaL_checknumber(lua, 2);
        y = luaL_checknumber(lua, 3);
        w = luaL_checknumber(lua, 4);
        h = luaL_checknumber(lua, 5);
    }
    lua_pushboolean(lua, x >= rect->x && y >= rect->y &&
                         x + w <= rect->x + rect->w &&
                         y + h <= rect->y + rect->h);
    return 1;
}

static int lua_rect_intersects(lua_State *lua)
{
    Rect *a = check_udata(lua, 1, REG_RECT, "rect expected");
    Rect *b = check_udata(lua, 2, REG_RECT, "rect expected");
    lua_pushboolean(lua, a->x < b->x + b->w && b->x < a->x + a->w &&
                         a->y < b->y + b->h && b->y < a->y + a->h);
    return 1;
}

static int lua_rect_union(lua_State *lua)
{
    Rect *a = check_udata(lua, 1, REG_RECT, "rect expected");
    Rect *b = check_udata(lua, 2, REG_RECT, "rect expected");
    double x = a->x < b->x ? a->x : b->x;
    double y = a->y < b->y ? a->y : b->y;
    double x2 = (a->x + a->w) > (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    double y2 = (a->y + a->h) > (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
    push_rect(lua, x, y, x2 - x, y2 - y);
    return 1;
}

static int lua_rect_fast_union(lua_State *lua)
{
    double x1 = luaL_checknumber(lua, 1);
    double y1 = luaL_checknumber(lua, 2);
    double w1 = luaL_checknumber(lua, 3);
    double h1 = luaL_checknumber(lua, 4);
    double x2 = luaL_checknumber(lua, 5);
    double y2 = luaL_checknumber(lua, 6);
    double w2 = luaL_checknumber(lua, 7);
    double h2 = luaL_checknumber(lua, 8);
    double x = x1 < x2 ? x1 : x2;
    double y = y1 < y2 ? y1 : y2;
    double xe = (x1 + w1) > (x2 + w2) ? (x1 + w1) : (x2 + w2);
    double ye = (y1 + h1) > (y2 + h2) ? (y1 + h1) : (y2 + h2);
    lua_pushnumber(lua, x);
    lua_pushnumber(lua, y);
    lua_pushnumber(lua, xe - x);
    lua_pushnumber(lua, ye - y);
    return 4;
}

static int lua_rect_inset(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    double dx = luaL_checknumber(lua, 2);
    double dy = luaL_checknumber(lua, 3);
    rect->x += dx;
    rect->y += dy;
    rect->w -= 2 * dx;
    rect->h -= 2 * dy;
    lua_pushvalue(lua, 1);
    return 1;
}

static int lua_rect_inset_by(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    double dx = luaL_checknumber(lua, 2);
    double dy = luaL_checknumber(lua, 3);
    push_rect(lua, rect->x + dx, rect->y + dy,
              rect->w - 2 * dx, rect->h - 2 * dy);
    return 1;
}

static int lua_rect_offset(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    rect->x += luaL_checknumber(lua, 2);
    rect->y += luaL_checknumber(lua, 3);
    lua_pushvalue(lua, 1);
    return 1;
}

static int lua_rect_copy(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    push_rect(lua, rect->x, rect->y, rect->w, rect->h);
    return 1;
}

static int lua_rect_unpack(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    lua_pushnumber(lua, rect->x);
    lua_pushnumber(lua, rect->y);
    lua_pushnumber(lua, rect->w);
    lua_pushnumber(lua, rect->h);
    return 4;
}

static int lua_rect_is_empty(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    lua_pushboolean(lua, rect->w <= 0 || rect->h <= 0);
    return 1;
}

static int lua_rect_is_equal(lua_State *lua)
{
    Rect *a = check_udata(lua, 1, REG_RECT, "rect expected");
    Rect *b = check_udata(lua, 2, REG_RECT, "rect expected");
    lua_pushboolean(lua, a->x == b->x && a->y == b->y &&
                         a->w == b->w && a->h == b->h);
    return 1;
}

static int lua_rect_intersection(lua_State *lua)
{
    Rect *a = check_udata(lua, 1, REG_RECT, "rect expected");
    Rect *b = check_udata(lua, 2, REG_RECT, "rect expected");
    double x = a->x > b->x ? a->x : b->x;
    double y = a->y > b->y ? a->y : b->y;
    double x2 = (a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    double y2 = (a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
    if (x2 <= x || y2 <= y) {
        lua_pushnil(lua);
        return 1;
    }
    push_rect(lua, x, y, x2 - x, y2 - y);
    return 1;
}

static int lua_rect_fast_intersection(lua_State *lua)
{
    double x1 = luaL_checknumber(lua, 1);
    double y1 = luaL_checknumber(lua, 2);
    double w1 = luaL_checknumber(lua, 3);
    double h1 = luaL_checknumber(lua, 4);
    double x2 = luaL_checknumber(lua, 5);
    double y2 = luaL_checknumber(lua, 6);
    double w2 = luaL_checknumber(lua, 7);
    double h2 = luaL_checknumber(lua, 8);
    double x = x1 > x2 ? x1 : x2;
    double y = y1 > y2 ? y1 : y2;
    double xe = (x1 + w1) < (x2 + w2) ? (x1 + w1) : (x2 + w2);
    double ye = (y1 + h1) < (y2 + h2) ? (y1 + h1) : (y2 + h2);
    if (xe <= x || ye <= y)
        return 0;
    lua_pushnumber(lua, x);
    lua_pushnumber(lua, y);
    lua_pushnumber(lua, xe - x);
    lua_pushnumber(lua, ye - y);
    return 4;
}

static int lua_rect_center_point(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    lua_newtable(lua);
    lua_pushnumber(lua, rect->x + rect->w / 2);
    lua_setfield(lua, -2, "x");
    lua_pushnumber(lua, rect->y + rect->h / 2);
    lua_setfield(lua, -2, "y");
    return 1;
}

static int lua_rect_index(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    const char *key = luaL_checkstring(lua, 2);
    if (strcmp(key, "x") == 0) {
        lua_pushnumber(lua, rect->x);
        return 1;
    }
    if (strcmp(key, "y") == 0) {
        lua_pushnumber(lua, rect->y);
        return 1;
    }
    if (strcmp(key, "width") == 0 || strcmp(key, "w") == 0) {
        lua_pushnumber(lua, rect->w);
        return 1;
    }
    if (strcmp(key, "height") == 0 || strcmp(key, "h") == 0) {
        lua_pushnumber(lua, rect->h);
        return 1;
    }
    lua_getfield(lua, LUA_REGISTRYINDEX, REG_RECT);
    lua_pushvalue(lua, 2);
    lua_gettable(lua, -2);
    return 1;
}

static int lua_rect_newindex(lua_State *lua)
{
    Rect *rect = check_udata(lua, 1, REG_RECT, "rect expected");
    const char *key = luaL_checkstring(lua, 2);
    double value = luaL_checknumber(lua, 3);
    if (strcmp(key, "x") == 0)
        rect->x = value;
    else if (strcmp(key, "y") == 0)
        rect->y = value;
    else if (strcmp(key, "width") == 0 || strcmp(key, "w") == 0)
        rect->w = value;
    else if (strcmp(key, "height") == 0 || strcmp(key, "h") == 0)
        rect->h = value;
    else
        return luaL_error(lua, "unknown rect field %s", key);
    return 0;
}

static const luaL_Reg rect_methods[] = {
    {"new", lua_rect_new},
    {"offsetBy", lua_rect_offset_by},
    {"offset", lua_rect_offset},
    {"inset", lua_rect_inset},
    {"insetBy", lua_rect_inset_by},
    {"copy", lua_rect_copy},
    {"unpack", lua_rect_unpack},
    {"isEmpty", lua_rect_is_empty},
    {"isEqual", lua_rect_is_equal},
    {"intersects", lua_rect_intersects},
    {"union", lua_rect_union},
    {"fast_union", lua_rect_fast_union},
    {"containsPoint", lua_rect_contains_point},
    {"containsRect", lua_rect_contains_rect},
    {"intersection", lua_rect_intersection},
    {"fast_intersection", lua_rect_fast_intersection},
    {"centerPoint", lua_rect_center_point},
    {NULL, NULL},
};

/* --- playdate.geometry.point ------------------------------------------- */
/* SDK: playdate.geometry.point.new(x, y); fields x, y (mutable);
 * methods unpack() and offsetBy(dx, dy) (returns a new point).
 * XTRIS's game manager keeps its draw offset as a point and calls
 * draw_offset:unpack() every frame. */

static void point_push(lua_State *lua, double x, double y)
{
    Point *point = lua_newuserdata(lua, sizeof *point);
    point->x = x;
    point->y = y;
    lua_getfield(lua, LUA_REGISTRYINDEX, REG_POINT);
    lua_setmetatable(lua, -2);
}

static int lua_point_gc(lua_State *lua)
{
    (void)lua;
    return 0;
}

static int lua_point_new(lua_State *lua)
{
    double x = luaL_checknumber(lua, 1);
    double y = luaL_checknumber(lua, 2);
    point_push(lua, x, y);
    return 1;
}

static int lua_point_unpack(lua_State *lua)
{
    Point *point = check_udata(lua, 1, REG_POINT, "point expected");
    lua_pushnumber(lua, point->x);
    lua_pushnumber(lua, point->y);
    return 2;
}

static int lua_point_offset_by(lua_State *lua)
{
    Point *point = check_udata(lua, 1, REG_POINT, "point expected");
    double dx = luaL_checknumber(lua, 2);
    double dy = luaL_checknumber(lua, 3);
    point_push(lua, point->x + dx, point->y + dy);
    return 1;
}

static int lua_point_copy(lua_State *lua)
{
    Point *point = check_udata(lua, 1, REG_POINT, "point expected");
    point_push(lua, point->x, point->y);
    return 1;
}

/* Arithmetic metamethods. The SDK's point type supports these, and
 * CoreLibs/animator.lua interpolates point values with
 * start + (end - start) * t, so a game animating a point (XTRIS's menu
 * slide) fails without them. Add/sub are point-point; mul/div take a
 * number on either side; unm negates; eq compares coordinates. */
static Point *point_arg(lua_State *lua, int index)
{
    if (lua_isuserdata(lua, index))
        return check_udata(lua, index, REG_POINT, "point expected");
    return NULL;
}

static int lua_point_add(lua_State *lua)
{
    Point *a = point_arg(lua, 1), *b = point_arg(lua, 2);
    if (!a || !b)
        return luaL_error(lua, "point addition needs two points");
    point_push(lua, a->x + b->x, a->y + b->y);
    return 1;
}

static int lua_point_sub(lua_State *lua)
{
    Point *a = point_arg(lua, 1), *b = point_arg(lua, 2);
    if (!a || !b)
        return luaL_error(lua, "point subtraction needs two points");
    point_push(lua, a->x - b->x, a->y - b->y);
    return 1;
}

static int lua_point_mul(lua_State *lua)
{
    Point *point = point_arg(lua, 1);
    if (point) {
        double n = luaL_checknumber(lua, 2);
        point_push(lua, point->x * n, point->y * n);
        return 1;
    }
    point = point_arg(lua, 2);
    if (point) {
        double n = luaL_checknumber(lua, 1);
        point_push(lua, point->x * n, point->y * n);
        return 1;
    }
    return luaL_error(lua, "point multiplication needs a point and a number");
}

static int lua_point_div(lua_State *lua)
{
    Point *point = point_arg(lua, 1);
    if (!point)
        return luaL_error(lua, "point division needs a point and a number");
    double n = luaL_checknumber(lua, 2);
    point_push(lua, point->x / n, point->y / n);
    return 1;
}

static int lua_point_unm(lua_State *lua)
{
    Point *point = check_udata(lua, 1, REG_POINT, "point expected");
    point_push(lua, -point->x, -point->y);
    return 1;
}

static int lua_point_eq(lua_State *lua)
{
    Point *a = point_arg(lua, 1), *b = point_arg(lua, 2);
    lua_pushboolean(lua, a && b && a->x == b->x && a->y == b->y);
    return 1;
}

static int lua_point_index(lua_State *lua)
{
    Point *point = check_udata(lua, 1, REG_POINT, "point expected");
    const char *key = luaL_checkstring(lua, 2);
    if (strcmp(key, "x") == 0) {
        lua_pushnumber(lua, point->x);
        return 1;
    }
    if (strcmp(key, "y") == 0) {
        lua_pushnumber(lua, point->y);
        return 1;
    }
    lua_getfield(lua, LUA_REGISTRYINDEX, REG_POINT);
    lua_pushvalue(lua, 2);
    lua_gettable(lua, -2);
    return 1;
}

static int lua_point_newindex(lua_State *lua)
{
    Point *point = check_udata(lua, 1, REG_POINT, "point expected");
    const char *key = luaL_checkstring(lua, 2);
    double value = luaL_checknumber(lua, 3);
    if (strcmp(key, "x") == 0)
        point->x = value;
    else if (strcmp(key, "y") == 0)
        point->y = value;
    else
        return luaL_error(lua, "unknown point field %s", key);
    return 0;
}

static const luaL_Reg point_methods[] = {
    {"new", lua_point_new},
    {"unpack", lua_point_unpack},
    {"offsetBy", lua_point_offset_by},
    {"copy", lua_point_copy},
    {NULL, NULL},
};

/* --- Installation -------------------------------------------------------- */

/* Make a table that doubles as a metatable: __index = itself, plus __gc. */
static void finish_table(lua_State *lua, lua_CFunction gc)
{
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, gc);
    lua_setfield(lua, -2, "__gc");
}

static void set_int(lua_State *lua, const char *name, int value)
{
    lua_pushinteger(lua, value);
    lua_setfield(lua, -2, name);
}

/* The playdate table is on top of the stack on entry and is left there. */
void image_install(lua_State *lua)
{
    lua_getfield(lua, -1, "graphics");
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_pushvalue(lua, -1);
        lua_setfield(lua, -3, "graphics");
    }

    /* graphics.image */
    lua_newtable(lua);
    luaL_setfuncs(lua, image_methods, 0);
    set_int(lua, "kImageUnflipped", 0);
    set_int(lua, "kImageFlippedX", 1);
    set_int(lua, "kImageFlippedY", 2);
    set_int(lua, "kImageFlippedXY", 3);
    set_int(lua, "kDitherTypeNone", 0);
    set_int(lua, "kDitherTypeDiagonalLine", 1);
    set_int(lua, "kDitherTypeVerticalLine", 2);
    set_int(lua, "kDitherTypeHorizontalLine", 3);
    set_int(lua, "kDitherTypeScreen", 4);
    set_int(lua, "kDitherTypeBayer2x2", 5);
    set_int(lua, "kDitherTypeBayer4x4", 6);
    set_int(lua, "kDitherTypeBayer8x8", 7);
    finish_table(lua, lua_image_gc);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_REGISTRYINDEX, REG_IMAGE);
    lua_setfield(lua, -2, "image");

    /* graphics.imagetable */
    lua_newtable(lua);
    luaL_setfuncs(lua, imagetable_methods, 0);
    finish_table(lua, lua_imagetable_gc);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_REGISTRYINDEX, REG_IMAGETABLE);
    lua_setfield(lua, -2, "imagetable");

    /* graphics.tilemap */
    lua_newtable(lua);
    luaL_setfuncs(lua, tilemap_methods, 0);
    finish_table(lua, lua_tilemap_gc);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_REGISTRYINDEX, REG_TILEMAP);
    lua_setfield(lua, -2, "tilemap");

    /* graphics.font */
    lua_newtable(lua);
    luaL_setfuncs(lua, font_methods, 0);
    set_int(lua, "kVariantNormal", 0);
    set_int(lua, "kVariantBold", 1);
    set_int(lua, "kVariantItalic", 2);
    finish_table(lua, lua_font_gc);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_REGISTRYINDEX, REG_FONT);
    lua_setfield(lua, -2, "font");

    /* Default font (a stub: no glyphs are rendered). */
    {
        Font default_font;
        memset(&default_font, 0, sizeof default_font);
        push_udata(lua, REG_FONT, &default_font, sizeof default_font);
        lua_setfield(lua, LUA_REGISTRYINDEX, "q2.default.font");
        lua_getfield(lua, LUA_REGISTRYINDEX, "q2.default.font");
        lua_setfield(lua, LUA_REGISTRYINDEX, "q2.current.font");
    }

    lua_pop(lua, 1); /* graphics */

    /* playdate.geometry.rect */
    lua_newtable(lua);
    lua_newtable(lua);
    luaL_setfuncs(lua, rect_methods, 0);
    lua_pushcfunction(lua, lua_rect_index);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, lua_rect_newindex);
    lua_setfield(lua, -2, "__newindex");
    lua_pushcfunction(lua, lua_rect_gc);
    lua_setfield(lua, -2, "__gc");
    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_REGISTRYINDEX, REG_RECT);
    lua_setfield(lua, -2, "rect");

    /* playdate.geometry.point */
    lua_newtable(lua);
    luaL_setfuncs(lua, point_methods, 0);
    lua_pushcfunction(lua, lua_point_index);
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, lua_point_newindex);
    lua_setfield(lua, -2, "__newindex");
    lua_pushcfunction(lua, lua_point_gc);
    lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, lua_point_add);
    lua_setfield(lua, -2, "__add");
    lua_pushcfunction(lua, lua_point_sub);
    lua_setfield(lua, -2, "__sub");
    lua_pushcfunction(lua, lua_point_mul);
    lua_setfield(lua, -2, "__mul");
    lua_pushcfunction(lua, lua_point_div);
    lua_setfield(lua, -2, "__div");
    lua_pushcfunction(lua, lua_point_unm);
    lua_setfield(lua, -2, "__unm");
    lua_pushcfunction(lua, lua_point_eq);
    lua_setfield(lua, -2, "__eq");
    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_REGISTRYINDEX, REG_POINT);
    lua_setfield(lua, -2, "point");
    lua_setfield(lua, -2, "geometry");
}
