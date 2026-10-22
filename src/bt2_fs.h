#pragma once

/* BerntTracker: small file helpers with UTF-8 paths, the same calls on every system (the Windows
** program uses Windows' wide-character functions underneath, so "æøå" and other letters work).
** On Windows both '/' and '\' can be used in paths.
**
** Nothing here overwrites a file unless the name of the function says so (bt2FsWriteReplace).
*/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#define BT2_SEP '\\'
#define BT2_SEP_STR "\\"
#else
#define BT2_SEP '/'
#define BT2_SEP_STR "/"
#endif

typedef struct bt2FileInfo_t
{
	bool exists, isDir;
	int64_t size;
	int64_t mtime; // seconds since 1970
} bt2FileInfo;

bool bt2FsInfo(const char *path, bt2FileInfo *info); // false (and exists = false) if there is nothing there
bool bt2FsIsDir(const char *path);
bool bt2FsExists(const char *path);
FILE *bt2FsOpen(const char *path, const char *mode);

bool bt2FsMkdir(const char *path);  // true if the folder is there afterwards
bool bt2FsMkdirs(const char *path); // like "mkdir -p"

bool bt2FsCreateEmpty(const char *path); // a new, empty file; never touches one that is there (true = made)

// the whole file (malloc'ed, with an extra 0 byte at the end); NULL if missing, unreadable or bigger than maxLen
uint8_t *bt2FsReadAll(const char *path, size_t *len, size_t maxLen);

// Writes "<path>.bt2part" first (a new file) and renames it when everything is written, so a file is never half
// written, and a file that is there is never written over (except by bt2FsWriteReplace).
bool bt2FsWriteNew(const char *path, const void *data, size_t len);     // only if there is no such file
bool bt2FsWriteReplace(const char *path, const void *data, size_t len); // replaces the file if it is there

bool bt2FsRemoveFile(const char *path); // one file (never a folder)

bool bt2FsFreeSpace(const char *dir, uint64_t *bytes); // free bytes on the disk the folder is on (false = not known)

typedef struct bt2DirIter_t bt2DirIter;
bt2DirIter *bt2FsDirOpen(const char *path);
bool bt2FsDirNext(bt2DirIter *d, const char **name, bool *isDir, int64_t *size); // skips "." and ".."
void bt2FsDirClose(bt2DirIter *d);

// path helpers (UTF-8)
void bt2PathJoin(char *out, size_t outLen, const char *a, const char *b); // a + separator + b
bool bt2PathInside(const char *path, const char *root); // path is root or inside it (on Windows: any case, '/' = '\')
const char *bt2PathAfter(const char *path, const char *root); // the part after "root\" ("" for root itself), or NULL

// a file or folder name every system accepts: no \ / : * ? " < > | or control characters, no dots at the start,
// on Windows no space/dot at the end and no device names (CON, NUL ...). At most maxBytes bytes (whole letters).
void bt2CleanName(const char *name, char *out, size_t outLen, size_t maxBytes);

// Windows' device names (CON, NUL, COM1 ..., also "nul.xm" and "con .xm"): such a "file" is a device
bool bt2FsIsDeviceName(const char *name);

// "song.xm" -> "song (2).xm" (then 3, 4 ...) when the name is taken
bool bt2UniquePath(const char *path, char *out, size_t outLen);

bool bt2Utf8Valid(const char *s, size_t len);
void bt2Cp1252ToUtf8(const char *in, char *out, size_t outLen); // old zip programs' names

#ifdef _WIN32
#include <wchar.h>
// UTF-8 <-> UTF-16 (malloc'ed; '/' becomes '\' in bt2Wide)
wchar_t *bt2Wide(const char *utf8);
char *bt2Utf8(const wchar_t *wide);
#endif
