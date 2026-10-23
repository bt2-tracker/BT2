/* BerntTracker (Windows): Disk Op. like on the iPad, and Modland. See bt2_windiskop.h and bt2_diskop.h.
**
** What is written: Modland's folders and the saved list (in %LOCALAPPDATA%\Bernttracker II), the songs
** opened from Modland (new files in Documents\BT2\Modland), and imported files (new files, never over
** a file that is there). Exp. writes a copy where the user chooses in Windows' "Save as" window.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <cderr.h>
#include <shlobj.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ft2_header.h"
#include "ft2_diskop.h"
#include "ft2_sysreqs.h"
#include "ft2_video.h"
#include "ft2_structs.h"
#include "bt2_diskop.h"
#include "bt2_windiskop.h"
#include "bt2_winhome.h"
#include "bt2_winoverlay.h"
#include "bt2_modland.h"
#include "bt2_import.h"
#include "bt2_fs.h"
#include "bt2_net.h"

#ifdef _MSC_VER
#pragma comment(lib, "comdlg32.lib")
#endif


static bool ready;
static wchar_t homeW[MAX_PATH], mirrorW[MAX_PATH];
static char homeU8[1024];

static void envUtf8(const wchar_t *name, char *out, size_t outLen)
{
	wchar_t w[1024];
	const DWORD n = GetEnvironmentVariableW(name, w, 1024);
	out[0] = '\0';
	if (n > 0 && n < 1024)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen, NULL, NULL);
}

void bt2DiskOpInit(void)
{
	const wchar_t *home = bt2HomeFolder();
	wchar_t local[MAX_PATH];
	if (home == NULL || FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, local)))
		return;

	char *homeTmp = bt2Utf8(home), *localTmp = bt2Utf8(local);
	if (homeTmp == NULL || localTmp == NULL)
	{
		free(homeTmp);
		free(localTmp);
		return;
	}

	bt2ModlandSetup s;
	memset(&s, 0, sizeof (s));

	char app[1024];
	snprintf(homeU8, sizeof (homeU8), "%s", homeTmp);
	snprintf(app, sizeof (app), "%s\\Bernttracker II", localTmp);
	snprintf(s.mirrorRoot, sizeof (s.mirrorRoot), "%s\\Modland", app);
	snprintf(s.listFile, sizeof (s.listFile), "%s\\modland-ft2.txt", app);
	snprintf(s.ownRoot, sizeof (s.ownRoot), "%s\\Modland", homeU8);
	envUtf8(L"BT2_MODLAND_LIST_URL", s.listUrl, sizeof (s.listUrl));
	envUtf8(L"BT2_MODLAND_FILE_URL", s.fileUrl, sizeof (s.fileUrl));
	free(homeTmp);
	free(localTmp);

	wchar_t *m = bt2Wide(s.mirrorRoot);
	if (m == NULL || wcslen(m) >= MAX_PATH || wcslen(home) >= MAX_PATH)
	{
		free(m);
		return;
	}

	wcscpy(mirrorW, m);
	wcscpy(homeW, home);
	free(m);

	bt2ModlandInit(&s);
	ready = true;
}

void bt2DiskOpShutdown(void)
{
	bt2ModlandShutdown();
}

const UNICHAR *bt2ModlandRootU(void)
{
	return mirrorW;
}

const UNICHAR *bt2HomeRootU(void)
{
	return homeW;
}

static char *cwdUtf8(void)
{
	wchar_t w[MAX_PATH + 1];
	if (_wgetcwd(w, MAX_PATH) == NULL)
		return NULL;

	return bt2Utf8(w);
}

// "C:\Users\x\Documents\BT2\Modland\a.xm" -> "BT2/Modland/a.xm" (for messages, like the web version)
static void homeRelative(const char *path, char *out, size_t outLen)
{
	const char *rel = bt2PathAfter(path, homeU8);
	if (rel == NULL)
	{
		snprintf(out, outLen, "%s", path);
		return;
	}

	snprintf(out, outLen, "BT2%s%s", (rel[0] != '\0') ? "/" : "", rel);
	for (char *p = out; *p != '\0'; p++)
	{
		if (*p == '\\')
			*p = '/';
	}
}

// ------------------------------------------------------------------------------------------------
// Disk Op.'s rules

bool bt2DiskOpReadOnlyHere(void)
{
	if (!ready)
		return false;

	char *cwd = cwdUtf8();
	const bool ro = (cwd != NULL) && bt2ModlandIsMirrorPath(cwd);
	free(cwd);
	return ro;
}

bool bt2DiskOpPrepareSaveDir(bool showFolder)
{
	if (!ready)
		return true;

	char *cwd = cwdUtf8();
	if (cwd == NULL || !bt2ModlandIsMirrorPath(cwd))
	{
		free(cwd);
		return true; // (anywhere else: saved where you are)
	}

	// Modland is read-only: the user's own folder for it instead (BT2/Modland/<the same folders>)
	char dest[2048];
	const bool haveDest = bt2ModlandOwnFolderFor(cwd, dest, sizeof (dest));
	free(cwd);

	wchar_t *destW = haveDest ? bt2Wide(dest) : NULL;
	if (destW == NULL || !bt2FsMkdirs(dest) || _wchdir(destW) != 0)
	{
		free(destW);
		okBox(0, "System message", "Can't save here! Go to one of your own folders first.", NULL);
		return false;
	}
	free(destW);

	if (showFolder)
		editor.diskOpReadDir = true; // (shows the folder the file goes to)

	char rel[2048], msg[2200];
	homeRelative(dest, rel, sizeof (rel));
	snprintf(msg, sizeof (msg), "Modland er bare til lesing. Lagrer i %s", rel);
	bt2Toast(msg);
	return true;
}

static bool sameDir(const wchar_t *a, const wchar_t *b)
{
	size_t la = wcslen(a), lb = wcslen(b);
	while (la > 3 && a[la - 1] == L'\\') la--;
	while (lb > 3 && b[lb - 1] == L'\\') lb--;
	return la == lb && la > 0 && _wcsnicmp(a, b, la) == 0;
}

void bt2DiskOpConfineDir(void)
{
	// (nothing: in the Windows program every folder is allowed - "Set path" can go to other disks)
}

bool bt2DiskOpGoParentTop(void)
{
	if (!ready)
		return false;

	wchar_t cwd[MAX_PATH + 1];
	if (_wgetcwd(cwd, MAX_PATH) == NULL)
		return false;

	// "../" at the top of the BT2 folder: it stays there; at Modland's top: the BT2 folder (like on the iPad)
	if (sameDir(cwd, homeW))
	{
		editor.diskOpReadDir = true;
		return true;
	}

	if (sameDir(cwd, mirrorW))
	{
		if (_wchdir(homeW) == 0)
			editor.diskOpReadDir = true;
		return true;
	}

	return false;
}

void bt2DiskOpBeforeList(const UNICHAR *dirU)
{
	if (!ready || dirU == NULL)
		return;

	char *dir = bt2Utf8(dirU);
	if (dir != NULL && bt2ModlandIsMirrorPath(dir))
	{
		bt2FsMkdirs(dir); // (Modland's top folder, the first time)
		bt2ModlandPopulate(dir);
	}
	free(dir);
}

// ------------------------------------------------------------------------------------------------
// Modland: the list

typedef struct listJob_t
{
	volatile int32_t cancel;
	bt2NetResult res;
	bool unreadable;
	char *text;
	size_t len;
} listJob;

#define LIST_LABEL "Henter Modland-listen (bare f\xC3\xB8rste gang)"

static void showProgress(const char *label, uint64_t got, uint64_t total)
{
	char text[1200];
	if (total > 0)
	{
		const uint64_t pct = (got >= total) ? 100 : (got * 100) / total;
		snprintf(text, sizeof (text), "%s \xE2\x80\xA6 %d %%", label, (int)pct);
		bt2NetBoxShow(text, (double)got / (double)total);
	}
	else
	{
		snprintf(text, sizeof (text), "%s \xE2\x80\xA6", label);
		bt2NetBoxShow(text, 0.0);
	}
}

static void listProgress(void *ud, uint64_t got, uint64_t total)
{
	showProgress(LIST_LABEL, got, total);
	(void)ud;
}

static int32_t listJobRun(void *arg)
{
	listJob *j = (listJob *)arg;
	j->text = bt2ModlandFetchList(&j->cancel, listProgress, j, &j->res, &j->unreadable, &j->len);
	return (j->text != NULL) ? 1 : 0;
}

bool bt2ModlandOpen(void)
{
	if (!ready)
		return false;

	if (!bt2ModlandIsLoaded())
	{
		bool old = false;
		if (bt2ModlandLoadSaved(&old))
		{
			if (old)
				bt2ModlandStartRefresh(); // (quietly; the new list is used the next time)
		}
		else
		{
			listJob *j = (listJob *)calloc(1, sizeof (listJob));
			if (j == NULL)
				return false;

			showProgress(LIST_LABEL, 0, 0);
			int32_t ok = 0;
			const bool ran = bt2RunJob(listJobRun, j, &j->cancel, &ok);
			bt2NetBoxHide();
			if (!ran)
				return false; // (the program is being closed; the job keeps j)

			if (!ok)
			{
				char msg[512];
				if (j->unreadable)
					snprintf(msg, sizeof (msg), "Kunne ikke lese Modland-listen");
				else
					bt2NetErrorText(&j->res, msg, sizeof (msg));

				if (msg[0] != '\0')
					bt2Toast(msg);

				free(j);
				return false;
			}

			const bool used = bt2ModlandUseList(j->text, j->len);
			free(j);
			if (!used)
			{
				bt2Toast("Kunne ikke lese Modland-listen");
				return false;
			}
		}
	}

	return bt2FsMkdirs(bt2ModlandGetSetup()->mirrorRoot);
}

void bt2DiskOpToast(const char *text)
{
	bt2Toast(text);
}

void bt2FindShownChanged(bool shown)
{
	(void)shown; // (the Windows program types into FT2's search box itself)
}

void bt2FindCleared(void)
{
}

// ------------------------------------------------------------------------------------------------
// Modland: a song is downloaded the first time it is opened

typedef struct songJob_t
{
	volatile int32_t cancel;
	bt2ModlandSong song;
	bt2NetResult res;
	bool writeFailed;
} songJob;

static void songProgress(void *ud, uint64_t got, uint64_t total)
{
	songJob *j = (songJob *)ud;
	showProgress(j->song.label, got, total);
}

static int32_t songJobRun(void *arg)
{
	songJob *j = (songJob *)arg;

	uint8_t *data = NULL;
	size_t len = 0;
	if (!bt2NetGet(j->song.url, bt2ModlandMaxSongSize(), &j->cancel, songProgress, j, &data, &len, &j->res))
		return 0;

	const bool ok = bt2ModlandStoreSong(&j->song, data, len); // (in Modland's folder: not kept unless the user saves it)
	free(data);

	if (!ok)
		j->writeFailed = true;

	return ok ? 1 : 0;
}

// a song in Modland's folders, before FT2 opens it (the rules are in bt2ModlandPrepareSong, the same as in the web version)
bool bt2PrepareFile(const UNICHAR *pathU)
{
	if (!ready || pathU == NULL)
		return true;

	wchar_t full[MAX_PATH * 2];
	const DWORD n = GetFullPathNameW(pathU, MAX_PATH * 2, full, NULL);
	if (n == 0 || n >= MAX_PATH * 2)
		return true;

	char *file = bt2Utf8(full);
	if (file == NULL)
		return true;

	bt2FileInfo info;
	if (!bt2ModlandIsMirrorPath(file) || !bt2FsInfo(file, &info) || info.isDir)
	{
		free(file);
		return true; // (not a song in Modland's folders: opened as usual)
	}

	if (!bt2ModlandIsLoaded() && !bt2ModlandOpen())
	{
		free(file);
		return info.size > 0; // (it said why; a song that came before can still be opened)
	}

	songJob *j = (songJob *)calloc(1, sizeof (songJob));
	if (j == NULL)
	{
		free(file);
		return true;
	}

	char msg[2400];
	const int32_t what = bt2ModlandPrepareSong(file, &j->song, msg, sizeof (msg));
	free(file);

	if (msg[0] != '\0')
		bt2Toast(msg);

	if (what != BT2_SONG_DOWNLOAD)
	{
		free(j);
		return what != BT2_SONG_FAILED;
	}

	// the first time: downloaded, with the download box
	showProgress(j->song.label, 0, 0);
	int32_t ok = 0;
	const bool ran = bt2RunJob(songJobRun, j, &j->cancel, &ok);
	bt2NetBoxHide();
	if (!ran)
		return false; // (the program is being closed; the job keeps j)

	if (!ok)
	{
		if (j->writeFailed || j->res.error == BT2_NET_OK)
			snprintf(msg, sizeof (msg), "Kunne ikke lagre filen");
		else
			bt2NetErrorText(&j->res, msg, sizeof (msg));

		if (msg[0] != '\0')
			bt2Toast(msg);

		free(j);
		return false;
	}

	bt2ModlandStoredMessage(&j->song, msg, sizeof (msg));
	bt2Toast(msg);
	free(j);
	return true;
}

bool bt2PrepareDroppedFile(const char *pathUtf8)
{
	wchar_t *w = bt2Wide(pathUtf8);
	if (w == NULL)
		return true;

	const bool ok = bt2PrepareFile(w);
	free(w);
	return ok;
}

// ------------------------------------------------------------------------------------------------
// Imp. and Exp.

static void toastFunc(const char *text) // (from the import's thread too: bt2Toast is thread-safe)
{
	bt2Toast(text);
}

typedef struct importJob_t
{
	volatile int32_t cancel;
	char **paths;
	int32_t count;
	char *dir;
	bool here;
	uint64_t start;
} importJob;

static void importProgress(void *ud, double done)
{
	importJob *j = (importJob *)ud;
	if (GetTickCount64() - j->start < 400)
		return; // (a quick import shows no box)

	char text[128];
	snprintf(text, sizeof (text), "Importerer \xE2\x80\xA6 %d %%", (int)(done * 100.0 + 0.5));
	bt2NetBoxShow(text, done);
}

static int32_t importJobRun(void *arg)
{
	importJob *j = (importJob *)arg;
	return bt2ImportFiles((const char *const *)j->paths, j->count, j->dir, homeU8, j->here, toastFunc, importProgress, j, &j->cancel);
}

static void importJobFree(importJob *j)
{
	for (int32_t i = 0; i < j->count; i++)
		free(j->paths[i]);
	free(j->paths);
	free(j->dir);
	free(j);
}

void pbDiskOpImport(void)
{
	if (!ready)
		return;

	static wchar_t names[65536];
	memset(names, 0, sizeof (names));

	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof (ofn));
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = video.hWnd;
	ofn.lpstrFilter = L"Alle filer (*.*)\0*.*\0Sanger, instrumenter og samples\0*.xm;*.mod;*.s3m;*.it;*.stm;*.xi;*.wav;*.aif;*.aiff;*.iff;*.flac;*.raw;*.zip\0\0";
	ofn.lpstrFile = names;
	ofn.nMaxFile = (DWORD)(sizeof (names) / sizeof (names[0]));
	ofn.lpstrTitle = L"Imp.: hent filer inn i BT2";
	ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

	if (!GetOpenFileNameW(&ofn))
	{
		if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL)
			bt2Toast("For mange filer p\xC3\xA5 \xC3\xA9n gang \xE2\x80\x93 velg f\xC3\xA6rre");
		return;
	}

	importJob *j = (importJob *)calloc(1, sizeof (importJob));
	if (j == NULL)
		return;

	// one file: its full path. Several: the folder, a 0, then the names (each ends with a 0; two 0s at the end).
	// Which one it is: the character before the first name is a 0 only when there are several.
	const bool several = (ofn.nFileOffset > 0 && names[ofn.nFileOffset - 1] == L'\0');
	int32_t cap = 0;

	if (!several)
	{
		cap = 1;
		j->paths = (char **)calloc(1, sizeof (char *));
		if (j->paths != NULL)
		{
			j->paths[0] = bt2Utf8(names);
			if (j->paths[0] != NULL)
				j->count = 1;
		}
	}
	else
	{
		for (const wchar_t *p = names + ofn.nFileOffset; *p != L'\0'; p += wcslen(p) + 1)
		{
			if (j->count >= cap)
			{
				const int32_t newCap = (cap > 0) ? cap * 2 : 64;
				char **bigger = (char **)realloc(j->paths, (size_t)newCap * sizeof (char *));
				if (bigger == NULL)
					break;
				j->paths = bigger;
				cap = newCap;
			}

			const size_t len = wcslen(names) + 1 + wcslen(p) + 1;
			wchar_t *full = (wchar_t *)malloc(len * sizeof (wchar_t));
			if (full == NULL)
				break;
			_snwprintf(full, len, L"%ls\\%ls", names, p);
			full[len - 1] = L'\0';

			j->paths[j->count] = bt2Utf8(full);
			free(full);
			if (j->paths[j->count] != NULL)
				j->count++;
		}
	}

	if (j->count == 0)
	{
		importJobFree(j);
		return;
	}

	// into the folder you are in, if it is one of your own (else the BT2 folder)
	char *cwd = cwdUtf8();
	j->here = (cwd != NULL) && bt2PathInside(cwd, homeU8) && !bt2ModlandIsMirrorPath(cwd);
	if (j->here)
	{
		j->dir = cwd;
	}
	else
	{
		free(cwd);
		j->dir = (char *)malloc(strlen(homeU8) + 1);
		if (j->dir != NULL)
			strcpy(j->dir, homeU8);
	}

	if (j->dir == NULL)
	{
		importJobFree(j);
		return;
	}

	// (in a worker thread: the window keeps going, and a long import shows the box with how far it has come)
	j->start = GetTickCount64();
	int32_t imported = 0;
	const bool ran = bt2RunJob(importJobRun, j, &j->cancel, &imported);
	bt2NetBoxHide();
	if (!ran)
		return; // (the program is being closed; the job keeps j)

	importJobFree(j);
	editor.diskOpReadDir = true; // (shows the new files)
}

void pbDiskOpExport(void)
{
	wchar_t src[MAX_PATH * 2];
	if (!diskOpGetFilePathW(src, MAX_PATH * 2) || GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES ||
		(GetFileAttributesW(src) & FILE_ATTRIBUTE_DIRECTORY))
	{
		bt2Toast("Lagre filen f\xC3\xB8rst (Save), s\xC3\xA5 Exp.");
		return;
	}

	const wchar_t *name = wcsrchr(src, L'\\');
	name = (name != NULL) ? name + 1 : src;

	wchar_t dest[MAX_PATH * 2];
	_snwprintf(dest, MAX_PATH * 2 - 1, L"%ls", name);
	dest[MAX_PATH * 2 - 1] = L'\0';

	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof (ofn));
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = video.hWnd;
	ofn.lpstrFilter = L"Alle filer (*.*)\0*.*\0\0";
	ofn.lpstrFile = dest;
	ofn.nMaxFile = MAX_PATH * 2;
	ofn.lpstrTitle = L"Exp.: lagre en kopi";
	ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

	if (!GetSaveFileNameW(&ofn))
		return;

	if (sameDir(src, dest))
		return; // (the file itself)

	if (!CopyFileW(src, dest, FALSE)) // (the user chose this name, and said yes if it was taken)
		bt2Toast("Kunne ikke lagre kopien");
}

#endif
