#pragma once

/* BerntTracker (Windows): the program updates itself from the web, like the web version does.
**
** Every Sync to GitHub builds a new Windows program and puts it on the web site (windows/), with
** windows/version.json listing its files with size and SHA-256. On the start screen the program
** compares that list with its own files. If they differ, it offers "Oppdater nå / Senere".
** An update downloads the new files next to the old ones, checks size and SHA-256, swaps ONLY the
** program's own files (the .exe and SDL2.dll) and starts the program again. Songs, samples, folders
** and settings are never touched.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#include <stdint.h>
#include <stdbool.h>

enum // what the start screen shows (the same steps as in the web version's app box)
{
	BT2_UPD_CHECKING = 0,
	BT2_UPD_LATEST,
	BT2_UPD_AVAILABLE,
	BT2_UPD_OFFLINE,
	BT2_UPD_CHECK_FAILED,
	BT2_UPD_DOWNLOADING,
	BT2_UPD_FAILED,     // the download or its check failed: "Oppdater nå" can be tried again
	BT2_UPD_NO_ACCESS,  // the program's folder can't be written to
	BT2_UPD_RESTARTING  // the new files are in place: the program starts again
};

typedef struct bt2UpdateInfo_t
{
	int32_t state;
	uint32_t changeCount; // new number every time something else is to be shown
	double progress; // 0..1 while downloading
	char remoteVersion[32], remoteDate[32];
} bt2UpdateInfo;

void bt2UpdateEarlyStartup(void); // first thing in main(): wait for an updating program to close, remove its leftovers
void bt2UpdateStartCheck(void); // looks for a new version on the web (in the background)
void bt2UpdateGetInfo(bt2UpdateInfo *info);
void bt2UpdateStartDownload(void); // "Oppdater nå"
bool bt2UpdateLaunchNew(void); // after BT2_UPD_RESTARTING: starts the new program (then close this one)
void bt2UpdateShutdown(void);

#endif
