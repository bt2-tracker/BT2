#pragma once

/* BerntTracker (Windows): what the web version shows on top of FT2's screen, drawn the same way:
**
** - the message at the top (#toast in index.html): for 2.6 seconds, fades in and out
** - the box at the bottom while something is downloaded (#net): a text and a progress bar
**
** Text is UTF-8. Sizes are the web page's CSS sizes times Windows' display zoom.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

void bt2Toast(const char *text);

void bt2NetBoxShow(const char *text, double progress); // progress 0..1 (from any thread)
void bt2NetBoxHide(void);

void bt2OverlayRender(SDL_Renderer *renderer); // from flipFrame(), after FT2's screen
void bt2OverlayFree(void);
float bt2OverlayScale(void); // Windows' display zoom (1.0 = 100 %)

/* Runs job(arg) in a worker thread. Meanwhile the window keeps showing FT2's screen and the download box,
** and input is dropped (like when FT2 is busy). *cancel is set if the program is being closed: then false
** is returned right away (the job finishes on its own, its result isn't used).
*/
bool bt2RunJob(int32_t (*job)(void *arg), void *arg, volatile int32_t *cancel, int32_t *result);

#endif
