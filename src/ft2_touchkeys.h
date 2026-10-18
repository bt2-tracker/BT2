#pragma once

// BerntTracker: on-screen keyboard ("Retro 2" layout) drawn below the FT2 screen.
// Works with mouse and touch. Toggle with Ctrl+K.

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#define TK_W 787 // keyboard width in "native" pixels (the FT2 screen is stretched to this width)
#define TK_H 147 // keyboard height in native pixels (the FT2 screen is 400)
#define TK_MINI_H 22 // height of the thin bar shown when the keyboard is hidden

bool tkIsEnabled(void);
bool tkIsShown(void);
int32_t tkGetAreaHeight(void); // TK_H when shown, TK_MINI_H when hidden
void tkToggle(void); // shows/hides the keyboard and resizes the window

bool tkInit(SDL_Renderer *renderer);
void tkFree(void);

// window/pixel rectangles of the keyboard area (set by ft2_video.c on every layout change)
void tkSetRects(const SDL_Rect *windowRect, const SDL_Rect *pixelRect);

void tkRender(SDL_Renderer *renderer); // called once per frame from flipFrame()
bool tkHandleEvent(SDL_Event *event); // true = event was used by the keyboard (may turn a click into a right click)
void tkApplyModifiers(void); // ORs latched on-screen Shift/Ctrl/Alt into keyb.* (from readKeyModifiers)
bool tkMouseIsOverKeyboard(void);
