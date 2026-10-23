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
void webWaitVoiceTrigger(volatile uint8_t *status); // replaces FT2's busy-wait for the mixer to start a voice
SDL_AudioDeviceID webAudioOpen(SDL_AudioCallback callback, const SDL_AudioSpec *want, SDL_AudioSpec *have);
void webAudioClose(void);
void webAudioPause(bool pause);
void webAudioLock(void);
void webAudioUnlock(void);
uint64_t webAudioPerfCounter(void); // replaces SDL_GetPerformanceCounter() on the audio thread
void webAudioSetLatencyFrames(int32_t frames); // ft2_audio.c

// main loop
void webInit(void);              // before the window is created
void webAfterSetup(void);        // right before the main loop starts
void webFrame(void);             // once per frame (from flipFrame)
void webWaitForNextFrame(void);  // gives control back to the browser until the next screen refresh
void webGetWindowSize(int32_t *w, int32_t *h);
bool webIsTouchDevice(void);

void webLongTaskYield(void); // call often during long work on the main thread (WAV rendering)

// keyboard
SDL_Keycode webFixKeycode(SDL_Scancode scancode, SDL_Keycode keycode);
void webToggleFullscreen(void); // Alt+Enter: the browser's fullscreen

// files
#define WEB_USER_ROOT "/home/web_user/BT2" // the user's own folders (stored in the browser)
void webSyncFiles(void);
// (Disk Op.'s Modland and folder rules: bt2_diskop.h, the same as in the Windows program)
void webHandlePageRequests(void);   // in the main loop: requests from the page (e.g. "show this folder" from the search window)

// Modland (online library): the folders are in memory only (bt2_modland.c, the same as in the Windows program)
#define WEB_MODLAND_ROOT "/Modland"

#endif
