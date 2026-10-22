/* BerntTracker (Windows): the search window, the same as the web version's. See bt2_winsearch.h.
**
** The sizes below are the web page's CSS sizes (#search in index.html), times Windows' display zoom.
** The colours are FT2's own (the palette), like in the web version.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft2_header.h"
#include "ft2_config.h"
#include "ft2_diskop.h"
#include "ft2_mouse.h"
#include "ft2_video.h"
#include "ft2_palette.h"
#include "ft2_structs.h"
#include "bt2_diskop.h"
#include "bt2_winsearch.h"
#include "bt2_winoverlay.h"
#include "bt2_winui.h"
#include "bt2_search.h"
#include "bt2_modland.h"
#include "bt2_fs.h"

#define SHOW_STEP 200      // results shown at first, and more with "Vis flere"
#define RUN_DELAY_MS 70    // after typing (like the web version)
#define WHEEL_STEP 100.0f  // CSS pixels per mouse wheel step (like Chrome)

// colours: FT2's palette, like the web version's --ft-* (applyColors in index.html)
enum { C_BG, C_PATTEXT, C_BLCK, C_BLCKTXT, C_DESK, C_TEXT, C_BTN, C_BTNTEXT, C_DESK2, C_DESK1, C_BTN2, C_BTN1, NUM_COLORS };

typedef struct frect_t { float x, y, w, h; } frect;

// what the mouse can press
enum { T_NONE = -1, T_BACKDROP = 0, T_PANEL, T_CLOSE, T_CLEAR, T_INPUT, T_WHERE, T_WHAT = T_WHERE + 3, T_MORE = T_WHAT + 4, T_SCROLLBAR, T_ROW, T_FOLDERBTN };

static const char *const whereText[3] = { "Alle", "Mine filer", "Modland" };
static const char *const whatText[4] = { "Alt", "Sanger", "Mapper", "Instr./samples" };

static struct
{
	bool open, dirty;
	int32_t where, what;

	char query[1024];
	int32_t caret; // (a byte position in query)
	bool selectAll;
	float inputScroll;
	uint64_t caretTime;

	bt2SearchPool own, mod;
	bool modReady, modLoading, modFailed;

	bt2SearchItem **results;
	int32_t numResults, sel, shown;
	float scroll; // the list's scroll position (CSS pixels)
	char statusMsg[256];
	uint64_t runAt; // a search waiting to run (after typing), 0 = none

	uint32_t col[NUM_COLORS];

	// the layout (CSS pixels; the parts are relative to the panel)
	float s, vw, vh;
	int32_t outW, outH;
	frect panel, title, closeBtn, input, list, status, whereLabel, whatLabel, whereBtn[3], whatBtn[4];
	float rowH, contentH;
	bool scrollbar;

	// the mouse
	int32_t press, pressRow;
	bool dragThumb;
	float dragStartY, dragStartScroll;
	SDL_Cursor *arrowCursor, *handCursor, *textCursor;
	int32_t cursorShown; // 0 arrow, 1 hand, 2 text

	// the picture
	bt2Canvas canvas;
	SDL_Texture *tex;
	int32_t texW, texH;
	float caretX, caretY, caretH; // (device pixels, in the panel)
	bool caretVisible;
} S;

// ------------------------------------------------------------------------------------------------
// fonts and text

static bt2Font font(float cssSize, int32_t weight)
{
	bt2Font f;
	f.size = (int32_t)floorf(cssSize * S.s + 0.5f);
	f.weight = weight;
	f.mono = false;
	return f;
}

static float textW(bt2Font f, const char *t) // (CSS pixels)
{
	return bt2TextWidth(f, t) / S.s;
}

static uint32_t mix(uint32_t fg, uint32_t bg, float a) // fg at opacity a over bg
{
	uint32_t out = 0xFF000000;
	for (int32_t sh = 0; sh <= 16; sh += 8)
	{
		const float f = (float)((fg >> sh) & 0xFF), b = (float)((bg >> sh) & 0xFF);
		out |= (uint32_t)(b + (f - b) * a + 0.5f) << sh;
	}
	return out;
}

// one line, made shorter with "…" if it is too wide (text-overflow: ellipsis)
static void fitText(bt2Font f, const char *text, float maxW, char *out, size_t outLen)
{
	snprintf(out, outLen, "%s", text);
	if (textW(f, out) <= maxW)
		return;

	const size_t len = strlen(text);
	size_t lo = 0, hi = len;
	while (lo < hi)
	{
		size_t mid = (lo + hi + 1) / 2;
		while (mid > 0 && ((uint8_t)text[mid] & 0xC0) == 0x80)
			mid--; // (whole letters)

		snprintf(out, outLen, "%.*s\xE2\x80\xA6", (int)mid, text);
		if (mid > lo && textW(f, out) <= maxW)
			lo = mid;
		else
			hi = (mid > lo) ? mid - 1 : lo;
	}

	while (lo > 0 && ((uint8_t)text[lo] & 0xC0) == 0x80)
		lo--;
	snprintf(out, outLen, "%.*s\xE2\x80\xA6", (int)lo, text);
}

// ------------------------------------------------------------------------------------------------
// drawing helpers (device pixels)

static int32_t px(float css)
{
	return (int32_t)floorf(css * S.s + 0.5f);
}

static void fill(bt2Canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
	int32_t x0 = (x < 0) ? 0 : x, y0 = (y < 0) ? 0 : y;
	int32_t x1 = (x + w > c->w) ? c->w : x + w, y1 = (y + h > c->h) ? c->h : y + h;
	for (int32_t yy = y0; yy < y1; yy++)
	{
		uint32_t *row = &c->px[(size_t)yy * c->w];
		for (int32_t xx = x0; xx < x1; xx++)
			row[xx] = color;
	}
}

// a CSS border of b pixels: top/left in one colour, bottom/right in another (with the corners cut diagonally)
static void bevel(bt2Canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, int32_t b, uint32_t tl, uint32_t br)
{
	for (int32_t yy = 0; yy < h; yy++)
	{
		const int32_t cy = y + yy;
		if (cy < 0 || cy >= c->h)
			continue;

		uint32_t *row = &c->px[(size_t)cy * c->w];
		for (int32_t xx = 0; xx < w; xx++)
		{
			const bool top = yy < b, left = xx < b, bottom = yy >= h - b, right = xx >= w - b;
			if (!top && !left && !bottom && !right)
			{
				xx = w - b - 1; // (the inside is left as it is)
				continue;
			}

			uint32_t color;
			if (top && right && !left)
				color = (xx + yy + 1 < w) ? tl : br;
			else if (bottom && left && !top)
				color = (xx + yy + 1 < h) ? tl : br;
			else
				color = (top || left) ? tl : br;

			const int32_t cx = x + xx;
			if (cx >= 0 && cx < c->w)
				row[cx] = color;
		}
	}
}

static void drawLine(bt2Canvas *c, bt2Font f, float xCss, float yCss, float lineHCss, uint32_t color, const char *text)
{
	bt2DrawText(c, f, xCss * S.s, yCss * S.s, bt2TextWidth(f, text) + 4.0f, lineHCss * S.s, BT2_ALIGN_LEFT, color, text);
}

// an FT2-looking button (.ft-btn): "on" = pressed in, with a dot in front
static void drawButton(bt2Canvas *c, frect r, float fontCss, const char *text, bool on, bool pressed)
{
	const int32_t x = px(r.x), y = px(r.y), w = px(r.x + r.w) - x, h = px(r.y + r.h) - y, b = px(2.0f);
	fill(c, x, y, w, h, S.col[C_BTN]);
	const bool in = on || pressed;
	bevel(c, x, y, w, h, b, in ? S.col[C_BTN2] : S.col[C_BTN1], in ? S.col[C_BTN1] : S.col[C_BTN2]);
	if (on)
	{
		const int32_t one = (px(1.0f) > 0) ? px(1.0f) : 1;
		bevel(c, x + b, y + b, w - 2 * b, h - 2 * b, one, S.col[C_BTN2], S.col[C_BTN2]);
	}

	char label[128];
	snprintf(label, sizeof (label), "%s%s", on ? "\xE2\x97\x8F\xC2\xA0" : "", text);
	const bt2Font f = font(fontCss, 700);
	const float lh = fontCss * 1.3f;
	const float tw = textW(f, label);
	drawLine(c, f, r.x + (r.w - tw) * 0.5f, r.y + (r.h - lh) * 0.5f, lh, S.col[C_BTNTEXT], label);
}

static float buttonW(const char *text, bool on, float fontCss, float padX)
{
	char label[128];
	snprintf(label, sizeof (label), "%s%s", on ? "\xE2\x97\x8F\xC2\xA0" : "", text);
	return textW(font(fontCss, 700), label) + 2.0f * padX + 4.0f;
}

// ------------------------------------------------------------------------------------------------
// the status line (the same texts as the web version)

static void statusText(char *out, size_t outLen)
{
	if (S.statusMsg[0] != '\0')
	{
		snprintf(out, outLen, "%s", S.statusMsg);
		return;
	}

	char count[32];
	if (S.numResults == 1)
		snprintf(count, sizeof (count), "1 treff");
	else
		snprintf(count, sizeof (count), "%d treff", (int)S.numResults);

	const char *extra = S.modLoading ? " \xC2\xB7 henter Modland-listen \xE2\x80\xA6"
		: ((S.where != BT2_WHERE_OWN && !S.modReady) ? " \xC2\xB7 Modland: skriv noe for \xC3\xA5 s\xC3\xB8ke der" : "");

	snprintf(out, outLen, "%s%s \xC2\xB7 \xE2\x86\x91\xE2\x86\x93 velg \xC2\xB7 Enter \xC3\xA5pne \xC2\xB7 Shift+Enter vis mappa \xC2\xB7 Esc lukk", count, extra);
}

// ------------------------------------------------------------------------------------------------
// layout

static int32_t shownCount(void)
{
	return (S.numResults < S.shown) ? S.numResults : S.shown;
}

static float filterRow(frect *label, frect *btns, int32_t n, const char *const *texts, int32_t on, float x0, float y, float w)
{
	// "Hvor"/"Hva" (44 px wide), then the buttons (gap 6, and onto a new line when there is no room)
	*label = (frect){ x0, y, 44.0f, 36.0f };
	float x = x0 + 44.0f + 6.0f, lineY = y;
	for (int32_t i = 0; i < n; i++)
	{
		const float bw = buttonW(texts[i], i == on, 15.0f, 12.0f);
		if (x > x0 && x + bw > x0 + w)
		{
			x = x0;
			lineY += 36.0f + 6.0f;
		}

		btns[i] = (frect){ x, lineY, bw, 36.0f };
		x += bw + 6.0f;
	}

	return lineY + 36.0f - y;
}

static void doLayout(void)
{
	S.vw = S.outW / S.s;
	S.vh = S.outH / S.s;

	// the panel: at most 780 x 660, 10 px from the window's edges
	const float pw = fmaxf(160.0f, fminf(780.0f, S.vw - 20.0f));
	const float ph = fmaxf(220.0f, fminf(660.0f, S.vh - 20.0f));
	S.panel = (frect){ floorf((S.vw - pw) * 0.5f), floorf((S.vh - ph) * 0.5f), pw, ph };

	// (inside the panel: 2 px border, 10 px padding, 8 px between the parts)
	const float cx = 12.0f, cw = pw - 24.0f, cy = 12.0f, ch = ph - 24.0f;
	float y = cy;

	const float closeW = buttonW("Lukk", false, 15.0f, 12.0f);
	S.closeBtn = (frect){ cx + cw - closeW, y, closeW, 36.0f };
	S.title = (frect){ cx, y + (36.0f - 18.0f * 1.3f) * 0.5f, cw - closeW - 10.0f, 18.0f * 1.3f };
	y += 36.0f + 8.0f;

	S.input = (frect){ cx, y, cw, 18.0f * 1.3f + 16.0f + 4.0f };
	y += S.input.h + 8.0f;

	y += filterRow(&S.whereLabel, S.whereBtn, 3, whereText, S.where, cx, y, cw) + 8.0f;
	y += filterRow(&S.whatLabel, S.whatBtn, 4, whatText, S.what, cx, y, cw) + 8.0f;

	char st[512];
	statusText(st, sizeof (st));
	const bt2Font fs = font(13.0f, 600);
	const int32_t lines = bt2TextLines(fs, cw * S.s, st);
	const float statusH = fmaxf(18.0f, ((lines > 0) ? lines : 1) * 13.0f * 1.3f);
	S.status = (frect){ cx, cy + ch - statusH, cw, statusH };

	S.list = (frect){ cx, y, cw, fmaxf(0.0f, S.status.y - 8.0f - y) };

	// a row: "padding: 6px 10px; min-height: 40px", the name (15 px) over the path (12 px)
	S.rowH = fmaxf(40.0f, 12.0f + 15.0f * 1.3f + 12.0f * 1.3f);

	const int32_t n = shownCount();
	if (n > 0)
		S.contentH = n * S.rowH + ((S.numResults > S.shown) ? 52.0f : 0.0f);
	else
		S.contentH = 14.0f + 15.0f * 1.3f + 14.0f;

	const float innerH = fmaxf(0.0f, S.list.h - 4.0f);
	S.scrollbar = S.contentH > innerH + 0.5f;

	const float maxScroll = fmaxf(0.0f, S.contentH - innerH);
	if (S.scroll > maxScroll) S.scroll = maxScroll;
	if (S.scroll < 0.0f) S.scroll = 0.0f;
}

// Chrome's "×" in a search box (type="search"): clears it. Only when there is text. (Relative to the panel.)
static bool clearButton(frect *r)
{
	if (S.query[0] == '\0')
		return false;

	const float size = 14.0f;
	*r = (frect){ S.input.x + S.input.w - 2.0f - 10.0f - size, S.input.y + (S.input.h - size) * 0.5f, size, size };
	return true;
}

static frect listInner(void)
{
	return (frect){ S.list.x + 2.0f, S.list.y + 2.0f, fmaxf(0.0f, S.list.w - 4.0f), fmaxf(0.0f, S.list.h - 4.0f) };
}

static float rowWidth(void)
{
	return listInner().w - (S.scrollbar ? 12.0f : 0.0f);
}

static frect thumbRect(void) // (relative to the panel)
{
	const frect in = listInner();
	const float trackH = in.h;
	float th = (S.contentH > 0.0f) ? trackH * (trackH / S.contentH) : trackH;
	if (th < 24.0f) th = fminf(24.0f, trackH);

	const float maxScroll = fmaxf(1.0f, S.contentH - trackH);
	const float ty = in.y + (trackH - th) * (S.scroll / maxScroll);
	return (frect){ in.x + in.w - 12.0f, ty, 12.0f, th };
}

// ------------------------------------------------------------------------------------------------
// the rows

static void fmtSize(int64_t n, char *out, size_t outLen) // (like the web version's fmtSize)
{
	if (n >= 1048576)
		snprintf(out, outLen, "%.1f MB", n / 1048576.0);
	else if (n >= 1024)
		snprintf(out, outLen, "%d kB", (int)floor(n / 1024.0 + 0.5));
	else if (n > 0)
		snprintf(out, outLen, "%d B", (int)n);
	else
		out[0] = '\0';
}

static void rowSource(const bt2SearchItem *it, char *line1, char *line2, size_t len)
{
	snprintf(line1, len, "%s", it->modland ? "Modland" : "Mine");
	if (it->kind == BT2_KIND_FOLDER)
	{
		if (it->count > 0)
			snprintf(line2, len, (it->count == 1) ? "%d sang" : "%d sanger", (int)it->count);
		else
			line2[0] = '\0';
	}
	else
	{
		fmtSize(it->size, line2, len);
	}
}

// where the parts of a row are (CSS pixels, relative to the row)
typedef struct rowParts_t
{
	float kindX, mainX, mainW, srcX, srcW, btnX, btnW, btnH;
	bool hasBtn;
} rowParts;

static void rowLayout(const bt2SearchItem *it, float rowW, rowParts *p)
{
	p->hasBtn = (it->kind != BT2_KIND_FOLDER);
	p->btnW = p->hasBtn ? buttonW("Mappe", false, 13.0f, 8.0f) : 0.0f;
	p->btnH = 30.0f;
	p->btnX = rowW - 10.0f - p->btnW;

	char l1[64], l2[64];
	rowSource(it, l1, l2, sizeof (l1));
	const bt2Font fsrc = font(12.0f, 700);
	p->srcW = fmaxf(64.0f, fmaxf(textW(fsrc, l1), textW(fsrc, l2)));
	p->srcX = (p->hasBtn ? p->btnX - 10.0f : rowW - 10.0f) - p->srcW;

	p->kindX = 10.0f;
	p->mainX = 10.0f + 18.0f + 10.0f;
	p->mainW = fmaxf(10.0f, p->srcX - 10.0f - p->mainX);
}

static void drawRow(bt2Canvas *c, const bt2SearchItem *it, float x, float y, float w, bool selected, bool btnPressed)
{
	const uint32_t bg = selected ? S.col[C_BLCK] : S.col[C_BG];
	const uint32_t fg = selected ? S.col[C_BLCKTXT] : S.col[C_PATTEXT];
	if (selected)
		fill(c, px(x), px(y), px(x + w) - px(x), px(y + S.rowH) - px(y), bg);

	rowParts p;
	rowLayout(it, w, &p);

	const bt2Font f15 = font(15.0f, 700), f12 = font(12.0f, 700), fPath = font(12.0f, 600);
	const float l15 = 15.0f * 1.3f, l12 = 12.0f * 1.3f;
	const float mainY = y + (S.rowH - (l15 + l12)) * 0.5f;

	// the kind: '/' (folder), '♪' (song), '∿' (instrument/sample)
	const char *kind = (it->kind == BT2_KIND_FOLDER) ? "/" : (it->kind == BT2_KIND_SONG) ? "\xE2\x99\xAA" : "\xE2\x88\xBF";
	const float kw = textW(f15, kind);
	drawLine(c, f15, x + p.kindX + (18.0f - kw) * 0.5f, y + (S.rowH - l15) * 0.5f, l15, mix(fg, bg, 0.9f), kind);

	// the name and the folder it is in
	char name[1024], fitted[1100];
	snprintf(name, sizeof (name), "%s%s", (it->kind == BT2_KIND_FOLDER) ? "/" : "", it->name);
	fitText(f15, name, p.mainW, fitted, sizeof (fitted));
	drawLine(c, f15, x + p.mainX, mainY, l15, fg, fitted);

	char path[2048];
	snprintf(path, sizeof (path), "%s%s%.*s", it->modland ? "Modland" : "BT2", (it->dirLen > 0) ? "/" : "", (int)it->dirLen, it->rel);
	fitText(fPath, path, p.mainW, fitted, sizeof (fitted));
	drawLine(c, fPath, x + p.mainX, mainY + l15, l12, mix(fg, bg, 0.65f), fitted);

	// "Mine"/"Modland" and the size (right-aligned)
	char l1[64], l2[64];
	rowSource(it, l1, l2, sizeof (l1));
	const uint32_t srcColor = mix(fg, bg, 0.8f);
	const float srcY = y + (S.rowH - ((l2[0] != '\0') ? 2.0f : 1.0f) * l12) * 0.5f;
	drawLine(c, f12, x + p.srcX + p.srcW - textW(f12, l1), srcY, l12, srcColor, l1);
	if (l2[0] != '\0')
		drawLine(c, f12, x + p.srcX + p.srcW - textW(f12, l2), srcY + l12, l12, srcColor, l2);

	if (p.hasBtn) // "Mappe": shows the folder in Disk Op.
		drawButton(c, (frect){ x + p.btnX, y + (S.rowH - p.btnH) * 0.5f, p.btnW, p.btnH }, 13.0f, "Mappe", false, btnPressed);
}

// ------------------------------------------------------------------------------------------------
// the picture

static void renderPanel(void)
{
	bt2Canvas *c = &S.canvas;

	fill(c, 0, 0, c->w, c->h, S.col[C_DESK]);
	bevel(c, 0, 0, c->w, c->h, px(2.0f), S.col[C_DESK1], S.col[C_DESK2]);

	// the title and "Lukk"
	drawLine(c, font(18.0f, 700), S.title.x, S.title.y, 18.0f * 1.3f, S.col[C_TEXT], "S\xC3\xB8k");
	drawButton(c, S.closeBtn, 15.0f, "Lukk", false, S.press == T_CLOSE);

	// the text box
	{
		const frect r = S.input;
		const int32_t x = px(r.x), y = px(r.y), w = px(r.x + r.w) - x, h = px(r.y + r.h) - y;
		fill(c, x, y, w, h, S.col[C_BG]);
		bevel(c, x, y, w, h, px(2.0f), S.col[C_DESK2], S.col[C_DESK1]);

		const bt2Font f = font(18.0f, 600);
		frect clr;
		const bool hasClear = clearButton(&clr);
		const float innerX = r.x + 2.0f + 10.0f, innerW = r.w - 4.0f - 20.0f - (hasClear ? 18.0f : 0.0f), lh = 18.0f * 1.3f, ty = r.y + 2.0f + 8.0f;

		char before[1024];
		snprintf(before, sizeof (before), "%.*s", (int)S.caret, S.query);
		const float caretPos = textW(f, before), fullW = textW(f, S.query);

		// (the text moves left when it is longer than the box, so the cursor is always seen)
		if (caretPos - S.inputScroll > innerW - 2.0f) S.inputScroll = caretPos - innerW + 2.0f;
		if (caretPos - S.inputScroll < 0.0f) S.inputScroll = caretPos;
		if (fullW - S.inputScroll < innerW - 2.0f) S.inputScroll = fmaxf(0.0f, fullW - innerW + 2.0f);

		bt2Canvas tmp;
		const int32_t tw = px(innerX + innerW) - px(innerX), th = px(r.y + r.h - 2.0f) - px(r.y + 2.0f);
		if (tw > 0 && th > 0 && bt2CanvasAlloc(&tmp, tw, th))
		{
			bt2CanvasClear(&tmp, 0x00000000);
			const float ox = -S.inputScroll, oy = ty - (r.y + 2.0f);
			if (S.query[0] == '\0')
			{
				bt2DrawText(&tmp, f, 0.0f, oy * S.s, bt2TextWidth(f, "x") * 40.0f, lh * S.s, BT2_ALIGN_LEFT, 0xFF757575,
					"Artist, l\xC3\xA5t eller mappe \xE2\x80\xA6");
			}
			else
			{
				uint32_t color = S.col[C_PATTEXT];
				if (S.selectAll) // (all of it is selected: typing replaces it)
				{
					fill(&tmp, px(ox), px(oy), px(ox + fullW) - px(ox), px(lh), 0xFF3390FF);
					color = 0xFFFFFFFF;
				}
				bt2DrawText(&tmp, f, ox * S.s, oy * S.s, bt2TextWidth(f, S.query) + 4.0f, lh * S.s, BT2_ALIGN_LEFT, color, S.query);
			}

			bt2Blit(c, &tmp, px(innerX), px(r.y + 2.0f));
			bt2CanvasFree(&tmp);
		}

		if (hasClear) // (two lines across, like Chrome's)
		{
			const float m = 3.5f, lineW = fmaxf(1.0f, 1.6f * S.s);
			bt2Line(c, (clr.x + m) * S.s, (clr.y + m) * S.s, (clr.x + clr.w - m) * S.s, (clr.y + clr.h - m) * S.s, lineW, S.col[C_PATTEXT]);
			bt2Line(c, (clr.x + clr.w - m) * S.s, (clr.y + m) * S.s, (clr.x + m) * S.s, (clr.y + clr.h - m) * S.s, lineW, S.col[C_PATTEXT]);
		}

		S.caretX = (innerX + caretPos - S.inputScroll) * S.s;
		S.caretY = (ty + (lh - 21.0f) * 0.5f) * S.s;
		S.caretH = 21.0f * S.s;
		S.caretVisible = !S.selectAll || S.query[0] == '\0';
	}

	// "Hvor" and "Hva"
	const bt2Font f15 = font(15.0f, 700);
	drawLine(c, f15, S.whereLabel.x, S.whereLabel.y + (36.0f - 19.5f) * 0.5f, 19.5f, S.col[C_TEXT], "Hvor");
	for (int32_t i = 0; i < 3; i++)
		drawButton(c, S.whereBtn[i], 15.0f, whereText[i], S.where == i, S.press == T_WHERE + i);

	drawLine(c, f15, S.whatLabel.x, S.whatLabel.y + (36.0f - 19.5f) * 0.5f, 19.5f, S.col[C_TEXT], "Hva");
	for (int32_t i = 0; i < 4; i++)
		drawButton(c, S.whatBtn[i], 15.0f, whatText[i], S.what == i, S.press == T_WHAT + i);

	// the list
	{
		const frect r = S.list;
		const int32_t x = px(r.x), y = px(r.y), w = px(r.x + r.w) - x, h = px(r.y + r.h) - y;
		if (w > 0 && h > 0)
		{
			fill(c, x, y, w, h, S.col[C_BG]);
			bevel(c, x, y, w, h, px(2.0f), S.col[C_DESK2], S.col[C_DESK1]);
		}

		const frect in = listInner();
		bt2Canvas lc;
		const int32_t lw = px(in.x + in.w) - px(in.x), lh = px(in.y + in.h) - px(in.y);
		if (lw > 0 && lh > 0 && bt2CanvasAlloc(&lc, lw, lh))
		{
			bt2CanvasClear(&lc, S.col[C_BG]);
			const float rw = rowWidth();
			const int32_t n = shownCount();

			if (n == 0)
			{
				const char *empty = S.modLoading ? "Henter Modland-listen \xE2\x80\xA6" : "Ingen treff.";
				drawLine(&lc, font(15.0f, 600), 14.0f, 14.0f - S.scroll, 19.5f, mix(S.col[C_PATTEXT], S.col[C_BG], 0.8f), empty);
			}

			int32_t first = (int32_t)floorf(S.scroll / S.rowH);
			if (first < 0) first = 0;
			for (int32_t i = first; i < n; i++)
			{
				const float ry = i * S.rowH - S.scroll;
				if (ry >= in.h)
					break;

				drawRow(&lc, S.results[i], 0.0f, ry, rw, i == S.sel, S.press == T_FOLDERBTN && S.pressRow == i);
			}

			if (S.numResults > S.shown)
			{
				char more[64];
				snprintf(more, sizeof (more), "Vis flere (%d til)", (int)(S.numResults - S.shown));
				const float by = n * S.rowH + 8.0f - S.scroll;
				drawButton(&lc, (frect){ 10.0f, by, buttonW(more, false, 15.0f, 12.0f), 36.0f }, 15.0f, more, false, S.press == T_MORE);
			}

			if (S.scrollbar)
			{
				const frect t = thumbRect();
				fill(&lc, px(in.w - 12.0f), 0, lw - px(in.w - 12.0f), lh, mix(S.col[C_DESK], S.col[C_BG], 0.35f));
				const int32_t tx = px(t.x - in.x) + px(2.0f), tyy = px(t.y - in.y), tww = px(12.0f) - 2 * px(2.0f), thh = px(t.y + t.h) - px(t.y);
				fill(&lc, tx, tyy, tww, thh, S.col[C_BTN]);
				bevel(&lc, tx, tyy, tww, thh, (px(1.0f) > 0) ? px(1.0f) : 1, S.col[C_BTN1], S.col[C_BTN2]);
			}

			bt2Blit(c, &lc, px(in.x), px(in.y));
			bt2CanvasFree(&lc);
		}
	}

	// the status line
	char st[512];
	statusText(st, sizeof (st));
	bt2DrawText(c, font(13.0f, 600), S.status.x * S.s, S.status.y * S.s, S.status.w * S.s, 13.0f * 1.3f * S.s, BT2_ALIGN_LEFT, S.col[C_TEXT], st);
}

static void loadColors(void)
{
	for (int32_t i = 0; i < NUM_COLORS; i++)
		S.col[i] = 0xFF000000 | (video.palette[i] & 0xFFFFFF);
}

void bt2SearchRender(SDL_Renderer *r, int32_t outW, int32_t outH)
{
	if (!S.open)
		return;

	const float s = bt2OverlayScale();
	if (outW != S.outW || outH != S.outH || s != S.s)
	{
		S.outW = outW;
		S.outH = outH;
		S.s = s;
		S.dirty = true;
	}

	// the window behind is darker (background: rgba(0,0,0,.5))
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 0, 0, 0, 128);
	SDL_RenderFillRect(r, NULL);

	if (S.dirty)
	{
		S.dirty = false;
		loadColors();
		doLayout();

		const int32_t w = px(S.panel.x + S.panel.w) - px(S.panel.x), h = px(S.panel.y + S.panel.h) - px(S.panel.y);
		if (S.tex == NULL || S.texW != w || S.texH != h)
		{
			if (S.tex != NULL)
				SDL_DestroyTexture(S.tex);
			bt2CanvasFree(&S.canvas);

			S.tex = NULL;
			if (w > 0 && h > 0 && bt2CanvasAlloc(&S.canvas, w, h))
			{
				S.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
				S.texW = w;
				S.texH = h;
			}
		}

		if (S.tex != NULL)
		{
			renderPanel();
			SDL_UpdateTexture(S.tex, NULL, S.canvas.px, S.canvas.w * (int32_t)sizeof (uint32_t));
		}
	}

	if (S.tex == NULL)
		return;

	// a soft shadow under the panel (box-shadow: 0 6px 30px rgba(0,0,0,.6))
	const SDL_Rect dst = { px(S.panel.x), px(S.panel.y), S.texW, S.texH };
	for (int32_t i = 6; i >= 1; i--)
	{
		const int32_t g = px(i * 4.0f);
		SDL_Rect sh = { dst.x - g, dst.y - g + px(6.0f), dst.w + 2 * g, dst.h + 2 * g };
		SDL_SetRenderDrawColor(r, 0, 0, 0, 18);
		SDL_RenderFillRect(r, &sh);
	}

	SDL_RenderCopy(r, S.tex, NULL, &dst);

	// the blinking text cursor
	const uint64_t t = GetTickCount64() - S.caretTime;
	if (S.caretVisible && (t / 530) % 2 == 0)
	{
		const int32_t cw = (px(1.0f) > 1) ? px(1.0f) : 1;
		SDL_Rect cr = { dst.x + (int32_t)floorf(S.caretX), dst.y + (int32_t)floorf(S.caretY), cw, (int32_t)floorf(S.caretH + 0.5f) };
		const uint32_t c = S.col[C_PATTEXT];
		SDL_SetRenderDrawColor(r, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
		SDL_RenderFillRect(r, &cr);
	}

	SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
}

// ------------------------------------------------------------------------------------------------
// searching

static void ensureModland(void)
{
	if (S.modReady || S.modLoading || S.modFailed)
		return;

	if (!bt2ModlandIsLoaded())
	{
		S.modLoading = true;
		S.dirty = true;

		const bool ok = bt2ModlandOpen(); // (the first time: downloaded, with the download box)

		S.modLoading = false;
		S.dirty = true;
		if (!S.open)
			return;

		if (!ok)
		{
			S.modFailed = true; // (tried again the next time the window is opened)
			snprintf(S.statusMsg, sizeof (S.statusMsg), "Fikk ikke hentet Modland-listen (ingen nett?).");
			return;
		}
	}

	bt2SearchBuildModland(&S.mod);
	S.modReady = true;
}

static bool hasWords(void)
{
	for (const char *p = S.query; *p != '\0'; p++)
	{
		if (*p != ' ' && *p != '\t')
			return true;
	}
	return false;
}

static void runNow(void)
{
	S.runAt = 0;
	S.statusMsg[0] = '\0';

	if (S.where != BT2_WHERE_OWN && !S.modReady && (hasWords() || S.where == BT2_WHERE_MODLAND))
	{
		ensureModland();
		if (!S.open)
			return;

		if (S.modFailed) // (it is tried again the next time the window is opened)
			snprintf(S.statusMsg, sizeof (S.statusMsg), "Fikk ikke hentet Modland-listen (ingen nett?).");
	}

	free(S.results);
	S.results = NULL;
	S.numResults = bt2SearchRun(S.query, S.where, S.what, &S.own, S.modReady ? &S.mod : NULL, &S.results);

	S.sel = 0;
	S.shown = SHOW_STEP;
	S.scroll = 0.0f;
	S.dirty = true;
}

void bt2SearchUpdate(void)
{
	if (S.open && S.runAt != 0 && GetTickCount64() >= S.runAt)
		runNow();
}

static void queryChanged(void)
{
	S.runAt = GetTickCount64() + RUN_DELAY_MS;
	S.caretTime = GetTickCount64();
	S.dirty = true;
}

// ------------------------------------------------------------------------------------------------
// opening, closing

static void setCursor(int32_t which)
{
	if (S.cursorShown == which)
		return;

	S.cursorShown = which;
	SDL_Cursor *c = (which == 1) ? S.handCursor : (which == 2) ? S.textCursor : S.arrowCursor;
	if (c != NULL)
		SDL_SetCursor(c);
}

void bt2SearchOpen(void)
{
	if (S.open || ui.sysReqShown || editor.editTextFlag || !editor.mainLoopOngoing)
		return;

	loadColors();

	char *home = bt2Utf8(bt2HomeRootU());
	if (home == NULL)
		return;

	// the last results point into the old list of the user's files, which is made again now
	free(S.results);
	S.results = NULL;
	S.numResults = 0;
	S.sel = 0;
	S.scroll = 0.0f;

	bt2SearchBuildOwn(&S.own, home);
	free(home);

	S.s = bt2OverlayScale();
	S.outW = S.outH = 0;
	SDL_GetRendererOutputSize(video.renderer, &S.outW, &S.outH);
	if (S.outW <= 0 || S.outH <= 0)
	{
		S.outW = SCREEN_W;
		S.outH = SCREEN_H;
	}

	S.open = true;
	S.modFailed = false;
	S.selectAll = true; // (like the web version: the last search is selected, typing replaces it)
	S.caret = (int32_t)strlen(S.query);
	S.inputScroll = 0.0f;
	S.caretTime = GetTickCount64();
	S.press = T_NONE;
	S.dragThumb = false;
	S.dirty = true;

	if (S.arrowCursor == NULL) S.arrowCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
	if (S.handCursor == NULL) S.handCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
	if (S.textCursor == NULL) S.textCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
	S.cursorShown = -1;
	setCursor(0);
	SDL_ShowCursor(SDL_ENABLE);
	SDL_StartTextInput();

	runNow();
	doLayout();
}

void bt2SearchClose(void)
{
	if (!S.open)
		return;

	S.open = false;
	S.runAt = 0;
	SDL_StopTextInput();

	// FT2's own mouse pointer again
	setMouseShape(config.mouseType);
	SDL_ShowCursor((config.specialFlags2 & HARDWARE_MOUSE) ? SDL_TRUE : SDL_FALSE);
}

bool bt2SearchIsOpen(void)
{
	return S.open;
}

void bt2SearchFree(void)
{
	bt2SearchClose();
	free(S.results);
	S.results = NULL;
	S.numResults = 0;
	bt2SearchFreePool(&S.own);
	bt2SearchFreePool(&S.mod);
	S.modReady = false;

	if (S.tex != NULL)
		SDL_DestroyTexture(S.tex);
	S.tex = NULL;
	bt2CanvasFree(&S.canvas);
}

// ------------------------------------------------------------------------------------------------
// doing what was chosen

static void folderOf(const char *rel, int32_t dirLen, char *out, size_t outLen)
{
	snprintf(out, outLen, "%.*s", (int)dirLen, rel);
}

// the item's folder here (own: in the BT2 folder; Modland: made and filled)
static bool localFolder(const bt2SearchItem *it, const char *modlandFolder, char *out, size_t outLen)
{
	if (it->modland)
		return bt2ModlandMaterialize(modlandFolder, out, outLen);

	char *home = bt2Utf8(bt2HomeRootU());
	if (home == NULL)
		return false;

	if (modlandFolder[0] == '\0')
		snprintf(out, outLen, "%s", home);
	else
		bt2PathJoin(out, outLen, home, modlandFolder);
	free(home);

	for (char *p = out; *p != '\0'; p++)
	{
		if (*p == '/')
			*p = '\\';
	}

	return true;
}

static void showFolder(const char *folder, int32_t item)
{
	if (!ui.diskOpShown)
		showDiskOpScreen();

	// the right kind of files (songs, instruments, samples), then the folder
	if (item != getDiskOpItem())
	{
		if (item == DISKOP_ITEM_INSTR)
			rbDiskOpInstr();
		else if (item == DISKOP_ITEM_SAMPLE)
			rbDiskOpSample();
		else
			rbDiskOpModule();
	}

	wchar_t *w = bt2Wide(folder);
	if (w == NULL || _wchdir(w) != 0)
		bt2Toast("Fant ikke mappa");
	free(w);

	editor.diskOpReadDir = true;
}

static bool endsWithXi(const char *name)
{
	const size_t n = strlen(name);
	return n >= 3 && name[n - 3] == '.' && (name[n - 2] == 'x' || name[n - 2] == 'X') && (name[n - 1] == 'i' || name[n - 1] == 'I');
}

static void act(const bt2SearchItem *it, bool showFolderToo)
{
	if (it == NULL)
		return;

	char folderRel[2048], folder[2048];
	if (it->kind == BT2_KIND_FOLDER)
		snprintf(folderRel, sizeof (folderRel), "%s", it->rel);
	else
		folderOf(it->rel, it->dirLen, folderRel, sizeof (folderRel));

	if (!localFolder(it, folderRel, folder, sizeof (folder)))
	{
		bt2SearchClose();
		bt2Toast("Fant ikke mappa");
		return;
	}

	if (it->kind == BT2_KIND_FOLDER || showFolderToo)
	{
		const int32_t item = (it->kind == BT2_KIND_SOUND) ? (endsWithXi(it->name) ? DISKOP_ITEM_INSTR : DISKOP_ITEM_SAMPLE)
			: ((S.what == BT2_WHAT_SOUND) ? DISKOP_ITEM_SAMPLE : DISKOP_ITEM_MODULE);
		bt2SearchClose();
		showFolder(folder, item);
		return;
	}

	// a song, an instrument or a sample: opened in FT2, like a dropped file (Modland's songs are downloaded first)
	char path[2600];
	if (it->modland)
	{
		if (!bt2ModlandMaterialize(it->rel, path, sizeof (path))) // (its name here)
		{
			bt2SearchClose();
			bt2Toast("Fant ikke filen");
			return;
		}
	}
	else
	{
		bt2PathJoin(path, sizeof (path), folder, it->name);
	}

	bt2SearchClose();

	SDL_Event ev;
	memset(&ev, 0, sizeof (ev));
	ev.type = SDL_DROPFILE;
	ev.drop.file = SDL_strdup(path);
	ev.drop.windowID = (video.window != NULL) ? SDL_GetWindowID(video.window) : 0;
	if (ev.drop.file != NULL && SDL_PushEvent(&ev) <= 0)
		SDL_free(ev.drop.file);
}

// ------------------------------------------------------------------------------------------------
// the keyboard

static void moveSel(int32_t d)
{
	const int32_t n = shownCount();
	if (n == 0)
		return;

	S.sel += d;
	if (S.sel < 0) S.sel = 0;
	if (S.sel > n - 1) S.sel = n - 1;

	// (scrolled so the row is seen)
	const frect in = listInner();
	const float top = S.sel * S.rowH, bottom = top + S.rowH;
	if (top < S.scroll)
		S.scroll = top;
	else if (bottom > S.scroll + in.h)
		S.scroll = bottom - in.h;

	S.dirty = true;
}

static int32_t prevChar(int32_t pos)
{
	if (pos <= 0)
		return 0;
	pos--;
	while (pos > 0 && ((uint8_t)S.query[pos] & 0xC0) == 0x80)
		pos--;
	return pos;
}

static int32_t nextChar(int32_t pos)
{
	const int32_t len = (int32_t)strlen(S.query);
	if (pos >= len)
		return len;
	pos++;
	while (pos < len && ((uint8_t)S.query[pos] & 0xC0) == 0x80)
		pos++;
	return pos;
}

static void insertText(const char *t)
{
	if (S.selectAll)
	{
		S.query[0] = '\0';
		S.caret = 0;
		S.selectAll = false;
	}

	const size_t len = strlen(S.query), add = strlen(t);
	if (len + add >= sizeof (S.query) - 1)
		return;

	memmove(S.query + S.caret + add, S.query + S.caret, len - (size_t)S.caret + 1);
	memcpy(S.query + S.caret, t, add);
	S.caret += (int32_t)add;
	queryChanged();
}

static void deleteRange(int32_t from, int32_t to)
{
	if (to <= from)
		return;

	memmove(S.query + from, S.query + to, strlen(S.query) - (size_t)to + 1);
	S.caret = from;
	queryChanged();
}

static void handleKey(const SDL_KeyboardEvent *k)
{
	const SDL_Keycode key = k->keysym.sym;
	const bool ctrl = (k->keysym.mod & KMOD_CTRL) != 0, shift = (k->keysym.mod & KMOD_SHIFT) != 0;

	switch (key)
	{
		case SDLK_ESCAPE: bt2SearchClose(); return;
		case SDLK_DOWN: moveSel(1); return;
		case SDLK_UP: moveSel(-1); return;
		case SDLK_PAGEDOWN: moveSel(10); return;
		case SDLK_PAGEUP: moveSel(-10); return;

		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			if (S.runAt != 0)
				runNow(); // (the search for what was just typed)
			if (S.sel >= 0 && S.sel < shownCount())
				act(S.results[S.sel], shift);
			return;

		case SDLK_BACKSPACE:
			if (S.selectAll)
			{
				S.selectAll = false;
				deleteRange(0, (int32_t)strlen(S.query));
			}
			else if (S.caret > 0)
			{
				deleteRange(prevChar(S.caret), S.caret);
			}
			return;

		case SDLK_DELETE:
			if (S.selectAll)
			{
				S.selectAll = false;
				deleteRange(0, (int32_t)strlen(S.query));
			}
			else
			{
				deleteRange(S.caret, nextChar(S.caret));
			}
			return;

		case SDLK_LEFT:
			S.caret = S.selectAll ? 0 : prevChar(S.caret);
			S.selectAll = false;
			S.caretTime = GetTickCount64();
			S.dirty = true;
			return;

		case SDLK_RIGHT:
			S.caret = S.selectAll ? (int32_t)strlen(S.query) : nextChar(S.caret);
			S.selectAll = false;
			S.caretTime = GetTickCount64();
			S.dirty = true;
			return;

		case SDLK_HOME:
			S.caret = 0;
			S.selectAll = false;
			S.caretTime = GetTickCount64();
			S.dirty = true;
			return;

		case SDLK_END:
			S.caret = (int32_t)strlen(S.query);
			S.selectAll = false;
			S.caretTime = GetTickCount64();
			S.dirty = true;
			return;

		case SDLK_a:
			if (ctrl)
			{
				S.selectAll = true;
				S.dirty = true;
			}
			return;

		case SDLK_c:
			if (ctrl && S.query[0] != '\0')
				SDL_SetClipboardText(S.query);
			return;

		case SDLK_v:
			if (ctrl && SDL_HasClipboardText())
			{
				char *t = SDL_GetClipboardText();
				if (t != NULL)
				{
					for (char *p = t; *p != '\0'; p++)
					{
						if ((uint8_t)*p < 0x20)
							*p = ' '; // (one line)
					}
					insertText(t);
					SDL_free(t);
				}
			}
			return;

		default:
			return;
	}
}

// ------------------------------------------------------------------------------------------------
// the mouse

static void eventToCss(int32_t wx, int32_t wy, float *x, float *y)
{
	int32_t ww = 0, wh = 0;
	SDL_GetWindowSize(video.window, &ww, &wh);
	float dx = (float)wx, dy = (float)wy;
	if (ww > 0 && wh > 0 && S.outW > 0)
	{
		dx = wx * (float)S.outW / ww;
		dy = wy * (float)S.outH / wh;
	}

	*x = dx / S.s;
	*y = dy / S.s;
}

static bool inside(frect r, float x, float y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// what is at (x, y) (CSS pixels in the window); *row = the row for T_ROW/T_FOLDERBTN
static int32_t hitTest(float x, float y, int32_t *row)
{
	*row = -1;
	if (!inside(S.panel, x, y))
		return T_BACKDROP;

	x -= S.panel.x;
	y -= S.panel.y;

	if (inside(S.closeBtn, x, y)) return T_CLOSE;
	frect clr;
	if (clearButton(&clr) && inside(clr, x, y)) return T_CLEAR;
	if (inside(S.input, x, y)) return T_INPUT;
	for (int32_t i = 0; i < 3; i++) if (inside(S.whereBtn[i], x, y)) return T_WHERE + i;
	for (int32_t i = 0; i < 4; i++) if (inside(S.whatBtn[i], x, y)) return T_WHAT + i;

	const frect in = listInner();
	if (inside(in, x, y))
	{
		if (S.scrollbar && x >= in.x + in.w - 12.0f)
			return T_SCROLLBAR;

		const float ly = y - in.y + S.scroll;
		const int32_t n = shownCount();
		const int32_t i = (int32_t)floorf(ly / S.rowH);
		if (i >= 0 && i < n)
		{
			*row = i;
			rowParts p;
			rowLayout(S.results[i], rowWidth(), &p);
			const float rx = x - in.x, ry = ly - i * S.rowH;
			if (p.hasBtn && rx >= p.btnX && rx < p.btnX + p.btnW && ry >= (S.rowH - p.btnH) * 0.5f && ry < (S.rowH + p.btnH) * 0.5f)
				return T_FOLDERBTN;
			return T_ROW;
		}

		if (S.numResults > S.shown)
		{
			char more[64];
			snprintf(more, sizeof (more), "Vis flere (%d til)", (int)(S.numResults - S.shown));
			const frect b = { in.x + 10.0f, in.y + n * S.rowH + 8.0f - S.scroll, buttonW(more, false, 15.0f, 12.0f), 36.0f };
			if (inside(b, x, y))
				return T_MORE;
		}
	}

	return T_PANEL;
}

static void placeCaret(float x) // (x: CSS pixels in the panel)
{
	const bt2Font f = font(18.0f, 600);
	const float want = x - (S.input.x + 2.0f + 10.0f) + S.inputScroll;

	int32_t best = 0;
	float bestDist = 1e9f;
	for (int32_t pos = 0;; pos = nextChar(pos))
	{
		char before[1024];
		snprintf(before, sizeof (before), "%.*s", (int)pos, S.query);
		const float d = fabsf(textW(f, before) - want);
		if (d < bestDist)
		{
			bestDist = d;
			best = pos;
		}

		if (S.query[pos] == '\0')
			break;
	}

	S.caret = best;
	S.selectAll = false;
	S.caretTime = GetTickCount64();
	S.dirty = true;
}

static void scrollBy(float d)
{
	S.scroll += d;
	const float maxScroll = fmaxf(0.0f, S.contentH - listInner().h);
	if (S.scroll > maxScroll) S.scroll = maxScroll;
	if (S.scroll < 0.0f) S.scroll = 0.0f;
	S.dirty = true;
}

static void mouseDown(float x, float y)
{
	int32_t row;
	S.press = hitTest(x, y, &row);
	S.pressRow = row;

	if (S.press == T_INPUT)
	{
		placeCaret(x - S.panel.x);
	}
	else if (S.press == T_SCROLLBAR)
	{
		const frect t = thumbRect();
		const float py = y - S.panel.y;
		if (py >= t.y && py < t.y + t.h)
		{
			S.dragThumb = true;
			S.dragStartY = py;
			S.dragStartScroll = S.scroll;
		}
		else
		{
			scrollBy((py < t.y) ? -listInner().h : listInner().h); // (a page up or down)
		}
	}

	S.dirty = true;
}

static void mouseUp(float x, float y)
{
	const int32_t pressed = S.press, pressedRow = S.pressRow;
	S.press = T_NONE;
	S.dragThumb = false;
	S.dirty = true;

	int32_t row;
	const int32_t now = hitTest(x, y, &row);
	if (now != pressed || row != pressedRow)
		return; // (let go somewhere else: nothing, like a click in the web version)

	if (pressed == T_BACKDROP || pressed == T_CLOSE)
	{
		bt2SearchClose();
	}
	else if (pressed == T_CLEAR)
	{
		S.query[0] = '\0';
		S.caret = 0;
		S.selectAll = false;
		S.inputScroll = 0.0f;
		queryChanged();
	}
	else if (pressed >= T_WHERE && pressed < T_WHERE + 3)
	{
		S.where = pressed - T_WHERE;
		runNow();
	}
	else if (pressed >= T_WHAT && pressed < T_WHAT + 4)
	{
		S.what = pressed - T_WHAT;
		runNow();
	}
	else if (pressed == T_MORE)
	{
		S.shown += SHOW_STEP;
	}
	else if ((pressed == T_ROW || pressed == T_FOLDERBTN) && row >= 0 && row < shownCount())
	{
		S.sel = row;
		act(S.results[row], pressed == T_FOLDERBTN);
	}
}

static void mouseMove(float x, float y)
{
	if (S.dragThumb)
	{
		const frect in = listInner(), t = thumbRect();
		const float track = fmaxf(1.0f, in.h - t.h);
		const float maxScroll = fmaxf(0.0f, S.contentH - in.h);
		S.scroll = S.dragStartScroll + ((y - S.panel.y) - S.dragStartY) * (maxScroll / track);
		scrollBy(0.0f);
		return;
	}

	int32_t row;
	const int32_t t = hitTest(x, y, &row);
	setCursor((t == T_INPUT) ? 2 : (t == T_CLOSE || (t >= T_WHERE && t < T_SCROLLBAR) || t == T_ROW || t == T_FOLDERBTN) ? 1 : 0);
}

// ------------------------------------------------------------------------------------------------
// events

bool bt2SearchShortcut(const SDL_Event *ev)
{
	if (ev->type != SDL_KEYDOWN || ev->key.keysym.scancode != SDL_SCANCODE_F)
		return false;

	const SDL_Keymod m = (SDL_Keymod)ev->key.keysym.mod;
	if (!(m & KMOD_CTRL) || (m & (KMOD_SHIFT | KMOD_ALT | KMOD_GUI)) || editor.editTextFlag || ui.sysReqShown)
		return false;

	bt2SearchOpen(); // (FT2 itself has nothing on Ctrl+F)
	return true;
}

bool bt2SearchHandleEvent(SDL_Event *ev)
{
	if (!S.open)
		return false;

	if (S.dirty && (ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEBUTTONUP || ev->type == SDL_MOUSEMOTION || ev->type == SDL_MOUSEWHEEL))
		doLayout(); // (where everything is now, before the next picture is drawn)

	switch (ev->type)
	{
		case SDL_KEYDOWN:
			handleKey(&ev->key);
			return true;

		case SDL_KEYUP:
			return true;

		case SDL_TEXTINPUT:
			insertText(ev->text.text);
			return true;

		case SDL_TEXTEDITING:
			return true;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		{
			if (ev->button.button != SDL_BUTTON_LEFT)
				return true;

			float x, y;
			eventToCss(ev->button.x, ev->button.y, &x, &y);
			if (ev->type == SDL_MOUSEBUTTONDOWN)
				mouseDown(x, y);
			else
				mouseUp(x, y);
			return true;
		}

		case SDL_MOUSEMOTION:
		{
			float x, y;
			eventToCss(ev->motion.x, ev->motion.y, &x, &y);
			mouseMove(x, y);
			return true;
		}

		case SDL_MOUSEWHEEL:
			scrollBy(-ev->wheel.y * WHEEL_STEP);
			return true;

		case SDL_FINGERDOWN:
		case SDL_FINGERUP:
		case SDL_FINGERMOTION:
			return true;

		case SDL_QUIT:
		case SDL_DROPFILE:
			bt2SearchClose(); // (FT2 handles these: its question would be hidden behind the window)
			return false;

		default:
			return false;
	}
}

#endif
