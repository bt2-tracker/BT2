/* BerntTracker (Windows): the message and the download box on top of FT2's screen.
** See bt2_winoverlay.h.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft2_header.h"
#include "ft2_video.h"
#include "ft2_mouse.h"
#include "ft2_events.h"
#include "ft2_touchkeys.h"
#include "bt2_winui.h"
#include "bt2_winoverlay.h"

// the web page's colours (index.html)
#define C_BOX     0xFF12161A
#define C_BORDER  0xFF596169
#define C_TEXT    0xFFDFDFE3
#define C_ACCENT  0xFF7AA8EC

#define TOAST_MS 2600
#define FADE_MS 250

typedef struct layer_t
{
	bt2Canvas canvas;
	SDL_Texture *tex;
	int32_t texW, texH;
	char drawn[1024]; // what the picture shows now
	float drawnScale;
	int32_t drawnW;
} layer;

static CRITICAL_SECTION lock;
static INIT_ONCE lockOnce = INIT_ONCE_STATIC_INIT;

static char toastText[1024];
static uint64_t toastStart; // 0 = no message
static layer toastLayer;

static char netText[1024];
static double netProgress, netShown; // (netShown: only in the main thread)
static bool netVisible, netReset;
static uint64_t netLastTick;
static layer netLayer;

static float scale = -1.0f;

static BOOL CALLBACK initLock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	InitializeCriticalSection(&lock);
	return TRUE;
	(void)once; (void)param; (void)ctx;
}

static void lockIt(void)
{
	InitOnceExecuteOnce(&lockOnce, initLock, NULL, NULL);
	EnterCriticalSection(&lock);
}

static void unlockIt(void)
{
	LeaveCriticalSection(&lock);
}

float bt2OverlayScale(void) // like Chrome: Windows' display zoom (100 %, 125 %, 150 % ...)
{
	if (scale < 0.0f)
	{
		HDC dc = GetDC(NULL);
		const int32_t dpi = (dc != NULL) ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
		if (dc != NULL)
			ReleaseDC(NULL, dc);

		scale = dpi / 96.0f;
		if (scale < 1.0f)
			scale = 1.0f;
	}

	return scale;
}

void bt2Toast(const char *text)
{
	lockIt();
	snprintf(toastText, sizeof (toastText), "%s", (text != NULL) ? text : "");
	toastStart = GetTickCount64();
	if (toastStart == 0)
		toastStart = 1;
	unlockIt();
}

void bt2NetBoxShow(const char *text, double progress)
{
	lockIt();
	snprintf(netText, sizeof (netText), "%s", (text != NULL) ? text : "");
	if (!netVisible)
		netReset = true; // (a new box: its bar starts at 0)
	netProgress = (progress < 0.0) ? 0.0 : (progress > 1.0) ? 1.0 : progress;
	netVisible = true;
	unlockIt();
}

void bt2NetBoxHide(void)
{
	lockIt();
	netVisible = false;
	unlockIt();
}

// ------------------------------------------------------------------------------------------------
// drawing

static bool layerSize(SDL_Renderer *r, layer *l, int32_t w, int32_t h)
{
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	if (l->tex != NULL && l->texW == w && l->texH == h)
		return true;

	if (l->tex != NULL)
		SDL_DestroyTexture(l->tex);
	bt2CanvasFree(&l->canvas);

	l->tex = NULL;
	l->drawn[0] = '\0';
	if (!bt2CanvasAlloc(&l->canvas, w, h))
		return false;

	l->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
	if (l->tex == NULL)
		return false;

	SDL_SetTextureBlendMode(l->tex, SDL_BLENDMODE_BLEND);
	l->texW = w;
	l->texH = h;
	return true;
}

static void layerFree(layer *l)
{
	if (l->tex != NULL)
		SDL_DestroyTexture(l->tex);
	l->tex = NULL;
	bt2CanvasFree(&l->canvas);
	l->drawn[0] = '\0';
}

typedef struct boxSize_t
{
	bt2Font font;
	float lineH, padX, padY, border, innerW, textW;
	int32_t lines, w, h;
} boxSize;

// a box like the web page's: "padding: 10px 16px; border: 1px; font-size: 15px", as wide as its text, at most
// half the window wide (left: 50% with translateX(-50%)), extraH = more contents below the text
static void measureBox(boxSize *b, const char *text, float s, int32_t outW, float minInnerW, float extraH)
{
	b->font.size = (int32_t)floorf(15.0f * s + 0.5f);
	b->font.weight = 400;
	b->font.mono = false;

	b->lineH = bt2LineHeight(b->font);
	b->padX = 16.0f * s;
	b->padY = 10.0f * s;
	b->border = fmaxf(1.0f, floorf(s + 0.5f));

	const float avail = fmaxf(40.0f * s, outW * 0.5f - 2.0f * (b->padX + b->border));
	b->textW = bt2TextWidth(b->font, text);
	b->innerW = fminf(fmaxf(b->textW, minInnerW), avail);
	b->lines = (b->textW > b->innerW) ? bt2TextLines(b->font, b->innerW, text) : 1;
	if (b->lines < 1)
		b->lines = 1;

	b->w = (int32_t)ceilf(b->innerW + 2.0f * (b->padX + b->border));
	b->h = (int32_t)ceilf(b->lines * b->lineH + extraH + 2.0f * (b->padY + b->border));
}

static void drawToast(SDL_Renderer *r, int32_t outW, uint64_t now)
{
	char text[1024];
	uint64_t start;

	lockIt();
	start = toastStart;
	snprintf(text, sizeof (text), "%s", toastText);
	unlockIt();

	if (start == 0 || text[0] == '\0')
		return;

	const uint64_t age = now - start;
	if (age >= TOAST_MS + FADE_MS)
	{
		lockIt();
		if (toastStart == start)
			toastStart = 0;
		unlockIt();
		layerFree(&toastLayer);
		return;
	}

	float alpha = 1.0f;
	if (age < FADE_MS)
		alpha = age / (float)FADE_MS;
	else if (age > TOAST_MS)
		alpha = 1.0f - (age - TOAST_MS) / (float)FADE_MS;

	const float s = bt2OverlayScale();
	boxSize b;
	measureBox(&b, text, s, outW, 0.0f, 0.0f);

	if (!layerSize(r, &toastLayer, b.w, b.h))
		return;

	layer *l = &toastLayer;
	if (strcmp(l->drawn, text) != 0 || l->drawnScale != s || l->drawnW != outW)
	{
		bt2CanvasClear(&l->canvas, 0x00000000);
		bt2RoundRect(&l->canvas, 0.0f, 0.0f, (float)b.w, (float)b.h, 8.0f * s, C_BOX, b.border, C_BORDER);
		bt2DrawText(&l->canvas, b.font, b.padX + b.border, b.padY + b.border, b.innerW + 1.0f, b.lineH, BT2_ALIGN_LEFT, C_TEXT, text);
		SDL_UpdateTexture(l->tex, NULL, l->canvas.px, l->canvas.w * (int32_t)sizeof (uint32_t));

		snprintf(l->drawn, sizeof (l->drawn), "%s", text);
		l->drawnScale = s;
		l->drawnW = outW;
	}

	SDL_Rect dst = { (outW - b.w) / 2, (int32_t)floorf(14.0f * s + 0.5f), b.w, b.h };
	SDL_SetTextureAlphaMod(l->tex, (Uint8)(alpha * 255.0f + 0.5f));
	SDL_RenderCopy(r, l->tex, NULL, &dst);
}

static void drawNetBox(SDL_Renderer *r, int32_t outW, int32_t outH, uint64_t now)
{
	char text[1024];
	bool visible, reset;
	double target;

	lockIt();
	visible = netVisible;
	target = netProgress;
	reset = netReset;
	netReset = false;
	snprintf(text, sizeof (text), "%s", netText);
	unlockIt();

	if (reset)
		netShown = 0.0;

	if (!visible)
	{
		if (netLayer.tex != NULL)
			layerFree(&netLayer);
		return;
	}

	// the bar moves smoothly to the new width (the web page: "transition: width .2s")
	const double dt = (netLastTick != 0) ? (double)(now - netLastTick) : 0.0;
	netLastTick = now;
	netShown += (target - netShown) * (1.0 - exp(-dt / 70.0));
	if (fabs(target - netShown) < 0.002)
		netShown = target;

	const float s = bt2OverlayScale();
	const float barW = 260.0f * s, barH = 6.0f * s, barGap = 8.0f * s;
	boxSize b;
	measureBox(&b, text, s, outW, barW, barGap + barH);

	if (!layerSize(r, &netLayer, b.w, b.h))
		return;

	// (redrawn every frame while it is shown: the bar moves; the box is small)
	layer *l = &netLayer;
	bt2CanvasClear(&l->canvas, 0x00000000);
	bt2RoundRect(&l->canvas, 0.0f, 0.0f, (float)b.w, (float)b.h, 8.0f * s, C_BOX, b.border, C_BORDER);
	bt2DrawText(&l->canvas, b.font, b.padX + b.border, b.padY + b.border, b.innerW + 1.0f, b.lineH, BT2_ALIGN_CENTER, C_TEXT, text);

	const float bx = floorf((b.w - barW) * 0.5f), by = b.padY + b.border + b.lines * b.lineH + barGap;
	bt2RoundRect(&l->canvas, bx, by, barW, barH, 3.0f * s, C_BOX, 0.0f, 0);
	if (netShown > 0.0)
		bt2RoundRect(&l->canvas, bx, by, (float)(barW * netShown), barH, 3.0f * s, C_ACCENT, 0.0f, 0);

	SDL_UpdateTexture(l->tex, NULL, l->canvas.px, l->canvas.w * (int32_t)sizeof (uint32_t));

	SDL_Rect dst = { (outW - b.w) / 2, outH - (int32_t)floorf(40.0f * s + 0.5f) - b.h, b.w, b.h };
	SDL_SetTextureAlphaMod(l->tex, 255);
	SDL_RenderCopy(r, l->tex, NULL, &dst);
}

void bt2OverlayRender(SDL_Renderer *r)
{
	int32_t outW = 0, outH = 0;
	if (r == NULL || SDL_GetRendererOutputSize(r, &outW, &outH) != 0 || outW <= 0 || outH <= 0)
		return;

	const uint64_t now = GetTickCount64();

	drawToast(r, outW, now);
	drawNetBox(r, outW, outH, now);
}

void bt2OverlayFree(void)
{
	layerFree(&toastLayer);
	layerFree(&netLayer);
}

// ------------------------------------------------------------------------------------------------
// work that takes a while (downloads), with the window kept alive

typedef struct jobCtx_t
{
	int32_t (*job)(void *arg);
	void *arg;
	int32_t result;
	volatile LONG done;
} jobCtx;

static int SDLCALL jobThread(void *p)
{
	jobCtx *c = (jobCtx *)p;
	c->result = c->job(c->arg);
	InterlockedExchange(&c->done, 1);
	return 0;
}

bool bt2RunJob(int32_t (*job)(void *arg), void *arg, volatile int32_t *cancel, int32_t *result)
{
	jobCtx *c = (jobCtx *)calloc(1, sizeof (jobCtx));
	if (c == NULL)
	{
		*result = job(arg);
		return true;
	}

	c->job = job;
	c->arg = arg;

	SDL_Thread *t = SDL_CreateThread(jobThread, "BT2 job", c);
	if (t == NULL)
	{
		free(c);
		*result = job(arg); // (then it is done here, with the window standing still meanwhile)
		return true;
	}

	bool quit = false;
	while (!c->done)
	{
		SDL_Event ev;
		while (SDL_PollEvent(&ev))
		{
			handleWaitVblQuirk(&ev);

			if (ev.type == SDL_QUIT)
				quit = true;
			else if (ev.type == SDL_DROPFILE && ev.drop.file != NULL)
				SDL_free(ev.drop.file);
			else if (ev.type == SDL_WINDOWEVENT || ev.type == SDL_FINGERUP)
				tkHandleEvent(&ev);
			// (everything else is dropped, like when FT2 is busy)
		}

		if (quit)
			break;

		readMouseXY();
		flipFrame(); // (FT2's screen as it was, with the download box; waits for the next frame)
	}

	if (quit)
	{
		if (cancel != NULL)
			*cancel = 1;

		SDL_DetachThread(t); // (c and arg are left to the job: it may still be running)

		SDL_Event q;
		memset(&q, 0, sizeof (q));
		q.type = SDL_QUIT;
		SDL_PushEvent(&q); // (FT2 handles the closing as usual)
		return false;
	}

	SDL_WaitThread(t, NULL);
	*result = c->result;
	free(c);
	return true;
}

#endif
