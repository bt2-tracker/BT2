/* BerntTracker (Windows): the user's own folders, like in the web version. See bt2_winhome.h.
**
** Never overwrites or deletes anything: folders are only made if they are missing, and a demo song is
** only copied if there is no file with that name already.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ft2_header.h"
#include "ft2_config.h"
#include "ft2_audio.h"
#include "ft2_palette.h"
#include "ft2_tables.h"
#include "mixer/ft2_mix.h"
#include "bt2_winhome.h"

#define MAX_DEMOS 2000

static wchar_t home[MAX_PATH];
static bool homeReady, firstTimeSettingsPending;
static HANDLE demoThread;
static volatile LONG demoCancel, demoNews;

static const wchar_t *kindFolder[5] = { L"Modules", L"Instruments", L"Samples", L"Patterns", L"Tracks" };

static bool isDir(const wchar_t *p)
{
	const DWORD a = GetFileAttributesW(p);
	return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool exists(const wchar_t *p)
{
	return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void join(wchar_t *out, size_t outLen, const wchar_t *a, const wchar_t *b)
{
	_snwprintf(out, outLen - 1, L"%ls\\%ls", a, b);
	out[outLen - 1] = L'\0';
}

void bt2HomeSetup(void)
{
	wchar_t docs[MAX_PATH];
	if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, docs)))
		return;

	join(home, MAX_PATH, docs, L"BT2");
	CreateDirectoryW(home, NULL); // (does nothing if it is there)
	if (!isDir(home))
		return;

	wchar_t p[MAX_PATH];
	for (int32_t i = 0; i < 5; i++)
	{
		join(p, MAX_PATH, home, kindFolder[i]);
		CreateDirectoryW(p, NULL);
	}

	join(p, MAX_PATH, home, L"Demo Tracks");
	CreateDirectoryW(p, NULL);

	homeReady = true;

	/* The web version's first-time settings (FT2's look and sound: the blue palette, linear
	** interpolation, volume ramping), once. They are saved when the sound has been set up
	** (saving the settings also saves the chosen sound card).
	*/
	join(p, MAX_PATH, home, L".defaults-v1");
	if (!exists(p))
	{
		config.cfg_StdPalNum = PAL_BLUES;
		setPalette(palTable[PAL_BLUES], false);
		config.interpolation = INTERPOLATION_LINEAR;
		audio.interpolationType = INTERPOLATION_LINEAR;
		config.specialFlags &= ~NO_VOLRAMP_FLAG;
		audioSetVolRamp(true);
		firstTimeSettingsPending = true;
	}
}

void bt2HomeFillDiskOpPath(int32_t kind, wchar_t *path, size_t pathLen, bool *pathSet)
{
	if (!homeReady || kind < 0 || kind > 4 || pathLen < 8)
		return;

	if (path[0] != L'\0' && isDir(path))
		return; // (a folder chosen in Config, and it is there)

	wchar_t p[MAX_PATH];
	join(p, MAX_PATH, home, kindFolder[kind]);
	if (!isDir(p) || wcslen(p) >= pathLen)
		return;

	wcscpy(path, p);
	if (pathSet != NULL)
		*pathSet = true;
}

// ------------------------------------------------------------------------------------------------
// the demo songs

typedef struct demoList_t
{
	char **items;
	int32_t count;
} demoList;

static void listAdd(demoList *l, const char *s)
{
	if (l->count >= MAX_DEMOS)
		return;

	char *copy = _strdup(s);
	if (copy == NULL)
		return;

	char **items = (char **)realloc(l->items, (size_t)(l->count + 1) * sizeof (char *));
	if (items == NULL)
	{
		free(copy);
		return;
	}

	l->items = items;
	l->items[l->count++] = copy;
}

static bool listHas(const demoList *l, const char *s)
{
	for (int32_t i = 0; i < l->count; i++)
	{
		if (strcmp(l->items[i], s) == 0)
			return true;
	}

	return false;
}

static void listFree(demoList *l)
{
	for (int32_t i = 0; i < l->count; i++)
		free(l->items[i]);

	free(l->items);
	l->items = NULL;
	l->count = 0;
}

// .demo-seen.json: a JSON list of the demo songs already put in place (paths with '/', as on the web)
static void readSeen(const wchar_t *file, demoList *seen)
{
	HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;

	const DWORD size = GetFileSize(f, NULL);
	if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024)
	{
		CloseHandle(f);
		return;
	}

	char *text = (char *)malloc(size + 1);
	DWORD got = 0;
	if (text == NULL || !ReadFile(f, text, size, &got, NULL))
	{
		free(text);
		CloseHandle(f);
		return;
	}
	CloseHandle(f);
	text[got] = '\0';

	// (strings in a list: "...", with \" and \\ inside)
	char item[1024];
	for (const char *p = text; *p != '\0'; p++)
	{
		if (*p != '"')
			continue;

		size_t n = 0;
		p++;
		while (*p != '\0' && *p != '"')
		{
			char ch = *p++;
			if (ch == '\\' && *p != '\0')
			{
				ch = *p++;
				if (ch == 'u' && strlen(p) >= 4) // (only plain ASCII is written as \u)
				{
					char hex[5] = { p[0], p[1], p[2], p[3], '\0' };
					ch = (char)strtoul(hex, NULL, 16);
					p += 4;
				}
			}

			if (n + 1 < sizeof (item))
				item[n++] = ch;
		}

		item[n] = '\0';
		if (n > 0)
			listAdd(seen, item);

		if (*p == '\0')
			break;
	}

	free(text);
}

static void writeSeen(const wchar_t *file, const demoList *seen)
{
	wchar_t tmp[MAX_PATH + 8];
	_snwprintf(tmp, MAX_PATH + 7, L"%ls.tmp", file);
	tmp[MAX_PATH + 7] = L'\0';

	HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;

	bool ok = true;
	DWORD wrote;
	ok &= WriteFile(f, "[", 1, &wrote, NULL) ? true : false;
	for (int32_t i = 0; i < seen->count && ok; i++)
	{
		char buf[2200];
		size_t n = 0;
		if (i > 0)
			buf[n++] = ',';
		buf[n++] = '"';
		for (const char *s = seen->items[i]; *s != '\0' && n < sizeof (buf) - 8; s++)
		{
			if (*s == '"' || *s == '\\')
			{
				buf[n++] = '\\';
				buf[n++] = *s;
			}
			else if ((unsigned char)*s < 0x20)
			{
				n += (size_t)sprintf(&buf[n], "\\u%04x", (unsigned char)*s);
			}
			else
			{
				buf[n++] = *s;
			}
		}
		buf[n++] = '"';
		ok &= WriteFile(f, buf, (DWORD)n, &wrote, NULL) ? true : false;
	}
	ok &= WriteFile(f, "]", 1, &wrote, NULL) ? true : false;
	CloseHandle(f);

	if (!ok || !MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileW(tmp);
		return;
	}

	SetFileAttributesW(file, FILE_ATTRIBUTE_HIDDEN);
}

static bool isSongFile(const wchar_t *name)
{
	const wchar_t *dot = wcsrchr(name, L'.');
	if (dot == NULL)
		return false;

	static const wchar_t *exts[] = { L".xm", L".mod", L".s3m", L".it", L".xi", L".wav" };
	for (int32_t i = 0; i < 6; i++)
	{
		if (_wcsicmp(dot, exts[i]) == 0)
			return true;
	}

	return false;
}

// every song file under root (relative paths with '/', UTF-8)
static void findDemos(const wchar_t *root, const wchar_t *rel, demoList *out, int32_t depth)
{
	if (depth > 8 || demoCancel)
		return;

	wchar_t pattern[MAX_PATH];
	if (rel[0] != L'\0')
		_snwprintf(pattern, MAX_PATH - 1, L"%ls\\%ls\\*", root, rel);
	else
		_snwprintf(pattern, MAX_PATH - 1, L"%ls\\*", root);
	pattern[MAX_PATH - 1] = L'\0';

	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;

	do
	{
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
			continue;

		wchar_t sub[MAX_PATH];
		if (rel[0] != L'\0')
			_snwprintf(sub, MAX_PATH - 1, L"%ls\\%ls", rel, fd.cFileName);
		else
			_snwprintf(sub, MAX_PATH - 1, L"%ls", fd.cFileName);
		sub[MAX_PATH - 1] = L'\0';

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			findDemos(root, sub, out, depth + 1);
		}
		else if (isSongFile(fd.cFileName))
		{
			char utf8[MAX_PATH * 3];
			if (WideCharToMultiByte(CP_UTF8, 0, sub, -1, utf8, sizeof (utf8), NULL, NULL) > 0)
			{
				for (char *c = utf8; *c != '\0'; c++)
				{
					if (*c == '\\')
						*c = '/';
				}

				listAdd(out, utf8);
			}
		}
	}
	while (FindNextFileW(h, &fd));

	FindClose(h);
}

static void makeParentFolders(const wchar_t *base, const wchar_t *relFile)
{
	wchar_t rel[MAX_PATH];
	wcsncpy(rel, relFile, MAX_PATH - 1);
	rel[MAX_PATH - 1] = L'\0';

	for (wchar_t *p = rel; *p != L'\0'; p++)
	{
		if (*p != L'\\')
			continue;

		*p = L'\0';
		wchar_t dir[MAX_PATH];
		join(dir, MAX_PATH, base, rel);
		CreateDirectoryW(dir, NULL);
		*p = L'\\';
	}
}

// where the program's own "Demo Tracks" folder is (next to it, or in the source folder when it is in release\win64)
static bool findDemoSource(wchar_t *out)
{
	wchar_t exe[MAX_PATH];
	const DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return false;

	wchar_t *slash = wcsrchr(exe, L'\\');
	if (slash == NULL)
		return false;
	*slash = L'\0';

	const wchar_t *tries[2] = { L"Demo Tracks", L"..\\..\\Demo Tracks" };
	for (int32_t i = 0; i < 2; i++)
	{
		wchar_t p[MAX_PATH], full[MAX_PATH];
		join(p, MAX_PATH, exe, tries[i]);
		if (GetFullPathNameW(p, MAX_PATH, full, NULL) > 0 && isDir(full))
		{
			wcscpy(out, full);
			return true;
		}
	}

	return false;
}

static DWORD WINAPI demoCopyThread(LPVOID arg)
{
	(void)arg;

	wchar_t src[MAX_PATH], dst[MAX_PATH], seenFile[MAX_PATH];
	if (!findDemoSource(src))
		return 0;

	join(dst, MAX_PATH, home, L"Demo Tracks");
	join(seenFile, MAX_PATH, home, L".demo-seen.json");

	wchar_t srcFull[MAX_PATH], dstFull[MAX_PATH];
	if (GetFullPathNameW(dst, MAX_PATH, dstFull, NULL) == 0 || GetFullPathNameW(src, MAX_PATH, srcFull, NULL) == 0 ||
		_wcsicmp(srcFull, dstFull) == 0)
		return 0; // (the program is in the BT2 folder itself: the songs are already there)

	demoList seen = { NULL, 0 }, demos = { NULL, 0 };
	readSeen(seenFile, &seen);
	findDemos(src, L"", &demos, 0);

	bool changed = false;
	int32_t added = 0;
	for (int32_t i = 0; i < demos.count && !demoCancel; i++)
	{
		if (listHas(&seen, demos.items[i]))
			continue; // (put in place before: if it was deleted, that was on purpose)

		wchar_t rel[MAX_PATH];
		if (MultiByteToWideChar(CP_UTF8, 0, demos.items[i], -1, rel, MAX_PATH) <= 0)
			continue;

		for (wchar_t *c = rel; *c != L'\0'; c++)
		{
			if (*c == L'/')
				*c = L'\\';
		}

		wchar_t from[MAX_PATH], to[MAX_PATH];
		join(from, MAX_PATH, src, rel);
		join(to, MAX_PATH, dst, rel);

		if (!exists(to))
		{
			// (copied under a temporary name first, so a stopped copy never looks like a whole song)
			wchar_t part[MAX_PATH + 8];
			_snwprintf(part, MAX_PATH + 7, L"%ls.part", to);
			part[MAX_PATH + 7] = L'\0';

			makeParentFolders(dst, rel);
			if (!CopyFileW(from, part, FALSE))
			{
				DeleteFileW(part);
				continue;
			}

			if (!MoveFileExW(part, to, MOVEFILE_WRITE_THROUGH)) // (no MOVEFILE_REPLACE_EXISTING: never over a file)
			{
				DeleteFileW(part);
				if (!exists(to))
					continue;
			}
			else
			{
				added++;
			}
		}

		listAdd(&seen, demos.items[i]);
		changed = true;
	}

	if (changed)
		writeSeen(seenFile, &seen);

	if (added > 0)
		InterlockedExchange(&demoNews, added);

	listFree(&seen);
	listFree(&demos);
	return 0;
}

void bt2HomeStartDemoCopy(void)
{
	if (firstTimeSettingsPending)
	{
		// (now the sound is set up, so saving the settings keeps the chosen sound card)
		firstTimeSettingsPending = false;
		if (saveConfig(CONFIG_HIDE_ERRORS))
		{
			wchar_t p[MAX_PATH];
			join(p, MAX_PATH, home, L".defaults-v1");
			HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, NULL);
			if (f != INVALID_HANDLE_VALUE)
				CloseHandle(f);
		}
	}

	if (!homeReady || demoThread != NULL)
		return;

	demoCancel = 0;
	demoThread = CreateThread(NULL, 0, demoCopyThread, NULL, 0, NULL);
}

bool bt2HomeTakeDemoNews(int32_t *added)
{
	const LONG n = InterlockedExchange(&demoNews, 0);
	*added = (int32_t)n;
	return n > 0;
}

const wchar_t *bt2HomeFolder(void)
{
	return homeReady ? home : NULL;
}

void bt2HomeShutdown(void)
{
	if (demoThread != NULL)
	{
		InterlockedExchange(&demoCancel, 1);
		WaitForSingleObject(demoThread, 10000); // (one file finishes copying)
		CloseHandle(demoThread);
		demoThread = NULL;
	}
}

#endif
