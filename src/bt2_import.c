/* BerntTracker: Disk Op.'s "Imp.", the same as the web version's importFiles(). See bt2_import.h.
**
** Only new files are written (bt2FsWriteNew), except an empty file (a demo song that was never opened)
** that a backup has the contents for. An import stops before the disk is full (FT2 must still be able to save
** songs) and after 4 GB (a zip file can hold much more than it looks like).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bt2_import.h"
#include "bt2_unzip.h"
#include "bt2_fs.h"

#define MAX_IMPORT_SIZE (1024u * 1024 * 1024)         // one file
#define MAX_IMPORT_TOTAL (4ull * 1024 * 1024 * 1024)  // all files in one import
#define DISK_RESERVE (256ull * 1024 * 1024)           // always left free on the disk
#define PATH_LEN 2048

typedef struct importCtx_t
{
	bt2ToastFunc toast;
	bt2ImportProgressFunc progress;
	void *ud;
	volatile int32_t *cancel;
	uint64_t written;
	int32_t stop; // 0, STOP_FULL, STOP_TOO_MUCH, STOP_CANCEL
	int32_t file, count;
} importCtx;

enum { STOP_FULL = 1, STOP_TOO_MUCH, STOP_CANCEL };

static bool stopped(importCtx *c)
{
	if (c->stop == 0 && c->cancel != NULL && *c->cancel != 0)
		c->stop = STOP_CANCEL; // (the program is being closed)

	return c->stop != 0;
}

// room for len more bytes in dir (else the import stops)
static bool room(importCtx *c, const char *dir, size_t len)
{
	if (stopped(c))
		return false;

	if (c->written + len > MAX_IMPORT_TOTAL)
	{
		c->stop = STOP_TOO_MUCH;
		return false;
	}

	uint64_t avail;
	if (bt2FsFreeSpace(dir, &avail) && avail < (uint64_t)len + DISK_RESERVE)
	{
		c->stop = STOP_FULL;
		return false;
	}

	return true;
}

static void progressTo(importCtx *c, double inFile) // inFile: 0..1 of the file being imported now
{
	if (c->progress != NULL && c->count > 0)
		c->progress(c->ud, (c->file + inFile) / c->count);
}

static const char *baseName(const char *path)
{
	const char *b = path;
	for (const char *p = path; *p != '\0'; p++)
	{
		if (*p == '/' || *p == BT2_SEP)
			b = p + 1;
	}

	return b;
}

static bool endsWithNoCase(const char *s, const char *ending)
{
	const size_t a = strlen(s), b = strlen(ending);
	if (a < b)
		return false;

	for (size_t i = 0; i < b; i++)
	{
		char x = s[a - b + i], y = ending[i];
		if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
		if (x != y)
			return false;
	}

	return true;
}

static bool partIs(const char *p, size_t len, const char *name)
{
	if (strlen(name) != len)
		return false;

	for (size_t i = 0; i < len; i++)
	{
		char x = p[i], y = name[i];
		if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
		if (x != y)
			return false;
	}

	return true;
}

// files from other systems that nobody wants (Mac and Windows leftovers)
static bool isJunk(const char *name)
{
	const char *p = name;
	while (*p != '\0')
	{
		const char *e = strchr(p, '/');
		const size_t len = (e != NULL) ? (size_t)(e - p) : strlen(p);
		if (partIs(p, len, "__macosx") || partIs(p, len, ".ds_store") || partIs(p, len, "thumbs.db"))
			return true;

		if (e == NULL)
			break;
		p = e + 1;
	}

	return false;
}

// a zip entry's name in UTF-8 (old zip programs used the Windows letters)
static void entryName(const bt2ZipEntry *e, char *out, size_t outLen)
{
	if (bt2Utf8Valid(e->name, strlen(e->name)))
		snprintf(out, outLen, "%s", e->name);
	else
		bt2Cp1252ToUtf8(e->name, out, outLen);
}

#define MAX_NAME_LETTERS 200 // in one name (counted like the web version always did: UTF-16 units)
#define NAME_LEN 1024        // (room for 200 letters in UTF-8, and a '_' in front)

// the bytes in the first maxUnits letters of s (whole letters; one outside the BMP counts as 2), and how many there were
static size_t lettersPrefix(const char *s, size_t maxUnits, size_t *units)
{
	const uint8_t *p = (const uint8_t *)s;
	size_t i = 0, u = 0;
	while (p[i] != '\0')
	{
		size_t n = 1;
		if (p[i] >= 0xC0)
		{
			while (n < 4 && (p[i + n] & 0xC0) == 0x80)
				n++;
		}

		const size_t w = (n == 4) ? 2 : 1;
		if (u + w > maxUnits)
			break;

		u += w;
		i += n;
	}

	if (units != NULL)
		*units = u;
	return i;
}

/* A name from outside (a file, a zip file's folder or file) made safe here (bt2CleanName). A name with more than 200
** letters is shortened; a file keeps its ending (".xm"), so FT2 still knows what it is.
*/
static void importName(const char *raw, bool isFile, char *out, size_t outLen)
{
	char cut[NAME_LEN];
	size_t units = 0;
	lettersPrefix(raw, (size_t)-1, &units);

	if (units <= MAX_NAME_LETTERS)
	{
		snprintf(cut, sizeof (cut), "%s", raw);
	}
	else
	{
		const char *dot = isFile ? strrchr(raw, '.') : NULL;
		size_t endUnits = 0;
		if (dot != NULL && (dot == raw || strlen(dot) > 16))
			dot = NULL; // (no ending, or not one)
		if (dot != NULL)
			lettersPrefix(dot, (size_t)-1, &endUnits);

		const size_t stem = lettersPrefix(raw, MAX_NAME_LETTERS - endUnits, NULL);
		snprintf(cut, sizeof (cut), "%.*s%s", (int)stem, raw, (dot != NULL) ? dot : "");
	}

	bt2CleanName(cut, out, outLen, outLen);
}

// "a/b/c.xm" -> "a\b\c.xm" here, every part made safe; empty parts, "." and ".." are left out. false = nothing left
static bool safeRelPath(const char *name, char *out, size_t outLen)
{
	size_t o = 0;
	out[0] = '\0';

	const char *p = name;
	while (*p != '\0')
	{
		const char *e = strchr(p, '/');
		const size_t len = (e != NULL) ? (size_t)(e - p) : strlen(p);

		if (len > 0 && !(len == 1 && p[0] == '.') && !(len == 2 && p[0] == '.' && p[1] == '.') && len < NAME_LEN)
		{
			char raw[NAME_LEN], clean[NAME_LEN];
			memcpy(raw, p, len);
			raw[len] = '\0';
			importName(raw, e == NULL, clean, sizeof (clean)); // (the last part is the file)

			const int32_t w = snprintf(out + o, outLen - o, "%s%s", (o > 0) ? BT2_SEP_STR : "", clean);
			if (w < 0 || (size_t)w >= outLen - o)
				return false;
			o += (size_t)w;
		}

		if (e == NULL)
			break;
		p = e + 1;
	}

	return o > 0;
}

static void makeParentDirs(const char *path)
{
	char dir[PATH_LEN];
	snprintf(dir, sizeof (dir), "%s", path);

	char *cut = NULL;
	for (char *p = dir; *p != '\0'; p++)
	{
		if (*p == '/' || *p == BT2_SEP)
			cut = p;
	}

	if (cut != NULL)
	{
		*cut = '\0';
		bt2FsMkdirs(dir);
	}
}

static bool sameContents(const char *path, const uint8_t *data, size_t len)
{
	size_t curLen = 0;
	uint8_t *cur = bt2FsReadAll(path, &curLen, MAX_IMPORT_SIZE);
	const bool same = (cur != NULL && curLen == len && memcmp(cur, data, len) == 0);
	free(cur);
	return same;
}

// "folder/song.xm" is taken: the first free "folder/song (N).xm" in out (true), or false if one of
// "song (2).xm", "song (3).xm" ... before it has exactly these contents
static bool sameCopyOrFreeName(const char *path, const uint8_t *data, size_t len, char *out, size_t outLen)
{
	const char *slash = NULL;
	for (const char *p = path; *p != '\0'; p++)
	{
		if (*p == '/' || *p == BT2_SEP)
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

		bt2FileInfo info;
		if (!bt2FsInfo(out, &info))
			return true; // (free)

		if (!info.isDir && (int64_t)len == info.size && sameContents(out, data, len))
			return false;
	}

	return false;
}

static bool isBackup(const bt2ZipEntry *entries, int32_t count)
{
	for (int32_t i = 0; i < count; i++)
	{
		if (strncmp(entries[i].name, "BT2/", 4) != 0 && strncmp(entries[i].name, "Innstillinger/", 14) != 0)
			return false;
	}

	return count > 0;
}

struct bt2Import_t
{
	importCtx c;
	char dir[PATH_LEN], userRoot[PATH_LEN], firstFile[PATH_LEN];
	bool dirIsHere;
	int32_t n, zips, backups, errors, given, copiesGiven;
};

struct bt2ImportZip_t
{
	bt2Import *im;
	const bt2ZipEntry *entries;
	int32_t count;
	bool backup;
	bool broken;                         // (unpacking: the rest of this zip file is not unpacked, like the web version always did)
	int32_t restored, failed;            // (a backup)
	char name[NAME_LEN], base[PATH_LEN]; // the zip file's name, and the new folder it is unpacked in
};

bt2Import *bt2ImportBegin(const char *dir, const char *userRoot, bool dirIsHere, bt2ToastFunc toast,
	bt2ImportProgressFunc progress, void *ud, volatile int32_t *cancel, int32_t count)
{
	bt2Import *im = (bt2Import *)calloc(1, sizeof (bt2Import));
	if (im == NULL)
		return NULL;

	im->c.toast = toast;
	im->c.progress = progress;
	im->c.ud = ud;
	im->c.cancel = cancel;
	im->c.count = count;
	snprintf(im->dir, sizeof (im->dir), "%s", dir);
	snprintf(im->userRoot, sizeof (im->userRoot), "%s", userRoot);
	im->dirIsHere = dirIsHere;
	return im;
}

static void cantImport(bt2Import *im, const char *name)
{
	char msg[1200];
	snprintf(msg, sizeof (msg), "Kunne ikke importere %s", name);
	im->c.toast(msg);
	im->errors++;
}

bt2ImportZip *bt2ImportZipBegin(bt2Import *im, const char *zipName, const bt2ZipEntry *entries, int32_t count)
{
	if (im == NULL)
		return NULL;

	importCtx *c = &im->c;
	zipName = baseName(zipName);
	c->file = im->given++;

	if (stopped(c))
		return NULL;

	progressTo(c, 0.0);

	bt2ImportZip *z = (entries != NULL && count > 0) ? (bt2ImportZip *)calloc(1, sizeof (bt2ImportZip)) : NULL;
	if (z == NULL)
	{
		cantImport(im, zipName); // (not a zip file we can read)
		return NULL;
	}

	z->im = im;
	z->entries = entries;
	z->count = count;
	snprintf(z->name, sizeof (z->name), "%s", zipName);

	// a backup (everything under "BT2/", from "Sikkerhetskopi (zip)"): the files back where they were
	z->backup = isBackup(entries, count);
	if (z->backup)
	{
		im->backups++;
	}
	else
	{
		// else unpacked, with its folders, into a new folder with the zip file's name
		im->zips++;

		char folderName[NAME_LEN], clean[NAME_LEN], tmp[PATH_LEN];
		const size_t len = strlen(zipName);
		snprintf(folderName, sizeof (folderName), "%.*s", (int)(endsWithNoCase(zipName, ".zip") ? len - 4 : len), zipName);
		importName(folderName, false, clean, sizeof (clean));
		bt2PathJoin(tmp, sizeof (tmp), im->dir, clean);
		if (!bt2UniquePath(tmp, z->base, sizeof (z->base)))
			z->broken = true;
	}

	return z;
}

bool bt2ImportZipWants(bt2ImportZip *z, int32_t i)
{
	if (z == NULL || i < 0 || i >= z->count || z->broken)
		return false;

	importCtx *c = &z->im->c;
	if (stopped(c))
		return false;

	progressTo(c, (double)i / z->count);

	const bt2ZipEntry *e = &z->entries[i];
	char name[NAME_LEN], rel[PATH_LEN];
	entryName(e, name, sizeof (name));

	if (z->backup)
	{
		// (the settings, "Innstillinger/", are not put back)
		if (e->dir || strncmp(e->name, "BT2/", 4) != 0 || !safeRelPath(name + 4, rel, sizeof (rel)))
			return false;

		if (e->size > MAX_IMPORT_SIZE)
		{
			z->failed++;
			return false;
		}

		return true;
	}

	if (e->dir || isJunk(name) || !safeRelPath(name, rel, sizeof (rel)))
		return false;

	if (e->size > MAX_IMPORT_SIZE)
	{
		z->broken = true; // (like the web version: the rest of this zip file is not unpacked)
		return false;
	}

	return room(c, z->im->dir, e->size); // (else the import stops)
}

// a backup: the file back where it was. Files that are there are left alone (one with other contents comes in as "name (2)").
static void restoreEntry(bt2ImportZip *z, const bt2ZipEntry *e, const uint8_t *content, size_t len)
{
	bt2Import *im = z->im;
	importCtx *c = &im->c;

	char name[NAME_LEN], rel[PATH_LEN], dest[PATH_LEN];
	entryName(e, name, sizeof (name));
	if (!safeRelPath(name + 4, rel, sizeof (rel)))
		return;

	bt2PathJoin(dest, sizeof (dest), im->userRoot, rel);

	if (content == NULL)
	{
		z->failed++;
		return;
	}

	bt2FileInfo info;
	if (bt2FsInfo(dest, &info))
	{
		if (!info.isDir && info.size == (int64_t)len && sameContents(dest, content, len))
			return; // (the same file)

		if (!info.isDir && info.size == 0) // an empty file (a demo song that was never opened)
		{
			if (!room(c, im->userRoot, len))
				return;

			if (bt2FsWriteReplace(dest, content, len))
			{
				z->restored++;
				c->written += len;
			}
			else
			{
				z->failed++;
			}
			return;
		}

		// a free "name (2)" ... - unless one of those already has these contents (the same backup again)
		char unique[PATH_LEN];
		if (!sameCopyOrFreeName(dest, content, len, unique, sizeof (unique)))
			return; // (it is there)
		snprintf(dest, sizeof (dest), "%s", unique);
	}

	if (!room(c, im->userRoot, len))
		return;

	makeParentDirs(dest);
	if (bt2FsWriteNew(dest, content, len))
	{
		z->restored++;
		c->written += len;
	}
	else
	{
		z->failed++;
	}
}

// a zip file that isn't a backup: the file, with its folders, in the new folder (never over a file that is there)
static void unpackEntry(bt2ImportZip *z, const bt2ZipEntry *e, const uint8_t *content, size_t len)
{
	bt2Import *im = z->im;

	char name[NAME_LEN], rel[PATH_LEN], tmp[PATH_LEN], dest[PATH_LEN];
	entryName(e, name, sizeof (name));
	if (!safeRelPath(name, rel, sizeof (rel)))
		return;

	bt2PathJoin(tmp, sizeof (tmp), z->base, rel);
	if (content == NULL || !bt2UniquePath(tmp, dest, sizeof (dest)))
	{
		z->broken = true; // (like the web version: the rest of this zip file is not unpacked)
		return;
	}

	makeParentDirs(dest);
	if (!bt2FsWriteNew(dest, content, len))
	{
		z->broken = true;
		return;
	}

	im->n++;
	im->c.written += len;
}

void bt2ImportZipEntry(bt2ImportZip *z, int32_t i, const uint8_t *data, size_t len)
{
	if (z == NULL || i < 0 || i >= z->count || z->broken || stopped(&z->im->c))
		return;

	if (z->backup)
		restoreEntry(z, &z->entries[i], data, len);
	else
		unpackEntry(z, &z->entries[i], data, len);
}

void bt2ImportZipEnd(bt2ImportZip *z)
{
	if (z == NULL)
		return;

	bt2Import *im = z->im;
	importCtx *c = &im->c;

	if (z->backup)
	{
		im->n += z->restored;
		if (c->stop == 0)
		{
			char msg[256];
			const char *files = (z->restored == 1) ? "fil" : "filer";
			if (z->failed > 0)
				snprintf(msg, sizeof (msg), "Sikkerhetskopi: %d %s lagt tilbake i BT2 (%d kunne ikke leses)", (int)z->restored, files, (int)z->failed);
			else
				snprintf(msg, sizeof (msg), "Sikkerhetskopi: %d %s lagt tilbake i BT2", (int)z->restored, files);
			c->toast(msg);
		}
	}
	else if (z->broken)
	{
		cantImport(im, z->name);
	}

	free(z);
}

void bt2ImportData(bt2Import *im, const char *name, const uint8_t *data, size_t len)
{
	if (im == NULL)
		return;

	name = baseName(name);

	// a zip file: one file in it at a time
	if (data != NULL && len <= MAX_IMPORT_SIZE && endsWithNoCase(name, ".zip"))
	{
		int32_t count = 0;
		bt2ZipEntry *entries = bt2ZipList(data, len, &count);
		bt2ImportZip *z = bt2ImportZipBegin(im, name, entries, count);
		for (int32_t i = 0; z != NULL && i < count; i++)
		{
			if (!bt2ImportZipWants(z, i))
				continue;

			size_t elen = 0;
			uint8_t *content = bt2ZipExtract(data, len, &entries[i], &elen);
			bt2ImportZipEntry(z, i, content, elen);
			free(content);
		}

		bt2ImportZipEnd(z);
		free(entries);
		return;
	}

	importCtx *c = &im->c;
	c->file = im->given++;

	if (stopped(c))
		return;

	progressTo(c, 0.0);

	if (data == NULL || len > MAX_IMPORT_SIZE)
	{
		cantImport(im, name);
		return;
	}

	if (!room(c, im->dir, len))
		return;

	char clean[NAME_LEN], tmp[PATH_LEN], dest[PATH_LEN];
	importName(name, true, clean, sizeof (clean));
	bt2PathJoin(tmp, sizeof (tmp), im->dir, clean);

	if (bt2UniquePath(tmp, dest, sizeof (dest)) && bt2FsWriteNew(dest, data, len))
	{
		im->n++;
		c->written += len;
		if (im->firstFile[0] == '\0')
			snprintf(im->firstFile, sizeof (im->firstFile), "%s", dest);
	}
	else
	{
		cantImport(im, name);
	}
}

void bt2ImportCopy(bt2Import *im, const char *relPath, const uint8_t *data, size_t len)
{
	if (im == NULL)
		return;

	importCtx *c = &im->c;
	c->file = im->given++;
	im->copiesGiven++;

	if (stopped(c))
		return;

	progressTo(c, 0.0);

	// the folders from the dropped folder, every part made safe ("a/b/c.xm"); never over a file that is there
	char rel[PATH_LEN], tmp[PATH_LEN], dest[PATH_LEN];
	if (data == NULL || len > MAX_IMPORT_SIZE || !safeRelPath(relPath, rel, sizeof (rel)))
	{
		cantImport(im, baseName(relPath));
		return;
	}

	if (!room(c, im->dir, len))
		return;

	bt2PathJoin(tmp, sizeof (tmp), im->dir, rel);
	if (bt2UniquePath(tmp, dest, sizeof (dest)))
	{
		makeParentDirs(dest);
		if (bt2FsWriteNew(dest, data, len))
		{
			im->n++;
			c->written += len;
			if (im->firstFile[0] == '\0')
				snprintf(im->firstFile, sizeof (im->firstFile), "%s", dest);
			return;
		}
	}

	cantImport(im, baseName(relPath));
}

const char *bt2ImportFirstFile(const bt2Import *im)
{
	return (im != NULL) ? im->firstFile : "";
}

int32_t bt2ImportEnd(bt2Import *im)
{
	if (im == NULL)
		return 0;

	importCtx *c = &im->c;
	const int32_t n = im->n;
	if (c->progress != NULL)
		c->progress(c->ud, 1.0);

	char msg[256];
	const char *files = (n == 1) ? "fil" : "filer";

	// stopped: why, and how far it came
	if (c->stop == STOP_FULL || c->stop == STOP_TOO_MUCH)
	{
		snprintf(msg, sizeof (msg), "Importen ble stoppet: %s (importerte %d %s)",
			(c->stop == STOP_FULL) ? "ikke nok plass p\xC3\xA5 disken" : "mer enn 4 GB", (int)n, files);
		c->toast(msg);
	}
	else if (c->stop == STOP_CANCEL || (n == 0 && im->errors > 0) ||
		(im->backups > 0 && im->backups == im->given - im->copiesGiven && im->copiesGiven == 0))
	{
		// (no summary: it would only hide the message above - backups, or nothing imported because of an error)
	}
	else if (im->copiesGiven == im->given)
	{
		snprintf(msg, sizeof (msg), "Kopierte %d %s", (int)n, files); // (a dropped folder)
		c->toast(msg);
	}
	else
	{
		snprintf(msg, sizeof (msg), "Importerte %d %s%s%s", (int)n, files, im->zips ? " (pakket ut zip)" : "",
			im->dirIsHere ? "" : " til BT2-mappa");
		c->toast(msg);
	}

	free(im);
	return n;
}

int32_t bt2ImportFiles(const char *const *paths, int32_t count, const char *dir, const char *userRoot, bool dirIsHere,
	bt2ToastFunc toast, bt2ImportProgressFunc progress, void *ud, volatile int32_t *cancel)
{
	bt2Import *im = bt2ImportBegin(dir, userRoot, dirIsHere, toast, progress, ud, cancel, count);
	if (im == NULL)
		return 0;

	for (int32_t f = 0; f < count && !stopped(&im->c); f++)
	{
		size_t len = 0;
		uint8_t *data = bt2FsReadAll(paths[f], &len, MAX_IMPORT_SIZE);
		bt2ImportData(im, paths[f], data, len);
		free(data);
	}

	return bt2ImportEnd(im);
}
