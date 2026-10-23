#pragma once

/* BerntTracker: Modland (https://modland.com, its "Fasttracker 2" folder) as folders in Disk Op.,
** the same as in the web version:
**
** - The list of songs comes from modland.com/allmods.zip the first time (only the Fasttracker 2 lines
**   are kept, "size<TAB>Artist/song.xm"). It is saved and used without internet after that, and
**   quietly fetched again in the background when it is more than 30 days old (used the next time).
** - The folders are made as they are opened in Disk Op. Every song is an empty file until it is opened.
**   Then it is downloaded and kept as the user's own copy in BT2/Modland/<the same folders>, which is
**   used from then on (also without internet).
** - Modland's folders are read-only: nothing can be saved, renamed or deleted there.
**
** Paths are UTF-8. On Windows the names are made safe for Windows (bt2CleanName), and names that would be
** the same file there ("Song.xm" and "song.xm") get a number ("song (2).xm").
*/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bt2_net.h"

#define BT2_MODLAND_LIST_URL "https://modland.com/allmods.zip"
#define BT2_MODLAND_FILE_URL "https://modland.com/pub/modules/Fasttracker%202/"
#define BT2_MODLAND_REFRESH_DAYS 30

typedef struct bt2ModlandSetup_t
{
	char mirrorRoot[1024]; // the Modland folders Disk Op. shows (empty files until a song is opened)
	char ownRoot[1024];    // the user's own copies (BT2/Modland)
	char listFile[1024];   // the saved list
	char listUrl[512];
	char fileUrl[512];
} bt2ModlandSetup;

void bt2ModlandInit(const bt2ModlandSetup *setup);
const bt2ModlandSetup *bt2ModlandGetSetup(void);

// ---- the list
bool bt2ModlandIsLoaded(void);
bool bt2ModlandLoadSaved(bool *old); // the saved list (false = none: it must be downloaded)

// allmods.zip (in memory) -> the Fasttracker 2 lines ("size<TAB>Artist/song.xm", malloc'ed). NULL = not a zip file with the list
char *bt2ModlandListFromZip(const uint8_t *zip, size_t zipLen, size_t *len);
void bt2ModlandSaveList(const char *text, size_t len); // in setup.listFile (the web version keeps it in the browser's cache)

// downloads allmods.zip, keeps the Fasttracker 2 lines and saves them (in a worker thread; the Windows program).
// NULL = failed (*res says why; *unreadable = the file came, but wasn't a zip file with the list).
char *bt2ModlandFetchList(volatile int32_t *cancel, bt2NetProgressFunc progress, void *ud, bt2NetResult *res, bool *unreadable, size_t *len);

bool bt2ModlandUseList(char *text, size_t len); // takes the text (from bt2ModlandFetchList), makes the folders' list
void bt2ModlandStartRefresh(void); // quietly in the background (the new list is used the next time)

// ---- the folders (thread-safe)
bool bt2ModlandIsMirrorPath(const char *path); // in the Modland folders (read-only)
void bt2ModlandPopulate(const char *dir);      // makes the folder's sub-folders and empty song files (once)

// a song file in the Modland folders: its path on Modland and the user's own copy. false = not a Modland song.
bool bt2ModlandResolve(const char *file, char *modlandPath, size_t mpLen, char *ownPath, size_t opLen);

// a folder or a song on Modland ("Artist/sub", "Artist/song.xm"): the folders on the way are made and filled.
// Its path here (the names here can differ: on Windows "Song.xm" and "song.xm" in one folder are "song (2).xm").
bool bt2ModlandMaterialize(const char *modlandPath, char *out, size_t outLen);

// the user's own folder for a Modland folder here (for saving: BT2/Modland/<the same folders>)
bool bt2ModlandOwnFolderFor(const char *mirrorDir, char *out, size_t outLen);

void bt2ModlandFileUrl(const char *modlandPath, char *out, size_t outLen);

// ---- a song, before it is opened (the same in the Windows program and the web version)
typedef struct bt2ModlandSong_t
{
	char file[2048];        // the song in Modland's folders here
	char modlandPath[2048]; // on Modland ("Artist/song.xm")
	char ownPath[2048];     // the user's own copy (BT2/Modland/...)
	char url[6200];         // (BT2_SONG_DOWNLOAD)
	char label[600];        // for the download box: "Henter «song.xm» fra Modland"
} bt2ModlandSong;

enum { BT2_SONG_OPEN = 0, BT2_SONG_DOWNLOAD, BT2_SONG_FAILED, BT2_SONG_NOT_MODLAND };

/* What happens before a song in Modland's folders is opened (the list must be loaded):
** - the user's own copy is there: it is what is opened, also after it was changed and saved (BT2_SONG_OPEN)
** - the song came before, but the user's copy is gone: a new copy of it is made (BT2_SONG_OPEN)
** - else it is downloaded from song->url (BT2_SONG_DOWNLOAD), then given to bt2ModlandStoreSong()
** msg: a message to show ("" = none).
*/
int32_t bt2ModlandPrepareSong(const char *file, bt2ModlandSong *song, char *msg, size_t msgLen);

// the downloaded song: the user's own copy (*ownSaved) and Modland's folder (false = it couldn't be written there)
bool bt2ModlandStoreSong(const bt2ModlandSong *song, const uint8_t *data, size_t len, bool *ownSaved);
void bt2ModlandStoredMessage(const bt2ModlandSong *song, bool ownSaved, char *msg, size_t msgLen); // "Lagret i BT2/Modland/..."
size_t bt2ModlandMaxSongSize(void);

// ---- all songs (for the search window)
int32_t bt2ModlandFileCount(void);
const char *bt2ModlandFilePath(int32_t i, size_t *len); // "Artist/song.xm" (not 0-terminated: *len bytes)
int64_t bt2ModlandFileSize(int32_t i);

void bt2ModlandShutdown(void);
