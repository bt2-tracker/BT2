/* BerntTracker (Windows): a small drawing kit for the screens that look like the web version.
** See bt2_winui.h. Text is drawn by Windows (GDI) in white on black into a work bitmap; how white
** each pixel became is how much of the text color goes onto the canvas there.
*/

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bt2_winui.h"

// ------------------------------------------------------------------------------------------------
// canvas

bool bt2CanvasAlloc(bt2Canvas *c, int32_t w, int32_t h)
{
	c->w = c->h = 0;
	c->px = NULL;
	if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
		return false;

	c->px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof (uint32_t));
	if (c->px == NULL)
		return false;

	c->w = w;
	c->h = h;
	return true;
}

void bt2CanvasFree(bt2Canvas *c)
{
	free(c->px);
	c->px = NULL;
	c->w = c->h = 0;
}

void bt2CanvasClear(bt2Canvas *c, uint32_t argb)
{
	const size_t n = (size_t)c->w * (size_t)c->h;
	for (size_t i = 0; i < n; i++)
		c->px[i] = argb;
}

// "source over": color (with its own alpha, times a coverage 0..1) onto a pixel
static inline void blendPixel(uint32_t *d, uint32_t color, float coverage)
{
	const float sa = (float)(color >> 24) * (1.0f / 255.0f) * coverage;
	if (sa <= 0.0f)
		return;

	const uint32_t dv = *d;
	if (sa >= 0.999f)
	{
		*d = 0xFF000000 | (color & 0x00FFFFFF);
		return;
	}

	const float da = (float)(dv >> 24) * (1.0f / 255.0f);
	const float oa = sa + da * (1.0f - sa);
	if (oa <= 0.0f)
	{
		*d = 0;
		return;
	}

	const float dw = da * (1.0f - sa);
	const float r = (((color >> 16) & 0xFF) * sa + ((dv >> 16) & 0xFF) * dw) / oa;
	const float g = (((color >>  8) & 0xFF) * sa + ((dv >>  8) & 0xFF) * dw) / oa;
	const float b = (((color      ) & 0xFF) * sa + ((dv      ) & 0xFF) * dw) / oa;

	*d = ((uint32_t)(oa * 255.0f + 0.5f) << 24) | ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
}

// signed distance from a point (relative to the center) to a rounded box (half size hx/hy, corner radius r)
static inline float roundBoxDistance(float px, float py, float hx, float hy, float r)
{
	const float qx = fabsf(px) - hx + r;
	const float qy = fabsf(py) - hy + r;
	const float ox = (qx > 0.0f) ? qx : 0.0f;
	const float oy = (qy > 0.0f) ? qy : 0.0f;
	const float inside = fminf(fmaxf(qx, qy), 0.0f);
	return sqrtf(ox * ox + oy * oy) + inside - r;
}

static inline float clamp01(float v)
{
	return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}

void bt2RoundRect(bt2Canvas *c, float x, float y, float w, float h, float radius, uint32_t fill, float borderW, uint32_t borderColor)
{
	if (w <= 0.0f || h <= 0.0f)
		return;

	const float hx = w * 0.5f, hy = h * 0.5f;
	const float cx = x + hx, cy = y + hy;
	float r = radius;
	if (r > hx) r = hx;
	if (r > hy) r = hy;
	if (r < 0.0f) r = 0.0f;

	const bool hasBorder = (borderW > 0.0f) && ((borderColor >> 24) != 0);
	const bool hasFill = ((fill >> 24) != 0);
	const float ihx = hx - borderW, ihy = hy - borderW;
	const float ir = (r - borderW > 0.0f) ? (r - borderW) : 0.0f;

	int32_t x0 = (int32_t)floorf(x), y0 = (int32_t)floorf(y);
	int32_t x1 = (int32_t)ceilf(x + w), y1 = (int32_t)ceilf(y + h);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > c->w) x1 = c->w;
	if (y1 > c->h) y1 = c->h;

	for (int32_t py = y0; py < y1; py++)
	{
		uint32_t *row = &c->px[(size_t)py * c->w];
		const float fy = (py + 0.5f) - cy;

		for (int32_t px = x0; px < x1; px++)
		{
			const float fx = (px + 0.5f) - cx;
			const float outer = clamp01(0.5f - roundBoxDistance(fx, fy, hx, hy, r));
			if (outer <= 0.0f)
				continue;

			if (!hasBorder)
			{
				if (hasFill)
					blendPixel(&row[px], fill, outer);
				continue;
			}

			float inner = 0.0f;
			if (ihx > 0.0f && ihy > 0.0f)
				inner = clamp01(0.5f - roundBoxDistance(fx, fy, ihx, ihy, ir));

			if (hasFill)
			{
				blendPixel(&row[px], borderColor, outer);
				blendPixel(&row[px], fill, inner);
			}
			else if (outer - inner > 0.0f)
			{
				blendPixel(&row[px], borderColor, outer - inner);
			}
		}
	}
}

void bt2Line(bt2Canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t color)
{
	const float r = width * 0.5f;
	int32_t bx0 = (int32_t)floorf(fminf(x0, x1) - r - 1.0f), by0 = (int32_t)floorf(fminf(y0, y1) - r - 1.0f);
	int32_t bx1 = (int32_t)ceilf(fmaxf(x0, x1) + r + 1.0f), by1 = (int32_t)ceilf(fmaxf(y0, y1) + r + 1.0f);
	if (bx0 < 0) bx0 = 0;
	if (by0 < 0) by0 = 0;
	if (bx1 > c->w) bx1 = c->w;
	if (by1 > c->h) by1 = c->h;

	const float dx = x1 - x0, dy = y1 - y0;
	const float len2 = dx * dx + dy * dy;

	for (int32_t py = by0; py < by1; py++)
	{
		for (int32_t px = bx0; px < bx1; px++)
		{
			const float fx = px + 0.5f - x0, fy = py + 0.5f - y0;
			float t = (len2 > 0.0f) ? (fx * dx + fy * dy) / len2 : 0.0f;
			t = clamp01(t);
			const float ex = fx - dx * t, ey = fy - dy * t;
			const float cov = clamp01(r + 0.5f - sqrtf(ex * ex + ey * ey));
			if (cov > 0.0f)
				blendPixel(&c->px[(size_t)py * c->w + px], color, cov);
		}
	}
}

void bt2Blit(bt2Canvas *dst, const bt2Canvas *src, int32_t x, int32_t y)
{
	for (int32_t sy = 0; sy < src->h; sy++)
	{
		const int32_t dy = y + sy;
		if (dy < 0 || dy >= dst->h)
			continue;

		const uint32_t *s = &src->px[(size_t)sy * src->w];
		uint32_t *d = &dst->px[(size_t)dy * dst->w];

		for (int32_t sx = 0; sx < src->w; sx++)
		{
			const int32_t dx = x + sx;
			if (dx < 0 || dx >= dst->w)
				continue;

			const uint32_t p = s[sx];
			if ((p >> 24) == 0)
				continue;

			blendPixel(&d[dx], p, 1.0f);
		}
	}
}

// ------------------------------------------------------------------------------------------------
// pictures

// (the class and interface ids, so no extra library is needed: the decoder is reached through COM)
static const GUID bt2_CLSID_WICImagingFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID bt2_IID_IWICImagingFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID bt2_GUID_WICPixelFormat32bppBGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

bool bt2DecodePng(const void *data, uint32_t size, bt2Canvas *out)
{
	out->w = out->h = 0;
	out->px = NULL;

	const HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	IWICImagingFactory *factory = NULL;
	IWICStream *stream = NULL;
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *conv = NULL;
	bool ok = false;

	if (SUCCEEDED(CoCreateInstance(&bt2_CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &bt2_IID_IWICImagingFactory, (void **)&factory)) &&
		SUCCEEDED(IWICImagingFactory_CreateStream(factory, &stream)) &&
		SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, size)) &&
		SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder)) &&
		SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) &&
		SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory, &conv)) &&
		SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &bt2_GUID_WICPixelFormat32bppBGRA,
			WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
	{
		UINT w = 0, h = 0;
		if (SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h)) && bt2CanvasAlloc(out, (int32_t)w, (int32_t)h))
		{
			// (BGRA bytes are 0xAARRGGBB in a little-endian uint32)
			if (SUCCEEDED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, (BYTE *)out->px)))
				ok = true;
			else
				bt2CanvasFree(out);
		}
	}

	if (conv != NULL) IWICFormatConverter_Release(conv);
	if (frame != NULL) IWICBitmapFrameDecode_Release(frame);
	if (decoder != NULL) IWICBitmapDecoder_Release(decoder);
	if (stream != NULL) IWICStream_Release(stream);
	if (factory != NULL) IWICImagingFactory_Release(factory);

	if (SUCCEEDED(co))
		CoUninitialize();

	return ok;
}

bool bt2LoadPngResource(const char *name, bt2Canvas *out)
{
	out->w = out->h = 0;
	out->px = NULL;

	HRSRC res = FindResourceA(NULL, name, MAKEINTRESOURCEA(10)); // (10 = RT_RCDATA)
	if (res == NULL)
		return false;

	HGLOBAL mem = LoadResource(NULL, res);
	const DWORD size = SizeofResource(NULL, res);
	const void *data = (mem != NULL) ? LockResource(mem) : NULL;
	if (data == NULL || size == 0)
		return false;

	return bt2DecodePng(data, size, out);
}

/* One direction of a resize, with premultiplied colors (so see-through edges don't get dark):
** shrinking averages the source pixels each new pixel covers, growing blends the two nearest.
*/
static void resizeAxis(const float *src, int32_t srcLen, int32_t count, int32_t srcStride, int32_t srcStep,
	float *dst, int32_t dstLen, int32_t dstStride, int32_t dstStep)
{
	const double scale = (double)srcLen / dstLen;

	for (int32_t line = 0; line < count; line++)
	{
		const float *s = src + (size_t)line * srcStride;
		float *d = dst + (size_t)line * dstStride;

		for (int32_t i = 0; i < dstLen; i++)
		{
			float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

			if (scale > 1.0) // shrink: area average
			{
				const double a = i * scale, b = (i + 1) * scale;
				int32_t j = (int32_t)a;
				double total = 0.0;
				for (; j < srcLen && j < b; j++)
				{
					double lo = (j > a) ? j : a;
					double hi = (j + 1 < b) ? (j + 1) : b;
					const float wgt = (float)(hi - lo);
					if (wgt <= 0.0f)
						continue;

					const float *p = s + (size_t)j * srcStep;
					acc[0] += p[0] * wgt; acc[1] += p[1] * wgt; acc[2] += p[2] * wgt; acc[3] += p[3] * wgt;
					total += wgt;
				}

				if (total > 0.0)
				{
					const float inv = (float)(1.0 / total);
					acc[0] *= inv; acc[1] *= inv; acc[2] *= inv; acc[3] *= inv;
				}
			}
			else // grow: linear
			{
				double pos = (i + 0.5) * scale - 0.5;
				if (pos < 0.0) pos = 0.0;
				int32_t j = (int32_t)pos;
				if (j > srcLen - 1) j = srcLen - 1;
				const int32_t j2 = (j + 1 < srcLen) ? (j + 1) : j;
				const float f = (float)(pos - j);

				const float *p = s + (size_t)j * srcStep, *q = s + (size_t)j2 * srcStep;
				for (int32_t k = 0; k < 4; k++)
					acc[k] = p[k] + (q[k] - p[k]) * f;
			}

			float *o = d + (size_t)i * dstStep;
			o[0] = acc[0]; o[1] = acc[1]; o[2] = acc[2]; o[3] = acc[3];
		}
	}
}

bool bt2Resize(const bt2Canvas *src, int32_t w, int32_t h, bt2Canvas *out)
{
	if (src->px == NULL || !bt2CanvasAlloc(out, w, h))
		return false;

	const int32_t sw = src->w, sh = src->h;
	float *a = (float *)malloc((size_t)sw * sh * 4 * sizeof (float));
	float *b = (float *)malloc((size_t)w * sh * 4 * sizeof (float));
	float *c = (float *)malloc((size_t)w * h * 4 * sizeof (float));
	if (a == NULL || b == NULL || c == NULL)
	{
		free(a); free(b); free(c);
		bt2CanvasFree(out);
		return false;
	}

	for (size_t i = 0; i < (size_t)sw * sh; i++)
	{
		const uint32_t p = src->px[i];
		const float al = (float)(p >> 24) * (1.0f / 255.0f);
		a[i*4+0] = ((p >> 16) & 0xFF) * al;
		a[i*4+1] = ((p >>  8) & 0xFF) * al;
		a[i*4+2] = ((p      ) & 0xFF) * al;
		a[i*4+3] = al;
	}

	// across (each row), then down (each column)
	resizeAxis(a, sw, sh, sw * 4, 4, b, w, w * 4, 4);
	resizeAxis(b, sh, w, 4, w * 4, c, h, 4, w * 4);

	for (size_t i = 0; i < (size_t)w * h; i++)
	{
		const float al = c[i*4+3];
		if (al <= 0.0001f)
		{
			out->px[i] = 0;
			continue;
		}

		float r = c[i*4+0] / al, g = c[i*4+1] / al, bl = c[i*4+2] / al;
		if (r > 255.0f) r = 255.0f;
		if (g > 255.0f) g = 255.0f;
		if (bl > 255.0f) bl = 255.0f;
		const float aa = (al > 1.0f) ? 1.0f : al;
		out->px[i] = ((uint32_t)(aa * 255.0f + 0.5f) << 24) | ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(bl + 0.5f);
	}

	free(a); free(b); free(c);
	return true;
}

// ------------------------------------------------------------------------------------------------
// text

typedef struct fontEntry_t
{
	int32_t size, weight;
	bool mono, symbol;
	HFONT font;
	int32_t height, ascent, lineH;
} fontEntry;

#define MAX_FONTS 160 // (emptied by bt2UiResetFonts() when the window changes size)

static fontEntry fonts[MAX_FONTS];
static int32_t numFonts;
static HDC textDC;
static HBITMAP textBmp, textOldBmp;
static uint32_t *textBits;
static int32_t textBmpW, textBmpH;

static bool ensureDC(void)
{
	if (textDC == NULL)
		textDC = CreateCompatibleDC(NULL);

	return (textDC != NULL);
}

static void freeFonts(void)
{
	for (int32_t i = 0; i < numFonts; i++)
	{
		if (fonts[i].font != NULL)
			DeleteObject(fonts[i].font);
	}

	numFonts = 0;
}

// symbol: "Segoe UI Symbol", for letters the normal font doesn't have (like ✓), as Chrome finds another font for them
static fontEntry *getFontKey(int32_t size, int32_t weight, bool mono, bool symbol)
{
	if (!ensureDC())
		return NULL;

	bt2Font f;
	f.size = (size < 4) ? 4 : ((size > 400) ? 400 : size);
	f.weight = weight;
	f.mono = mono;

	for (int32_t i = 0; i < numFonts; i++)
	{
		if (fonts[i].size == f.size && fonts[i].weight == f.weight && fonts[i].mono == f.mono && fonts[i].symbol == symbol)
			return &fonts[i];
	}

	if (numFonts >= MAX_FONTS)
	{
		SelectObject(textDC, GetStockObject(SYSTEM_FONT));
		freeFonts();
	}

	LOGFONTW lf;
	memset(&lf, 0, sizeof (lf));
	lf.lfHeight = -f.size; // (negative: the em size, like CSS)
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_TT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY;

	if (symbol)
	{
		lf.lfWeight = FW_NORMAL;
		lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
		wcscpy(lf.lfFaceName, L"Segoe UI Symbol");
	}
	else if (f.mono)
	{
		lf.lfWeight = FW_NORMAL;
		lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
		wcscpy(lf.lfFaceName, L"Consolas");
	}
	else if (f.weight >= 550 && f.weight < 650)
	{
		lf.lfWeight = FW_SEMIBOLD;
		lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
		wcscpy(lf.lfFaceName, L"Segoe UI Semibold");
	}
	else
	{
		lf.lfWeight = (f.weight >= 650) ? FW_BOLD : FW_NORMAL;
		lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
		wcscpy(lf.lfFaceName, L"Segoe UI");
	}

	HFONT font = CreateFontIndirectW(&lf);
	if (font == NULL)
		return NULL;

	fontEntry *e = &fonts[numFonts++];
	e->size = f.size;
	e->weight = f.weight;
	e->mono = f.mono;
	e->symbol = symbol;
	e->font = font;

	SelectObject(textDC, font);
	TEXTMETRICW tm;
	if (GetTextMetricsW(textDC, &tm))
	{
		e->height = tm.tmHeight;
		e->ascent = tm.tmAscent;
		e->lineH = tm.tmHeight + tm.tmExternalLeading;
	}
	else
	{
		e->height = e->lineH = (f.size * 4) / 3;
		e->ascent = f.size;
	}

	return e;
}

static fontEntry *getFont(bt2Font f)
{
	return getFontKey(f.size, f.weight, f.mono, false);
}

static wchar_t *toWide(const char *text, int32_t *outLen)
{
	*outLen = 0;
	if (text == NULL)
		text = "";

	const int32_t n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	wchar_t *w = (wchar_t *)malloc((size_t)(n > 0 ? n : 1) * sizeof (wchar_t));
	if (w == NULL)
		return NULL;

	if (n <= 0 || MultiByteToWideChar(CP_UTF8, 0, text, -1, w, n) <= 0)
	{
		w[0] = L'\0';
		return w;
	}

	*outLen = n - 1;
	return w;
}

// parts of a text: the font's own letters, and letters it doesn't have (drawn with the symbol font)
#define MAX_SEGS 32

typedef struct textSeg_t
{
	int32_t start, len;
	bool symbol;
} textSeg;

static int32_t textSegments(fontEntry *fe, const wchar_t *s, int32_t len, textSeg *segs)
{
	if (len <= 0)
		return 0;

	WORD stackIdx[256];
	WORD *idx = (len <= 256) ? stackIdx : (WORD *)malloc((size_t)len * sizeof (WORD));

	SelectObject(textDC, fe->font);
	const bool ok = (idx != NULL) && !fe->mono && GetGlyphIndicesW(textDC, s, len, idx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR;

	int32_t n = 0;
	if (!ok)
	{
		segs[0].start = 0;
		segs[0].len = len;
		segs[0].symbol = false;
		n = 1;
	}
	else
	{
		for (int32_t i = 0; i < len;)
		{
			const bool missing = (idx[i] == 0xFFFF);
			int32_t j = i + 1;
			while (j < len && ((idx[j] == 0xFFFF) == missing))
				j++;

			if (n < MAX_SEGS)
			{
				segs[n].start = i;
				segs[n].len = j - i;
				segs[n].symbol = missing;
				n++;
			}
			else
			{
				segs[n - 1].len += j - i; // (very mixed text: the rest as it is)
			}

			i = j;
		}
	}

	if (idx != stackIdx)
		free(idx);

	return n;
}

static float plainWidth(fontEntry *fe, const wchar_t *s, int32_t len)
{
	if (fe == NULL || len <= 0)
		return 0.0f;

	SelectObject(textDC, fe->font);
	SIZE sz;
	if (!GetTextExtentPoint32W(textDC, s, len, &sz))
		return 0.0f;

	return (float)sz.cx;
}

static float runWidth(fontEntry *fe, const wchar_t *s, int32_t len)
{
	if (len <= 0)
		return 0.0f;

	textSeg segs[MAX_SEGS];
	const int32_t n = textSegments(fe, s, len, segs);

	float w = 0.0f;
	for (int32_t i = 0; i < n; i++)
	{
		fontEntry *e = segs[i].symbol ? getFontKey(fe->size, 400, false, true) : fe;
		w += plainWidth(e, &s[segs[i].start], segs[i].len);
	}

	return w;
}

static bool ensureTextBitmap(int32_t w, int32_t h)
{
	if (textBmp != NULL && w <= textBmpW && h <= textBmpH)
		return true;

	if (w < textBmpW) w = textBmpW;
	if (h < textBmpH) h = textBmpH;
	w = (w + 63) & ~63;
	h = (h + 15) & ~15;

	if (textBmp != NULL)
	{
		SelectObject(textDC, textOldBmp);
		DeleteObject(textBmp);
		textBmp = NULL;
		textBits = NULL;
	}

	BITMAPINFO bi;
	memset(&bi, 0, sizeof (bi));
	bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h; // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void *bits = NULL;
	textBmp = CreateDIBSection(textDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (textBmp == NULL || bits == NULL)
	{
		textBmp = NULL;
		textBmpW = textBmpH = 0;
		return false;
	}

	textOldBmp = (HBITMAP)SelectObject(textDC, textBmp);
	textBits = (uint32_t *)bits;
	textBmpW = w;
	textBmpH = h;
	return true;
}

// one run of text with its top-left corner at (x, top)
static void drawRun(bt2Canvas *c, fontEntry *fe, int32_t x, int32_t top, const wchar_t *s, int32_t len, uint32_t color)
{
	if (len <= 0)
		return;

	textSeg segs[MAX_SEGS];
	const int32_t numSegs = textSegments(fe, s, len, segs);
	const float w = runWidth(fe, s, len);

	const int32_t pad = fe->size / 3 + 2; // (room for letters that reach outside their box)
	const int32_t padTop = fe->size / 2;  // (a symbol can be taller than the letters)
	const int32_t bw = (int32_t)w + pad * 2, bh = fe->height + padTop * 2;
	if (!ensureTextBitmap(bw, bh))
		return;

	for (int32_t y = 0; y < bh; y++)
		memset(&textBits[(size_t)y * textBmpW], 0, (size_t)bw * 4);

	SetTextColor(textDC, RGB(255, 255, 255));
	SetBkMode(textDC, TRANSPARENT);
	SetTextAlign(textDC, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);

	// (all parts on the same baseline)
	float xo = 0.0f;
	for (int32_t i = 0; i < numSegs; i++)
	{
		fontEntry *e = segs[i].symbol ? getFontKey(fe->size, 400, false, true) : fe;
		if (e == NULL)
			continue;

		SelectObject(textDC, e->font);
		TextOutW(textDC, pad + (int32_t)xo, padTop + fe->ascent, &s[segs[i].start], segs[i].len);
		xo += plainWidth(e, &s[segs[i].start], segs[i].len);
	}
	GdiFlush();

	for (int32_t y = 0; y < bh; y++)
	{
		const int32_t dy = top - padTop + y;
		if (dy < 0 || dy >= c->h)
			continue;

		const uint32_t *src = &textBits[(size_t)y * textBmpW];
		uint32_t *dst = &c->px[(size_t)dy * c->w];

		for (int32_t xx = 0; xx < bw; xx++)
		{
			const uint32_t p = src[xx] & 0x00FFFFFF;
			if (p == 0)
				continue;

			const int32_t dx = x - pad + xx;
			if (dx < 0 || dx >= c->w)
				continue;

			// (the brightest of the three: the same for gray smoothing, and safe if Windows used ClearType)
			uint32_t cov = (p >> 16) & 0xFF;
			if (((p >> 8) & 0xFF) > cov) cov = (p >> 8) & 0xFF;
			if ((p & 0xFF) > cov) cov = p & 0xFF;

			blendPixel(&dst[dx], color, (float)cov * (1.0f / 255.0f));
		}
	}
}

typedef struct lineSpan_t
{
	int32_t start, len;
	float w;
} lineSpan;

// word wrapping: breaks at spaces (and at '\n'), a word wider than the box gets a line of its own
static int32_t wrapLines(fontEntry *fe, const wchar_t *s, int32_t n, float maxW, lineSpan *lines, int32_t maxLines)
{
	int32_t count = 0, i = 0;

	if (n == 0 && maxLines > 0)
	{
		lines[0].start = 0;
		lines[0].len = 0;
		lines[0].w = 0.0f;
		return 1;
	}

	while (i < n && count < maxLines)
	{
		while (i < n && s[i] == L' ')
			i++;

		const int32_t start = i;
		int32_t end = i;
		float endW = 0.0f;

		while (end < n && s[end] != L'\n')
		{
			int32_t k = end;
			while (k < n && s[k] == L' ')
				k++;
			while (k < n && s[k] != L' ' && s[k] != L'\n')
				k++;

			const float w = runWidth(fe, &s[start], k - start);
			if (w <= maxW + 0.5f || end == start)
			{
				end = k;
				endW = w;
			}
			else
			{
				break;
			}
		}

		// (no spaces at the end of a line)
		int32_t trimmed = end;
		while (trimmed > start && s[trimmed-1] == L' ')
			trimmed--;
		if (trimmed != end)
			endW = runWidth(fe, &s[start], trimmed - start);

		lines[count].start = start;
		lines[count].len = trimmed - start;
		lines[count].w = endW;
		count++;

		i = end;
		if (i < n && s[i] == L'\n')
		{
			i++;
			if (i == n && count < maxLines) // (a '\n' at the very end: an empty last line)
			{
				lines[count].start = i;
				lines[count].len = 0;
				lines[count].w = 0.0f;
				count++;
			}
		}
	}

	return count;
}

#define MAX_LINES 64

float bt2LineHeight(bt2Font f)
{
	fontEntry *fe = getFont(f);
	return (fe != NULL) ? (float)fe->lineH : (float)f.size * 1.33f;
}

float bt2TextWidth(bt2Font f, const char *text)
{
	fontEntry *fe = getFont(f);
	int32_t n;
	wchar_t *w = toWide(text, &n);
	const float width = (fe != NULL && w != NULL) ? runWidth(fe, w, n) : 0.0f;
	free(w);
	return width;
}

int32_t bt2TextLines(bt2Font f, float maxW, const char *text)
{
	fontEntry *fe = getFont(f);
	int32_t n;
	wchar_t *w = toWide(text, &n);
	if (fe == NULL || w == NULL)
	{
		free(w);
		return 1;
	}

	lineSpan lines[MAX_LINES];
	const int32_t count = wrapLines(fe, w, n, maxW, lines, MAX_LINES);
	free(w);
	return (count > 0) ? count : 1;
}

float bt2TextBlockWidth(bt2Font f, float maxW, const char *text)
{
	fontEntry *fe = getFont(f);
	int32_t n;
	wchar_t *w = toWide(text, &n);
	if (fe == NULL || w == NULL)
	{
		free(w);
		return 0.0f;
	}

	lineSpan lines[MAX_LINES];
	const int32_t count = wrapLines(fe, w, n, maxW, lines, MAX_LINES);
	free(w);

	float widest = 0.0f;
	for (int32_t i = 0; i < count; i++)
	{
		if (lines[i].w > widest)
			widest = lines[i].w;
	}

	return widest;
}

void bt2DrawText(bt2Canvas *c, bt2Font f, float x, float y, float w, float lineH, int32_t align, uint32_t color, const char *text)
{
	fontEntry *fe = getFont(f);
	int32_t n;
	wchar_t *ws = toWide(text, &n);
	if (fe == NULL || ws == NULL)
	{
		free(ws);
		return;
	}

	lineSpan lines[MAX_LINES];
	const int32_t count = wrapLines(fe, ws, n, w, lines, MAX_LINES);

	// (CSS puts the letters in the middle of their line box)
	const float halfLeading = (lineH - (float)fe->height) * 0.5f;

	for (int32_t i = 0; i < count; i++)
	{
		float lx = x;
		if (align == BT2_ALIGN_CENTER)
			lx = x + (w - lines[i].w) * 0.5f;
		else if (align == BT2_ALIGN_RIGHT)
			lx = x + (w - lines[i].w);

		const float top = y + i * lineH + halfLeading;
		drawRun(c, fe, (int32_t)floorf(lx + 0.5f), (int32_t)floorf(top + 0.5f), &ws[lines[i].start], lines[i].len, color);
	}

	free(ws);
}

// <b>...</b> runs
typedef struct richRun_t
{
	int32_t start, len;
	bool bold;
} richRun;

static int32_t splitRich(const wchar_t *s, int32_t n, wchar_t *plain, richRun *runs, int32_t maxRuns)
{
	int32_t count = 0, out = 0;
	bool bold = false;
	int32_t runStart = 0;

	for (int32_t i = 0; i <= n; i++)
	{
		const bool open = (i + 3 <= n && wcsncmp(&s[i], L"<b>", 3) == 0);
		const bool close = (i + 4 <= n && wcsncmp(&s[i], L"</b>", 4) == 0);

		if (i == n || open || close)
		{
			if (out > runStart && count < maxRuns)
			{
				runs[count].start = runStart;
				runs[count].len = out - runStart;
				runs[count].bold = bold;
				count++;
			}

			if (i == n)
				break;

			bold = open;
			i += open ? 2 : 3;
			runStart = out;
			continue;
		}

		plain[out++] = s[i];
	}

	plain[out] = L'\0';
	return count;
}

float bt2RichWidth(bt2Font f, const char *text)
{
	bt2Font fb = f;
	fb.weight = 700;
	fontEntry *fe = getFont(f);
	fontEntry *feb = getFont(fb);
	fe = getFont(f); // (the second call may have replaced the cache)

	int32_t n;
	wchar_t *ws = toWide(text, &n);
	wchar_t *plain = (wchar_t *)malloc((size_t)(n + 1) * sizeof (wchar_t));
	if (fe == NULL || feb == NULL || ws == NULL || plain == NULL)
	{
		free(ws); free(plain);
		return 0.0f;
	}

	richRun runs[16];
	const int32_t count = splitRich(ws, n, plain, runs, 16);

	float w = 0.0f;
	for (int32_t i = 0; i < count; i++)
		w += runWidth(runs[i].bold ? feb : fe, &plain[runs[i].start], runs[i].len);

	free(ws); free(plain);
	return w;
}

void bt2DrawRich(bt2Canvas *c, bt2Font f, float x, float y, float lineH, uint32_t color, uint32_t boldColor, const char *text)
{
	bt2Font fb = f;
	fb.weight = 700;
	fontEntry *feb = getFont(fb);
	fontEntry *fe = getFont(f);
	feb = getFont(fb);

	int32_t n;
	wchar_t *ws = toWide(text, &n);
	wchar_t *plain = (wchar_t *)malloc((size_t)(n + 1) * sizeof (wchar_t));
	if (fe == NULL || feb == NULL || ws == NULL || plain == NULL)
	{
		free(ws); free(plain);
		return;
	}

	richRun runs[16];
	const int32_t count = splitRich(ws, n, plain, runs, 16);

	float px = x;
	for (int32_t i = 0; i < count; i++)
	{
		fontEntry *e = runs[i].bold ? feb : fe;
		const float top = y + (lineH - (float)e->height) * 0.5f;
		drawRun(c, e, (int32_t)floorf(px + 0.5f), (int32_t)floorf(top + 0.5f), &plain[runs[i].start], runs[i].len, runs[i].bold ? boldColor : color);
		px += runWidth(e, &plain[runs[i].start], runs[i].len);
	}

	free(ws); free(plain);
}

void bt2UiResetFonts(void)
{
	if (textDC != NULL)
		SelectObject(textDC, GetStockObject(SYSTEM_FONT));

	freeFonts();
}

void bt2UiFree(void)
{
	if (textDC != NULL)
	{
		SelectObject(textDC, GetStockObject(SYSTEM_FONT));
		if (textBmp != NULL)
		{
			SelectObject(textDC, textOldBmp);
			DeleteObject(textBmp);
			textBmp = NULL;
			textBits = NULL;
			textBmpW = textBmpH = 0;
		}
	}

	freeFonts();

	if (textDC != NULL)
	{
		DeleteDC(textDC);
		textDC = NULL;
	}
}

#endif
