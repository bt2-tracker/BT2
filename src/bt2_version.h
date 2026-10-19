#ifndef BT2_VERSION_H
#define BT2_VERSION_H

/* BerntTracker: the version shown on the start screen, and used when looking for updates.
** The Windows build script (verktoy/bygg-windows.sh) takes it from web/index.html (BT2_VERSION and
** BT2_DATE) and writes bt2_build.h, so the web version and the Windows program always have the same
** number. A build without that file (for instance Visual Studio's) is called "dev".
*/

#ifdef BT2_BUILD_HEADER
#include "bt2_build.h"
#endif

#ifndef BT2_VERSION
#define BT2_VERSION "dev"
#endif

#ifndef BT2_DATE
#define BT2_DATE "-"
#endif

#endif
