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

/* v0.35: files and folders dragged onto the window from Windows, like the web version: they are copied into
** the folder Disk Op. is in (if it is one of your own, else the BT2 folder) with the same rules as Imp.
** (never over a file, zip files unpacked), and one song/instrument/sample is then opened. Files already in
** Documents\BT2 are opened where they are (not copied). Only drops from Windows (SDL_DROPBEGIN ... COMPLETE);
** the program's own "open this file" events (Find, Modland) are not affected.
*/
void bt2DropBegin(void);
bool bt2DropCollect(const char *pathUtf8); // true: it is part of a drop from Windows (taken care of here)
void bt2DropComplete(void);
void bt2DropTestFromEnv(void); // (for the automatic tests: BT2_TEST_DROP="C:\\a.xm|C:\\folder" is dropped at start)

#endif
