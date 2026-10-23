#pragma once

/* BerntTracker: Disk Op.'s "Imp." - files are copied into the folder you are in (or the BT2 folder),
** the same as the web version:
**
** - never over a file that is there: "song.xm" becomes "song (2).xm"
** - a zip file is unpacked, with its folders, into a new folder with the zip file's name
** - a backup zip (everything under "BT2/", from "Sikkerhetskopi (zip)") puts the files back where they were;
**   files that are already there are left alone (one with other contents comes in as "name (2)")
**
** Paths are UTF-8. Messages (Norwegian, like the web version) go to toast(). An import stops before the disk is
** full (FT2 must still be able to save songs there) and after 4 GB in all.
*/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef void (*bt2ToastFunc)(const char *text);
typedef void (*bt2ImportProgressFunc)(void *ud, double done); // 0..1

/* dir: the folder to put the files in. userRoot: the BT2 folder (for backups). dirIsHere: false when dir isn't the folder
** Disk Op. shows (then the message says "til BT2-mappa"). progress may be NULL. *cancel != 0 stops it (may be NULL).
** count: how many files will come (for the progress). (Can run in a worker thread when toast() and progress() can.)
*/
typedef struct bt2Import_t bt2Import;
bt2Import *bt2ImportBegin(const char *dir, const char *userRoot, bool dirIsHere, bt2ToastFunc toast,
	bt2ImportProgressFunc progress, void *ud, volatile int32_t *cancel, int32_t count);
void bt2ImportData(bt2Import *im, const char *name, const uint8_t *data, size_t len); // a file (data NULL: it couldn't be read)
void bt2ImportCopy(bt2Import *im, const char *relPath, const uint8_t *data, size_t len); // a file from a dropped folder ("a/b.xm")
const char *bt2ImportFirstFile(const bt2Import *im); // the first file written that wasn't in a zip ("" if none)
int32_t bt2ImportEnd(bt2Import *im); // the message, and im is freed. Returns the number of files written.

/* A zip file, one file in it at a time (bt2ImportData does this with bt2_unzip.c; the web version unpacks with the
** browser, so a big zip file never has to be in the program's memory all at once). entries: the zip file's table of
** contents (bt2ZipList's, or the same read by the page), kept until bt2ImportZipEnd. For every entry: bt2ImportZipWants,
** and if true its contents with bt2ImportZipEntry (data NULL: it couldn't be unpacked). NULL = nothing to do (the
** message is given).
*/
struct bt2ZipEntry_t;
typedef struct bt2ImportZip_t bt2ImportZip;
bt2ImportZip *bt2ImportZipBegin(bt2Import *im, const char *zipName, const struct bt2ZipEntry_t *entries, int32_t count);
bool bt2ImportZipWants(bt2ImportZip *z, int32_t i);
void bt2ImportZipEntry(bt2ImportZip *z, int32_t i, const uint8_t *data, size_t len);
void bt2ImportZipEnd(bt2ImportZip *z); // (z is freed)

// the same for files on the disk (the Windows program's Imp.)
int32_t bt2ImportFiles(const char *const *paths, int32_t count, const char *dir, const char *userRoot, bool dirIsHere,
	bt2ToastFunc toast, bt2ImportProgressFunc progress, void *ud, volatile int32_t *cancel);
