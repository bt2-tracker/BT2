#pragma once

/* BerntTracker (Windows): the start screen, the same as the web version's (web/index.html, #start):
** the FT2 About-screen stars behind the logo, «Trykk for å starte», «Start i fullskjerm», the sound,
** «Lydbuffer», the version, the box that looks for updates, «Lydtest» and the credits.
**
** When the web version's start screen changes, this one should change the same way.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#include <stdbool.h>

// Shows the start screen until the user starts. false: close the program instead (the window was
// closed, or the program was updated and starts again by itself).
bool bt2StartScreen(void);

void bt2SaveDropouts(void); // v0.35: from the main loop, breaks in the sound are kept for the next sound test

#endif
