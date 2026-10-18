// BerntTracker: on-screen keyboard ("Retro 2" layout) drawn below the FT2 screen.
//
// The keys send the same scancodes/keycodes as a real PC keyboard, so all of the
// normal FT2 key handling (notes, play/stop, F-keys, numpad, Ctrl+letter screens)
// works unchanged. Shift/Ctrl/Alt/Fn and numpad '+' are "sticky": tap once and they
// stay on until the next key has been pressed and released.

// for finding memory leaks in debug mode with Visual Studio
#if defined _DEBUG && defined _MSC_VER
#include <crtdbg.h>
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft2_header.h"
#include "ft2_touchkeys.h"
#include "ft2_keyboard.h"
#include "ft2_video.h"
#include "ft2_gui.h"
#include "ft2_bmp.h"
#include "ft2_tables.h"
#include "ft2_structs.h"
#include "ft2_replayer.h"
#include "ft2_textboxes.h"
#include "ft2_config.h"
#include "ft2_unicode.h"

// ------------------------------------------------------------------------------------------------
// layout

enum // what a key does
{
	K_NORMAL = 0, // sends its scancode
	K_SHIFT,      // sticky left Shift
	K_CTRL,       // sticky left Ctrl
	K_ALT,        // sticky left Alt
	K_FN,         // sticky Fn layer (number row = F1..F12, arrows = PgUp/Home/PgDn/End)
	K_KPPLUS,     // sticky numpad '+' (instrument bank +4)
	K_OCT_DN,
	K_OCT_UP,
	K_HIDE        // hides the keyboard (a thin bar with a "Keyboard" button stays)
};

typedef struct tkKey_t
{
	uint8_t row, rows, type;
	float x, w; // in key units
	SDL_Scancode scancode, fnScancode; // fnScancode: what the key sends in the Fn layer (0 = same)
	const char *label; // label on the key (font1). \x01..\x06 = special glyphs, \x8F/\x92/\x9D = Å/Æ/Ø
	char ch, chShift; // character when typing text (song/instrument names etc.), 0 = none
} tkKey_t;

#define N 15.4f // x position of the numpad

static const tkKey_t keys[] =
{
	// row 0: Fn + number row + backspace
	{ 0,1,K_FN,    0.0f, 1.0f, 0, 0, "Fn", 0, 0 },
	{ 0,1,K_NORMAL,1.0f, 1.0f, SDL_SCANCODE_1, SDL_SCANCODE_F1, "1", '1', '!' },
	{ 0,1,K_NORMAL,2.0f, 1.0f, SDL_SCANCODE_2, SDL_SCANCODE_F2, "2", '2', '"' },
	{ 0,1,K_NORMAL,3.0f, 1.0f, SDL_SCANCODE_3, SDL_SCANCODE_F3, "3", '3', '#' },
	{ 0,1,K_NORMAL,4.0f, 1.0f, SDL_SCANCODE_4, SDL_SCANCODE_F4, "4", '4', '$' },
	{ 0,1,K_NORMAL,5.0f, 1.0f, SDL_SCANCODE_5, SDL_SCANCODE_F5, "5", '5', '%' },
	{ 0,1,K_NORMAL,6.0f, 1.0f, SDL_SCANCODE_6, SDL_SCANCODE_F6, "6", '6', '&' },
	{ 0,1,K_NORMAL,7.0f, 1.0f, SDL_SCANCODE_7, SDL_SCANCODE_F7, "7", '7', '/' },
	{ 0,1,K_NORMAL,8.0f, 1.0f, SDL_SCANCODE_8, SDL_SCANCODE_F8, "8", '8', '(' },
	{ 0,1,K_NORMAL,9.0f, 1.0f, SDL_SCANCODE_9, SDL_SCANCODE_F9, "9", '9', ')' },
	{ 0,1,K_NORMAL,10.0f,1.0f, SDL_SCANCODE_0, SDL_SCANCODE_F10, "0", '0', '=' },
	{ 0,1,K_NORMAL,11.0f,1.0f, SDL_SCANCODE_MINUS, SDL_SCANCODE_F11, "+", '+', '?' },
	{ 0,1,K_NORMAL,12.0f,1.0f, SDL_SCANCODE_EQUALS, SDL_SCANCODE_F12, "\\", '\\', '`' },
	{ 0,1,K_NORMAL,13.0f,1.0f, SDL_SCANCODE_BACKSPACE, 0, "\x05", 0, 0 },
	{ 0,1,K_HIDE,  14.0f,1.0f, 0, 0, "\x07", 0, 0 },

	// row 1: Tab + QWERTY + Ins
	{ 1,1,K_NORMAL,0.0f, 1.5f, SDL_SCANCODE_TAB, 0, "Tab", 0, 0 },
	{ 1,1,K_NORMAL,1.5f, 1.0f, SDL_SCANCODE_Q, 0, "Q", 'q', 'Q' },
	{ 1,1,K_NORMAL,2.5f, 1.0f, SDL_SCANCODE_W, 0, "W", 'w', 'W' },
	{ 1,1,K_NORMAL,3.5f, 1.0f, SDL_SCANCODE_E, 0, "E", 'e', 'E' },
	{ 1,1,K_NORMAL,4.5f, 1.0f, SDL_SCANCODE_R, 0, "R", 'r', 'R' },
	{ 1,1,K_NORMAL,5.5f, 1.0f, SDL_SCANCODE_T, 0, "T", 't', 'T' },
	{ 1,1,K_NORMAL,6.5f, 1.0f, SDL_SCANCODE_Y, 0, "Y", 'y', 'Y' },
	{ 1,1,K_NORMAL,7.5f, 1.0f, SDL_SCANCODE_U, 0, "U", 'u', 'U' },
	{ 1,1,K_NORMAL,8.5f, 1.0f, SDL_SCANCODE_I, 0, "I", 'i', 'I' },
	{ 1,1,K_NORMAL,9.5f, 1.0f, SDL_SCANCODE_O, 0, "O", 'o', 'O' },
	{ 1,1,K_NORMAL,10.5f,1.0f, SDL_SCANCODE_P, 0, "P", 'p', 'P' },
	{ 1,1,K_NORMAL,11.5f,1.0f, SDL_SCANCODE_LEFTBRACKET, 0, "\x8F", (char)0x86, (char)0x8F }, // Å
	{ 1,1,K_NORMAL,12.5f,1.0f, SDL_SCANCODE_RIGHTBRACKET, 0, "\x06", 0, 0 }, // ¨
	{ 1,1,K_NORMAL,13.5f,1.5f, SDL_SCANCODE_INSERT, 0, "Ins", 0, 0 },

	// row 2: Caps + ASDF + Del
	{ 2,1,K_NORMAL,0.0f, 1.75f, SDL_SCANCODE_CAPSLOCK, 0, "Caps", 0, 0 },
	{ 2,1,K_NORMAL,1.75f,1.0f, SDL_SCANCODE_A, 0, "A", 'a', 'A' },
	{ 2,1,K_NORMAL,2.75f,1.0f, SDL_SCANCODE_S, 0, "S", 's', 'S' },
	{ 2,1,K_NORMAL,3.75f,1.0f, SDL_SCANCODE_D, 0, "D", 'd', 'D' },
	{ 2,1,K_NORMAL,4.75f,1.0f, SDL_SCANCODE_F, 0, "F", 'f', 'F' },
	{ 2,1,K_NORMAL,5.75f,1.0f, SDL_SCANCODE_G, 0, "G", 'g', 'G' },
	{ 2,1,K_NORMAL,6.75f,1.0f, SDL_SCANCODE_H, 0, "H", 'h', 'H' },
	{ 2,1,K_NORMAL,7.75f,1.0f, SDL_SCANCODE_J, 0, "J", 'j', 'J' },
	{ 2,1,K_NORMAL,8.75f,1.0f, SDL_SCANCODE_K, 0, "K", 'k', 'K' },
	{ 2,1,K_NORMAL,9.75f,1.0f, SDL_SCANCODE_L, 0, "L", 'l', 'L' },
	{ 2,1,K_NORMAL,10.75f,1.0f,SDL_SCANCODE_SEMICOLON, 0, "\x9D", (char)0x9B, (char)0x9D }, // Ø
	{ 2,1,K_NORMAL,11.75f,1.0f,SDL_SCANCODE_APOSTROPHE, 0, "\x92", (char)0x91, (char)0x92 }, // Æ
	{ 2,1,K_NORMAL,12.75f,1.0f,SDL_SCANCODE_BACKSLASH, 0, "'", '\'', '*' },
	{ 2,1,K_NORMAL,13.75f,1.25f,SDL_SCANCODE_DELETE, 0, "Del", 0, 0 },

	// row 3: Shift + < + ZXCV + up + right Shift (record)
	{ 3,1,K_SHIFT, 0.0f, 1.5f, 0, 0, "Shift", 0, 0 },
	{ 3,1,K_NORMAL,1.5f, 1.0f, SDL_SCANCODE_NONUSBACKSLASH, 0, "<", '<', '>' },
	{ 3,1,K_NORMAL,2.5f, 1.0f, SDL_SCANCODE_Z, 0, "Z", 'z', 'Z' },
	{ 3,1,K_NORMAL,3.5f, 1.0f, SDL_SCANCODE_X, 0, "X", 'x', 'X' },
	{ 3,1,K_NORMAL,4.5f, 1.0f, SDL_SCANCODE_C, 0, "C", 'c', 'C' },
	{ 3,1,K_NORMAL,5.5f, 1.0f, SDL_SCANCODE_V, 0, "V", 'v', 'V' },
	{ 3,1,K_NORMAL,6.5f, 1.0f, SDL_SCANCODE_B, 0, "B", 'b', 'B' },
	{ 3,1,K_NORMAL,7.5f, 1.0f, SDL_SCANCODE_N, 0, "N", 'n', 'N' },
	{ 3,1,K_NORMAL,8.5f, 1.0f, SDL_SCANCODE_M, 0, "M", 'm', 'M' },
	{ 3,1,K_NORMAL,9.5f, 1.0f, SDL_SCANCODE_COMMA, 0, ",", ',', ';' },
	{ 3,1,K_NORMAL,10.5f,1.0f, SDL_SCANCODE_PERIOD, 0, ".", '.', ':' },
	{ 3,1,K_NORMAL,11.5f,1.0f, SDL_SCANCODE_SLASH, 0, "-", '-', '_' },
	{ 3,1,K_NORMAL,12.5f,1.25f,SDL_SCANCODE_UP, SDL_SCANCODE_PAGEUP, "\x03", 0, 0 },
	{ 3,1,K_NORMAL,13.75f,1.25f,SDL_SCANCODE_RSHIFT, 0, "Shift", 0, 0 },

	// row 4: Ctrl Alt Space Oct-/+ AltGr Ctrl arrows
	{ 4,1,K_CTRL,  0.0f, 1.5f, 0, 0, "Ctrl", 0, 0 },
	{ 4,1,K_ALT,   1.5f, 1.5f, 0, 0, "Alt", 0, 0 },
	{ 4,1,K_NORMAL,3.0f, 2.75f,SDL_SCANCODE_SPACE, 0, "Space", ' ', ' ' },
	{ 4,1,K_OCT_DN,5.75f,1.0f, 0, 0, "Oct -", 0, 0 },
	{ 4,1,K_OCT_UP,6.75f,1.0f, 0, 0, "Oct +", 0, 0 },
	{ 4,1,K_NORMAL,7.75f,1.75f,SDL_SCANCODE_RALT, 0, "AltGr", 0, 0 },
	{ 4,1,K_NORMAL,9.5f, 1.75f,SDL_SCANCODE_RCTRL, 0, "Ctrl", 0, 0 },
	{ 4,1,K_NORMAL,11.25f,1.25f,SDL_SCANCODE_LEFT, SDL_SCANCODE_HOME, "\x01", 0, 0 },
	{ 4,1,K_NORMAL,12.5f,1.25f,SDL_SCANCODE_DOWN, SDL_SCANCODE_PAGEDOWN, "\x04", 0, 0 },
	{ 4,1,K_NORMAL,13.75f,1.25f,SDL_SCANCODE_RIGHT, SDL_SCANCODE_END, "\x02", 0, 0 },

	// numpad
	{ 0,1,K_NORMAL,N+0.0f,1.0f, SDL_SCANCODE_NUMLOCKCLEAR, 0, "Num", 0, 0 },
	{ 0,1,K_NORMAL,N+1.0f,1.0f, SDL_SCANCODE_KP_DIVIDE, 0, "/", 0, 0 },
	{ 0,1,K_NORMAL,N+2.0f,1.0f, SDL_SCANCODE_KP_MULTIPLY, 0, "*", 0, 0 },
	{ 0,1,K_NORMAL,N+3.0f,1.0f, SDL_SCANCODE_KP_MINUS, 0, "-", 0, 0 },
	{ 1,1,K_NORMAL,N+0.0f,1.0f, SDL_SCANCODE_KP_7, 0, "7", 0, 0 },
	{ 1,1,K_NORMAL,N+1.0f,1.0f, SDL_SCANCODE_KP_8, 0, "8", 0, 0 },
	{ 1,1,K_NORMAL,N+2.0f,1.0f, SDL_SCANCODE_KP_9, 0, "9", 0, 0 },
	{ 1,2,K_KPPLUS,N+3.0f,1.0f, SDL_SCANCODE_KP_PLUS, 0, "+", 0, 0 },
	{ 2,1,K_NORMAL,N+0.0f,1.0f, SDL_SCANCODE_KP_4, 0, "4", 0, 0 },
	{ 2,1,K_NORMAL,N+1.0f,1.0f, SDL_SCANCODE_KP_5, 0, "5", 0, 0 },
	{ 2,1,K_NORMAL,N+2.0f,1.0f, SDL_SCANCODE_KP_6, 0, "6", 0, 0 },
	{ 3,1,K_NORMAL,N+0.0f,1.0f, SDL_SCANCODE_KP_1, 0, "1", 0, 0 },
	{ 3,1,K_NORMAL,N+1.0f,1.0f, SDL_SCANCODE_KP_2, 0, "2", 0, 0 },
	{ 3,1,K_NORMAL,N+2.0f,1.0f, SDL_SCANCODE_KP_3, 0, "3", 0, 0 },
	{ 3,2,K_NORMAL,N+3.0f,1.0f, SDL_SCANCODE_KP_ENTER, 0, "Enter", 0, 0 },
	{ 4,1,K_NORMAL,N+0.0f,2.0f, SDL_SCANCODE_KP_0, 0, "0", 0, 0 },
	{ 4,1,K_NORMAL,N+2.0f,1.0f, SDL_SCANCODE_KP_PERIOD, 0, ",", 0, 0 },
};

#define NUM_KEYS ((int32_t)(sizeof (keys) / sizeof (keys[0])))

#define KEY_U ((TK_W - 4) / 19.4f) // width of one key unit in pixels
#define ROW_PITCH 29
#define KEY_H 27

// ------------------------------------------------------------------------------------------------
// colors (key fill, text) - same as the "Retro 2" design

enum { C_WHITE, C_BLACK, C_OFF, C_REC, C_OCTAVE, C_JUMP, C_PLAY, C_NAV, C_EDIT, C_HEX, C_INSTR, C_SCREEN, C_MOD, C_ACTIVE, C_IDLE, C_NUM };

static const uint32_t catFill[C_NUM] =
{
	0xDFDFE3, 0x18181C, 0x8E2F2A, 0xE8413C, 0xF2B53A, 0x38B2A5, 0x6CC46F, 0x7AA8EC,
	0xA98BEA, 0xEE9256, 0xF4C08F, 0xE58FB8, 0x828286, 0xFFFFFF, 0x3B4045
};

static const uint32_t catText[C_NUM] =
{
	0x000000, 0xDFDFE3, 0xFFFFFF, 0xFFFFFF, 0x000000, 0x000000, 0x000000, 0x000000,
	0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x7A7F86
};

#define COL_DESK 0x2D3135
#define COL_FW_LIGHT 0x596169
#define COL_FW_DARK 0x0C1010

// ------------------------------------------------------------------------------------------------
// state

#define MAX_POINTERS 10
#define MOUSE_POINTER_ID (-1000)

typedef struct pointer_t
{
	bool active;
	int64_t id; // SDL finger id, or MOUSE_POINTER_ID
	int16_t key;
	SDL_Scancode sentScancode; // what we sent on key down (for the matching key up)
} pointer_t;

static bool shown = false, shiftOn, ctrlOn, altOn, fnOn, kpPlusOn, mouseOverKb;
static uint8_t heldCount[NUM_KEYS];
static pointer_t pointers[MAX_POINTERS];
static uint32_t *kbBuffer;
static uint64_t lastStateHash;
static SDL_Texture *kbTexture;
static SDL_Rect kbWinRect, kbPixRect;

bool tkIsEnabled(void)
{
	return true; // the keyboard area (full keyboard or the thin bar) is always part of the layout
}

bool tkIsShown(void)
{
	return shown;
}

int32_t tkGetAreaHeight(void)
{
	return shown ? TK_H : TK_MINI_H;
}

bool tkMouseIsOverKeyboard(void)
{
	return mouseOverKb;
}

void tkToggle(void)
{
	shown ^= 1;
	lastStateHash = 0;

	if (video.fullscreen)
	{
		enterFullscreen(); // recalculates the layout
	}
	else
	{
		setWindowSizeFromConfig(true);
		SDL_SetWindowPosition(video.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	}
}

void tkSetRects(const SDL_Rect *windowRect, const SDL_Rect *pixelRect)
{
	kbWinRect = *windowRect;
	kbPixRect = *pixelRect;
}

bool tkInit(SDL_Renderer *renderer)
{
	kbBuffer = (uint32_t *)malloc(TK_W * TK_H * sizeof (uint32_t));
	if (kbBuffer == NULL)
		return false;

	SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "nearest");
	kbTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, TK_W, TK_H);
	if (kbTexture == NULL)
		return false;

	SDL_SetTextureBlendMode(kbTexture, SDL_BLENDMODE_NONE);
	lastStateHash = 0;
	return true;
}

void tkFree(void)
{
	if (kbTexture != NULL)
	{
		SDL_DestroyTexture(kbTexture);
		kbTexture = NULL;
	}

	if (kbBuffer != NULL)
	{
		free(kbBuffer);
		kbBuffer = NULL;
	}
}

// ------------------------------------------------------------------------------------------------
// drawing helpers (into kbBuffer)

static uint32_t tk_mix(uint32_t a, uint32_t b, int32_t t256) // t256 = 0..256
{
	int32_t r = ((a >> 16) & 0xFF) + ((((int32_t)((b >> 16) & 0xFF) - (int32_t)((a >> 16) & 0xFF)) * t256) >> 8);
	int32_t g = ((a >>  8) & 0xFF) + ((((int32_t)((b >>  8) & 0xFF) - (int32_t)((a >>  8) & 0xFF)) * t256) >> 8);
	int32_t bl = (a & 0xFF) + ((((int32_t)(b & 0xFF) - (int32_t)(a & 0xFF)) * t256) >> 8);
	return (uint32_t)((r << 16) | (g << 8) | bl);
}

static void tk_fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col)
{
	for (int32_t yy = y; yy < y+h; yy++)
	{
		if (yy < 0 || yy >= TK_H) continue;
		for (int32_t xx = x; xx < x+w; xx++)
		{
			if (xx >= 0 && xx < TK_W)
				kbBuffer[(yy * TK_W) + xx] = col;
		}
	}
}

static void tk_putPixel(int32_t x, int32_t y, uint32_t col)
{
	if (x >= 0 && x < TK_W && y >= 0 && y < TK_H)
		kbBuffer[(y * TK_W) + x] = col;
}

static void tk_drawGlyphRows(int32_t x, int32_t y, const char *const *rows, int32_t numRows, uint32_t col)
{
	for (int32_t yy = 0; yy < numRows; yy++)
	{
		for (int32_t xx = 0; rows[yy][xx] != '\0'; xx++)
		{
			if (rows[yy][xx] == '#')
				tk_putPixel(x + xx, y + yy, col);
		}
	}
}

// special glyphs for font1-sized labels
static const char *glyphLeft[7]  = { "...#....", "..##....", ".#######", "########", ".#######", "..##....", "...#...." };
static const char *glyphRight[7] = { "....#...", "....##..", "#######.", "########", "#######.", "....##..", "....#..." };
static const char *glyphUp[7]    = { "...##...", "..####..", ".######.", "########", "..####..", "..####..", "..####.." };
static const char *glyphDown[7]  = { "..####..", "..####..", "..####..", "########", ".######.", "..####..", "...##..." };
static const char *glyphBksp[7]  = { "....#######", "...##.....#", "..###.#.#.#", ".####..#..#", "..###.#.#.#", "...##.....#", "....#######" };
static const char *glyphUmlaut[2] = { "##.##", "##.##" };
static const char *glyphHide[7]  = { "..........", "#########.", ".#######..", "..#####...", "...###....", "....#.....", ".........." };
static const char *glyphUpSmall[4] = { "...#...", "..###..", ".#####.", "#######" };

static void tk_bigText(int32_t x, int32_t y, const char *s, uint32_t col) // font1 (the main FT2 font)
{
	for (; *s != '\0'; s++)
	{
		const uint8_t c = (uint8_t)*s;
		switch (c)
		{
			case 1: tk_drawGlyphRows(x, y+1, glyphLeft,  7, col); x += 10; continue;
			case 2: tk_drawGlyphRows(x, y+1, glyphRight, 7, col); x += 10; continue;
			case 3: tk_drawGlyphRows(x, y+1, glyphUp,    7, col); x += 10; continue;
			case 4: tk_drawGlyphRows(x, y+1, glyphDown,  7, col); x += 10; continue;
			case 5: tk_drawGlyphRows(x, y+1, glyphBksp,  7, col); x += 13; continue;
			case 6: tk_drawGlyphRows(x, y+6, glyphUmlaut,2, col); x += 7;  continue;
			case 7: tk_drawGlyphRows(x, y+1, glyphHide,  7, col); x += 11; continue;
			default: break;
		}

		const uint8_t chr = c & 0x7F;
		if (chr != ' ')
		{
			const uint8_t *src = &bmp.font1[chr * FONT1_CHAR_W];
			for (int32_t yy = 0; yy < FONT1_CHAR_H; yy++)
			{
				for (int32_t xx = 0; xx < FONT1_CHAR_W; xx++)
				{
					if (src[xx] != 0)
						tk_putPixel(x + xx, y + yy, col);
				}
				src += FONT1_WIDTH;
			}
		}

		x += font1Widths[chr];
	}
}

// extra glyphs for the tiny font (font3 only has 0-9 and A-Z)
typedef struct tinyGlyph_t { char ch; const char *rows[7]; } tinyGlyph_t;
static const tinyGlyph_t tinyExtra[] =
{
	{ '.', { "...", "...", "...", "...", "...", ".#.", "..." } },
	{ ',', { "...", "...", "...", "...", "...", ".#.", "#.." } },
	{ '/', { "...", "..#", "..#", ".#.", "#..", "#..", "..." } },
	{ '+', { "...", "...", ".#.", "###", ".#.", "...", "..." } },
	{ '-', { "...", "...", "...", "###", "...", "...", "..." } },
	{ ':', { "...", "...", ".#.", "...", ".#.", "...", "..." } },
	{ '#', { ".....", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", "....." } },
};

static void tk_tinyText(int32_t x, int32_t y, const char *s, uint32_t col) // font3 (tiny FT2 font)
{
	for (; *s != '\0'; s++)
	{
		char c = *s;
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';

		if (c == ' ')
		{
			x += 3;
			continue;
		}

		int32_t idx = -1;
		     if (c >= '0' && c <= '9') idx = c - '0';
		else if (c >= 'A' && c <= 'Z') idx = 10 + (c - 'A');

		if (idx >= 0)
		{
			const uint8_t *src = &bmp.font3[idx * FONT3_CHAR_W];
			for (int32_t yy = 0; yy < FONT3_CHAR_H; yy++)
			{
				for (int32_t xx = 0; xx < FONT3_CHAR_W; xx++)
				{
					if (src[xx] != 0)
						tk_putPixel(x + xx, y + yy, col);
				}
				src += FONT3_WIDTH;
			}
			x += 4;
			continue;
		}

		for (int32_t i = 0; i < (int32_t)(sizeof (tinyExtra) / sizeof (tinyExtra[0])); i++)
		{
			if (tinyExtra[i].ch == c)
			{
				tk_drawGlyphRows(x, y, tinyExtra[i].rows, 7, col);
				x += (int32_t)strlen(tinyExtra[i].rows[0]) + 1;
				break;
			}
		}
	}
}

static void tk_noteText(int32_t x, int32_t y, int32_t note, uint32_t col) // small note font from the pattern editor
{
	static const uint8_t noteChar1[12] = { 8, 8, 9, 9, 10, 11, 11, 12, 12, 13, 13, 14 }; // C C D D E F F G G A A B
	static const uint8_t noteChar2[12] = { 16, 15, 16, 15, 16, 16, 15, 16, 15, 16, 15, 16 }; // '-' or '#'

	note--;
	const int32_t n = note % 12;
	const int32_t oct = note / 12;
	const uint8_t chars[3] = { noteChar1[n], noteChar2[n], (uint8_t)oct };

	for (int32_t i = 0; i < 3; i++)
	{
		const uint8_t *src = &bmp.font7[chars[i] * FONT7_CHAR_W];
		for (int32_t yy = 0; yy < FONT7_CHAR_H; yy++)
		{
			for (int32_t xx = 0; xx < FONT7_CHAR_W; xx++)
			{
				if (src[xx] != 0)
					tk_putPixel(x + xx, y + yy, col);
			}
			src += FONT7_WIDTH;
		}
		x += FONT7_CHAR_W;
	}
}

static void tk_drawButton(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fill, bool pressed)
{
	const uint32_t light = tk_mix(fill, 0xFFFFFF, 141); // ~55% towards white
	const uint32_t dark = tk_mix(fill, 0x000000, 128); // ~50% towards black
	const uint32_t a = pressed ? dark : light;
	const uint32_t b = pressed ? light : dark;

	tk_fillRect(x, y, w, h, 0x000000);
	tk_fillRect(x+1, y+1, w-2, h-2, fill);
	tk_fillRect(x+1, y+1, w-2, 1, a);
	tk_fillRect(x+1, y+1, 1, h-2, a);
	tk_fillRect(x+1, y+h-2, w-2, 1, b);
	tk_fillRect(x+w-2, y+1, 1, h-2, b);
}

// ------------------------------------------------------------------------------------------------
// which "layer" is shown

enum { L_NORMAL, L_FN, L_CTRL, L_HEX, L_EFX, L_VOL, L_TEXT };

static int32_t tk_getLayer(void)
{
	if (editor.editTextFlag || ui.sysReqShown) return L_TEXT;
	if (fnOn) return L_FN;
	if (ctrlOn) return L_CTRL;

	if (ui.patternEditorShown && cursor.object != CURSOR_NOTE)
	{
		if (cursor.object == CURSOR_EFX0) return L_EFX;
		if (cursor.object == CURSOR_VOL1) return L_VOL;
		return L_HEX;
	}

	return L_NORMAL;
}

static bool tk_isLetterKey(SDL_Scancode sc)
{
	return sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z;
}

static bool tk_isDigitKey(SDL_Scancode sc)
{
	return sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0;
}

static const char *tk_ctrlLetterLabel(SDL_Scancode sc)
{
	switch (sc)
	{
		case SDL_SCANCODE_A: return "Adv.ed";
		case SDL_SCANCODE_B: return "About";
		case SDL_SCANCODE_C: return "Config";
		case SDL_SCANCODE_D: return "Disk op";
		case SDL_SCANCODE_E: return "S.E.Ext";
		case SDL_SCANCODE_H: return "Help";
		case SDL_SCANCODE_I: return "Instr.ed";
		case SDL_SCANCODE_K: return "Hide kb";
		case SDL_SCANCODE_M: return "I.E.Ext";
		case SDL_SCANCODE_N: return "Nibbles";
		case SDL_SCANCODE_P: return "Pattern";
		case SDL_SCANCODE_R: return "Trim";
		case SDL_SCANCODE_S: return "Smp.ed";
		case SDL_SCANCODE_T: return "Transp";
		case SDL_SCANCODE_V: return "Scale v";
		case SDL_SCANCODE_X: return "Main";
		case SDL_SCANCODE_Z: return "Extend";
		default: return NULL;
	}
}

static bool tk_isVolColumnKey(SDL_Scancode sc)
{
	switch (sc)
	{
		case SDL_SCANCODE_0: case SDL_SCANCODE_1: case SDL_SCANCODE_2: case SDL_SCANCODE_3: case SDL_SCANCODE_4:
		case SDL_SCANCODE_MINUS: case SDL_SCANCODE_EQUALS: case SDL_SCANCODE_D: case SDL_SCANCODE_U:
		case SDL_SCANCODE_S: case SDL_SCANCODE_V: case SDL_SCANCODE_P: case SDL_SCANCODE_L:
		case SDL_SCANCODE_R: case SDL_SCANCODE_M:
			return true;
		default: return false;
	}
}

static bool tk_isKeyHeld(int32_t i)
{
	return heldCount[i] > 0;
}

// works out label, sub-label and color category of key 'i' in the current layer
static void tk_getKeyLook(int32_t i, int32_t layer, char *label, char *sub, int32_t *cat, int32_t *note)
{
	const tkKey_t *k = &keys[i];
	const SDL_Scancode sc = k->scancode;

	strcpy(label, k->label);
	sub[0] = '\0';
	*note = 0;
	*cat = C_IDLE;

	switch (k->type)
	{
		case K_FN:
			*cat = fnOn ? C_ACTIVE : C_MOD;
			sprintf(sub, "Oct %d", editor.curOctave);
			return;

		case K_SHIFT: *cat = shiftOn ? C_ACTIVE : C_MOD; strcpy(sub, shiftOn ? "On" : "More"); return;
		case K_CTRL:  *cat = ctrlOn  ? C_ACTIVE : C_MOD; strcpy(sub, ctrlOn  ? "On" : "More"); return;
		case K_ALT:   *cat = altOn   ? C_ACTIVE : C_MOD; strcpy(sub, altOn   ? "On" : "Mark"); return;

		case K_OCT_DN: *cat = C_OCTAVE; strcpy(sub, "Lower"); return;
		case K_OCT_UP: *cat = C_OCTAVE; strcpy(sub, "Higher"); return;
		case K_HIDE:   *cat = C_MOD;    strcpy(sub, "Hide"); return;

		case K_KPPLUS:
			*cat = kpPlusOn ? C_ACTIVE : C_INSTR;
			strcpy(sub, ctrlOn ? "Vol +" : "Blk +4");
			return;

		default: break;
	}

	// keys that look the same in all layers
	switch (sc)
	{
		case SDL_SCANCODE_BACKSPACE: *cat = C_EDIT; strcpy(sub, "Del prev"); return;
		case SDL_SCANCODE_TAB:       *cat = C_NAV;  strcpy(sub, "Next ch."); return;
		case SDL_SCANCODE_INSERT:    *cat = C_EDIT; strcpy(sub, "Insert"); return;
		case SDL_SCANCODE_DELETE:    *cat = C_EDIT; strcpy(sub, "Delete"); return;
		case SDL_SCANCODE_RSHIFT:
			if (layer == L_TEXT) { *cat = C_NAV; strcpy(label, "Enter"); strcpy(sub, "Done"); } // finishes text editing
			else { *cat = C_REC; strcpy(sub, "Rec ptn."); }
			return;
		case SDL_SCANCODE_RALT:      *cat = C_PLAY; strcpy(sub, "Play ptn."); return;
		case SDL_SCANCODE_RCTRL:     *cat = C_PLAY; strcpy(sub, "Play song"); return;
		case SDL_SCANCODE_SPACE:     *cat = C_PLAY; strcpy(sub, (layer == L_TEXT) ? "" : "Stop / Edit"); return;

		case SDL_SCANCODE_UP:
			*cat = C_NAV;
			if (layer == L_FN) { strcpy(label, "PgUp"); strcpy(sub, "-16"); }
			else strcpy(sub, "Row up");
			return;

		case SDL_SCANCODE_DOWN:
			*cat = C_NAV;
			if (layer == L_FN) { strcpy(label, "PgDn"); strcpy(sub, "+16"); }
			else strcpy(sub, "Row dn");
			return;

		case SDL_SCANCODE_LEFT:
			*cat = C_NAV;
			     if (layer == L_FN) { strcpy(label, "Home"); strcpy(sub, "Row 0"); }
			else if (layer == L_CTRL) strcpy(sub, "Ptn -");
			else strcpy(sub, "Left");
			return;

		case SDL_SCANCODE_RIGHT:
			*cat = C_NAV;
			     if (layer == L_FN) { strcpy(label, "End"); strcpy(sub, "Last"); }
			else if (layer == L_CTRL) strcpy(sub, "Ptn +");
			else strcpy(sub, "Right");
			return;

		case SDL_SCANCODE_NUMLOCKCLEAR: *cat = C_INSTR; strcpy(sub, "Blk 1"); return;
		case SDL_SCANCODE_KP_DIVIDE:    *cat = C_INSTR; strcpy(sub, "Blk 2"); return;
		case SDL_SCANCODE_KP_MULTIPLY:  *cat = C_INSTR; strcpy(sub, "Blk 3"); return;
		case SDL_SCANCODE_KP_MINUS:     *cat = C_INSTR; strcpy(sub, (layer == L_CTRL) ? "Vol -" : "Blk 4"); return;
		case SDL_SCANCODE_KP_9:         *cat = C_IDLE; return;
		case SDL_SCANCODE_KP_ENTER:     *cat = C_INSTR; strcpy(sub, "Bank"); return;
		case SDL_SCANCODE_KP_0:         *cat = C_INSTR; strcpy(sub, "No ins"); return;
		case SDL_SCANCODE_KP_PERIOD:    *cat = C_INSTR; strcpy(sub, "Clear"); return;

		default: break;
	}

	if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_8)
	{
		*cat = C_INSTR;
		sprintf(sub, "Ins %d", (sc - SDL_SCANCODE_KP_1) + 1);
		return;
	}

	// character keys: depends on the layer
	switch (layer)
	{
		case L_TEXT:
			*cat = (k->ch != 0) ? C_WHITE : C_IDLE;
			return;

		case L_FN:
			if (k->fnScancode >= SDL_SCANCODE_F1 && k->fnScancode <= SDL_SCANCODE_F12)
			{
				const int32_t f = (k->fnScancode - SDL_SCANCODE_F1) + 1;
				sprintf(label, "F%d", f);
				     if (f <= 7) { *cat = C_OCTAVE; sprintf(sub, "Oct %d", f - 1); }
				else if (f == 8) { *cat = C_OCTAVE; strcpy(sub, "Oct 6"); } // F8 also selects octave 6 in FT2
				else             { *cat = C_JUMP; sprintf(sub, "Jump %d", f - 8); }
				return;
			}
			break; // the rest of the keys are normal in the Fn layer

		case L_CTRL:
		{
			const char *s = tk_ctrlLetterLabel(sc);
			if (s != NULL)
			{
				*cat = C_SCREEN;
				strcpy(sub, s);
			}
			else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_4)
			{
				*cat = C_SCREEN;
				sprintf(sub, "Conf %d", (sc - SDL_SCANCODE_1) + 1);
			}
			return;
		}

		case L_HEX:
			if (tk_isDigitKey(sc) || (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_F))
			{
				*cat = C_HEX;
				sprintf(sub, "Hex %s", k->label);
			}
			return;

		case L_EFX:
			if (tk_isDigitKey(sc) || tk_isLetterKey(sc))
			{
				*cat = C_HEX;
				sprintf(sub, "Efx %s", k->label);
			}
			return;

		case L_VOL:
			if (tk_isVolColumnKey(sc))
			{
				*cat = C_HEX;
				strcpy(sub, "Vol");
			}
			return;

		default: break;
	}

	// normal layer: notes
	const int32_t n = scancodeKeyToNote(sc);
	if (n == NOTE_OFF)
	{
		*cat = C_OFF;
		strcpy(sub, "Key off");
	}
	else if (n > 0 && n <= 96)
	{
		static const bool sharp[12] = { 0,1,0,1,0,0,1,0,1,0,1,0 };
		*cat = sharp[(n-1) % 12] ? C_BLACK : C_WHITE;
		*note = n;
	}
	else if (n > 96)
	{
		*cat = C_WHITE; // note key, but out of range at this octave
	}
}

static void tk_drawMiniBar(void) // shown when the keyboard is hidden
{
	tk_fillRect(0, 0, TK_W, TK_MINI_H, COL_DESK);
	tk_fillRect(0, 0, TK_W, 1, COL_FW_LIGHT);
	tk_fillRect(0, 0, 1, TK_MINI_H, COL_FW_LIGHT);
	tk_fillRect(0, TK_MINI_H-1, TK_W, 1, COL_FW_DARK);
	tk_fillRect(TK_W-1, 0, 1, TK_MINI_H, COL_FW_DARK);

	char str[32];
	sprintf(str, "OCT %d", editor.curOctave);
	tk_tinyText(8, 8, str, 0x7D92AA);
	tk_tinyText(TK_W - 214, 8, "CTRL+K OR TAP HERE", 0x7D92AA);

	// "Keyboard" button
	const int32_t bw = 110, bx = TK_W - bw - 3, by = 2, bh = TK_MINI_H - 4;
	tk_drawButton(bx, by, bw, bh, catFill[C_MOD], false);
	tk_drawGlyphRows(bx + 8, by + 4, glyphUpSmall, 4, 0x000000);
	tk_bigText(bx + 22, by + 3, "Keyboard", 0x000000);
}

static void tk_drawKeyboard(void)
{
	if (!shown)
	{
		tk_drawMiniBar();
		return;
	}

	const int32_t layer = tk_getLayer();

	// panel (FT2 "framework")
	tk_fillRect(0, 0, TK_W, TK_H, COL_DESK);
	tk_fillRect(0, 0, TK_W, 1, COL_FW_LIGHT);
	tk_fillRect(0, 0, 1, TK_H, COL_FW_LIGHT);
	tk_fillRect(0, TK_H-1, TK_W, 1, COL_FW_DARK);
	tk_fillRect(TK_W-1, 0, 1, TK_H, COL_FW_DARK);

	char label[32], sub[32];
	for (int32_t i = 0; i < NUM_KEYS; i++)
	{
		const tkKey_t *k = &keys[i];
		int32_t cat, note;
		tk_getKeyLook(i, layer, label, sub, &cat, &note);

		const int32_t x1 = 2 + (int32_t)(k->x * KEY_U + 0.5f);
		const int32_t x2 = 2 + (int32_t)((k->x + k->w) * KEY_U + 0.5f);
		const int32_t w = (x2 - x1) - 2;
		const int32_t y = 2 + (k->row * ROW_PITCH);
		const int32_t h = KEY_H + (ROW_PITCH * (k->rows - 1));

		const bool pressed = tk_isKeyHeld(i);
		const uint32_t fill = catFill[cat];
		const uint32_t text = catText[cat];
		const uint32_t subCol = tk_mix(text, fill, 38);
		const int32_t o = pressed ? 1 : 0;

		tk_drawButton(x1, y, w, h, fill, pressed);
		tk_bigText(x1 + 4 + o, y + 3 + o, label, text);

		if (note > 0)
			tk_noteText(x1 + 4 + o, y + 17 + o, note, subCol);
		else if (sub[0] != '\0')
			tk_tinyText(x1 + 4 + o, y + 17 + o, sub, subCol);
	}
}

static uint64_t tk_getStateHash(void)
{
	uint64_t h = 1469598103934665603ULL;
	#define HASH(v) h = (h ^ (uint64_t)(v)) * 1099511628211ULL

	HASH(shown);
	HASH(tk_getLayer());
	HASH(shiftOn); HASH(ctrlOn); HASH(altOn); HASH(fnOn); HASH(kpPlusOn);
	HASH(editor.curOctave);
	for (int32_t i = 0; i < NUM_KEYS; i++)
		HASH(heldCount[i]);

	#undef HASH
	return h | 1; // never 0 (0 = "force redraw")
}

void tkRender(SDL_Renderer *renderer)
{
	if (kbTexture == NULL || kbBuffer == NULL)
		return;

	const uint64_t hash = tk_getStateHash();
	if (hash != lastStateHash)
	{
		lastStateHash = hash;
		tk_drawKeyboard();
		SDL_UpdateTexture(kbTexture, NULL, kbBuffer, TK_W * sizeof (uint32_t));
	}

	SDL_Rect src = { 0, 0, TK_W, tkGetAreaHeight() };
	SDL_RenderCopy(renderer, kbTexture, &src, &kbPixRect);
}

// ------------------------------------------------------------------------------------------------
// input

void tkApplyModifiers(void)
{
	if (!shown)
		return;

	if (shiftOn) keyb.leftShiftPressed = true;
	if (ctrlOn)  keyb.leftCtrlPressed = true;
	if (altOn)   keyb.leftAltPressed = true;

	if (shiftOn || ctrlOn || altOn)
		keyb.keyModifierDown = true;
}

static int32_t tk_keyAt(int32_t winX, int32_t winY)
{
	if (kbWinRect.w <= 0 || kbWinRect.h <= 0)
		return -1;

	const int32_t x = (int32_t)(((int64_t)(winX - kbWinRect.x) * TK_W) / kbWinRect.w);
	const int32_t y = (int32_t)(((int64_t)(winY - kbWinRect.y) * TK_H) / kbWinRect.h);
	if (x < 0 || x >= TK_W || y < 0 || y >= TK_H || !shown)
		return -1;

	// find the nearest key (so the 2px gaps between keys also count)
	int32_t best = -1, bestDist = 1 << 30;
	for (int32_t i = 0; i < NUM_KEYS; i++)
	{
		const tkKey_t *k = &keys[i];
		const int32_t x1 = 2 + (int32_t)(k->x * KEY_U + 0.5f);
		const int32_t x2 = 2 + (int32_t)((k->x + k->w) * KEY_U + 0.5f);
		const int32_t y1 = 2 + (k->row * ROW_PITCH);
		const int32_t y2 = y1 + KEY_H + (ROW_PITCH * (k->rows - 1));

		const int32_t dx = (x < x1) ? (x1 - x) : ((x >= x2) ? (x - x2 + 1) : 0);
		const int32_t dy = (y < y1) ? (y1 - y) : ((y >= y2) ? (y - y2 + 1) : 0);
		const int32_t d = (dx * dx) + (dy * dy);
		if (d < bestDist)
		{
			bestDist = d;
			best = i;
		}
	}

	return (bestDist <= 9) ? best : -1;
}

static bool tk_pointInKeyboard(int32_t winX, int32_t winY)
{
	return winX >= kbWinRect.x && winX < kbWinRect.x+kbWinRect.w &&
	       winY >= kbWinRect.y && winY < kbWinRect.y+kbWinRect.h;
}

// While a dialog box (system request) is open, it reads SDL events itself, so we post real events
static void tk_pushKeyEvent(SDL_Scancode sc, bool down)
{
	SDL_Event e;
	memset(&e, 0, sizeof (e));
	e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	e.key.windowID = SDL_GetWindowID(video.window);
	e.key.keysym.scancode = sc;
	e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
	e.key.keysym.mod = (Uint16)((shiftOn ? KMOD_LSHIFT : 0) | (ctrlOn ? KMOD_LCTRL : 0) | (altOn ? KMOD_LALT : 0));
	SDL_PushEvent(&e);
}

static void tk_pushTextEvent(char cp850Char)
{
	char tmp[2] = { cp850Char, '\0' };
	char *utf8 = cp850ToUtf8(tmp);
	if (utf8 == NULL)
		return;

	SDL_Event e;
	memset(&e, 0, sizeof (e));
	e.type = SDL_TEXTINPUT;
	e.text.windowID = SDL_GetWindowID(video.window);
	strncpy(e.text.text, utf8, sizeof (e.text.text) - 1);
	free(utf8);
	SDL_PushEvent(&e);
}

static void tk_sendKeyDown(SDL_Scancode sc)
{
	if (ui.sysReqShown)
	{
		tk_pushKeyEvent(sc, true);
		return;
	}

	readKeyModifiers(); // includes our sticky modifiers (tkApplyModifiers)
	keyDownHandler(sc, SDL_GetKeyFromScancode(sc), false);
}

static void tk_sendKeyUp(SDL_Scancode sc)
{
	if (ui.sysReqShown)
	{
		tk_pushKeyEvent(sc, false);
		return;
	}

	readKeyModifiers();
	keyUpHandler(sc, SDL_GetKeyFromScancode(sc));
}

static void tk_releaseStickyModifiers(void)
{
	// one-shot modifiers are released after a normal key has been used (unless a finger still holds them)
	for (int32_t i = 0; i < NUM_KEYS; i++)
	{
		if (heldCount[i] > 0)
		{
			switch (keys[i].type)
			{
				case K_SHIFT: case K_CTRL: case K_ALT: case K_FN: case K_KPPLUS:
					return; // a modifier is physically held - keep them all
				default: break;
			}
		}
	}

	shiftOn = ctrlOn = altOn = fnOn = false;

	if (kpPlusOn)
	{
		kpPlusOn = false;
		tk_sendKeyUp(SDL_SCANCODE_KP_PLUS);
	}
}

static void tk_pressKey(int32_t pointerSlot, int32_t keyIndex)
{
	const tkKey_t *k = &keys[keyIndex];
	pointer_t *p = &pointers[pointerSlot];

	p->key = (int16_t)keyIndex;
	p->sentScancode = SDL_SCANCODE_UNKNOWN;
	heldCount[keyIndex]++;

	switch (k->type)
	{
		case K_SHIFT: shiftOn ^= 1; return;
		case K_CTRL:  ctrlOn  ^= 1; return;
		case K_ALT:   altOn   ^= 1; return;
		case K_FN:    fnOn    ^= 1; return;

		case K_KPPLUS:
		{
			if (ctrlOn)
			{
				// Ctrl + numpad '+' = master volume up (normal key)
				tk_sendKeyDown(SDL_SCANCODE_KP_PLUS);
				p->sentScancode = SDL_SCANCODE_KP_PLUS;
				return;
			}

			kpPlusOn ^= 1;
			if (kpPlusOn)
				tk_sendKeyDown(SDL_SCANCODE_KP_PLUS);
			else
				tk_sendKeyUp(SDL_SCANCODE_KP_PLUS);
		}
		return;

		case K_OCT_DN:
			if (editor.curOctave > 0)
				editor.curOctave--;
			return;

		case K_OCT_UP:
			if (editor.curOctave < 6)
				editor.curOctave++;
			return;

		case K_HIDE:
			heldCount[keyIndex] = 0;
			p->key = -1;
			tkToggle();
			return;

		default: break;
	}

	// typing text (song name, instrument names, disk op. filenames...)
	if (editor.editTextFlag && k->ch != 0)
	{
		const char c = shiftOn ? k->chShift : k->ch;
		if (ui.sysReqShown)
			tk_pushTextEvent(c); // dialog box with a text field (e.g. "Make dir.")
		else
			handleTextEditInputChar(c);
		return;
	}

	SDL_Scancode sc = k->scancode;
	if (fnOn && k->fnScancode != 0)
		sc = k->fnScancode;

	// while typing text, the right Shift key works as Enter (the keyboard has no other Enter key)
	if ((editor.editTextFlag || ui.sysReqShown) && sc == SDL_SCANCODE_RSHIFT)
		sc = SDL_SCANCODE_RETURN;

	// Ctrl+K on the on-screen keyboard hides it
	if (ctrlOn && sc == SDL_SCANCODE_K)
	{
		ctrlOn = false;
		tkToggle();
		return;
	}

	tk_sendKeyDown(sc);
	p->sentScancode = sc;
}

static void tk_releaseKey(int32_t pointerSlot)
{
	pointer_t *p = &pointers[pointerSlot];
	const int32_t keyIndex = p->key;

	if (keyIndex >= 0 && keyIndex < NUM_KEYS && heldCount[keyIndex] > 0)
		heldCount[keyIndex]--;

	if (p->sentScancode != SDL_SCANCODE_UNKNOWN)
		tk_sendKeyUp(p->sentScancode);

	if (keyIndex >= 0 && keyIndex < NUM_KEYS)
	{
		switch (keys[keyIndex].type)
		{
			case K_SHIFT: case K_CTRL: case K_ALT: case K_FN: case K_KPPLUS:
				break; // modifiers stay on

			default:
				tk_releaseStickyModifiers();
				break;
		}
	}

	p->active = false;
	p->key = -1;
}

static int32_t tk_findPointer(int64_t id)
{
	for (int32_t i = 0; i < MAX_POINTERS; i++)
	{
		if (pointers[i].active && pointers[i].id == id)
			return i;
	}
	return -1;
}

static void tk_pointerDown(int64_t id, int32_t winX, int32_t winY)
{
	if (!shown)
	{
		tkToggle(); // tapping the thin bar brings the keyboard back
		return;
	}

	const int32_t key = tk_keyAt(winX, winY);
	if (key < 0)
		return;

	// a "down" for a pointer we think is still down: its "up" was lost (e.g. eaten by a dialog box)
	const int32_t old = tk_findPointer(id);
	if (old >= 0)
		tk_releaseKey(old);

	for (int32_t i = 0; i < MAX_POINTERS; i++)
	{
		if (!pointers[i].active)
		{
			pointers[i].active = true;
			pointers[i].id = id;
			tk_pressKey(i, key);
			return;
		}
	}
}

static bool tk_pointerUp(int64_t id)
{
	const int32_t slot = tk_findPointer(id);
	if (slot < 0)
		return false;

	tk_releaseKey(slot);
	return true;
}

bool tkHandleEvent(const SDL_Event *event)
{
	switch (event->type)
	{
		case SDL_MOUSEMOTION:
		{
			if (event->motion.which == SDL_TOUCH_MOUSEID)
				return false;

			const bool over = tk_pointInKeyboard(event->motion.x, event->motion.y);
			if (over != mouseOverKb)
			{
				mouseOverKb = over;

				// show the normal OS mouse pointer over the keyboard (the FT2 pointer lives inside the FT2 screen)
				if (!(config.specialFlags2 & HARDWARE_MOUSE))
				{
					SDL_ShowCursor(over ? SDL_TRUE : SDL_FALSE);
				}
			}
		}
		return false;

		case SDL_MOUSEBUTTONDOWN:
		{
			if (!tk_pointInKeyboard(event->button.x, event->button.y))
				return false;

			if (event->button.which != SDL_TOUCH_MOUSEID && event->button.button == SDL_BUTTON_LEFT)
				tk_pointerDown(MOUSE_POINTER_ID, event->button.x, event->button.y);

			return true; // never let FT2 see clicks on the keyboard
		}

		case SDL_MOUSEBUTTONUP:
		{
			if (event->button.which != SDL_TOUCH_MOUSEID && event->button.button == SDL_BUTTON_LEFT)
			{
				if (tk_pointerUp(MOUSE_POINTER_ID))
					return true;
			}

			// a touch on the keyboard also makes an emulated mouse click; swallow it
			if (event->button.which == SDL_TOUCH_MOUSEID && tk_pointInKeyboard(event->button.x, event->button.y))
				return true;
		}
		return false;

		case SDL_FINGERDOWN:
		{
			int32_t winW, winH;
			SDL_GetWindowSize(video.window, &winW, &winH);
			const int32_t x = (int32_t)(event->tfinger.x * winW);
			const int32_t y = (int32_t)(event->tfinger.y * winH);

			if (!tk_pointInKeyboard(x, y))
				return false;

			tk_pointerDown((int64_t)event->tfinger.fingerId, x, y);
			return true;
		}

		case SDL_FINGERUP:
			return tk_pointerUp((int64_t)event->tfinger.fingerId);

		case SDL_WINDOWEVENT:
		{
			// release everything if we lose focus (a key could get stuck otherwise)
			if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
			{
				for (int32_t i = 0; i < MAX_POINTERS; i++)
				{
					if (pointers[i].active)
						tk_releaseKey(i);
				}
			}
		}
		return false;

		default: break;
	}

	return false;
}
