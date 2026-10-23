#pragma once

/* BerntTracker (Windows): a small drawing kit for the screens that look like the web version
** (the start screen). It draws into plain pixel buffers that are
** shown with SDL: shapes with smooth edges, pictures (PNG), and text with Windows' own fonts
** (Segoe UI, the same font Chrome uses for the web version on Windows).
**
** Sizes are in real pixels: the caller multiplies the web page's CSS sizes by its scale.
** Text is UTF-8.
*/

#ifdef _WIN32

#include <stdint.h>
#include <stdbool.h>

typedef struct bt2Canvas_t
{
	int32_t w, h;
	uint32_t *px; // 0xAARRGGBB, not premultiplied
} bt2Canvas;

typedef struct bt2Font_t
{
	int32_t size;   // pixels (like CSS font-size)
	int32_t weight; // 400 (normal), 600 (semibold) or 700 (bold)
	bool mono;      // Consolas (for the sound test's text box)
} bt2Font;

enum
{
	BT2_ALIGN_LEFT = 0,
	BT2_ALIGN_CENTER = 1,
	BT2_ALIGN_RIGHT = 2
};

bool bt2CanvasAlloc(bt2Canvas *c, int32_t w, int32_t h);
void bt2CanvasFree(bt2Canvas *c);
void bt2CanvasClear(bt2Canvas *c, uint32_t argb);

// a rectangle with rounded corners and an optional border (fill may be fully transparent)
void bt2RoundRect(bt2Canvas *c, float x, float y, float w, float h, float radius, uint32_t fill, float borderW, uint32_t borderColor);

// a line with round ends and smooth edges (for the check mark in a check box)
void bt2Line(bt2Canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t color);

// draws src onto dst at (x, y), blending with src's alpha
void bt2Blit(bt2Canvas *dst, const bt2Canvas *src, int32_t x, int32_t y);

bool bt2DecodePng(const void *data, uint32_t size, bt2Canvas *out); // (Windows' own decoder, WIC)
bool bt2LoadPngResource(const char *name, bt2Canvas *out); // an RCDATA resource in the program
bool bt2Resize(const bt2Canvas *src, int32_t w, int32_t h, bt2Canvas *out); // smooth
bool bt2ScaleUp(const bt2Canvas *src, int32_t k, bt2Canvas *out); // k x k screen pixels per pixel (sharp, for pixel art)

float bt2LineHeight(bt2Font f); // CSS "line-height: normal" for this font
float bt2TextWidth(bt2Font f, const char *text); // one line
int32_t bt2TextLines(bt2Font f, float maxW, const char *text); // lines after word wrapping ('\n' breaks too)
float bt2TextBlockWidth(bt2Font f, float maxW, const char *text); // the widest of those lines

// word-wrapped text in the box starting at (x, y) with width w; lineH = the CSS line height in pixels
void bt2DrawText(bt2Canvas *c, bt2Font f, float x, float y, float w, float lineH, int32_t align, uint32_t color, const char *text);

// one line where <b>...</b> parts are bold and drawn in boldColor (like ".mode b" on the web page)
float bt2RichWidth(bt2Font f, const char *text);
void bt2DrawRich(bt2Canvas *c, bt2Font f, float x, float y, float lineH, uint32_t color, uint32_t boldColor, const char *text);

void bt2UiResetFonts(void); // forgets the fonts made so far (call when no text is being drawn, e.g. at a new size)
void bt2UiFree(void); // fonts and the text bitmap

#endif
