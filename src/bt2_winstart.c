/* BerntTracker (Windows): the start screen, the same as the web version's. See bt2_winstart.h.
**
** Sizes and colors below are the web page's (web/index.html, the CSS for #start), in CSS pixels.
** They are multiplied by "s" (pixels per CSS pixel) when drawn: the screen's zoom (like Chrome's),
** made smaller if the window is too low to show everything.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ft2_header.h"
#include "ft2_config.h"
#include "ft2_video.h"
#include "ft2_audio.h"
#include "ft2_mouse.h"
#include "ft2_hpc.h"
#include "ft2_wasapi.h"
#include "ft2_asio.h"
#include "bt2_winui.h"
#include "bt2_winstart.h"
#include "bt2_winupdate.h"
#include "bt2_winhome.h"
#include "bt2_version.h"

// colors (web/index.html)
#define C_TEXT    0xFFDFDFE3 // --text
#define C_DIM     0xFF8F96A0 // --dim
#define C_ACCENT  0xFF7AA8EC // --accent
#define C_PANEL   0xFF2D3135 // --bg (the sound test's background)
#define C_YELLOW  0xFFF0D070 // #updateText
#define C_CREDITS 0xFF5D646C // #credits
#define C_LINK    0xFF7D92AA // #credits a

// the texts (the same as on the web page where they mean the same)
#define LABEL_START      "Trykk for å starte"
#define LABEL_FULLSCREEN "Start i fullskjerm"
#define LABEL_BUFFER     "Lydbuffer:"
#define LABEL_SYSTEM     "Lydsystem:"   // (v0.31: ASIO or Windows' ordinary sound, only in the Windows program)
#define LABEL_FOLDER     "Åpne BT2-mappa"
#define NOTE_BUFFER      "Minst gir lavest forsinkelse. Knitrer lyden, velg Normal eller Stor."
#define NOTE_APP         "Appen sjekker etter ny versjon hver gang den åpnes, og du velger selv om du vil oppdatere. " \
                         "En oppdatering endrer bare selve programmet – låtene, samplene, mappene og innstillingene " \
                         "dine blir aldri rørt."
#define HINT_PC          "Trykk <b>Alt+Enter</b> for fullskjerm (<b>Esc</b> går ut igjen)."
#define HINT_CTRL_A      "Hold <b>Ctrl</b> og trykk <b>pil opp/ned</b>" // (one line, or two in a narrow window)
#define HINT_CTRL_B      "for å hoppe 16 rader om gangen (som PgUp/PgDn)."
#define CREDITS_A        "Basert på "
#define CREDITS_LINK     "ft2-clone"
#define CREDITS_B        "av Olav «8bitbubsy» Sørensen (BSD-3)."
#define CREDITS_C        "Grafikk av Magnus «Vogue» Högdahl (CC BY-NC-SA 4.0)."
#define CREDITS_URL      L"https://github.com/8bitbubsy/ft2-clone"

typedef struct buttonStyle_t
{
	float fontSize;
	int32_t weight;
	float padX, padY, radius, border, minH;
	uint32_t bg, line, color;
} buttonStyle;

static const buttonStyle styleBig     = { 20.0f, 600, 36.0f, 16.0f, 10.0f, 2.0f, 56.0f, 0xFF6CC46F, 0xFF357A38, 0xFF000000 }; // button.big
static const buttonStyle styleSmall   = { 15.0f, 400, 18.0f, 10.0f,  8.0f, 1.0f, 44.0f, 0xFF3B4045, 0xFF596169, C_TEXT };     // button.small
static const buttonStyle styleSmallOn = { 15.0f, 400, 18.0f, 10.0f,  8.0f, 1.0f, 44.0f, 0xFF4F6E99, 0xFF7AA8EC, C_TEXT };     // button.small.on

enum
{
	HIT_START, HIT_FULLSCREEN, HIT_BUFFER0, HIT_BUFFER1, HIT_BUFFER2, HIT_UPDATE_NOW, HIT_UPDATE_LATER,
	HIT_DIAG, HIT_FOLDER, HIT_LINK, HIT_DIAG_CLOSE, HIT_SYSTEM0, HIT_SYSTEM1,
	HIT_COUNT
};

static SDL_Rect hitRect[HIT_COUNT];
static bool hitShown[HIT_COUNT];

static float s = 1.0f; // pixels per CSS pixel
static float vw, vh;   // the window, in CSS pixels
static int32_t outW, outH;
static bool compact;   // a low window: the web page's "max-height: 560px" layout
static bt2Canvas ui, logoSrc, logoScaled;
static bool logoLoaded;
static SDL_Texture *uiTex, *starTex;
static SDL_Cursor *arrowCursor, *handCursor;

static bool optFullscreen, updateBoxHidden, diagOpen, restartFailed, startFocused;
static char toastText[120];
static uint64_t toastUntil;
static int32_t bufferChoice; // 0 = Minst, 1 = Normal, 2 = Stor (FT2's Smaller/Medium/Bigger latency)
static bt2UpdateInfo upd;
static char audioLine[200];
static char systemNote[200]; // (v0.31: why ASIO isn't used although it is chosen, or empty)

// ------------------------------------------------------------------------------------------------
// the stars: the same as the About screen in FT2 (ft2_about.c), as in the web version

#define STARS 1500
#define STAR_W 632
#define TWO_PI 6.283185307179586

static float starX[STARS], starY[STARS], starZ[STARS];
static double rotX, rotY, rotZ;
static uint32_t *starBuf;
static int32_t starH;

static void starsInit(void)
{
	for (int32_t i = 0; i < STARS; i++)
	{
		starX[i] = (float)((rand() & 32767) - 16384);
		starY[i] = (float)((rand() & 32767) - 16384);
		starZ[i] = (float)((rand() & 32767) - 16384);
	}

	rotX = rotY = rotZ = 0.0;
}

static void starsMove(double steps) // (FT2 moves them a fixed step per 60 Hz frame)
{
	for (int32_t i = 0; i < STARS; i++)
	{
		starZ[i] += (float)(3.0 * steps);
		if (starZ[i] > 16384.0f)
			starZ[i] -= 32768.0f;
	}

	rotX -= 0.0003 * steps;
	rotY -= 0.0002 * steps;
	rotZ += 0.0001 * steps;
}

static void starsDraw(void)
{
	const uint32_t BLACK = 0xFF000000;
	const int32_t W = STAR_W, H = starH;
	if (starBuf == NULL || starTex == NULL)
		return;

	for (int32_t i = 0; i < W * H; i++)
		starBuf[i] = BLACK;

	const double xs = sin(rotX * TWO_PI), xc = cos(rotX * TWO_PI);
	const double ys = sin(rotY * TWO_PI), yc = cos(rotY * TWO_PI);
	const double zs = sin(rotZ * TWO_PI), zc = cos(rotZ * TWO_PI);
	const float mxx = (float)(xc * zc + zs * xs * ys), myx = (float)(xs * yc), mzx = (float)(zc * xs * ys - xc * zs);
	const float mxy = (float)(zs * xc * ys - xs * zc), myy = (float)(xc * yc), mzy = (float)(xs * zs + zc * xc * ys);
	const float mxz = (float)(yc * zs), myz = (float)(-ys), mzz = (float)(yc * zc);

	for (int32_t i = 0; i < STARS; i++)
	{
		const float z = mxz * starX[i] + myz * starY[i] + mzz * starZ[i] + 16384.0f;
		if (z <= 0.0f)
			continue;

		const int32_t y = (int32_t)((mxy * starX[i] + myy * starY[i] + mzy * starZ[i]) * 400.0f / z) + (H >> 1);
		if (y < 0 || y >= H)
			continue;

		const int32_t x = (int32_t)((mxx * starX[i] + myx * starY[i] + mzx * starZ[i]) * 400.0f / z) + (W >> 1);
		if (x < 0 || x >= W)
			continue;

		const int32_t p = y * W + x;
		if (starBuf[p] != BLACK)
			continue;

		const float v = 255.0f - fminf(255.0f, z / 128.0f);
		const int32_t r = (int32_t)fmaxf(0.0f, v - 96.0f);
		const int32_t g = (int32_t)fmaxf(0.0f, v - 48.0f);
		const int32_t b = (int32_t)fminf(255.0f, v + 64.0f);
		starBuf[p] = BLACK | (r << 16) | (g << 8) | b;

		// a little shine around the star (31 %, like FT2)
		const uint32_t sc = BLACK | ((uint32_t)(r * 0.31f) << 16) | ((uint32_t)(g * 0.31f) << 8) | (uint32_t)(b * 0.31f);
		if (x > 0     && starBuf[p - 1] == BLACK) starBuf[p - 1] = sc;
		if (x < W - 1 && starBuf[p + 1] == BLACK) starBuf[p + 1] = sc;
		if (y > 0     && starBuf[p - W] == BLACK) starBuf[p - W] = sc;
		if (y < H - 1 && starBuf[p + W] == BLACK) starBuf[p + W] = sc;
	}

	SDL_UpdateTexture(starTex, NULL, starBuf, W * (int32_t)sizeof (uint32_t));
}

// ------------------------------------------------------------------------------------------------
// the sound

static int32_t latencyMs(void) // -1 = not known
{
	if (audio.haveFreq == 0)
		return -1;

	if (audio.dev == WIN_ASIO_DEVICE_ID)
	{
		int32_t frames = winAsioOutputLatencyFrames();
		if (frames <= 0)
			frames = (int32_t)audio.haveSamples * 2;

		return (int32_t)floor((frames * 1000.0) / audio.haveFreq + 0.5);
	}

	if (audio.dev == WIN_EXCLUSIVE_DEVICE_ID) // (its block, plus the one being played)
		return (int32_t)floor((audio.haveSamples * 2 * 1000.0) / audio.haveFreq + 0.5);

	return -1;
}

static const char *engineName(void)
{
	if (audio.dev == WIN_ASIO_DEVICE_ID) return "ASIO";
	if (audio.dev == WIN_EXCLUSIVE_DEVICE_ID) return "Windows eksklusiv";
	if (audio.dev != 0) return "Windows vanlig";
	return "ingen lydenhet";
}

static void buildSystemNote(void);

static void buildAudioLine(void)
{
	const int32_t ms = latencyMs();
	if (audio.dev == 0)
		snprintf(audioLine, sizeof (audioLine), "Lyd: <b>%s</b>", engineName());
	else if (ms > 0)
		snprintf(audioLine, sizeof (audioLine), "Lyd: <b>%s</b> · %u Hz · ca. %d ms", engineName(), audio.haveFreq, ms);
	else
		snprintf(audioLine, sizeof (audioLine), "Lyd: <b>%s</b> · %u Hz · buffer %u samples", engineName(), audio.haveFreq, audio.haveSamples);

	buildSystemNote();
}

static void buildSystemNote(void)
{
	systemNote[0] = '\0';
	if ((config.specialFlags2 & WIN_SHARED_AUDIO) || audio.dev == WIN_ASIO_DEVICE_ID)
		return;

	if (!winAsioInstalled())
		snprintf(systemNote, sizeof (systemNote), "Fant ingen ASIO-driver (f.eks. ASIO4ALL), så lyden går via %s.", (audio.dev == WIN_EXCLUSIVE_DEVICE_ID) ? "Windows eksklusiv" : "Windows vanlig");
	else
		snprintf(systemNote, sizeof (systemNote), "ASIO startet ikke. Lukk andre programmer som bruker lyden, og velg ASIO på nytt.");
}

static int32_t bufferChoiceFromConfig(void)
{
	if (config.specialFlags & BUFFSIZE_512) return 0;
	if (config.specialFlags & BUFFSIZE_2048) return 2;
	return 1;
}

static const char *bufferName(int32_t choice)
{
	return (choice == 0) ? "Minst" : (choice == 2) ? "Stor" : "Normal";
}

static void windowsVersion(char *out, size_t outLen)
{
	typedef LONG (WINAPI *RtlGetVersionFunc)(OSVERSIONINFOW *);
	snprintf(out, outLen, "ukjent");

	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	RtlGetVersionFunc getVersion = (ntdll != NULL) ? (RtlGetVersionFunc)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
	if (getVersion == NULL)
		return;

	OSVERSIONINFOW v;
	memset(&v, 0, sizeof (v));
	v.dwOSVersionInfoSize = sizeof (v);
	if (getVersion(&v) == 0)
	{
		const char *name = (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) ? "Windows 11" :
			(v.dwMajorVersion == 10) ? "Windows 10" : "Windows";
		snprintf(out, outLen, "%s (%lu.%lu, bygg %lu)", name, (unsigned long)v.dwMajorVersion,
			(unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber);
	}
}

// the sound test's text (like the web version's «Lydtest»)
static void buildDiagText(char *out, size_t outLen)
{
	char engine[120], buffer[160], extra[120], win[80];
	extra[0] = '\0';

	if (audio.dev == WIN_ASIO_DEVICE_ID)
	{
		snprintf(engine, sizeof (engine), "ASIO (%s)", winAsioDriverName());
		const int32_t frames = winAsioOutputLatencyFrames();
		if (frames > 0 && audio.haveFreq > 0)
			snprintf(extra, sizeof (extra), "Driveren sier:  %.1f ms ut (%d samples)\n", (frames * 1000.0) / audio.haveFreq, frames);
	}
	else if (audio.dev == WIN_EXCLUSIVE_DEVICE_ID)
	{
		snprintf(engine, sizeof (engine), "Windows eksklusiv (WASAPI)");
	}
	else if (audio.dev != 0)
	{
		snprintf(engine, sizeof (engine), "Windows vanlig (delt)");
	}
	else
	{
		snprintf(engine, sizeof (engine), "ingen lydenhet");
	}

	if (audio.haveFreq > 0)
	{
		snprintf(buffer, sizeof (buffer), "%s, %u samples (%.1f ms) (velges på startskjermen)", bufferName(bufferChoice),
			audio.haveSamples, (audio.haveSamples * 1000.0) / audio.haveFreq);
	}
	else
	{
		snprintf(buffer, sizeof (buffer), "%s", bufferName(bufferChoice));
	}

	windowsVersion(win, sizeof (win));

	const int32_t ms = latencyMs();
	char latency[80];
	if (ms > 0)
		snprintf(latency, sizeof (latency), "Ca. forsinkelse: %d ms\n", ms);
	else
		latency[0] = '\0';

	snprintf(out, outLen,
		"Motor:          %s\n"
		"Plattform:      Windows (program)\n"
		"Versjon:        %s · %s\n"
		"Samplerate:     %u Hz\n"
		"Lydbuffer:      %s\n"
		"%s%s"
		"System:         %s",
		engine, BT2_VERSION, BT2_DATE, audio.haveFreq, buffer, extra, latency, win);
}

// ------------------------------------------------------------------------------------------------
// drawing helpers (positions and sizes in CSS pixels)

static float snapPx(float css)
{
	return floorf(css * s + 0.5f);
}

static float borderPx(float css) // (Chrome draws a 1px border as whole screen pixels)
{
	const float b = floorf(css * s);
	return (b < 1.0f) ? 1.0f : b;
}

static bt2Font fontCss(float cssSize, int32_t weight)
{
	bt2Font f;
	f.size = (int32_t)floorf(cssSize * s + 0.5f);
	if (f.size < 6)
		f.size = 6;
	f.weight = weight;
	f.mono = false;
	return f;
}

static float lineCss(bt2Font f)
{
	return bt2LineHeight(f) / s;
}

static float widthCss(bt2Font f, const char *t)
{
	return bt2TextWidth(f, t) / s;
}

static float linesCss(bt2Font f, float w, float lineH, const char *t)
{
	return (float)bt2TextLines(f, w * s, t) * lineH;
}

static void setHit(int32_t id, float x, float y, float w, float h)
{
	hitRect[id].x = (int32_t)floorf(x * s);
	hitRect[id].y = (int32_t)floorf(y * s);
	hitRect[id].w = (int32_t)ceilf(w * s);
	hitRect[id].h = (int32_t)ceilf(h * s);
	hitShown[id] = true;
}

static void text(bt2Canvas *c, bt2Font f, float x, float y, float w, float lineH, int32_t align, uint32_t color, const char *t)
{
	if (c != NULL)
		bt2DrawText(c, f, x * s, y * s, w * s, lineH * s, align, color, t);
}

static void buttonSize(const buttonStyle *st, const char *label, float *w, float *h)
{
	const bt2Font f = fontCss(st->fontSize, st->weight);
	*w = widthCss(f, label) + st->padX * 2.0f + st->border * 2.0f;
	*h = lineCss(f) + st->padY * 2.0f + st->border * 2.0f;
	if (*h < st->minH)
		*h = st->minH;
}

static void drawButton(bt2Canvas *c, const buttonStyle *st, const char *label, float x, float y, float w, float h, int32_t hit)
{
	if (c == NULL)
		return;

	const float px = snapPx(x), py = snapPx(y), pw = snapPx(x + w) - px, ph = snapPx(y + h) - py;
	bt2RoundRect(c, px, py, pw, ph, st->radius * s, st->bg, borderPx(st->border), st->line);

	const bt2Font f = fontCss(st->fontSize, st->weight);
	const float l = bt2LineHeight(f);
	bt2DrawText(c, f, px, py + (ph - l) * 0.5f, pw, l, BT2_ALIGN_CENTER, st->color, label);

	if (hit >= 0)
		setHit(hit, x, y, w, h);
}

static void drawCheckbox(bt2Canvas *c, float x, float y, bool on) // (Chrome's check box on Windows, 18px)
{
	const float px = snapPx(x), py = snapPx(y), sz = snapPx(x + 18.0f) - px;
	if (on)
	{
		bt2RoundRect(c, px, py, sz, sz, 2.5f * s, 0xFF0075FF, 0.0f, 0);
		const float k = sz / 18.0f;
		bt2Line(c, px + 4.2f * k, py + 9.2f * k, px + 7.6f * k, py + 12.6f * k, 2.2f * k, 0xFFFFFFFF);
		bt2Line(c, px + 7.6f * k, py + 12.6f * k, px + 13.9f * k, py + 5.6f * k, 2.2f * k, 0xFFFFFFFF);
	}
	else
	{
		bt2RoundRect(c, px, py, sz, sz, 2.5f * s, 0xFFFFFFFF, borderPx(1.0f), 0xFF767676);
	}
}

// The logo is pixel art (like FT2's About screen), so it is shown with a whole number of screen pixels per
// logo pixel, as large as fits in the web page's box: min(92vw, 760px) wide, 64px high in a low window
// (the same rule as fitLogos() in index.html). 0 = the box is smaller than the logo.
static int32_t logoScale(void)
{
	if (!logoLoaded || logoSrc.w <= 0 || logoSrc.h <= 0)
		return 0;

	const float boxW = compact ? 1e9f : fminf(vw * 0.92f, 760.0f);
	const float boxH = compact ? 64.0f : 1e9f;
	return (int32_t)floorf(fminf(boxW * s / logoSrc.w, boxH * s / logoSrc.h) + 1e-4f);
}

static void logoSize(float *w, float *h) // (CSS pixels)
{
	const float lw = logoLoaded ? (float)logoSrc.w : 486.0f, lh = logoLoaded ? (float)logoSrc.h : 78.0f;
	const int32_t k = logoScale();
	if (k >= 1)
	{
		*w = k * lw / s;
		*h = k * lh / s;
	}
	else if (compact)
	{
		*h = 64.0f;
		*w = *h * lw / lh;
	}
	else
	{
		*w = fminf(vw * 0.92f, 760.0f);
		*h = *w * lh / lw;
	}
}

static void drawLogo(bt2Canvas *c, float x, float y, float w, float h)
{
	const int32_t pw = (int32_t)snapPx(w), ph = (int32_t)snapPx(h);
	if (!logoLoaded || pw <= 0 || ph <= 0)
	{
		// (the web page's fallback when the picture is missing: «Bernttracker II» in text)
		const bt2Font f = fontCss(34.0f, 700);
		const float l = lineCss(f);
		const float tw = bt2RichWidth(f, "Bernttracker <b>II</b>") / s;
		bt2DrawRich(c, f, (vw - tw) * 0.5f * s, (y + (h - l) * 0.5f) * s, l * s, C_TEXT, C_ACCENT, "Bernttracker <b>II</b>");
		return;
	}

	const int32_t k = logoScale();
	const int32_t tw = (k >= 1) ? k * logoSrc.w : pw, th = (k >= 1) ? k * logoSrc.h : ph;
	if (logoScaled.px == NULL || logoScaled.w != tw || logoScaled.h != th)
	{
		bt2CanvasFree(&logoScaled);
		if (k >= 1 ? !bt2ScaleUp(&logoSrc, k, &logoScaled) : !bt2Resize(&logoSrc, pw, ph, &logoScaled))
			return;
	}

	bt2Blit(c, &logoScaled, (int32_t)snapPx(x), (int32_t)snapPx(y));
}

// ------------------------------------------------------------------------------------------------
// the start screen's parts, top to bottom (c == NULL: only measure). Returns the height.

static void appBoxTexts(char *status, size_t statusLen, char *updText, size_t updLen, bool *showBar, bool *showUpdate)
{
	*showBar = false;
	*showUpdate = false;
	updText[0] = '\0';

	const bool sameVersion = (strcmp(upd.remoteVersion, BT2_VERSION) == 0);
	if (upd.remoteVersion[0] != '\0')
	{
		if (sameVersion)
			snprintf(updText, updLen, "En ny utgave av versjon %s (%s) er klar på nettet.", upd.remoteVersion, upd.remoteDate);
		else
			snprintf(updText, updLen, "Ny versjon %s (%s) er klar på nettet.", upd.remoteVersion, upd.remoteDate);
	}

	switch (upd.state)
	{
		case BT2_UPD_CHECKING:
			snprintf(status, statusLen, "Sjekker om det finnes en ny versjon …");
			break;

		case BT2_UPD_LATEST:
			snprintf(status, statusLen, "✓ Du har nyeste versjon.");
			break;

		case BT2_UPD_OFFLINE:
			snprintf(status, statusLen, "Ingen nett nå – bruker versjonen som er installert.");
			break;

		case BT2_UPD_AVAILABLE:
			snprintf(status, statusLen, "Versjon %s er installert.", BT2_VERSION);
			*showUpdate = !updateBoxHidden;
			break;

		case BT2_UPD_DOWNLOADING:
			snprintf(status, statusLen, "Laster ned versjon %s … %d %%", upd.remoteVersion, (int32_t)(upd.progress * 100.0 + 0.5));
			*showBar = true;
			break;

		case BT2_UPD_FAILED:
			snprintf(status, statusLen, "Oppdateringen er ikke helt klar på nettet ennå. Prøv igjen om noen minutter.");
			*showUpdate = !updateBoxHidden;
			break;

		case BT2_UPD_NO_ACCESS:
			snprintf(status, statusLen, "Kunne ikke oppdatere: programmet får ikke lov til å endre mappa det ligger i. "
				"Last ned den nye versjonen fra nettsiden.");
			break;

		case BT2_UPD_RESTARTING:
			if (restartFailed)
				snprintf(status, statusLen, "✓ Oppdatert. Lukk programmet og start det igjen.");
			else
				snprintf(status, statusLen, "✓ Oppdatert. Starter på nytt …");
			break;

		default:
			snprintf(status, statusLen, "Kunne ikke sjekke om det finnes en ny versjon nå. Prøver igjen neste gang.");
			break;
	}
}

static float buildAppBox(bt2Canvas *c, float cx, float top)
{
	const float boxW = fminf(560.0f, vw - 32.0f);
	const float inner = boxW - 28.0f - 2.0f; // (padding 10px 14px, border 1px)
	const bt2Font f13 = fontCss(13.0f, 400), f12 = fontCss(12.0f, 400);
	const float l13 = 13.0f * 1.45f, l12 = 12.0f * 1.45f; // (line-height: 1.45)

	char status[300], updText[200];
	bool showBar, showUpdate;
	appBoxTexts(status, sizeof (status), updText, sizeof (updText), &showBar, &showUpdate);

	const float hStatus = linesCss(f13, inner, l13, status);
	float h = hStatus;
	if (showBar)
		h += 8.0f + 6.0f;

	float hUpd = 0.0f, nowW = 0.0f, nowH = 0.0f, laterW = 0.0f, laterH = 0.0f;
	if (showUpdate)
	{
		hUpd = linesCss(f13, inner, l13, updText);
		buttonSize(&styleSmall, "Oppdater nå", &nowW, &nowH);
		buttonSize(&styleSmall, "Senere", &laterW, &laterH);
		h += 6.0f + hUpd + 8.0f + fmaxf(nowH, laterH);
	}

	const float hNote = linesCss(f12, inner, l12, NOTE_APP);
	h += 6.0f + hNote;

	const float boxH = h + 20.0f + 2.0f;
	if (c == NULL)
		return boxH;

	const float bx = cx - boxW * 0.5f;
	const float px = snapPx(bx), py = snapPx(top);
	bt2RoundRect(c, px, py, snapPx(bx + boxW) - px, snapPx(top + boxH) - py, 8.0f * s, 0xFF23272B, borderPx(1.0f), 0xFF3B4045);

	const float tx = bx + 15.0f;
	float y = top + 11.0f;

	text(c, f13, tx, y, inner, l13, BT2_ALIGN_CENTER, C_TEXT, status);
	y += hStatus;

	if (showBar)
	{
		y += 8.0f;
		const float barX = cx - 130.0f;
		bt2RoundRect(c, snapPx(barX), snapPx(y), snapPx(260.0f), snapPx(6.0f), 3.0f * s, 0xFF12161A, 0.0f, 0);
		const float fillW = 260.0f * (float)upd.progress;
		if (fillW > 0.5f)
			bt2RoundRect(c, snapPx(barX), snapPx(y), snapPx(fmaxf(fillW, 6.0f)), snapPx(6.0f), 3.0f * s, C_ACCENT, 0.0f, 0);
		y += 6.0f;
	}

	if (showUpdate)
	{
		y += 6.0f;
		text(c, f13, tx, y, inner, l13, BT2_ALIGN_CENTER, C_YELLOW, updText);
		y += hUpd + 8.0f;

		const float rowH = fmaxf(nowH, laterH);
		float x = cx - (nowW + 10.0f + laterW) * 0.5f;
		drawButton(c, &styleSmall, "Oppdater nå", x, y, nowW, rowH, HIT_UPDATE_NOW);
		x += nowW + 10.0f;
		drawButton(c, &styleSmall, "Senere", x, y, laterW, rowH, HIT_UPDATE_LATER);
		y += rowH;
	}

	y += 6.0f;
	text(c, f12, tx, y, inner, l12, BT2_ALIGN_CENTER, C_DIM, NOTE_APP);

	return boxH;
}

static float buildStart(bt2Canvas *c, float top)
{
	const float gap = compact ? 10.0f : 14.0f;
	const float cx = vw * 0.5f;
	const float textW = vw - 32.0f; // (the page's 16px padding on each side)
	float y = top;

	// the logo (img.logo3d, sharp pixels, see logoScale(); 6px under it, 0 in a low window)
	float logoW, logoH;
	logoSize(&logoW, &logoH);

	if (c != NULL)
		drawLogo(c, cx - logoW * 0.5f, y, logoW, logoH);
	y += logoH + (compact ? 0.0f : 6.0f) + gap;

	// «Trykk for å starte» (it has the keyboard, so it shows Chrome's focus ring as on the web page)
	float bw, bh;
	buttonSize(&styleBig, LABEL_START, &bw, &bh);
	drawButton(c, &styleBig, LABEL_START, cx - bw * 0.5f, y, bw, bh, HIT_START);
	if (c != NULL && startFocused)
	{
		const float px = snapPx(cx - bw * 0.5f), py = snapPx(y);
		const float pw = snapPx(cx + bw * 0.5f) - px, ph = snapPx(y + bh) - py;
		const float ring = floorf(2.0f * s);
		bt2RoundRect(c, px - 1.0f, py - 1.0f, pw + 2.0f, ph + 2.0f, styleBig.radius * s + 1.0f, 0x00000000, 1.0f, 0xFFFFFFFF);
		bt2RoundRect(c, px, py, pw, ph, styleBig.radius * s, 0x00000000, ring, 0xFF101010);
	}
	y += bh + gap;

	// «Start i fullskjerm» (label.check: padding 6px 10px, an 18px box, 8px between, 15px text)
	{
		const bt2Font f = fontCss(15.0f, 400);
		const float l = lineCss(f);
		const float inner = fmaxf(18.0f, l);
		const float rw = 10.0f + 18.0f + 8.0f + widthCss(f, LABEL_FULLSCREEN) + 10.0f;
		const float rh = inner + 12.0f;
		if (c != NULL)
		{
			const float rx = cx - rw * 0.5f;
			drawCheckbox(c, rx + 10.0f, y + 6.0f + (inner - 18.0f) * 0.5f, optFullscreen);
			text(c, f, rx + 36.0f, y + 6.0f + (inner - l) * 0.5f, rw, l, BT2_ALIGN_LEFT, C_TEXT, LABEL_FULLSCREEN);
			setHit(HIT_FULLSCREEN, rx, y, rw, rh);
		}
		y += rh + gap;
	}

	// the sound (.mode: 13px, the name in <b>)
	const bt2Font f13 = fontCss(13.0f, 400);
	const float l13 = lineCss(f13);
	if (c != NULL)
	{
		const float aw = bt2RichWidth(f13, audioLine) / s;
		bt2DrawRich(c, f13, snapPx(cx - aw * 0.5f), y * s, l13 * s, C_DIM, C_TEXT, audioLine);
	}
	y += l13 + gap;

	// «Lydsystem: ASIO Windows» (v0.31; ASIO is the standard, «Windows» is Windows' ordinary shared sound)
	{
		static const char *names[2] = { "ASIO", "Windows" };
		const int32_t chosen = (config.specialFlags2 & WIN_SHARED_AUDIO) ? 1 : 0;
		float w[2], rowH = 44.0f;
		const float labelW = widthCss(f13, LABEL_SYSTEM);
		float total = labelW;
		for (int32_t i = 0; i < 2; i++)
		{
			float hh;
			buttonSize(&styleSmall, names[i], &w[i], &hh);
			if (hh > rowH)
				rowH = hh;
			total += 10.0f + w[i];
		}

		if (c != NULL)
		{
			float x = cx - total * 0.5f;
			text(c, f13, x, y + (rowH - l13) * 0.5f, labelW + 4.0f, l13, BT2_ALIGN_LEFT, C_DIM, LABEL_SYSTEM);
			x += labelW + 10.0f;

			for (int32_t i = 0; i < 2; i++)
			{
				drawButton(c, (chosen == i) ? &styleSmallOn : &styleSmall, names[i], x, y, w[i], rowH, HIT_SYSTEM0 + i);
				x += w[i] + 10.0f;
			}
		}
		y += rowH + gap;

		if (systemNote[0] != '\0')
		{
			const float hSys = linesCss(f13, textW, l13, systemNote);
			text(c, f13, 16.0f, y, textW, l13, BT2_ALIGN_CENTER, C_DIM, systemNote);
			y += hSys + gap;
		}
	}

	// «Lydbuffer: Minst Normal Stor»
	{
		static const char *names[3] = { "Minst", "Normal", "Stor" };
		float w[3], rowH = 44.0f;
		const float labelW = widthCss(f13, LABEL_BUFFER);
		float total = labelW;
		for (int32_t i = 0; i < 3; i++)
		{
			float hh;
			buttonSize(&styleSmall, names[i], &w[i], &hh);
			if (hh > rowH)
				rowH = hh;
			total += 10.0f + w[i];
		}

		if (c != NULL)
		{
			float x = cx - total * 0.5f;
			text(c, f13, x, y + (rowH - l13) * 0.5f, labelW + 4.0f, l13, BT2_ALIGN_LEFT, C_DIM, LABEL_BUFFER);
			x += labelW + 10.0f;

			for (int32_t i = 0; i < 3; i++)
			{
				drawButton(c, (bufferChoice == i) ? &styleSmallOn : &styleSmall, names[i], x, y, w[i], rowH, HIT_BUFFER0 + i);
				x += w[i] + 10.0f;
			}
		}
		y += rowH + gap;
	}

	// the note under it
	const float hNote = linesCss(f13, textW, l13, NOTE_BUFFER);
	text(c, f13, 16.0f, y, textW, l13, BT2_ALIGN_CENTER, C_DIM, NOTE_BUFFER);
	y += hNote + gap;

	// the version
	char version[80];
	snprintf(version, sizeof (version), "Versjon %s · %s", BT2_VERSION, BT2_DATE);
	text(c, f13, 16.0f, y, textW, l13, BT2_ALIGN_CENTER, C_DIM, version);
	y += l13 + gap;

	// updates
	y += buildAppBox(c, cx, y) + gap;

	// the tips
	if (c != NULL)
	{
		const float hw = bt2RichWidth(f13, HINT_PC) / s;
		bt2DrawRich(c, f13, snapPx(cx - hw * 0.5f), y * s, l13 * s, C_DIM, C_TEXT, HINT_PC);
	}
	y += l13 + gap;

	// Ctrl+Up/Down = PgUp/PgDn (v0.27, as #ctrlHint on the web page)
	{
		const float wa = bt2RichWidth(f13, HINT_CTRL_A) / s, wb = bt2RichWidth(f13, HINT_CTRL_B) / s;
		const float space = widthCss(f13, " ");
		if (wa + space + wb <= textW)
		{
			if (c != NULL)
			{
				const float x = cx - (wa + space + wb) * 0.5f;
				bt2DrawRich(c, f13, snapPx(x), y * s, l13 * s, C_DIM, C_TEXT, HINT_CTRL_A);
				bt2DrawRich(c, f13, snapPx(x + wa + space), y * s, l13 * s, C_DIM, C_TEXT, HINT_CTRL_B);
			}
			y += l13 + gap;
		}
		else
		{
			if (c != NULL)
			{
				bt2DrawRich(c, f13, snapPx(cx - wa * 0.5f), y * s, l13 * s, C_DIM, C_TEXT, HINT_CTRL_A);
				bt2DrawRich(c, f13, snapPx(cx - wb * 0.5f), (y + l13) * s, l13 * s, C_DIM, C_TEXT, HINT_CTRL_B);
			}
			y += 2.0f * l13 + gap;
		}
	}

	// «Lydtest» and «Åpne BT2-mappa» (where the web version has «Sikkerhetskopi (zip)»: on a PC the files
	// are in an ordinary folder, so this shows them)
	float dw, dh, fw, fh;
	buttonSize(&styleSmall, "Lydtest", &dw, &dh);
	buttonSize(&styleSmall, LABEL_FOLDER, &fw, &fh);
	const float rowW = dw + 10.0f + fw, rowH = fmaxf(dh, fh);
	drawButton(c, &styleSmall, "Lydtest", cx - rowW * 0.5f, y, dw, rowH, HIT_DIAG);
	drawButton(c, &styleSmall, LABEL_FOLDER, cx - rowW * 0.5f + dw + 10.0f, y, fw, rowH, HIT_FOLDER);
	y += rowH;

	return y - top;
}

static void drawCredits(bt2Canvas *c)
{
	const bt2Font f = fontCss(11.0f, 400);
	const float l = lineCss(f);
	// ("Basert på " ends with a space; the others are put after a space)
	const float wa = widthCss(f, CREDITS_A), wl = widthCss(f, CREDITS_LINK), wb = widthCss(f, CREDITS_B);
	const float wc = widthCss(f, CREDITS_C), space = widthCss(f, " ");
	const bool oneLine = (wa + wl + space + wb + space + wc) <= vw - 20.0f;

	float y = vh - 10.0f - (oneLine ? l : l * 2.0f);
	const float line1 = wa + wl + space + wb + (oneLine ? space + wc : 0.0f);
	float x = (vw - line1) * 0.5f;

	text(c, f, x, y, wa + 4.0f, l, BT2_ALIGN_LEFT, C_CREDITS, CREDITS_A);
	text(c, f, x + wa, y, wl + 4.0f, l, BT2_ALIGN_LEFT, C_LINK, CREDITS_LINK);
	bt2RoundRect(c, snapPx(x + wa), floorf((y + l * 0.82f) * s), snapPx(x + wa + wl) - snapPx(x + wa), 1.0f, 0.0f, C_LINK, 0.0f, 0);
	setHit(HIT_LINK, x + wa, y, wl, l);
	text(c, f, x + wa + wl + space, y, wb + 4.0f, l, BT2_ALIGN_LEFT, C_CREDITS, CREDITS_B);

	if (oneLine)
		text(c, f, x + wa + wl + space + wb + space, y, wc + 4.0f, l, BT2_ALIGN_LEFT, C_CREDITS, CREDITS_C);
	else
		text(c, f, 0.0f, y + l, vw, l, BT2_ALIGN_CENTER, C_CREDITS, CREDITS_C);
}

// the sound test (#diag: the page's background, from the top, the text in a dark box)
static void buildDiag(bt2Canvas *c)
{
	const float cx = vw * 0.5f;
	float y = 40.0f;

	const bt2Font title = fontCss(24.0f, 700);
	const float lt = lineCss(title);
	text(c, title, 0.0f, y, vw, lt, BT2_ALIGN_CENTER, C_TEXT, "Lydtest");
	y += lt + 14.0f;

	char diag[1200];
	buildDiagText(diag, sizeof (diag));

	bt2Font mono = fontCss(14.0f, 400);
	mono.mono = true;
	const float lineH = 14.0f * 1.5f;

	// (the box is as wide as the longest line, at least 300px)
	float widest = 0.0f;
	int32_t lines = 0;
	char *copy = _strdup(diag);
	if (copy != NULL)
	{
		for (char *line = strtok(copy, "\n"); line != NULL; line = strtok(NULL, "\n"))
		{
			const float w = widthCss(mono, line);
			if (w > widest)
				widest = w;
			lines++;
		}
		free(copy);
	}

	const float boxW = fminf(fmaxf(widest + 32.0f, 300.0f), vw - 32.0f);
	const float boxH = lines * lineH + 24.0f;
	const float bx = cx - boxW * 0.5f;
	bt2RoundRect(c, snapPx(bx), snapPx(y), snapPx(bx + boxW) - snapPx(bx), snapPx(y + boxH) - snapPx(y), 8.0f * s, 0xFF12161A, 0.0f, 0);
	bt2DrawText(c, mono, (bx + 16.0f) * s, (y + 12.0f) * s, (boxW - 32.0f) * s, lineH * s, BT2_ALIGN_LEFT, C_TEXT, diag);
	y += boxH + 14.0f;

	const bt2Font f15 = fontCss(15.0f, 400);
	const char *note = "Lyden går rett til lydkortet (ASIO eller Windows eksklusiv) når det er mulig.\n"
		"Lukk andre programmer som bruker lyden hvis ASIO ikke starter.";
	const float hn = linesCss(f15, fminf(520.0f, vw - 32.0f), 15.0f * 1.4f, note);
	text(c, f15, cx - fminf(520.0f, vw - 32.0f) * 0.5f, y, fminf(520.0f, vw - 32.0f), 15.0f * 1.4f, BT2_ALIGN_CENTER, C_DIM, note);
	y += hn + 14.0f;

	float bw, bh;
	buttonSize(&styleSmall, "Lukk", &bw, &bh);
	drawButton(c, &styleSmall, "Lukk", cx - bw * 0.5f, y, bw, bh, HIT_DIAG_CLOSE);
}

// ------------------------------------------------------------------------------------------------

static void destroyTextures(void)
{
	if (uiTex != NULL)
	{
		SDL_DestroyTexture(uiTex);
		uiTex = NULL;
	}

	if (starTex != NULL)
	{
		SDL_DestroyTexture(starTex);
		starTex = NULL;
	}
}

static float screenScale(void) // like Chrome: Windows' display zoom (100 %, 125 %, 150 % ...)
{
	HDC dc = GetDC(NULL);
	const int32_t dpi = (dc != NULL) ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
	if (dc != NULL)
		ReleaseDC(NULL, dc);

	const float z = dpi / 96.0f;
	return (z < 1.0f) ? 1.0f : z;
}

// the scale and layout for the window's size (everything must fit above the credits)
static void chooseScale(void)
{
	const float zoom = screenScale();

	for (int32_t pass = 0; pass < 2; pass++)
	{
		compact = (pass == 1);
		s = zoom;

		for (int32_t i = 0; i < 4; i++)
		{
			vw = outW / s;
			vh = outH / s;
			const float need = buildStart(NULL, 0.0f) + 32.0f + 2.0f * 34.0f; // (page padding, room for the credits)
			if (need <= vh + 0.5f)
				break;

			s *= (vh / need) * 0.99f;
		}

		if (s >= zoom * 0.72f || pass == 1)
			break;
	}

	if (s < 0.5f)
		s = 0.5f;

	vw = outW / s;
	vh = outH / s;
}

static bool layout(void)
{
	int32_t w = 0, h = 0;
	SDL_GetRendererOutputSize(video.renderer, &w, &h);
	if (w <= 0 || h <= 0)
		return false;

	destroyTextures();
	bt2CanvasFree(&ui);

	outW = w;
	outH = h;
	bt2UiResetFonts(); // (the sizes of the old scale aren't needed anymore)
	chooseScale();

	if (!bt2CanvasAlloc(&ui, outW, outH))
		return false;

	uiTex = SDL_CreateTexture(video.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, outW, outH);
	if (uiTex == NULL)
		return false;
	SDL_SetTextureBlendMode(uiTex, SDL_BLENDMODE_BLEND);

	// the stars are drawn 632 pixels wide (FT2's screen), as high as the window's shape, and made bigger
	starH = (int32_t)floor((double)STAR_W * outH / outW + 0.5);
	if (starH < 120) starH = 120;
	if (starH > 1600) starH = 1600;

	free(starBuf);
	starBuf = (uint32_t *)malloc((size_t)STAR_W * starH * sizeof (uint32_t));
	starTex = SDL_CreateTexture(video.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, STAR_W, starH);
	if (starTex != NULL)
		SDL_SetTextureScaleMode(starTex, SDL_ScaleModeNearest);

	return true;
}

static void render(void)
{
	memset(hitShown, 0, sizeof (hitShown));

	if (diagOpen)
	{
		bt2CanvasClear(&ui, C_PANEL);
		buildDiag(&ui);
	}
	else
	{
		bt2CanvasClear(&ui, 0x00000000);

		// in the middle (justify-content: center, padding 16px), clear of the credits at the bottom
		const float h = buildStart(NULL, 0.0f);
		float top = 16.0f + (vh - 32.0f - h) * 0.5f;
		if (top + h > vh - 34.0f)
			top = vh - 34.0f - h;
		if (top < 16.0f)
			top = 16.0f;

		buildStart(&ui, top);
		drawCredits(&ui);
	}

	// a message at the top (#toast), e.g. new demo songs
	if (toastUntil != 0 && SDL_GetPerformanceCounter() < toastUntil)
	{
		const bt2Font f = fontCss(15.0f, 400);
		const float l = lineCss(f);
		const float w = fminf(widthCss(f, toastText), vw - 64.0f) + 32.0f + 2.0f, h = l + 20.0f + 2.0f;
		const float x = (vw - w) * 0.5f;
		bt2RoundRect(&ui, snapPx(x), snapPx(14.0f), snapPx(x + w) - snapPx(x), snapPx(14.0f + h) - snapPx(14.0f), 8.0f * s,
			0xFF12161A, borderPx(1.0f), 0xFF596169);
		text(&ui, f, x + 17.0f, 14.0f + 11.0f, w - 34.0f, l, BT2_ALIGN_CENTER, C_TEXT, toastText);
	}

	if (uiTex != NULL)
		SDL_UpdateTexture(uiTex, NULL, ui.px, ui.w * (int32_t)sizeof (uint32_t));
}

static int32_t hitAt(int32_t x, int32_t y)
{
	for (int32_t i = 0; i < HIT_COUNT; i++)
	{
		if (hitShown[i] && x >= hitRect[i].x && x < hitRect[i].x + hitRect[i].w && y >= hitRect[i].y && y < hitRect[i].y + hitRect[i].h)
			return i;
	}

	return -1;
}

static void windowToPixels(int32_t *x, int32_t *y)
{
	int32_t ww = 0, wh = 0;
	SDL_GetWindowSize(video.window, &ww, &wh);
	if (ww > 0 && wh > 0)
	{
		*x = (int32_t)((int64_t)*x * outW / ww);
		*y = (int32_t)((int64_t)*y * outH / wh);
	}
}

static void setAudioSystem(int32_t choice) // v0.31: 0 = ASIO (standard), 1 = Windows' ordinary sound
{
	const bool shared = (choice == 1);
	if (shared == ((config.specialFlags2 & WIN_SHARED_AUDIO) != 0) && (shared || audio.dev == WIN_ASIO_DEVICE_ID))
		return; // (choosing ASIO again tries to start it again, e.g. after closing another program)

	if (shared)
		config.specialFlags2 |= WIN_SHARED_AUDIO;
	else
		config.specialFlags2 &= ~WIN_SHARED_AUDIO;

	setNewAudioSettings(); // (opens the sound again right away)
	saveConfig(CONFIG_HIDE_ERRORS);
	buildAudioLine();
}

static void setBufferChoice(int32_t choice)
{
	if (choice == bufferChoice)
		return;

	bufferChoice = choice;
	config.specialFlags &= ~(BUFFSIZE_512 + BUFFSIZE_1024 + BUFFSIZE_2048);
	config.specialFlags |= (choice == 0) ? BUFFSIZE_512 : (choice == 2) ? BUFFSIZE_2048 : BUFFSIZE_1024;

	setNewAudioSettings(); // (opens the sound again with the new buffer right away)
	saveConfig(CONFIG_HIDE_ERRORS);
	bufferChoice = bufferChoiceFromConfig(); // (FT2 goes back to the old one if the new one doesn't work)
	buildAudioLine();
}

bool bt2StartScreen(void)
{
	optFullscreen = (config.windowFlags & START_IN_FULLSCR) ? true : false;
	bufferChoice = bufferChoiceFromConfig();
	updateBoxHidden = false;
	diagOpen = false;
	restartFailed = false;
	startFocused = true;
	toastUntil = 0;
	buildAudioLine();

	bt2HomeStartDemoCopy();
	bt2UpdateStartCheck();
	bt2UpdateGetInfo(&upd);

	logoLoaded = bt2LoadPngResource("BT2LOGO", &logoSrc);
	starsInit();

	arrowCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
	handCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
	SDL_SetCursor(arrowCursor);
	SDL_ShowCursor(SDL_ENABLE);

	bool result = true, done = false, needLayout = true, needRender = true;
	int32_t pressed = -1, lastHover = -1;
	uint32_t lastChange = upd.changeCount;
	uint64_t restartAt = 0, lastFrame = SDL_GetPerformanceCounter(), lastDiag = 0;
	const uint64_t freq = SDL_GetPerformanceFrequency();

	hpc_ResetCounters(&video.vblankHpc);

	while (!done)
	{
		bool startNow = false;

		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			switch (e.type)
			{
				case SDL_QUIT:
					result = false;
					done = true;
					break;

				case SDL_WINDOWEVENT:
					if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED)
						needLayout = true;
					else if (e.window.event == SDL_WINDOWEVENT_EXPOSED)
						needRender = true;
					break;

				case SDL_RENDER_TARGETS_RESET:
				case SDL_RENDER_DEVICE_RESET:
					needLayout = true;
					break;

				case SDL_MOUSEMOTION:
				{
					int32_t x = e.motion.x, y = e.motion.y;
					windowToPixels(&x, &y);
					const int32_t h = hitAt(x, y);
					if ((h >= 0) != (lastHover >= 0))
						SDL_SetCursor((h >= 0) ? handCursor : arrowCursor);
					lastHover = h;
				}
				break;

				case SDL_MOUSEBUTTONDOWN:
					if (e.button.button == SDL_BUTTON_LEFT)
					{
						int32_t x = e.button.x, y = e.button.y;
						windowToPixels(&x, &y);
						pressed = hitAt(x, y);
					}
					break;

				case SDL_MOUSEBUTTONUP:
				{
					if (e.button.button != SDL_BUTTON_LEFT)
						break;

					int32_t x = e.button.x, y = e.button.y;
					windowToPixels(&x, &y);
					const int32_t h = hitAt(x, y);
					const int32_t was = pressed;
					pressed = -1;
					if (h < 0 || h != was) // (a click: pressed and let go on the same thing)
						break;

					if (h != HIT_START && h != HIT_LINK && startFocused)
					{
						startFocused = false; // (like the web page: the clicked button takes the keyboard)
						needRender = true;
					}

					switch (h)
					{
						case HIT_START: startNow = true; break;

						case HIT_FULLSCREEN:
							optFullscreen = !optFullscreen;
							if (optFullscreen)
								config.windowFlags |= START_IN_FULLSCR;
							else
								config.windowFlags &= ~START_IN_FULLSCR;
							saveConfig(CONFIG_HIDE_ERRORS);
							needRender = true;
							break;

						case HIT_BUFFER0:
						case HIT_BUFFER1:
						case HIT_BUFFER2:
							setBufferChoice(h - HIT_BUFFER0);
							needRender = true;
							break;

						case HIT_SYSTEM0:
						case HIT_SYSTEM1:
							setAudioSystem(h - HIT_SYSTEM0);
							needRender = true;
							break;

						case HIT_UPDATE_NOW:
							updateBoxHidden = false;
							bt2UpdateStartDownload();
							needRender = true;
							break;

						case HIT_UPDATE_LATER:
							updateBoxHidden = true;
							needRender = true;
							break;

						case HIT_DIAG:
							diagOpen = true;
							lastDiag = SDL_GetPerformanceCounter();
							needRender = true;
							break;

						case HIT_DIAG_CLOSE:
							diagOpen = false;
							needRender = true;
							break;

						case HIT_LINK:
							ShellExecuteW(NULL, L"open", CREDITS_URL, NULL, NULL, SW_SHOWNORMAL);
							break;

						case HIT_FOLDER:
							if (bt2HomeFolder() != NULL)
								ShellExecuteW(NULL, L"open", bt2HomeFolder(), NULL, NULL, SW_SHOWNORMAL);
							break;

						default: break;
					}

					// (the thing under the pointer may have moved)
					const int32_t now = hitAt(x, y);
					if ((now >= 0) != (lastHover >= 0))
						SDL_SetCursor((now >= 0) ? handCursor : arrowCursor);
					lastHover = now;
				}
				break;

				case SDL_KEYDOWN:
				{
					const SDL_Keycode k = e.key.keysym.sym;
					if (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))
					{
						if (!e.key.repeat)
						{
							toggleFullscreen();
							needLayout = true;
						}
						break;
					}

					if (diagOpen)
					{
						if (k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE)
						{
							diagOpen = false;
							needRender = true;
						}
						break;
					}

					if (k == SDLK_ESCAPE)
					{
						if (video.fullscreen && !e.key.repeat) // (Esc leaves fullscreen, as in the web version)
						{
							toggleFullscreen();
							needLayout = true;
						}
					}
					else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) && !e.key.repeat)
					{
						startNow = true; // (the start button has the keyboard, as on the web page)
					}
				}
				break;

				default: break;
			}
		}

		if (done)
			break;

		if (startNow && upd.state != BT2_UPD_RESTARTING)
			break;

		// news from the update check
		bt2UpdateGetInfo(&upd);
		if (upd.changeCount != lastChange)
		{
			lastChange = upd.changeCount;
			needRender = true;
		}

		if (upd.state == BT2_UPD_RESTARTING && !restartFailed)
		{
			const uint64_t now = SDL_GetPerformanceCounter();
			if (restartAt == 0)
			{
				restartAt = now + (freq * 6) / 10; // (shows «Oppdatert» for a moment, like the web version)
			}
			else if (now >= restartAt)
			{
				closeAudio(); // (the new program needs the sound card, ASIO only lets one program use it)
				if (bt2UpdateLaunchNew())
				{
					result = false;
					break;
				}

				// couldn't start it: carry on with this one (the new files are used next time)
				restartFailed = true;
				setNewAudioSettings();
				buildAudioLine();
				needRender = true;
			}
		}

		// new demo songs: the same message as the web version
		int32_t demosAdded;
		if (bt2HomeTakeDemoNews(&demosAdded))
		{
			snprintf(toastText, sizeof (toastText), "%d %s i «Demo Tracks»", demosAdded, (demosAdded == 1) ? "ny demo-låt" : "nye demo-låter");
			toastUntil = SDL_GetPerformanceCounter() + (freq * 26) / 10;
			needRender = true;
		}
		else if (toastUntil != 0 && SDL_GetPerformanceCounter() >= toastUntil)
		{
			toastUntil = 0;
			needRender = true;
		}

		// the sound test's numbers can change (a new buffer, the driver)
		if (diagOpen && SDL_GetPerformanceCounter() - lastDiag > freq)
		{
			lastDiag = SDL_GetPerformanceCounter();
			needRender = true;
		}

		int32_t w = 0, h = 0;
		SDL_GetRendererOutputSize(video.renderer, &w, &h);
		if (w != outW || h != outH)
			needLayout = true;

		if (needLayout)
		{
			needLayout = false;
			if (!layout())
			{
				SDL_Delay(50); // (minimized, or the screen is changing: try again)
				needLayout = true;
				continue;
			}

			buildAudioLine();
			needRender = true;
		}

		if (needRender)
		{
			needRender = false;
			render();
		}

		// the stars move with the time that has passed (at most 6 FT2 frames at once)
		const uint64_t nowTick = SDL_GetPerformanceCounter();
		double steps = (double)(nowTick - lastFrame) * 60.0 / (double)freq;
		lastFrame = nowTick;
		if (steps > 6.0)
			steps = 6.0;

		if (!diagOpen)
		{
			starsMove(steps);
			starsDraw();
		}

		SDL_SetRenderDrawColor(video.renderer, 0, 0, 0, 255);
		SDL_RenderClear(video.renderer);
		if (!diagOpen && starTex != NULL)
			SDL_RenderCopy(video.renderer, starTex, NULL, NULL);
		if (uiTex != NULL)
			SDL_RenderCopy(video.renderer, uiTex, NULL, NULL);
		SDL_RenderPresent(video.renderer);

		if (SDL_GetWindowFlags(video.window) & SDL_WINDOW_MINIMIZED)
			SDL_Delay(16);
		else if (!video.vsync60HzPresent)
			hpc_Wait(&video.vblankHpc);
	}

	// back to FT2's screen
	destroyTextures();
	bt2CanvasFree(&ui);
	bt2CanvasFree(&logoScaled);
	bt2CanvasFree(&logoSrc);
	free(starBuf);
	starBuf = NULL;
	bt2UiFree();

	createMouseCursors();
	setMouseShape(config.mouseType);
	if (arrowCursor != NULL) { SDL_FreeCursor(arrowCursor); arrowCursor = NULL; }
	if (handCursor != NULL) { SDL_FreeCursor(handCursor); handCursor = NULL; }

	SDL_SetRenderDrawColor(video.renderer, 0, 0, 0, 255);

	if (result && optFullscreen && !video.fullscreen)
		toggleFullscreen();

	return result;
}

#endif
