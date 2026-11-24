#include "picker.h"
#include "card.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int compare_names(const void *a, const void *b)
{
    return strcmp(a, b);
}

void picker_scan(Picker *picker)
{
    picker->count = 1;
    strcpy(picker->games[0], "CRANK DEMO");
    DIR *dir = opendir(CARD_ROOT "/games");
    if (!dir)
        return;
    struct dirent *entry;
    while (picker->count < sizeof picker->games / sizeof picker->games[0] &&
           (entry = readdir(dir))) {
        size_t len = strlen(entry->d_name);
        if (len <= 4 || len >= sizeof picker->games[0] ||
            strcmp(entry->d_name + len - 4, ".pdx") != 0)
            continue;
        char path[sizeof CARD_ROOT + 270];
        snprintf(path, sizeof path, CARD_ROOT "/games/%s", entry->d_name);
        struct stat info;
        if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
            strcpy(picker->games[picker->count++], entry->d_name);
    }
    closedir(dir);
    qsort(picker->games + 1, picker->count - 1, sizeof picker->games[0], compare_names);
}

void picker_path(const Picker *picker, char *path, size_t size)
{
    if (picker->selected == 0)
        snprintf(path, size, CARD_ROOT "/demo.pdx");
    else
        snprintf(path, size, CARD_ROOT "/games/%s", picker->games[picker->selected]);
}

static void rect(uint8_t frame[PICKER_HEIGHT][PICKER_WIDTH],
                 int x, int y, int width, int height, int ink)
{
    for (int row = y; row < y + height; row++)
        memset(&frame[row][x], ink, width);
}

void picker_label(uint8_t frame[PICKER_HEIGHT][PICKER_WIDTH],
                  int x, int y, const char *text, size_t limit, int ink)
{
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.:/?";
    static const uint16_t glyphs[] = {
        025755, 065656, 034443, 065556, 074647, 074644, 034553, 055755,
        072227, 011152, 055655, 044447, 057755, 057775, 025552, 065644,
        025573, 065655, 034216, 072222, 055557, 055552, 055575, 055255,
        055222, 071247, 025552, 026227, 025247, 061216, 055711, 074616,
        034652, 071122, 025252, 025316, 000000, 000700, 000002, 000007,
        002020, 011244, 071202,
    };
    /* Five 3-bit rows per glyph, from top to bottom. */
    for (size_t i = 0; i < limit && text[i] && x + 6 <= PICKER_WIDTH; i++, x += 8) {
        const char *found = strchr(chars, toupper((unsigned char)text[i]));
        uint16_t bits = found ? glyphs[found - chars] : glyphs[sizeof glyphs / sizeof *glyphs - 1];
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (bits & (1u << ((4 - row) * 3 + 2 - col)))
                    rect(frame, x + 2 * col, y + 2 * row, 2, 2, ink);
    }
}

static void cover(uint8_t frame[PICKER_HEIGHT][PICKER_WIDTH],
                  int x, int y, int width, int height, int selected)
{
    rect(frame, x, y, width, height, 1);
    rect(frame, x + 2, y + 2, width - 4, height - 4, 0);
    rect(frame, x + 10, y + 10, width - 20, height - 30, 1);
    picker_label(frame, x + width / 2 - 15, y + height / 2 - 12, "PDX", 3, 0);
    if (selected)
        rect(frame, x + width / 2 - 12, y + height - 12, 24, 3, 1);
}

void picker_draw(const Picker *picker, uint8_t frame[PICKER_HEIGHT][PICKER_WIDTH],
                 const char *error, int menu)
{
    memset(frame, 0, PICKER_HEIGHT * PICKER_WIDTH);
    picker_label(frame, 16, 14, error[0] ? "CANNOT RUN GAME" : "Q2 PLAYDATE", 46, 1);
    if (error[0]) {
        size_t length = strlen(error);
        for (size_t offset = 0; offset < 184 && offset < length; offset += 46)
            picker_label(frame, 16, 52 + (int)(offset / 46) * 20, error + offset, 46, 1);
        picker_label(frame, 16, 216, "BACK OR CENTRE TO DISMISS", 46, 1);
        return;
    }

    if (menu) {
        picker_label(frame, 120, 75, "RESUME", 22, 1);
        picker_label(frame, 120, 115, "GAME LIBRARY", 22, 1);
        picker_label(frame, 120, 155, "EXIT TO Q2 POD", 22, 1);
        picker_label(frame, 16, 216, "CENTRE LIBRARY  BACK RESUME", 46, 1);
        return;
    }

    if (picker->selected > 0)
        cover(frame, 37, 66, 90, 86, 0);
    if (picker->selected + 1 < picker->count)
        cover(frame, 273, 66, 90, 86, 0);
    cover(frame, 133, 42, 134, 128, 1);

    const char *name = picker->games[picker->selected];
    size_t length = strlen(name) - (picker->selected ? 4 : 0);
    if (length > 46)
        length = 46;
    picker_label(frame, 200 - 4 * (int)length, 186, name, length, 1);
    picker_label(frame, 16, 216, "PLAY: GET GAMES     CENTRE: OPEN", 46, 1);
}
