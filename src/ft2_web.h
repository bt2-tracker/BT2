#pragma once

// BerntTracker: things that are only used in the web (browser) version.
// Everything here is only compiled when building with Emscripten.

#ifdef __EMSCRIPTEN__

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

/* Threads: the web version runs without real threads (it's faster to start, and works
** without special server settings). Work that FT2 does in a background thread is
** simply done right away instead. The channel scopes are updated once per frame.
*/
SDL_Thread *webCreateThread(SDL_ThreadFunction fn, const char *name, void *data);
void webDetachThread(SDL_Thread *thread);
void webWaitThread(SDL_Thread *thread, int *status);

#define SDL_CreateThread(fn, name, data) webCreateThread((fn), (name), (data))
#define SDL_DetachThread(thread) webDetachThread(thread)
#define SDL_WaitThread(thread, status) webWaitThread((thread), (status))

// audio: low-latency audio via an AudioWorklet (FT2's mixer runs on the browser's audio thread)
SDL_AudioDeviceID webAudioOpen(SDL_AudioCallback callback, const SDL_AudioSpec *want, SDL_AudioSpec *have);
void webAudioClose(void);
void webAudioPause(bool pause);
void webAudioLock(void);
void webAudioUnlock(void);
uint64_t webAudioPerfCounter(void); // replaces SDL_GetPerformanceCounter() on the audio thread

// main loop
void webInit(void);              // before the window is created
void webAfterSetup(void);        // right before the main loop starts
void webFrame(void);             // once per frame (from flipFrame)
void webWaitForNextFrame(void);  // gives control back to the browser until the next screen refresh
void webGetWindowSize(int32_t *w, int32_t *h);
bool webIsTouchDevice(void);

// files
void webSyncFiles(void); // save the in-app file system to the browser's storage (IndexedDB)

#endif
