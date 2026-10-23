/* BerntTracker: "Find" as one of FT2's own screens (see bt2_find.h). Laid out like FT2's Help screen: the choices
** on the left, the hits in a black list box with a scroll bar on the right, the search box above it.
*/

#if defined __EMSCRIPTEN__ || defined _WIN32

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft2_header.h"
#include "ft2_gui.h"
#include "ft2_structs.h"
#include "ft2_diskop.h"
#include "ft2_mouse.h"
#include "ft2_keyboard.h"
#include "ft2_textboxes.h"
#include "ft2_unicode.h"
#include "ft2_pattern_ed.h"
#include "ft2_video.h"
#include "bt2_diskop.h"
#include "bt2_search.h"
#include "bt2_modland.h"
#include "bt2_fs.h"
#include "bt2_find.h"

// the list box (inside its frame), one hit per row like Disk Op.'s list
#define LIST_X 131
#define LIST_Y 19
#define LIST_W 477
#define LIST_H 138
#define ROW_H 11
#define ROWS 12
#define NAME_X 134
#define WHERE_X 336
#define SIZE_RIGHT 604
#define STATUS_Y 161

#define QUERY_LEN 100

static char query[QUERY_LEN + 1]; // the search box (FT2's letters: code page 850, like its other text boxes)
static char searched[QUERY_LEN + 1];
static bool searchedOnce, modTried;
static int32_t where = BT2_WHERE_ALL, what = BT2_WHAT_ALL;
static bt2SearchPool ownPool, modPool;
static bool modReady, toldShown;
static bt2SearchItem **results;
static int32_t numResults, top, sel = -1;
static uint32_t lastClickTicks;
static int32_t lastClickRow = -1;

// (the web page's text box: what is typed there, and its keys, are used in the main loop)
static char pendingQuery[QUERY_LEN * 4 + 1];
static bool havePendingQuery;
static struct { SDL_Keycode key; bool shift; } pendingKeys[32];
static int32_t numPendingKeys;
static char queryUtf8[QUERY_LEN * 4 + 1];

static char *homeUtf8(void) // the BT2 folder (malloc'ed)
{
#ifdef _WIN32
	return bt2Utf8(bt2HomeRootU());
#else
	return strdup(bt2HomeRootU());
#endif
}

// the web page: the Find screen came or went (it gives the screen a text box for the iPad's keyboard)
static void tellShown(bool shown)
{
	if (shown != toldShown)
	{
		toldShown = shown;
		bt2FindShownChanged(shown);
	}
}

// ------------------------------------------------------------------------------------------------
// drawing

static void textClip(uint16_t x, uint16_t y, uint8_t pal, const char *utf8, uint16_t clipX)
{
	char *s = utf8ToCp850((char *)utf8, true); // (FT2's font: code page 850, with æøå)
	if (s == NULL)
		return;

	// (a copy with room for ".."; far more letters than a row can show)
	char text[300];
	snprintf(text, 257, "%s", s);
	free(s);

	// too long: cut, with ".." at the end (like Disk Op.)
	if (x + textWidth(text) > clipX)
	{
		size_t n = strlen(text);
		while (n > 0 && x + textWidth(text) + textWidth("..") > clipX)
			text[--n] = '\0';
		if (n + 3 <= sizeof (text))
			strcat(text, "..");
	}

	textOutClipX(x, y, pal, text, clipX);
}

static bool hasWord(const char *s) // (only spaces = nothing typed)
{
	for (; *s != '\0'; s++)
	{
		if (*s != ' ')
			return true;
	}

	return false;
}

static void sizeText(const bt2SearchItem *it, char *out, size_t outLen)
{
	out[0] = '\0';
	if (it->kind == BT2_KIND_FOLDER)
	{
		if (it->modland && it->count > 0)
			snprintf(out, outLen, "%d %s", (int)it->count, (it->count == 1) ? "song" : "songs");
		return;
	}

	const int64_t s = it->size;
	if (s < 0)
		return;
	else if (s < 1000)
		snprintf(out, outLen, "%d", (int)s);
	else if (s < 1000 * 1000)
		snprintf(out, outLen, "%dk", (int)((s + 999) / 1000));
	else
		snprintf(out, outLen, "%dM", (int)((s + 999999) / 1000000));
}

static void drawStatus(void)
{
	fillRect(131, STATUS_Y - 1, 478, 11, PAL_DESKTOP);

	char msg[160];
	if (!modReady && modTried && where != BT2_WHERE_OWN)
		snprintf(msg, sizeof (msg), "%d %s  (Modland's list isn't here)", (int)numResults, (numResults == 1) ? "hit" : "hits");
	else if (where != BT2_WHERE_OWN && !modReady && !hasWord(query))
		snprintf(msg, sizeof (msg), "%d %s  (type to search in Modland too)", (int)numResults, (numResults == 1) ? "hit" : "hits");
	else
		snprintf(msg, sizeof (msg), "%d %s   Enter: open   Shift+Enter: show folder   Esc: exit", (int)numResults, (numResults == 1) ? "hit" : "hits");

	textOutShadow(133, STATUS_Y, PAL_FORGRND, PAL_DSKTOP2, msg);
}

static void drawList(void)
{
	clearRect(LIST_X, LIST_Y, LIST_W, LIST_H);

	for (int32_t r = 0; r < ROWS; r++)
	{
		const int32_t i = top + r;
		if (i >= numResults)
			break;

		const bt2SearchItem *it = results[i];
		const uint16_t y = LIST_Y + 1 + (uint16_t)(r * ROW_H);

		if (i == sel)
			fillRect(LIST_X, y - 1, LIST_W, ROW_H, PAL_BOXSLCT); // (like FT2's list of audio devices)

		// the name (a folder with "\" in front, like Disk Op.)
		uint16_t x = NAME_X;
		if (it->kind == BT2_KIND_FOLDER)
		{
			charOut(x, y, PAL_BLCKTXT, DIR_DELIMITER);
			x += FONT1_CHAR_W;
		}
		textClip(x, y, PAL_BLCKTXT, it->name, WHERE_X - 6);

		// where it is
		char whereText[2200];
		snprintf(whereText, sizeof (whereText), "%s%s%.*s", it->modland ? "Modland" : "BT2", (it->dirLen > 0) ? "/" : "",
			(int)it->dirLen, it->rel);
		textClip(WHERE_X, y, (i == sel) ? PAL_BLCKTXT : PAL_PATTEXT, whereText, SIZE_RIGHT - 52);

		char size[32];
		sizeText(it, size, sizeof (size));
		if (size[0] != '\0')
			textOut(SIZE_RIGHT - textWidth(size), y, PAL_BLCKTXT, size);
	}

	if (numResults == 0)
	{
		const char *t = hasWord(query) ? "No hits." : "Type what you are looking for: artist, song or folder.";
		textOut(NAME_X, LIST_Y + 1, PAL_PATTEXT, t);
	}

	setScrollBarEnd(SB_FIND_LIST, (uint32_t)((numResults > 0) ? numResults : 1));
	setScrollBarPos(SB_FIND_LIST, (uint32_t)top, DONT_TRIGGER_CALLBACK);
	drawStatus();
}

static void drawFindScreen(void)
{
	drawFramework(0,     0, 128, 173, FRAMEWORK_TYPE1);
	drawFramework(128,   0, 504, 173, FRAMEWORK_TYPE1);
	drawFramework(164,   2, 466,  14, FRAMEWORK_TYPE2); // (the search box)
	drawFramework(130,  18, 479, 140, FRAMEWORK_TYPE2); // (the list)

	textOutShadow(4,     3, PAL_FORGRND, PAL_DSKTOP2, "Where:");
	textOutShadow(21,   17, PAL_FORGRND, PAL_DSKTOP2, "All");
	textOutShadow(21,   31, PAL_FORGRND, PAL_DSKTOP2, "My files");
	textOutShadow(21,   45, PAL_FORGRND, PAL_DSKTOP2, "Modland");
	textOutShadow(4,    59, PAL_FORGRND, PAL_DSKTOP2, "What:");
	textOutShadow(21,   73, PAL_FORGRND, PAL_DSKTOP2, "All");
	textOutShadow(21,   87, PAL_FORGRND, PAL_DSKTOP2, "Songs");
	textOutShadow(21,  101, PAL_FORGRND, PAL_DSKTOP2, "Folders");
	textOutShadow(21,  115, PAL_FORGRND, PAL_DSKTOP2, "Instr./Smp.");
	textOutShadow(133,   4, PAL_FORGRND, PAL_DSKTOP2, "Find:");

	showPushButton(PB_FIND_OPEN);
	showPushButton(PB_FIND_FOLDER);
	showPushButton(PB_FIND_CLEAR);
	showPushButton(PB_FIND_EXIT);
	showPushButton(PB_FIND_LIST_UP);
	showPushButton(PB_FIND_LIST_DOWN);

	uncheckRadioButtonGroup(RB_GROUP_FIND_WHERE);
	radioButtons[RB_FIND_WHERE_ALL + where].state = RADIOBUTTON_CHECKED;
	showRadioButtonGroup(RB_GROUP_FIND_WHERE);

	uncheckRadioButtonGroup(RB_GROUP_FIND_WHAT);
	radioButtons[RB_FIND_WHAT_ALL + what].state = RADIOBUTTON_CHECKED;
	showRadioButtonGroup(RB_GROUP_FIND_WHAT);

	showScrollBar(SB_FIND_LIST);

	textBoxes[TB_FIND_QUERY].textPtr = query;
	showTextBox(TB_FIND_QUERY);
	drawTextBox(TB_FIND_QUERY);

	drawList();
}

// ------------------------------------------------------------------------------------------------
// searching

static void search(void)
{
	// Modland's songs: its list is fetched the first time it is needed (the download box shows how far it has come)
	if (!modReady && !modTried && where != BT2_WHERE_OWN && hasWord(query))
	{
		modTried = true;
		if (bt2ModlandIsLoaded() || bt2ModlandOpen())
		{
			bt2SearchBuildModland(&modPool);
			modReady = true;
		}
	}

	char *q = cp850ToUtf8(query);
	free(results);
	results = NULL;
	numResults = bt2SearchRun((q != NULL) ? q : "", where, what, &ownPool, modReady ? &modPool : NULL, &results);
	free(q);

	snprintf(searched, sizeof (searched), "%s", query);
	searchedOnce = true;
	top = 0;
	sel = (numResults > 0) ? 0 : -1;

	if (ui.bt2FindShown)
		drawList();
}

static void editQuery(void) // typing always goes to the search box while the screen is shown
{
	if (!ui.bt2FindShown || ui.sysReqShown || editor.editTextFlag)
		return;

	textBox_t *t = &textBoxes[TB_FIND_QUERY];
	mouse.lastEditBox = TB_FIND_QUERY;
	setTextCursorToEnd(t);
	editor.editTextFlag = true;
	editor.textCursorBlinkCounter = 0;
	SDL_StartTextInput();
}

// ------------------------------------------------------------------------------------------------
// doing what was chosen

static void chdirUtf8(const char *path, bool *ok)
{
#ifdef _WIN32
	wchar_t *w = bt2Wide(path);
	*ok = (w != NULL) && _wchdir(w) == 0;
	free(w);
#else
	*ok = chdir(path) == 0;
#endif
}

static void showFolder(const char *folder, int32_t item)
{
	hideFindScreen();
	showDiskOpScreen(); // (Disk Op. in the top left, FT2's own screens on the right)

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

	bool ok;
	chdirUtf8(folder, &ok);
	if (!ok)
		bt2DiskOpToast("Fant ikke mappa");

	editor.diskOpReadDir = true;
}

static void openFile(const char *file) // like a dropped file (Modland's songs are downloaded first)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof (ev));
	ev.type = SDL_DROPFILE;
	ev.drop.file = SDL_strdup(file);
	ev.drop.windowID = (video.window != NULL) ? SDL_GetWindowID(video.window) : 0;
	if (ev.drop.file != NULL && SDL_PushEvent(&ev) <= 0)
		SDL_free(ev.drop.file);
}

static void act(bool showFolderToo)
{
	if (sel < 0 || sel >= numResults)
		return;

	const bt2SearchItem *it = results[sel];

	// where it leads (bt2SearchTarget)
	char *home = homeUtf8();
	char folder[2048], file[2600];
	int32_t item = BT2_ITEM_MODULE;
	const bool found = (home != NULL) && bt2SearchTarget(it, what, home, folder, sizeof (folder), file, sizeof (file), &item);
	free(home);

	if (!found)
	{
		bt2DiskOpToast("Fant ikke mappa");
		return;
	}

	if (it->kind == BT2_KIND_FOLDER || showFolderToo)
	{
		showFolder(folder, (item == BT2_ITEM_INSTR) ? DISKOP_ITEM_INSTR : (item == BT2_ITEM_SAMPLE) ? DISKOP_ITEM_SAMPLE : DISKOP_ITEM_MODULE);
		return;
	}

	// back to what was there (FT2's play buttons and keys), then the song is opened. Find remembers the hit, so the
	// next song is Ctrl+F, Down, Enter away.
	exitFindScreen();
	openFile(file);
}

// ------------------------------------------------------------------------------------------------
// the screen

void showFindScreen(void)
{
	if (ui.extendedPatternEditor)
		exitPatternEditorExtended();

	hideTopScreen();
	ui.bt2FindShown = true;
	tellShown(true); // (first: finding may have to fetch Modland's list)

	// the hit chosen last time (chosen again below, at the same place in the list, if it is still found)
	char *lastRel = NULL;
	bool lastModland = false;
	const int32_t lastRow = sel - top;
	if (sel >= 0 && sel < numResults)
	{
		lastRel = strdup(results[sel]->rel);
		lastModland = results[sel]->modland;
	}

	// the user's own files as they are now (the hits point into the old list: they go first)
	free(results);
	results = NULL;
	numResults = 0;
	top = 0;
	sel = -1;

	char *home = homeUtf8();
	if (home != NULL)
		bt2SearchBuildOwn(&ownPool, home);
	free(home);
	modTried = false;

	drawFindScreen();
	search();

	if (lastRel != NULL)
	{
		for (int32_t i = 0; i < numResults; i++)
		{
			if (results[i]->modland == lastModland && strcmp(results[i]->rel, lastRel) == 0)
			{
				sel = i;
				top = sel - lastRow;
				if (top > numResults - ROWS) top = numResults - ROWS;
				if (top > sel) top = sel;
				if (top < 0) top = 0;
				if (top < sel - (ROWS - 1)) top = sel - (ROWS - 1);
				drawList();
				break;
			}
		}
		free(lastRel);
	}

	editQuery();
}

void redrawFindScreen(void) // (FT2 draws its top screens again, e.g. after the extended pattern editor or a loaded song)
{
	if (!ui.bt2FindShown)
		return;

	hideTopLeftMainScreen(); // (nothing of FT2's main screen stays clickable under Find)
	hideTopRightMainScreen();

	tellShown(true);
	drawFindScreen();
	editQuery();
}

void hideFindScreen(void)
{
	if (editor.editTextFlag && mouse.lastEditBox == TB_FIND_QUERY)
		exitTextEditing();

	hidePushButton(PB_FIND_OPEN);
	hidePushButton(PB_FIND_FOLDER);
	hidePushButton(PB_FIND_CLEAR);
	hidePushButton(PB_FIND_EXIT);
	hidePushButton(PB_FIND_LIST_UP);
	hidePushButton(PB_FIND_LIST_DOWN);
	hideRadioButtonGroup(RB_GROUP_FIND_WHERE);
	hideRadioButtonGroup(RB_GROUP_FIND_WHAT);
	hideScrollBar(SB_FIND_LIST);
	hideTextBox(TB_FIND_QUERY);

	ui.bt2FindShown = false;
	numPendingKeys = 0;
	havePendingQuery = false;
	tellShown(false);
}

void exitFindScreen(void)
{
	hideFindScreen();
	showTopScreen(RESTORE_SCREENS); // (back to what was there, usually Disk Op.)
}

static bool handleKey(SDL_Keycode key, bool shift);

void findScreenFrame(void)
{
	if (!ui.bt2FindShown)
		return;

	// the web page's text box
	if (havePendingQuery)
	{
		havePendingQuery = false;
		char *c = utf8ToCp850(pendingQuery, true);
		snprintf(query, sizeof (query), "%s", (c != NULL) ? c : "");
		free(c);

		textBox_t *t = &textBoxes[TB_FIND_QUERY];
		if (editor.editTextFlag && mouse.lastEditBox == TB_FIND_QUERY)
			setTextCursorToEnd(t);
		else
			drawTextBox(TB_FIND_QUERY);
	}

	if (!searchedOnce || strcmp(query, searched) != 0)
		search(); // (filters as you type)

	for (int32_t i = 0; i < numPendingKeys && ui.bt2FindShown; i++)
		handleKey(pendingKeys[i].key, pendingKeys[i].shift);
	numPendingKeys = 0;

	editQuery();
}

void findScreenSetQuery(const char *utf8)
{
	snprintf(pendingQuery, sizeof (pendingQuery), "%s", utf8);
	havePendingQuery = true;
}

void findScreenQueueKey(SDL_Keycode key, bool shift)
{
	if (numPendingKeys < (int32_t)(sizeof (pendingKeys) / sizeof (pendingKeys[0])))
	{
		pendingKeys[numPendingKeys].key = key;
		pendingKeys[numPendingKeys].shift = shift;
		numPendingKeys++;
	}
}

const char *findScreenQuery(void)
{
	char *u = cp850ToUtf8(query);
	snprintf(queryUtf8, sizeof (queryUtf8), "%s", (u != NULL) ? u : "");
	free(u);
	return queryUtf8;
}

void bt2SearchOpen(void) // "Find" (Disk Op.) and Ctrl+F
{
	if (!ui.sysReqShown && !ui.bt2FindShown && editor.mainLoopOngoing && !editor.NI_Play && !editor.busy)
		showFindScreen();
}

// ------------------------------------------------------------------------------------------------
// choosing

static void choose(int32_t i)
{
	if (numResults == 0)
		return;

	if (i < 0) i = 0;
	if (i > numResults - 1) i = numResults - 1;
	sel = i;

	// (scrolled so the hit is seen)
	if (sel < top)
		top = sel;
	else if (sel >= top + ROWS)
		top = sel - ROWS + 1;

	drawList();
}

bool findScreenKeyDown(SDL_Keycode key)
{
	editQuery(); // (a key right after a click outside the search box: it goes to the search box all the same)
	return handleKey(key, keyb.leftShiftPressed);
}

static bool handleKey(SDL_Keycode key, bool shift)
{
	if (!ui.bt2FindShown || ui.sysReqShown)
		return false;

	if (!searchedOnce || strcmp(query, searched) != 0)
		search(); // (letters typed in the same moment as Enter or an arrow: the hits for what is there now)

	switch (key)
	{
		case SDLK_UP:       choose(sel - 1); return true;
		case SDLK_DOWN:     choose(sel + 1); return true;
		case SDLK_PAGEUP:   choose(sel - ROWS); return true;
		case SDLK_PAGEDOWN: choose(sel + ROWS); return true;

		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			if (keyb.leftAltPressed)
				return false; // (Alt+Enter: fullscreen, as in FT2's other text boxes)

			act(shift);
			return true;

		case SDLK_ESCAPE:
			exitFindScreen();
			return true;

		case SDLK_HOME:
		case SDLK_END:
			if (query[0] == '\0') // (with text: the start/end of the text)
			{
				choose((key == SDLK_HOME) ? 0 : numResults - 1);
				return true;
			}
			return false;

		default: return false;
	}
}

bool findScreenMouseDown(void)
{
	if (!ui.bt2FindShown || !mouse.leftButtonPressed)
		return false;

	if (mouse.x < LIST_X || mouse.x >= LIST_X + LIST_W || mouse.y < LIST_Y || mouse.y >= LIST_Y + ROWS * ROW_H)
		return false;

	const int32_t row = (mouse.y - LIST_Y) / ROW_H;
	const int32_t i = top + row;
	if (i >= numResults)
		return true;

	// a second click on the chosen hit (or a double-click) opens it
	const uint32_t now = SDL_GetTicks();
	const bool again = (i == sel) && (row == lastClickRow) && (now - lastClickTicks < 600);
	lastClickTicks = now;
	lastClickRow = row;

	choose(i);
	if (again)
	{
		lastClickRow = -1;
		act(false);
	}

	return true;
}

void findScreenWheel(bool up)
{
	if (!ui.bt2FindShown)
		return;

	if (up)
		scrollBarScrollUp(SB_FIND_LIST, 3);
	else
		scrollBarScrollDown(SB_FIND_LIST, 3);
}

#ifdef __EMSCRIPTEN__
// ------------------------------------------------------------------------------------------------
// for the automatic tests: what the screen shows (JSON; the first hits)

static size_t jsonText(char *out, size_t outLen, size_t pos, const char *s, size_t n)
{
	if (pos + 1 < outLen)
		out[pos++] = '"';

	for (size_t i = 0; i < n && s[i] != '\0' && pos + 8 < outLen; i++)
	{
		const unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\')
		{
			out[pos++] = '\\';
			out[pos++] = (char)c;
		}
		else if (c < 0x20)
		{
			pos += (size_t)snprintf(out + pos, outLen - pos, "\\u%04x", c);
		}
		else
		{
			out[pos++] = (char)c;
		}
	}

	if (pos + 1 < outLen)
		out[pos++] = '"';
	out[(pos < outLen) ? pos : outLen - 1] = '\0';
	return pos;
}

const char *findScreenStateJson(void)
{
	static char buf[64 * 1024];
	const size_t len = sizeof (buf);
	size_t pos = (size_t)snprintf(buf, len, "{\"shown\":%d,\"query\":", ui.bt2FindShown ? 1 : 0);
	const char *q = findScreenQuery();
	pos = jsonText(buf, len, pos, q, strlen(q));
	pos += (size_t)snprintf(buf + pos, len - pos, ",\"n\":%d,\"sel\":%d,\"top\":%d,\"where\":%d,\"what\":%d,\"modReady\":%d,"
		"\"editing\":%d,\"hits\":[", (int)numResults, (int)sel, (int)top, (int)where, (int)what, modReady ? 1 : 0,
		(editor.editTextFlag && mouse.lastEditBox == TB_FIND_QUERY) ? 1 : 0);

	for (int32_t i = 0; i < numResults && i < 40 && pos + 600 < len; i++)
	{
		const bt2SearchItem *it = results[i];
		if (i > 0)
			buf[pos++] = ',';
		pos += (size_t)snprintf(buf + pos, len - pos, "{\"kind\":%d,\"modland\":%d,\"count\":%d,\"rel\":", (int)it->kind,
			it->modland ? 1 : 0, (int)it->count);
		pos = jsonText(buf, len, pos, it->rel, 400);
		if (pos + 3 < len)
		{
			buf[pos++] = '}';
			buf[pos] = '\0';
		}
	}

	if (pos + 3 < len)
	{
		buf[pos++] = ']';
		buf[pos++] = '}';
		buf[pos] = '\0';
	}

	return buf;
}
#endif

// ------------------------------------------------------------------------------------------------
// FT2's widgets

void pbFindOpen(void)   { act(false); }
void pbFindFolder(void) { act(true); }

void pbFindClear(void)
{
	query[0] = '\0';
	textBoxes[TB_FIND_QUERY].cursorPos = 0;
	textBoxes[TB_FIND_QUERY].bufOffset = 0;
	drawTextBox(TB_FIND_QUERY);
	bt2FindCleared(); // (the web page's text box too)
}

void pbFindListUp(void)   { scrollBarScrollUp(SB_FIND_LIST, 1); }
void pbFindListDown(void) { scrollBarScrollDown(SB_FIND_LIST, 1); }

void sbFindListPos(uint32_t pos)
{
	if ((int32_t)pos != top)
	{
		top = (int32_t)pos;
		drawList();
	}
}

static void setWhere(int32_t w, uint16_t rb)
{
	where = w;
	checkRadioButton(rb);
	modTried = false;
	search();
}

static void setWhat(int32_t w, uint16_t rb)
{
	what = w;
	checkRadioButton(rb);
	search();
}

void rbFindWhereAll(void)     { setWhere(BT2_WHERE_ALL, RB_FIND_WHERE_ALL); }
void rbFindWhereOwn(void)     { setWhere(BT2_WHERE_OWN, RB_FIND_WHERE_OWN); }
void rbFindWhereModland(void) { setWhere(BT2_WHERE_MODLAND, RB_FIND_WHERE_MODLAND); }
void rbFindWhatAll(void)      { setWhat(BT2_WHAT_ALL, RB_FIND_WHAT_ALL); }
void rbFindWhatSongs(void)    { setWhat(BT2_WHAT_SONG, RB_FIND_WHAT_SONGS); }
void rbFindWhatFolders(void)  { setWhat(BT2_WHAT_FOLDER, RB_FIND_WHAT_FOLDERS); }
void rbFindWhatSounds(void)   { setWhat(BT2_WHAT_SOUND, RB_FIND_WHAT_SOUNDS); }

#endif
