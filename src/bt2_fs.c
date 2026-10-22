/* BerntTracker: small file helpers with UTF-8 paths. See bt2_fs.h. */

#include <stdlib.h>
#include <string.h>
#include "bt2_fs.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#ifndef __EMSCRIPTEN__
#include <sys/statvfs.h>
#endif
#endif

#define PART_ENDING ".bt2part" // (a file being written; renamed when it is complete)

// "<path>.bt2part", or "<path>.2.bt2part", "<path>.3.bt2part" ... (attempt = 1, 2, 3 ...)
static bool partName(const char *path, int32_t attempt, char *out, size_t outLen)
{
	const int32_t n = (attempt <= 1) ? snprintf(out, outLen, "%s" PART_ENDING, path) : snprintf(out, outLen, "%s.%d" PART_ENDING, path, (int)attempt);
	return n > 0 && (size_t)n < outLen;
}

// ------------------------------------------------------------------------------------------------
// text helpers

bool bt2Utf8Valid(const char *s, size_t len)
{
	const uint8_t *p = (const uint8_t *)s, *end = p + len;
	while (p < end)
	{
		const uint8_t c = *p;
		if (c < 0x80)
		{
			p++;
			continue;
		}

		int32_t n;
		uint32_t cp;
		if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
		else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
		else return false;

		if (end - p <= n)
			return false;

		for (int32_t i = 1; i <= n; i++)
		{
			if ((p[i] & 0xC0) != 0x80)
				return false;
			cp = (cp << 6) | (p[i] & 0x3F);
		}

		if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			return false;

		p += n + 1;
	}

	return true;
}

static size_t putUtf8(uint32_t cp, char *out, size_t room)
{
	if (cp < 0x80)
	{
		if (room < 1) return 0;
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800)
	{
		if (room < 2) return 0;
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (room < 3) return 0;
	out[0] = (char)(0xE0 | (cp >> 12));
	out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[2] = (char)(0x80 | (cp & 0x3F));
	return 3;
}

void bt2Cp1252ToUtf8(const char *in, char *out, size_t outLen)
{
	// 0x80..0x9F in Windows-1252 (the others are the same as Unicode; unused ones stay as they are, like in browsers)
	static const uint16_t c1[32] = {
		0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
		0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178 };

	if (outLen == 0)
		return;

	size_t o = 0;
	for (const uint8_t *p = (const uint8_t *)in; *p != '\0'; p++)
	{
		const uint32_t cp = (*p >= 0x80 && *p <= 0x9F) ? c1[*p - 0x80] : *p;
		const size_t n = putUtf8(cp, out + o, outLen - 1 - o);
		if (n == 0)
			break;
		o += n;
	}
	out[o] = '\0';
}

#ifdef _WIN32
wchar_t *bt2Wide(const char *utf8)
{
	if (utf8 == NULL)
		return NULL;

	const int32_t n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	if (n <= 0)
		return NULL;

	wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof (wchar_t));
	if (w == NULL)
		return NULL;

	if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) <= 0)
	{
		free(w);
		return NULL;
	}

	for (wchar_t *p = w; *p != L'\0'; p++)
	{
		if (*p == L'/')
			*p = L'\\';
	}

	return w;
}

char *bt2Utf8(const wchar_t *wide)
{
	if (wide == NULL)
		return NULL;

	const int32_t n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
	if (n <= 0)
		return NULL;

	char *s = (char *)malloc((size_t)n);
	if (s == NULL)
		return NULL;

	if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, s, n, NULL, NULL) <= 0)
	{
		free(s);
		return NULL;
	}

	return s;
}
#endif

// ------------------------------------------------------------------------------------------------
// paths

static bool isSep(char c)
{
#ifdef _WIN32
	return c == '\\' || c == '/';
#else
	return c == '/';
#endif
}

static bool sameChar(char a, char b)
{
	if (isSep(a) && isSep(b))
		return true;
#ifdef _WIN32
	if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
	if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
#endif
	return a == b;
}

void bt2PathJoin(char *out, size_t outLen, const char *a, const char *b)
{
	if (outLen == 0)
		return;

	const size_t la = strlen(a);
	const bool sep = (la > 0 && isSep(a[la - 1]));
	snprintf(out, outLen, "%s%s%s", a, sep ? "" : BT2_SEP_STR, b);
}

const char *bt2PathAfter(const char *path, const char *root)
{
	if (path == NULL || root == NULL)
		return NULL;

	size_t n = strlen(root);
	while (n > 0 && isSep(root[n - 1]))
		n--;

	if (n == 0)
		return NULL;

	for (size_t i = 0; i < n; i++)
	{
		if (path[i] == '\0' || !sameChar(path[i], root[i]))
			return NULL;
	}

	if (path[n] == '\0')
		return path + n;

	if (!isSep(path[n]))
		return NULL;

	const char *p = path + n;
	while (isSep(*p))
		p++;

	return p;
}

bool bt2PathInside(const char *path, const char *root)
{
	return bt2PathAfter(path, root) != NULL;
}

bool bt2FsIsDeviceName(const char *name)
{
	/* Windows' device names: CON, PRN, AUX, NUL, COM0-9, LPT0-9 (also COM and LPT with a small 1, 2 or 3 above
	** the line), CONIN$ and CONOUT$ - in any case, also with an ending ("nul.xm") and with spaces before the
	** dot or at the end ("con .xm", "nul "). A file with such a name is a device, not a file.
	*/
	size_t n = 0;
	while (name[n] != '\0' && name[n] != '.')
		n++;
	while (n > 0 && name[n - 1] == ' ')
		n--;

	if (n < 3 || n > 7)
		return false;

	char base[8];
	for (size_t i = 0; i < n; i++)
	{
		const char c = name[i];
		base[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
	}
	base[n] = '\0';

	if (!strcmp(base, "CON") || !strcmp(base, "PRN") || !strcmp(base, "AUX") || !strcmp(base, "NUL") ||
		!strcmp(base, "CONIN$") || !strcmp(base, "CONOUT$"))
	{
		return true;
	}

	if (strncmp(base, "COM", 3) != 0 && strncmp(base, "LPT", 3) != 0)
		return false;

	if (n == 4)
		return base[3] >= '0' && base[3] <= '9';

	// the small 1, 2 and 3 (UTF-8: C2 B9, C2 B2, C2 B3)
	return n == 5 && (uint8_t)base[3] == 0xC2 && ((uint8_t)base[4] == 0xB9 || (uint8_t)base[4] == 0xB2 || (uint8_t)base[4] == 0xB3);
}

void bt2CleanName(const char *name, char *out, size_t outLen, size_t maxBytes)
{
	if (outLen == 0)
		return;

	if (maxBytes > outLen - 2)
		maxBytes = outLen - 2; // (room for a '_' in front)

	size_t o = 0;
	const uint8_t *p = (const uint8_t *)name;

	// dots at the start become one '_' (".hidden" names would not show, ".." would be the folder above)
	if (*p == '.')
	{
		while (*p == '.')
			p++;
		out[o++] = '_';
	}

	while (*p != '\0')
	{
		// whole letters only (never half of a UTF-8 sequence)
		size_t n = 1;
		if (*p >= 0xC0)
		{
			while ((p[n] & 0xC0) == 0x80 && n < 4)
				n++;
		}

		if (o + n > maxBytes)
			break;

		if (n == 1)
		{
			const char c = (char)*p;
			out[o++] = ((uint8_t)c < 0x20 || strchr("\\/:*?\"<>|", c) != NULL) ? '_' : c;
		}
		else
		{
			memcpy(out + o, p, n);
			o += n;
		}

		p += n;
	}

#ifdef _WIN32
	// Windows drops spaces and dots at the end of a name
	while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.'))
		out[o - 1] = '_';
#endif

	if (o == 0)
		out[o++] = '_';

	out[o] = '\0';

#ifdef _WIN32
	if (bt2FsIsDeviceName(out) && o + 1 < outLen)
	{
		memmove(out + 1, out, o + 1);
		out[0] = '_';
	}
#endif
}

bool bt2UniquePath(const char *path, char *out, size_t outLen)
{
	if (!bt2FsExists(path))
	{
		snprintf(out, outLen, "%s", path);
		return strlen(path) < outLen;
	}

	// "folder/song.xm" -> stem "folder/song", ending ".xm"
	const char *slash = NULL;
	for (const char *p = path; *p != '\0'; p++)
	{
		if (isSep(*p))
			slash = p;
	}

	const char *dot = strrchr(path, '.');
	if (dot != NULL && slash != NULL && dot < slash)
		dot = NULL;

	const size_t stemLen = (dot != NULL) ? (size_t)(dot - path) : strlen(path);
	const char *ending = (dot != NULL) ? dot : "";

	for (int32_t i = 2; i < 100000; i++)
	{
		const int32_t n = snprintf(out, outLen, "%.*s (%d)%s", (int)stemLen, path, (int)i, ending);
		if (n < 0 || (size_t)n >= outLen)
			return false;

		if (!bt2FsExists(out))
			return true;
	}

	return false;
}

// ------------------------------------------------------------------------------------------------
// files

#ifdef _WIN32

bool bt2FsInfo(const char *path, bt2FileInfo *info)
{
	memset(info, 0, sizeof (bt2FileInfo));

	wchar_t *w = bt2Wide(path);
	if (w == NULL)
		return false;

	WIN32_FILE_ATTRIBUTE_DATA a;
	const BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
	free(w);
	if (!ok)
		return false;

	info->exists = true;
	info->isDir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	info->size = ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;

	const int64_t t = ((int64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
	info->mtime = (t - 116444736000000000LL) / 10000000LL;
	return true;
}

FILE *bt2FsOpen(const char *path, const char *mode)
{
	wchar_t *w = bt2Wide(path), wmode[8];
	if (w == NULL)
		return NULL;

	size_t i = 0;
	for (; mode[i] != '\0' && i < 7; i++)
		wmode[i] = (wchar_t)mode[i];
	wmode[i] = L'\0';

	FILE *f = _wfopen(w, wmode);
	free(w);
	return f;
}

bool bt2FsMkdir(const char *path)
{
	wchar_t *w = bt2Wide(path);
	if (w == NULL)
		return false;

	CreateDirectoryW(w, NULL); // (fails harmlessly if it is there)
	const DWORD a = GetFileAttributesW(w);
	free(w);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool bt2FsCreateEmpty(const char *path)
{
	wchar_t *w = bt2Wide(path);
	if (w == NULL)
		return false;

	HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL); // (fails if it is there)
	free(w);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	CloseHandle(h);
	return true;
}

uint8_t *bt2FsReadAll(const char *path, size_t *len, size_t maxLen)
{
	*len = 0;
	wchar_t *w = bt2Wide(path);
	if (w == NULL)
		return NULL;

	HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	free(w);
	if (h == INVALID_HANDLE_VALUE)
		return NULL;

	LARGE_INTEGER size;
	if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart > (uint64_t)maxLen)
	{
		CloseHandle(h);
		return NULL;
	}

	const size_t n = (size_t)size.QuadPart;
	uint8_t *data = (uint8_t *)malloc(n + 1);
	if (data == NULL)
	{
		CloseHandle(h);
		return NULL;
	}

	size_t got = 0;
	while (got < n)
	{
		DWORD chunk = (n - got > 0x10000000) ? 0x10000000 : (DWORD)(n - got), r = 0;
		if (!ReadFile(h, data + got, chunk, &r, NULL) || r == 0)
			break;
		got += r;
	}

	CloseHandle(h);
	if (got != n)
	{
		free(data);
		return NULL;
	}

	data[n] = 0;
	*len = n;
	return data;
}

static bool writeVia(const char *path, const void *data, size_t len, bool replace)
{
	wchar_t *w = bt2Wide(path), *wp = NULL;
	if (w == NULL)
		return false;

	// the contents go to a new file of its own first (CREATE_NEW: a file that is there is never written over)
	HANDLE h = INVALID_HANDLE_VALUE;
	for (int32_t attempt = 1; attempt <= 50 && h == INVALID_HANDLE_VALUE; attempt++)
	{
		char part[2048];
		free(wp);
		wp = partName(path, attempt, part, sizeof (part)) ? bt2Wide(part) : NULL;
		if (wp == NULL)
			break;

		h = CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
			break;
	}

	bool ok = false;
	if (h != INVALID_HANDLE_VALUE)
	{
		size_t put = 0;
		ok = true;
		while (put < len)
		{
			DWORD chunk = (len - put > 0x10000000) ? 0x10000000 : (DWORD)(len - put), wrote = 0;
			if (!WriteFile(h, (const uint8_t *)data + put, chunk, &wrote, NULL) || wrote == 0)
			{
				ok = false;
				break;
			}
			put += wrote;
		}

		if (!CloseHandle(h))
			ok = false;

		if (ok)
			ok = MoveFileExW(wp, w, replace ? MOVEFILE_REPLACE_EXISTING : 0) != 0; // (without "replace": fails if it is there)

		if (!ok)
			DeleteFileW(wp);
	}

	free(w);
	free(wp);
	return ok;
}

bool bt2FsFreeSpace(const char *dir, uint64_t *bytes)
{
	wchar_t *w = bt2Wide(dir);
	if (w == NULL)
		return false;

	ULARGE_INTEGER avail;
	const BOOL ok = GetDiskFreeSpaceExW(w, &avail, NULL, NULL);
	free(w);
	if (!ok)
		return false;

	*bytes = (uint64_t)avail.QuadPart;
	return true;
}

bool bt2FsRemoveFile(const char *path)
{
	wchar_t *w = bt2Wide(path);
	if (w == NULL)
		return false;

	const BOOL ok = DeleteFileW(w);
	free(w);
	return ok != 0;
}

struct bt2DirIter_t
{
	HANDLE h;
	bool first;
	WIN32_FIND_DATAW fd;
	char name[1024];
};

bt2DirIter *bt2FsDirOpen(const char *path)
{
	char pattern[2048];
	bt2PathJoin(pattern, sizeof (pattern), path, "*");

	wchar_t *w = bt2Wide(pattern);
	if (w == NULL)
		return NULL;

	bt2DirIter *d = (bt2DirIter *)calloc(1, sizeof (bt2DirIter));
	if (d == NULL)
	{
		free(w);
		return NULL;
	}

	d->h = FindFirstFileW(w, &d->fd);
	free(w);
	if (d->h == INVALID_HANDLE_VALUE)
	{
		free(d);
		return NULL;
	}

	d->first = true;
	return d;
}

bool bt2FsDirNext(bt2DirIter *d, const char **name, bool *isDir, int64_t *size)
{
	for (;;)
	{
		if (!d->first && !FindNextFileW(d->h, &d->fd))
			return false;
		d->first = false;

		if (!wcscmp(d->fd.cFileName, L".") || !wcscmp(d->fd.cFileName, L".."))
			continue;

		if (WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->name, sizeof (d->name), NULL, NULL) <= 0)
			continue;

		*name = d->name;
		if (isDir != NULL) *isDir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (size != NULL) *size = ((int64_t)d->fd.nFileSizeHigh << 32) | d->fd.nFileSizeLow;
		return true;
	}
}

void bt2FsDirClose(bt2DirIter *d)
{
	if (d == NULL)
		return;

	FindClose(d->h);
	free(d);
}

#else // not Windows

bool bt2FsInfo(const char *path, bt2FileInfo *info)
{
	memset(info, 0, sizeof (bt2FileInfo));

	struct stat st;
	if (stat(path, &st) != 0)
		return false;

	info->exists = true;
	info->isDir = S_ISDIR(st.st_mode);
	info->size = (int64_t)st.st_size;
	info->mtime = (int64_t)st.st_mtime;
	return true;
}

FILE *bt2FsOpen(const char *path, const char *mode)
{
	return fopen(path, mode);
}

bool bt2FsMkdir(const char *path)
{
	mkdir(path, 0777);
	return bt2FsIsDir(path);
}

bool bt2FsCreateEmpty(const char *path)
{
	const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666); // (fails if it is there)
	if (fd < 0)
		return false;

	close(fd);
	return true;
}

uint8_t *bt2FsReadAll(const char *path, size_t *len, size_t maxLen)
{
	*len = 0;
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;

	struct stat st;
	if (fstat(fileno(f), &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)maxLen)
	{
		fclose(f);
		return NULL;
	}

	const size_t n = (size_t)st.st_size;
	uint8_t *data = (uint8_t *)malloc(n + 1);
	if (data == NULL)
	{
		fclose(f);
		return NULL;
	}

	const size_t got = fread(data, 1, n, f);
	fclose(f);
	if (got != n)
	{
		free(data);
		return NULL;
	}

	data[n] = 0;
	*len = n;
	return data;
}

static bool writeVia(const char *path, const void *data, size_t len, bool replace)
{
	if (!replace && bt2FsExists(path))
		return false;

	// the contents go to a new file of its own first (O_EXCL: a file that is there is never written over)
	char part[2048];
	int fd = -1;
	for (int32_t attempt = 1; attempt <= 50 && fd < 0; attempt++)
	{
		if (!partName(path, attempt, part, sizeof (part)))
			return false;

		fd = open(part, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd < 0 && errno != EEXIST)
			return false;
	}

	if (fd < 0)
		return false;

	FILE *f = fdopen(fd, "wb");
	if (f == NULL)
	{
		close(fd);
		remove(part);
		return false;
	}

	bool ok = (len == 0) || fwrite(data, 1, len, f) == len;
	if (fclose(f) != 0)
		ok = false;

	if (ok && !replace)
	{
		// a new name for the file (link fails if the name is taken, also by a file that came meanwhile)
		if (link(part, path) == 0)
		{
			remove(part);
			return true;
		}

		if (errno == EEXIST || bt2FsExists(path)) // (else no links here: renamed below)
			ok = false;
	}

	if (ok)
		ok = rename(part, path) == 0;

	if (!ok)
		remove(part);

	return ok;
}

bool bt2FsFreeSpace(const char *dir, uint64_t *bytes)
{
#ifdef __EMSCRIPTEN__
	(void)dir;
	(void)bytes;
	return false; // (not known)
#else
	struct statvfs sv;
	if (statvfs(dir, &sv) != 0)
		return false;

	*bytes = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
	return true;
#endif
}

bool bt2FsRemoveFile(const char *path)
{
	return unlink(path) == 0;
}

struct bt2DirIter_t
{
	DIR *d;
	char path[1024];
	char name[1024];
};

bt2DirIter *bt2FsDirOpen(const char *path)
{
	bt2DirIter *d = (bt2DirIter *)calloc(1, sizeof (bt2DirIter));
	if (d == NULL)
		return NULL;

	d->d = opendir(path);
	if (d->d == NULL)
	{
		free(d);
		return NULL;
	}

	snprintf(d->path, sizeof (d->path), "%s", path);
	return d;
}

bool bt2FsDirNext(bt2DirIter *d, const char **name, bool *isDir, int64_t *size)
{
	struct dirent *e;
	while ((e = readdir(d->d)) != NULL)
	{
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;

		snprintf(d->name, sizeof (d->name), "%s", e->d_name);

		char full[2048];
		bt2PathJoin(full, sizeof (full), d->path, e->d_name);
		bt2FileInfo info;
		bt2FsInfo(full, &info);

		*name = d->name;
		if (isDir != NULL) *isDir = info.isDir;
		if (size != NULL) *size = info.size;
		return true;
	}

	return false;
}

void bt2FsDirClose(bt2DirIter *d)
{
	if (d == NULL)
		return;

	closedir(d->d);
	free(d);
}

#endif

bool bt2FsIsDir(const char *path)
{
	bt2FileInfo info;
	return bt2FsInfo(path, &info) && info.isDir;
}

bool bt2FsExists(const char *path)
{
	bt2FileInfo info;
	return bt2FsInfo(path, &info);
}

bool bt2FsMkdirs(const char *path)
{
	char tmp[2048];
	if ((size_t)snprintf(tmp, sizeof (tmp), "%s", path) >= sizeof (tmp))
		return false;

	for (char *p = tmp + 1; *p != '\0'; p++)
	{
		if (isSep(*p) && !isSep(p[-1]))
		{
			const char c = *p;
			*p = '\0';
			if (!bt2FsIsDir(tmp))
				bt2FsMkdir(tmp); // (fails harmlessly for "C:" and the like)
			*p = c;
		}
	}

	return bt2FsMkdir(tmp);
}

bool bt2FsWriteNew(const char *path, const void *data, size_t len)
{
	return writeVia(path, data, len, false);
}

bool bt2FsWriteReplace(const char *path, const void *data, size_t len)
{
	return writeVia(path, data, len, true);
}
