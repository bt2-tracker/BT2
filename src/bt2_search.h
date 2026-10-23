#pragma once

/* BerntTracker: what the search window finds (Disk Op.'s "Find", Ctrl+F), the same rules as the web
** version's search window (BT2.search in index.html): songs, folders, instruments and samples in the
** user's own files (the BT2 folder) and in Modland.
**
** Typing filters at once: every word must be in the path. Best first: all words in the name itself,
** the name starting with the first word, folders, and (with nothing typed) the user's own files.
*/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum
{
	BT2_KIND_FOLDER = 0,
	BT2_KIND_SONG,
	BT2_KIND_SOUND, // instruments and samples
	BT2_KIND_OTHER
};

enum { BT2_WHERE_ALL = 0, BT2_WHERE_OWN, BT2_WHERE_MODLAND };
enum { BT2_WHAT_ALL = 0, BT2_WHAT_SONG, BT2_WHAT_FOLDER, BT2_WHAT_SOUND };

typedef struct bt2SearchItem_t
{
	const char *rel;     // "Artist/song.xm": the path in the BT2 folder or in Modland ('/' between the parts)
	const char *low;     // the same in small letters
	const char *name;    // the last part of rel
	const char *lowName;
	int32_t dirLen;      // the folder part of rel ("Artist"), 0 = the top
	int64_t size;        // files
	int32_t count;       // Modland's folders: the number of songs in them
	uint8_t kind;        // BT2_KIND_...
	bool modland;
	int32_t rank;
} bt2SearchItem;

typedef struct bt2SearchPool_t
{
	bt2SearchItem *items;
	int32_t count, cap;
	char **blocks; // (the texts)
	int32_t numBlocks;
	size_t blockUsed, blockSize;
} bt2SearchPool;

uint8_t bt2SearchKindOf(const char *name); // by the file name's ending

void bt2SearchBuildOwn(bt2SearchPool *pool, const char *userRoot); // walks the BT2 folder
void bt2SearchBuildModland(bt2SearchPool *pool);                   // from Modland's list (bt2_modland.c)
void bt2SearchFreePool(bt2SearchPool *pool);

// the results, best first (*results: malloc'ed list of pointers into the pools). Returns how many.
int32_t bt2SearchRun(const char *query, int32_t where, int32_t what, const bt2SearchPool *own, const bt2SearchPool *mod,
	bt2SearchItem ***results);

void bt2Utf8Lower(const char *in, char *out, size_t outLen); // small letters (also æøå, é, ö ...)

// Disk Op.'s kinds of files (the same numbers as DISKOP_ITEM_...)
enum { BT2_ITEM_MODULE = 0, BT2_ITEM_INSTR = 1, BT2_ITEM_SAMPLE = 2 };

/* Where a hit leads: its folder here (Modland's folders are made and filled) and, for a song, instrument or sample,
** the file. *diskOpItem: the kind of files Disk Op. shows for it. false = the folder or file isn't there.
** (The same in the Windows program and the web version.)
*/
bool bt2SearchTarget(const bt2SearchItem *it, int32_t what, const char *userRoot, char *folder, size_t folderLen,
	char *file, size_t fileLen, int32_t *diskOpItem);
