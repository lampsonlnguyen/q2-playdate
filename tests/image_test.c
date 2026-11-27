/* Decoder test for the PDI/PDT format. Usage:
 *   image-test file.pdi [file.pdt ...]
 * For each file, prints the logical size, cell count and a pixel dump:
 * '#' = white, '.' = black, ' ' = transparent (masked out or outside
 * a trimmed bitmap). Exits non-zero on any decode failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "lua.h"

/* Stubs for the card.c graphics state (unused by the decoder). */
DrawTarget *gfx_target(void) { return NULL; }
int gfx_draw_mode(void) { return 0; }
void gfx_offset(int *x, int *y) { *x = *y = 0; }
void gfx_clip(int *x, int *y, int *w, int *h) { *x = *y = 0; *w = *h = -1; }
int bundle_image_payload(lua_State *lua, const char *name, int *is_table,
                         uint8_t **payload, size_t *payload_len)
{
    (void)lua; (void)name; (void)is_table; (void)payload; (void)payload_len;
    return -1;
}

static void dump_image(const Image *image)
{
    for (int y = 0; y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            int sx = x - image->xoff, sy = y - image->yoff;
            if (sx < 0 || sy < 0 || sx >= image->bw || sy >= image->bh) {
                putchar(' ');
                continue;
            }
            if (image->mask &&
                !((image->mask[sy * image->rowbytes + sx / 8] >> (7 - sx % 8)) & 1)) {
                putchar(' ');
                continue;
            }
            putchar((image->bitmap[sy * image->rowbytes + sx / 8] >> (7 - sx % 8)) & 1
                        ? '#' : '.');
        }
        putchar('\n');
    }
}

static int has_suffix(const char *path, const char *suffix)
{
    size_t a = 0, b = 0;
    while (path[a])
        a++;
    while (suffix[b])
        b++;
    return a >= b && strcmp(path + a - b, suffix) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.pdi [file.pdt ...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        const char *base = strrchr(argv[i], '/');
        printf("== %s\n", base ? base + 1 : argv[i]);
        if (has_suffix(argv[i], ".pdi")) {
            Image *image = NULL;
            if (image_load_file(argv[i], &image) != 0) {
                fprintf(stderr, "decode failed: %s\n", argv[i]);
                return 1;
            }
            printf("size %dx%d bitmap %dx%d offset %d,%d mask %s\n",
                   image->width, image->height, image->bw, image->bh,
                   image->xoff, image->yoff, image->mask ? "yes" : "no");
            dump_image(image);
            image_free(image);
            free(image);
        } else if (has_suffix(argv[i], ".pdt")) {
            Imagetable *table = NULL;
            if (imagetable_load_file(argv[i], &table) != 0) {
                fprintf(stderr, "decode failed: %s\n", argv[i]);
                return 1;
            }
            printf("cells %d size %dx%d\n", table->count, table->width, table->height);
            for (int c = 0; c < table->count; c++) {
                printf("-- cell %d\n", c + 1);
                dump_image(&table->cells[c]);
            }
            imagetable_free(table);
            free(table);
        } else {
            fprintf(stderr, "unknown extension: %s\n", argv[i]);
            return 2;
        }
    }
    return 0;
}

int bundle_font_payload(lua_State *lua, const char *name,
                        uint8_t **payload, size_t *payload_len)
{
    (void)lua; (void)name; (void)payload; (void)payload_len;
    return -1;
}
