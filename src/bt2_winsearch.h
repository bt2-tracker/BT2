#pragma once

/* BerntTracker (Windows): the search window - the same window as the web version's (#search in
** index.html): Disk Op.'s "Find" button or Ctrl+F. Typing filters at once; a song opens in FT2, a
** folder opens in Disk Op. Songs from Modland are downloaded and kept like when they are opened in
** Disk Op. While it is open it has the keyboard and the mouse; FT2 keeps playing behind it.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

// (bt2SearchOpen() is in bt2_diskop.h)
bool bt2SearchIsOpen(void);
void bt2SearchClose(void);
bool bt2SearchShortcut(const SDL_Event *ev);  // Ctrl+F opens it (true = the key was used)
bool bt2SearchHandleEvent(SDL_Event *ev);     // true = used (while it is open: all keys and the mouse)
void bt2SearchUpdate(void);                   // once per frame (the main loop): searches after typing
void bt2SearchRender(SDL_Renderer *r, int32_t outW, int32_t outH); // (from bt2OverlayRender)
void bt2SearchFree(void);

#endif
