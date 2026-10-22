#pragma once

/* BerntTracker (Windows): Disk Op. like on the iPad (see bt2_diskop.h), and Modland:
**
**   %LOCALAPPDATA%\Bernttracker II\Modland            Modland's folders (empty files until a song is opened)
**   %LOCALAPPDATA%\Bernttracker II\modland-ft2.txt    the saved list of songs
**   Documents\BT2\Modland                             the songs that were opened (the user's own copies)
**
** BT2_MODLAND_LIST_URL and BT2_MODLAND_FILE_URL can point somewhere else (for testing).
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#include <stdbool.h>

void bt2DiskOpInit(void); // after bt2HomeSetup()
void bt2DiskOpShutdown(void);

#endif
