#pragma once

/* BerntTracker (Windows): the user's own folders, set up like in the web version:
**
**   Documents\BT2\Modules, Instruments, Samples, Patterns, Tracks   (Disk Op. starts in these)
**   Documents\BT2\Demo Tracks                                         (the demo songs)
**
** The demo songs come in a "Demo Tracks" folder next to the program. Songs that aren't in
** Documents\BT2\Demo Tracks yet are copied there; nothing there is ever overwritten or deleted, and a
** song the user has deleted isn't copied again (the same list as the web version: .demo-seen.json).
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>

void bt2HomeSetup(void); // after the config is loaded: the folders, and the web version's first-time settings

// Disk Op.'s start folder for a file kind (0 = modules, 1 = instruments, 2 = samples, 3 = patterns, 4 = tracks):
// if path is empty or isn't a folder, it is set to the BT2 folder for that kind (and *pathSet to true)
void bt2HomeFillDiskOpPath(int32_t kind, wchar_t *path, size_t pathLen, bool *pathSet);

void bt2HomeStartDemoCopy(void); // copies new demo songs (in the background)
bool bt2HomeTakeDemoNews(int32_t *added); // true once, when new demo songs were put in place (for the message)
const wchar_t *bt2HomeFolder(void); // Documents\BT2 (NULL if it couldn't be made)
void bt2HomeShutdown(void);

#endif
