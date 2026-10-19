#pragma once

// BerntTracker (Windows): sound output through an ASIO driver, e.g. ASIO4ALL (see ft2_asio.cpp)

#ifdef _WIN32

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// audio.dev while the ASIO output is in use (not a real SDL device)
#define WIN_ASIO_DEVICE_ID 0x7FFF0002

/* Opens the ASIO driver (ASIO4ALL if it is installed, else the first one) and starts mixing with FT2's
** callback (paused, like SDL). bufferSetting: FT2's 512/1024/2048 ("Smaller/Medium/Bigger latency") =
** 64 / 256 / 512 samples (or the nearest the driver allows).
** Returns WIN_ASIO_DEVICE_ID, or 0 if there is no ASIO driver or it couldn't be started.
*/
SDL_AudioDeviceID winAsioOpen(SDL_AudioCallback callback, const SDL_AudioSpec *want, SDL_AudioSpec *have, int32_t bufferSetting);

void winAsioClose(void);
void winAsioLock(void);
void winAsioUnlock(void);
void winAsioPause(bool pause);
bool winAsioActive(void);
bool winAsioResetRequested(void); // the driver asks to be started again (its settings changed)
bool winAsioInstalled(void);
const char *winAsioStatus(void); // short text for Config > Audio
const char *winAsioInfo(void); // longer text for the info box (Ctrl+Shift+F)
const char *winAsioDriverName(void); // e.g. "ASIO4ALL v2" (for the start screen and the sound test)
int32_t winAsioOutputLatencyFrames(void); // what the driver says its output delay is (samples, 0 = unknown)

#ifdef __cplusplus
}
#endif

#endif
