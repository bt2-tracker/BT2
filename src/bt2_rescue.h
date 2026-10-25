#pragma once

/* BerntTracker (web and Windows, v0.34): the rescue copy ("redningskopi") of the song being worked on.
**
** While the song has unsaved changes, a copy of it is written now and then to the program's own folder
** (never to the user's folders, and never over a file of the user's):
**
**   web:      /home/web_user/.bt2-rescue/redning.xm   (stored in the browser like the user's files)
**   Windows:  %LOCALAPPDATA%\Bernttracker II\Redning\redning.xm
**
** When the song is saved (or another song is loaded), the copy is deleted. If the program was closed
** before the song was saved (the iPad closed the page in the background, the power went, a crash, or the
** user left without saving), the next start asks whether to get the song back.
*/

#if defined _WIN32 || defined __EMSCRIPTEN__

#include <stdint.h>
#include <stdbool.h>

void bt2RescueTick(void);      // every frame in the main loop (asks at start, writes the copy when it is time)
void bt2RescueOnExit(void);    // the program is closed normally: a last copy if the song is unsaved, else delete it
void bt2RescueAfterLoad(void); // a song was loaded: if it was the rescue copy, give it back its name and folder
bool bt2RescueNow(void);       // write the copy right away if there are unsaved changes (e.g. the app goes to the background)
bool bt2RescueOnCrash(void);   // the program crashed: true if nothing is lost (no unsaved changes, or the copy was written)

#endif
