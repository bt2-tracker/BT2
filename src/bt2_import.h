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

typedef void (*bt2ToastFunc)(const char *text);
typedef void (*bt2ImportProgressFunc)(void *ud, double done); // 0..1

// paths: the files to import. dir: the folder to put them in. userRoot: the BT2 folder (for backups).
// dirIsHere: false when dir isn't the folder Disk Op. shows (then the message says "til BT2-mappa").
// progress may be NULL. *cancel != 0 stops it (may be NULL). Returns the number of files written.
// (Can run in a worker thread when toast() and progress() can.)
int32_t bt2ImportFiles(const char *const *paths, int32_t count, const char *dir, const char *userRoot, bool dirIsHere,
	bt2ToastFunc toast, bt2ImportProgressFunc progress, void *ud, volatile int32_t *cancel);
