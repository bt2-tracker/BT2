#pragma once

/* BerntTracker: Disk Op. like on the iPad, in both the web version and the Windows program:
**
** - the buttons "../", "BT2", "Imp.", "Exp.", "Mod-land" and "Find" (instead of FT2's drive buttons)
** - Modland's folders, where nothing can be saved, renamed or deleted, and whose songs are downloaded
**   the first time they are opened
** - "../" from the top of the BT2 folder or Modland goes to the BT2 folder
** - new names for files and folders must be plain names (no / or \, not "." or "..")
**
** The web version does these in ft2_web.c (and index.html), the Windows program in bt2_windiskop.c.
*/

#include <stdint.h>
#include <stdbool.h>
#include "ft2_unicode.h"

#if defined __EMSCRIPTEN__ || defined _WIN32

#define BT2_DISKOP 1

bool bt2PrepareFile(const UNICHAR *pathU);        // before a file is opened: a Modland song is downloaded first (false = failed)
bool bt2PrepareDroppedFile(const char *pathUtf8); // the same, for a file opened from the search window (or dropped)
bool bt2DiskOpReadOnlyHere(void);                 // true in Modland's folders
bool bt2DiskOpPrepareSaveDir(bool showFolder);    // before saving: never into Modland (then BT2/Modland/..., and says so).
                                                  // showFolder: Disk Op. then shows the folder the file goes to
void bt2DiskOpConfineDir(void);                   // before a folder is listed (main thread)
void bt2DiskOpBeforeList(const UNICHAR *dirU);    // (in the thread that lists the folder) Modland's folders are filled
bool bt2ModlandOpen(void);                        // the list of songs (the first time from modland.com); false = not there
const UNICHAR *bt2ModlandRootU(void);             // Modland's top folder
const UNICHAR *bt2HomeRootU(void);                // the BT2 folder
void bt2SearchOpen(void);                         // the search window ("Find", Ctrl+F)

#if defined _WIN32 && !defined __EMSCRIPTEN__
bool bt2DiskOpGoParentTop(void); // "../" at the top of the BT2 folder or Modland (true = done here)
#endif

#endif
