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

		if (len > 0 && !(len == 1 && p[0] == '.') && !(len == 2 && p[0] == '.' && p[1] == '.') && len < 1024)
		{
			char raw[1024], clean[256];
			memcpy(raw, p, len);
			raw[len] = '\0';
			bt2CleanName(raw, clean, sizeof (clean), 200);

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

// a backup zip: the files back where they were. Returns the number put back.
static int32_t restoreBackup(importCtx *c, const uint8_t *zip, size_t zipLen, const bt2ZipEntry *entries, int32_t count, const char *userRoot)
{
	int32_t restored = 0, failed = 0;

	for (int32_t i = 0; i < count && !stopped(c); i++)
	{
		const bt2ZipEntry *e = &entries[i];
		progressTo(c, (double)i / count);
		if (e->dir || strncmp(e->name, "BT2/", 4) != 0) // (the settings are not put back)
			continue;

		char name[1024], rel[PATH_LEN], dest[PATH_LEN];
		entryName(e, name, sizeof (name));
		if (!safeRelPath(name + 4, rel, sizeof (rel)))
			continue;

		bt2PathJoin(dest, sizeof (dest), userRoot, rel);

		if (e->size > MAX_IMPORT_SIZE)
		{
			failed++;
			continue;
		}

		size_t len = 0;
		uint8_t *content = bt2ZipExtract(zip, zipLen, e, &len);
		if (content == NULL)
		{
			failed++;
			continue;
		}

		bool done = false;
		bt2FileInfo info;
		if (bt2FsInfo(dest, &info))
		{
			if (!info.isDir && info.size == (int64_t)len && sameContents(dest, content, len))
			{
				free(content);
				continue; // (the same file)
			}

			if (!info.isDir && info.size == 0) // an empty file (a demo song that was never opened)
			{
				if (!room(c, userRoot, len))
				{
					free(content);
					break;
				}

				if (bt2FsWriteReplace(dest, content, len))
				{
					restored++;
					c->written += len;
				}
				else
				{
					failed++;
				}
				done = true;
			}
			else
			{
				// a free "name (2)" ... - unless one of those already has these contents (the same backup again)
				char unique[PATH_LEN];
				if (!sameCopyOrFreeName(dest, content, len, unique, sizeof (unique)))
				{
					free(content);
					continue; // (it is there)
				}
				snprintf(dest, sizeof (dest), "%s", unique);
			}
		}

		if (!done)
		{
			if (!room(c, userRoot, len))
			{
				free(content);
				break;
			}

			makeParentDirs(dest);
			if (bt2FsWriteNew(dest, content, len))
			{
				restored++;
				c->written += len;
			}
			else
			{
				failed++;
			}
		}

		free(content);
	}

	if (c->stop == 0)
	{
		char msg[256];
		const char *files = (restored == 1) ? "fil" : "filer";
		if (failed > 0)
			snprintf(msg, sizeof (msg), "Sikkerhetskopi: %d %s lagt tilbake i BT2 (%d kunne ikke leses)", (int)restored, files, (int)failed);
		else
			snprintf(msg, sizeof (msg), "Sikkerhetskopi: %d %s lagt tilbake i BT2", (int)restored, files);
		c->toast(msg);
	}

	return restored;
}

int32_t bt2ImportFiles(const char *const *paths, int32_t count, const char *dir, const char *userRoot, bool dirIsHere,
	bt2ToastFunc toast, bt2ImportProgressFunc progress, void *ud, volatile int32_t *cancel)
{
	importCtx c;
	memset(&c, 0, sizeof (c));
	c.toast = toast;
	c.progress = progress;
	c.ud = ud;
	c.cancel = cancel;
	c.count = count;

	int32_t n = 0, zips = 0, backups = 0, errors = 0;

	for (int32_t f = 0; f < count && !stopped(&c); f++)
	{
		const char *name = baseName(paths[f]);
		char msg[1200];
		c.file = f;
		progressTo(&c, 0.0);

		size_t len = 0;
		uint8_t *data = bt2FsReadAll(paths[f], &len, MAX_IMPORT_SIZE);
		if (data == NULL)
		{
			snprintf(msg, sizeof (msg), "Kunne ikke importere %s", name);
			toast(msg);
			errors++;
			continue;
		}

		bool failed = false;
		if (endsWithNoCase(name, ".zip"))
		{
			int32_t entryCount = 0;
			bt2ZipEntry *entries = bt2ZipList(data, len, &entryCount);
			if (entries == NULL)
			{
				failed = true;
			}
			else if (isBackup(entries, entryCount))
			{
				n += restoreBackup(&c, data, len, entries, entryCount, userRoot);
				backups++;
			}
			else
			{
				// unpacked, with its folders, into a new folder with the zip file's name
				char folderName[1024], clean[256], base[PATH_LEN], tmp[PATH_LEN];
				snprintf(folderName, sizeof (folderName), "%.*s", (int)(strlen(name) - 4), name);
				bt2CleanName(folderName, clean, sizeof (clean), 200);
				bt2PathJoin(tmp, sizeof (tmp), dir, clean);
				if (!bt2UniquePath(tmp, base, sizeof (base)))
					failed = true;

				for (int32_t i = 0; i < entryCount && !failed && !stopped(&c); i++)
				{
					const bt2ZipEntry *e = &entries[i];
					progressTo(&c, (double)i / entryCount);

					char ename[1024], rel[PATH_LEN], dest[PATH_LEN];
					entryName(e, ename, sizeof (ename));
					if (e->dir || isJunk(ename) || !safeRelPath(ename, rel, sizeof (rel)))
						continue;

					if (e->size > MAX_IMPORT_SIZE)
					{
						failed = true; // (like the web version: the rest of this zip file is not unpacked)
						break;
					}

					if (!room(&c, dir, e->size))
						break;

					bt2PathJoin(tmp, sizeof (tmp), base, rel);
					if (!bt2UniquePath(tmp, dest, sizeof (dest)))
					{
						failed = true;
						break;
					}

					size_t elen = 0;
					uint8_t *content = bt2ZipExtract(data, len, e, &elen);
					if (content == NULL)
					{
						failed = true; // (like the web version: the rest of this zip file is not unpacked)
						break;
					}

					makeParentDirs(dest);
					if (bt2FsWriteNew(dest, content, elen))
					{
						n++;
						c.written += elen;
					}
					else
					{
						failed = true;
					}
					free(content);
				}

				zips++;
			}

			free(entries);
		}
		else if (room(&c, dir, len))
		{
			char clean[256], tmp[PATH_LEN], dest[PATH_LEN];
			bt2CleanName(name, clean, sizeof (clean), 200);
			bt2PathJoin(tmp, sizeof (tmp), dir, clean);

			if (bt2UniquePath(tmp, dest, sizeof (dest)) && bt2FsWriteNew(dest, data, len))
			{
				n++;
				c.written += len;
			}
			else
			{
				failed = true;
			}
		}

		free(data);

		if (failed)
		{
			snprintf(msg, sizeof (msg), "Kunne ikke importere %s", name);
			toast(msg);
			errors++;
		}
	}

	if (progress != NULL)
		progress(ud, 1.0);

	char msg[256];
	const char *files = (n == 1) ? "fil" : "filer";

	// stopped: why, and how far it came
	if (c.stop == STOP_FULL || c.stop == STOP_TOO_MUCH)
	{
		snprintf(msg, sizeof (msg), "Importen ble stoppet: %s (importerte %d %s)",
			(c.stop == STOP_FULL) ? "ikke nok plass p\xC3\xA5 disken" : "mer enn 4 GB", (int)n, files);
		toast(msg);
		return n;
	}

	// the summary (not when it would only hide the message above: backups, or nothing imported because of an error)
	if (c.stop == STOP_CANCEL || backups == count || (n == 0 && errors > 0))
		return n;

	snprintf(msg, sizeof (msg), "Importerte %d %s%s%s", (int)n, files, zips ? " (pakket ut zip)" : "", dirIsHere ? "" : " til BT2-mappa");
	toast(msg);

	return n;
}
