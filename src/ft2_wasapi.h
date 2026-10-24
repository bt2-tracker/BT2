#pragma once

// BerntTracker (Windows): sound output in WASAPI exclusive mode (see ft2_wasapi.c)

#ifdef _WIN32

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

// audio.dev while the exclusive output is in use (not a real SDL device)
#define WIN_EXCLUSIVE_DEVICE_ID 0x7FFF0001

/* Opens the sound card in exclusive mode and starts mixing with FT2's callback (paused, like SDL).
** deviceName: FT2's chosen output device (NULL = Windows' default). bufferSamples (v0.32): the buffer
** chosen on the start screen, 32-512 samples (not smaller than the sound card allows).
** shared (v0.31): Windows' ordinary shared sound instead, with small blocks (Windows 10's low-latency
** shared mode), for "Windows" on the start screen.
** Returns WIN_EXCLUSIVE_DEVICE_ID, or 0 if the sound card can't be used that way (then use SDL as before).
*/
SDL_AudioDeviceID winExclusiveOpen(const char *deviceName, SDL_AudioCallback callback, const SDL_AudioSpec *want,
	SDL_AudioSpec *have, int32_t bufferSamples, bool shared);

void winExclusiveClose(void);
void winExclusiveLock(void);
void winExclusiveUnlock(void);
void winExclusivePause(bool pause);
bool winExclusiveActive(void);
bool winExclusiveShared(void); // (v0.31) the shared mode with small blocks is in use
bool winExclusiveDeviceLost(void); // the sound card went away (unplugged, changed): open the sound again
const char *winExclusiveStatus(void); // short text for Config > Audio ("3.0ms", "busy" ...)
const char *winExclusiveInfo(void); // longer text for the info box (Ctrl+Shift+F)

#endif
