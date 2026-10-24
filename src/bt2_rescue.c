/* BerntTracker (web and Windows, v0.34): the rescue copy of the song being worked on (see bt2_rescue.h).
**
** - Written only while the song has unsaved changes, 3 seconds after the last change, then at most once a
**   minute (and only if something changed since the last copy). Also right away when the web app goes to
**   the background (the iPad may close it there without warning), and when the program is closed.
** - Written to "redning.xm.part" first and then renamed, so a copy is never half written.
** - Samples are written from a copy (saveXMRescue()), so it can run while the song plays, without a sound
**   glitch, and the song itself is never changed.
** - When the song has no unsaved changes any more (saved, or another song loaded), the copy is deleted.
** - At the next start, if a copy is there: "Restore it?" Yes = the song is loaded again with its own name and
**   folder (so Save puts it where it was going), still marked as unsaved. No = the copy is kept one more
**   time as "redning-forrige.xm" (nothing is offered for it), and the next one replaces it.
** - Nothing here writes to the user's folders, or over a file of the user's.
*/

#if defined _WIN32 || defined __EMSCRIPTEN__

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <stdio.h>
#include <unistd.h>
#include <emscripten.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ft2_header.h"
#include "ft2_unicode.h"
#include "ft2_structs.h"
#include "ft2_replayer.h"
#include "ft2_sysreqs.h"
#include "ft2_diskop.h"
#include "ft2_module_saver.h"
#include "ft2_module_loader.h"
#include "ft2_events.h"
#include "bt2_fs.h"
#include "bt2_rescue.h"

#define QUIET_MS 3000       // wait this long after the last change
#define INTERVAL_MS 60000   // at most one copy a minute
#define NAME_MAX_SHOWN 40   // (the song's name in the question)

static bool inited, ready, asked, restoring;
static uint32_t copiedCount, lastCount, lastChangeMs, lastCopyMs, restoreStartMs;
static bool copyOnDisk; // the copy on disk is this session's (of the unsaved changes)
static char dir[1024], xmPath[1100], partPath[1100], metaPath[1100], prevPath[1100];
static char restoreName[512], restoreDir[1024];

#ifdef __EMSCRIPTEN__
EM_JS(int, bt2_rescueStarted, (void), { return (globalThis.BT2 && globalThis.BT2.started && !globalThis.BT2.readOnly) ? 1 : 0; });
EM_JS(void, bt2_rescueSync, (void), { if (globalThis.BT2 && globalThis.BT2.syncFiles) globalThis.BT2.syncFiles(); });
#endif

static uint32_t nowMs(void)
{
	return SDL_GetTicks();
}

static UNICHAR *toUnichar(const char *utf8)
{
#ifdef _WIN32
	return bt2Wide(utf8);
#else
	return strdup(utf8);
#endif
}

static char *unicharToUtf8(const UNICHAR *u)
{
	if (u == NULL)
		return NULL;
#ifdef _WIN32
	return bt2Utf8(u);
#else
	return strdup(u);
#endif
}

static bool renameReplace(const char *from, const char *to)
{
#ifdef _WIN32
	wchar_t *a = bt2Wide(from), *b = bt2Wide(to);
	const bool ok = (a != NULL && b != NULL && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
	free(a);
	free(b);
	return ok;
#else
	return rename(from, to) == 0;
#endif
}

static void init(void)
{
	if (inited)
		return;
	inited = true;

#ifdef _WIN32
	wchar_t local[MAX_PATH];
	if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, local)))
		return;

	char *localU8 = bt2Utf8(local);
	if (localU8 == NULL)
		return;
	snprintf(dir, sizeof (dir), "%s\\Bernttracker II\\Redning", localU8);
	free(localU8);
#else
	snprintf(dir, sizeof (dir), "/home/web_user/.bt2-rescue");
#endif

	if (!bt2FsMkdirs(dir))
	{
		dir[0] = '\0';
		return;
	}

	bt2PathJoin(xmPath, sizeof (xmPath), dir, "redning.xm");
	bt2PathJoin(partPath, sizeof (partPath), dir, "redning.xm.part");
	bt2PathJoin(metaPath, sizeof (metaPath), dir, "redning.txt");
	bt2PathJoin(prevPath, sizeof (prevPath), dir, "redning-forrige.xm");
	bt2FsRemoveFile(partPath); // (a copy that was being written when the program stopped)
	ready = true;
}

// the song's name (UTF-8, as in the window title) and the folder Disk Op. saves songs in
static void currentNameAndDir(char *name, size_t nameLen, char *folder, size_t folderLen)
{
	const char *n = getCurrSongFilename();
	snprintf(name, nameLen, "%s", (n != NULL && n[0] != '\0') ? n : "untitled.xm");

	folder[0] = '\0';
	char *d = unicharToUtf8(getDiskOpModPath());
	if (d != NULL)
	{
		snprintf(folder, folderLen, "%s", d);
		free(d);
	}
}

static bool programBusy(void) // (loading, saving, rendering, or a dialog box is shown)
{
	return editor.busy || editor.wavIsRendering || editor.loadMusicEvent != 0 || ui.sysReqShown;
}

static bool busy(void)
{
	return programBusy() || restoring;
}

static bool writeCopy(void)
{
	if (!ready || dir[0] == '\0')
		return false;

	UNICHAR *partU = toUnichar(partPath);
	if (partU == NULL)
		return false;

	const uint32_t count = bt2SongEditCount;
	bool ok = saveXMRescue(partU);
	free(partU);

	if (ok)
		ok = renameReplace(partPath, xmPath);
	else
		bt2FsRemoveFile(partPath);

	if (ok)
	{
		char name[512], folder[1024], meta[1800];
		currentNameAndDir(name, sizeof (name), folder, sizeof (folder));
		const int len = snprintf(meta, sizeof (meta), "bt2-rescue 1\nname=%s\ndir=%s\ntime=%lld\n", name, folder, (long long)time(NULL));
		bt2FsWriteReplace(metaPath, meta, (len > 0 && len < (int)sizeof (meta)) ? (size_t)len : 0);

		copiedCount = count;
		copyOnDisk = true;
#ifdef __EMSCRIPTEN__
		bt2_rescueSync(); // (store it in the browser now)
#endif
	}

	lastCopyMs = nowMs();
	return ok;
}

static void deleteCopy(void)
{
	if (!ready)
		return;

	bt2FsRemoveFile(xmPath);
	bt2FsRemoveFile(metaPath);
	copyOnDisk = false;
#ifdef __EMSCRIPTEN__
	bt2_rescueSync();
#endif
}

static void readMeta(char *name, size_t nameLen, char *folder, size_t folderLen, long long *when)
{
	snprintf(name, nameLen, "untitled.xm");
	folder[0] = '\0';
	*when = 0;

	size_t len = 0;
	char *text = (char *)bt2FsReadAll(metaPath, &len, 16384);
	if (text == NULL)
		return;

	for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n"))
	{
		if (!strncmp(line, "name=", 5) && line[5] != '\0') snprintf(name, nameLen, "%s", line + 5);
		else if (!strncmp(line, "dir=", 4)) snprintf(folder, folderLen, "%s", line + 4);
		else if (!strncmp(line, "time=", 5)) *when = atoll(line + 5);
	}

	free(text);
}

static void askAtStart(void)
{
	asked = true;
	if (!bt2FsExists(xmPath))
		return;

	char name[512], folder[1024];
	long long when = 0;
	readMeta(name, sizeof (name), folder, sizeof (folder), &when);

	bt2FileInfo info;
	if (when <= 0 && bt2FsInfo(xmPath, &info))
		when = info.mtime;

	char whenText[32] = "";
	time_t t = (time_t)when;
	struct tm *tmv = (when > 0) ? localtime(&t) : NULL;
	if (tmv != NULL)
		strftime(whenText, sizeof (whenText), " (%d.%m %H:%M)", tmv);

	// the name in FT2's own letters, not too long for the box
	char shown[NAME_MAX_SHOWN + 4];
	char *cp = utf8ToCp850(name, true);
	snprintf(shown, sizeof (shown), "%s", (cp != NULL) ? cp : "untitled.xm");
	free(cp);
	if (strlen(shown) > NAME_MAX_SHOWN)
		strcpy(&shown[NAME_MAX_SHOWN - 3], "...");

	char text[200];
	snprintf(text, sizeof (text), "Restore the unsaved song \"%s\"%s?", shown, whenText);

	if (okBox(2, "Unsaved song from last time", text, NULL) == 1)
	{
		UNICHAR *xmU = toUnichar(xmPath);
		if (xmU != NULL)
		{
			snprintf(restoreName, sizeof (restoreName), "%s", name);
			snprintf(restoreDir, sizeof (restoreDir), "%s", folder);
			restoring = true;
			restoreStartMs = nowMs();
			editor.loadMusicEvent = EVENT_LOADMUSIC_DISKOP; // (set up like a song opened in Disk Op., not played)
			loadMusic(xmU); // (bt2RescueAfterLoad() gives it its name back when it is loaded)
			free(xmU);
		}
		return;
	}

	// no: keep it one more time, out of the way (the next "no" replaces it)
	bt2FsRemoveFile(prevPath);
	renameReplace(xmPath, prevPath);
	bt2FsRemoveFile(metaPath);
#ifdef __EMSCRIPTEN__
	bt2_rescueSync();
#endif
}

void bt2RescueAfterLoad(void)
{
	if (!restoring)
		return;
	restoring = false;

	// only if it really was the rescue copy that was loaded
	char *loaded = unicharToUtf8(editor.tmpFilenameU);
	const bool isCopy = (loaded != NULL && bt2PathInside(loaded, xmPath));
	free(loaded);
	if (!isCopy)
		return;

	if (restoreDir[0] != '\0')
		diskOpSetModPathUtf8(restoreDir); // (only if the folder is still there)

	UNICHAR *nameU = toUnichar(restoreName);
	if (nameU != NULL)
	{
		diskOpSetFilename(DISKOP_ITEM_MODULE, nameU);
		free(nameU);
	}

	setSongModifiedFlag(); // (it isn't saved yet: the user saves it)
	copiedCount = bt2SongEditCount; // (the copy on disk is this song)
	lastCount = copiedCount;
	copyOnDisk = true;
	lastCopyMs = nowMs();
}

void bt2RescueTick(void)
{
	init();
	if (!ready)
		return;

#ifdef __EMSCRIPTEN__
	if (!bt2_rescueStarted()) // (the web page's start screen is still shown, or this copy of the app stores nothing)
		return;
#endif

	if (!asked)
	{
		if (!busy())
			askAtStart();
		return;
	}

	const uint32_t now = nowMs();
	if (restoring)
	{
		if (!programBusy() && now - restoreStartMs > 10000)
			restoring = false; // (the copy couldn't be loaded: carry on as normal)
		return;
	}

	const uint32_t count = bt2SongEditCount;
	if (count != lastCount)
	{
		lastCount = count;
		lastChangeMs = now;
	}

	if (!song.isModified)
	{
		if (copyOnDisk)
			deleteCopy(); // (saved, or another song: nothing to rescue)
		return;
	}

	if (count == copiedCount && copyOnDisk)
		return; // (nothing new since the last copy)

	if (now - lastChangeMs < QUIET_MS || busy())
		return;

	if (copyOnDisk && now - lastCopyMs < INTERVAL_MS)
		return;

	writeCopy();
}

bool bt2RescueNow(void)
{
	init();
	if (!ready || !asked || restoring || busy() || !song.isModified)
		return false;

	if (copyOnDisk && bt2SongEditCount == copiedCount)
		return true; // (the copy is up to date)

	return writeCopy();
}

void bt2RescueOnExit(void)
{
	init();
	if (!ready || !asked || restoring)
		return;

	if (song.isModified)
	{
		if (!(copyOnDisk && bt2SongEditCount == copiedCount))
			writeCopy(); // (the user may have left without saving: the next start asks)
	}
	else if (copyOnDisk)
	{
		deleteCopy();
	}
}

#ifdef __EMSCRIPTEN__
// the page calls this when it goes to the background (the iPad may close it there without warning)
EMSCRIPTEN_KEEPALIVE int32_t webRescueNow(void)
{
	return bt2RescueNow() ? 1 : 0;
}
#endif

#endif
