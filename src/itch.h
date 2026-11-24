#pragma once

#include "picker.h"

typedef void (*ItchPresent)(void *context, uint8_t picture[PICKER_HEIGHT][PICKER_WIDTH]);

void itch_browse(int input, ItchPresent present, void *context);
