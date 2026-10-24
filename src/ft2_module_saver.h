#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ft2_unicode.h"

void saveMusic(UNICHAR *filenameU);
bool saveXM(UNICHAR *filenameU);
bool saveXMRescue(UNICHAR *filenameU); // BerntTracker: the rescue copy (see bt2_rescue.c)
