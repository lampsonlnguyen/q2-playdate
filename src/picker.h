#pragma once

#include <stddef.h>
#include <stdint.h>

enum { PICKER_WIDTH = 400, PICKER_HEIGHT = 240 };

typedef struct {
    char games[64][256];
    size_t count;
    size_t selected;
} Picker;

void picker_scan(Picker *picker);
void picker_path(const Picker *picker, char *path, size_t size);
void picker_draw(const Picker *picker, uint8_t frame[PICKER_HEIGHT][PICKER_WIDTH],
                 const char *error, int menu);
