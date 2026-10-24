#pragma once

// BerntTracker (Windows): sound output in WASAPI exclusive mode (see ft2_wasapi.c)

#ifdef _WIN32

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

// audio.dev while the exclusive output is in use (not a real SDL device)
#define WIN_EXCLUSIVE_DEVICE_ID 0x7FFF0001

/* Opens the sound card in exclusive mode and starts mixing with FT2's callback (paused, like SDL).
** deviceName: FT2's chosen output device (NULL = Windows' default). bufferSetting: FT2's 512/1024/2048
** ("Smaller/Medium/Bigger latency") = the sound card's smallest block / ~10 ms / ~20 ms.
** Returns WIN_EXCLUSIVE_DEVICE_ID, or 0 if the sound card can't be used that way (then use SDL as before).
*/
SDL_AudioDeviceID winExclusiveOpen(const char *deviceName, SDL_AudioCallback callback, const SDL_AudioSpec *want,
	SDL_AudioSpec *have, int32_t bufferSetting);

void winExclusiveClose(void);
void winExclusiveLock(void);
void winExclusiveUnlock(void);
void winExclusivePause(bool pause);
bool winExclusiveActive(void);
bool winExclusiveDeviceLost(void); // the sound card went away (unplugged, changed): open the sound again
const char *winExclusiveStatus(void); // short text for Config > Audio ("3.0ms", "busy" ...)
const char *winExclusiveInfo(void); // longer text for the info box (Ctrl+Shift+F)

#endif
