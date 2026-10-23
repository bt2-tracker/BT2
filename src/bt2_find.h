#pragma once

/* BerntTracker: "Find" (Disk Op.'s "Find" button, Ctrl+F) as one of FT2's own screens: drawn with FT2's frames,
** buttons, radio buttons, scroll bar, text box and font, in the top half of the screen like FT2's Help screen.
** The same in the web version and the Windows program (Fredrik, 23.10.2026: "it should look like it is 100%
** part of the program"). What is found is bt2_search.c: songs, folders, instruments and samples in the user's
** own files and in Modland.
**
** Typing goes to the search box at once (it is always being edited while the screen is shown) and filters as
** you type. Up/Down/PgUp/PgDn/Home/End choose, Enter opens (a folder is shown in Disk Op.; for a song, Find goes back
** to what was there so FT2's play buttons and keys are at hand), Shift+Enter shows the hit's folder in Disk Op., Esc
** closes. Click a hit to choose it, double-click it (two clicks within 0.6 s) to open it; the wheel scrolls the list.
** The text and the chosen hit are kept for the next time Find is opened.
*/

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#if defined __EMSCRIPTEN__ || defined _WIN32

void showFindScreen(void);
void hideFindScreen(void);
void exitFindScreen(void);
void redrawFindScreen(void);

void findScreenFrame(void);              // in the main loop while the screen is shown
bool findScreenKeyDown(SDL_Keycode key); // true = the key was used here
bool findScreenMouseDown(void);          // a click in the list (true = used)
void findScreenWheel(bool up);

// the web page's hidden text box (the iPad's own keyboard): what is typed (UTF-8), and the keys it doesn't use itself
// (Up/Down/PgUp/PgDn/Home/End/Enter/Esc). Used in the main loop.
void findScreenSetQuery(const char *utf8);
void findScreenQueueKey(SDL_Keycode key, bool shift);
const char *findScreenQuery(void); // what is in the search box now (UTF-8)

#ifdef __EMSCRIPTEN__
const char *findScreenStateJson(void); // (for the automatic tests: what the screen shows)
#endif

// FT2's widgets (ft2_pushbuttons.c, ft2_radiobuttons.c, ft2_scrollbars.c)
void pbFindOpen(void);
void pbFindFolder(void);
void pbFindClear(void);
void pbFindListUp(void);
void pbFindListDown(void);
void rbFindWhereAll(void);
void rbFindWhereOwn(void);
void rbFindWhereModland(void);
void rbFindWhatAll(void);
void rbFindWhatSongs(void);
void rbFindWhatFolders(void);
void rbFindWhatSounds(void);
void sbFindListPos(uint32_t pos);

#endif
