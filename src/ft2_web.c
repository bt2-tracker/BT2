// BerntTracker: web (browser) version glue. Only compiled with Emscripten.
//
// Two builds:
//  BT2_WEB_WORKLET defined: can also run FT2's mixer on the browser's audio thread
//                           (needs a "cross-origin isolated" page / SharedArrayBuffer).
//  otherwise:               the mixer always runs on the main thread.
// See "audio" below for the three sound engines.

#ifdef __EMSCRIPTEN__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> // getcwd(), chdir()
#include <limits.h>
#include <emscripten.h>
#ifdef BT2_WEB_WORKLET
#include <emscripten/webaudio.h>
#endif
#include "ft2_header.h"
#include "ft2_web.h"
#include "bt2_diskop.h"
#include "ft2_config.h"
#include "ft2_structs.h"
#include "ft2_video.h"
#include "ft2_diskop.h"
#include "ft2_audio.h"
#include "scopes/ft2_scopes.h"
#include "ft2_palette.h"
#include "ft2_tables.h"
#include "ft2_keyboard.h"
#include "ft2_replayer.h"
#include "ft2_sysreqs.h"
#include "ft2_pattern_ed.h"
#include "mixer/ft2_mix.h"
#include "ft2_touchkeys.h"
#include "ft2_mouse.h"
#include "bt2_fs.h"
#include "bt2_modland.h"
#include "bt2_search.h"
#include "bt2_import.h"
#include "bt2_unzip.h"

void webScopesTick(int32_t updates); // ft2_scopes.c
void webDiskOpStorePathsInConfig(void); // ft2_diskop.c
const char *webDiskOpGetFilePath(void); // ft2_diskop.c
bool webDiskOpIsShown(void);

// ------------------------------------------------------------------------------------------------
// threads (run right away, see ft2_web.h)

static int32_t dummyThreadObject;

SDL_Thread *webCreateThread(SDL_ThreadFunction fn, const char *name, void *data)
{
	(void)name;
	if (fn != NULL)
		fn(data);

	return (SDL_Thread *)&dummyThreadObject;
}

void webDetachThread(SDL_Thread *thread)
{
	(void)thread;
}

void webWaitThread(SDL_Thread *thread, int *status)
{
	(void)thread;
	if (status != NULL)
		*status = 0;
}

// ------------------------------------------------------------------------------------------------
// frame timing

EM_ASYNC_JS(void, bt2_waitFrame, (void), {
	await globalThis.BT2.waitFrame();
});

void webWaitForNextFrame(void)
{
	bt2_waitFrame();
}

EM_JS(int32_t, bt2_innerWidth, (void), { return Math.max(320, window.innerWidth|0); });
EM_JS(int32_t, bt2_innerHeight, (void), { return Math.max(200, window.innerHeight|0); });
EM_JS(int32_t, bt2_isTouch, (void), { return (('ontouchstart' in window) || navigator.maxTouchPoints > 0) ? 1 : 0; });

void webGetWindowSize(int32_t *w, int32_t *h)
{
	*w = bt2_innerWidth();
	*h = bt2_innerHeight();
}

bool webIsTouchDevice(void)
{
	return bt2_isTouch() != 0;
}

// ------------------------------------------------------------------------------------------------
// audio
//
// Three "engines" (chosen in the sound test, remembered in the browser, switched without reloading):
//  0 = worklet: FT2's mixer runs on the browser's audio thread (Wasm AudioWorklet, needs isolation)
//  1 = compat:  the mixer runs on the main thread, small blocks are queued to a plain AudioWorklet
//  2 = SP:      the mixer runs on the main thread inside a ScriptProcessorNode
// All of them use the same AudioContext, so the sample rate never changes when switching.

#define CHUNK_MAX 1024

static SDL_AudioCallback audioCb;
static uint32_t audioRate = 48000;
static uint64_t perfFreq = 1000000;
static volatile int32_t audioMode = 1;
static volatile int32_t audioIsPaused = 1;
static volatile int32_t audioIs16Bit; // FT2's "16-bit" setting: the mixer makes 16-bit samples, turned into floats here
static int32_t mainLockDepth;
static float chunkBuffer[CHUNK_MAX * 2];
static int16_t chunkBuffer16[CHUNK_MAX * 4]; // (as many bytes as chunkBuffer, in case a block is ever written as floats)
static bool engineAttached;

EM_JS(int32_t, bt2_wantedEngine, (int32_t hasWorklet), {
	return globalThis.BT2.wantedEngineCode(hasWorklet);
});

EM_JS(int32_t, bt2_engineLatencyFrames, (void), {
	return globalThis.BT2.engineLatencyFrames();
});

// FT2's "Smaller/Medium/Bigger latency" (Config > Audio): the smallest queue for the main-thread engines
EM_JS(void, bt2_setLatencyChoice, (int32_t samples), {
	globalThis.BT2.setLatencyChoice(samples);
});

// run FT2's mixer for one block, always giving floats to the browser
static void runMixer(float *out, int16_t *out16, int32_t frames)
{
	if (audioIs16Bit)
	{
		// 16-bit: FT2 rounds the mix to 16 bits (with a little dither), like a 16-bit sound card
		audioCb(NULL, (Uint8 *)out16, frames * 2 * sizeof (int16_t));
		for (int32_t i = 0; i < frames * 2; i++)
			out[i] = (float)out16[i] * (1.0f / 32768.0f);
	}
	else
	{
		audioCb(NULL, (Uint8 *)out, frames * 2 * sizeof (float));
	}
}

#ifdef BT2_WEB_WORKLET

#define QUANTUM 128

static EMSCRIPTEN_WEBAUDIO_T audioCtx;
static volatile int32_t audioLockState; // 0 = free, 1 = main thread, 2 = audio thread
static volatile int32_t audioReady, audioFailed, workletStarted, audioThreadDead;
static float interleaved[QUANTUM * 2];
static int16_t interleaved16[QUANTUM * 4]; // (as many bytes as interleaved, in case a block is ever written as floats)
static volatile uint64_t framesRendered;
static volatile uint32_t audioGaps; // blocks the mixer could not fill (shown in the sound test)

// audio thread only: smooth over gaps in the sound (no hard jump to silence and back = no click)
#define DECLICK_FADE_IN 64
#define LOCK_SPIN_LONG 100000 // a fraction of a millisecond
#define LOCK_SPIN_SHORT 1000
static float lastOutL, lastOutR;
static int32_t fadeInLeft, lockSpinLimit = LOCK_SPIN_LONG;

static void soundGapFadeOut(float *outL, float *outR)
{
	// nothing to play this time: let the last sample fade away instead of dropping to zero at once
	float l = lastOutL, r = lastOutR;
	for (int32_t i = 0; i < QUANTUM; i++)
	{
		l *= 0.98f;
		r *= 0.98f;
		outL[i] = l;
		outR[i] = r;
	}

	if (l > -1e-6f && l < 1e-6f) l = 0.0f;
	if (r > -1e-6f && r < 1e-6f) r = 0.0f;
	lastOutL = l;
	lastOutR = r;
	fadeInLeft = DECLICK_FADE_IN; // and fade in when the sound comes back
}

static void soundGapFadeIn(float *outL, float *outR)
{
	for (int32_t i = 0; i < QUANTUM && fadeInLeft > 0; i++, fadeInLeft--)
	{
		const float g = 1.0f - (float)fadeInLeft * (1.0f / DECLICK_FADE_IN);
		outL[i] *= g;
		outR[i] *= g;
	}

	lastOutL = outL[QUANTUM-1];
	lastOutR = outR[QUANTUM-1];
}
static volatile uint32_t anchorSeq;
static volatile uint64_t anchorCounter, anchorFrames;
static uint8_t audioThreadStack[512 * 1024] __attribute__((aligned(16)));

EM_ASYNC_JS(int32_t, bt2_attachEngine, (int32_t ctxHandle, int32_t mode), {
	return await globalThis.BT2.openEngine(emscriptenGetAudioObject(ctxHandle), mode);
});

static EM_BOOL processAudio(int numInputs, const AudioSampleFrame *inputs, int numOutputs, AudioSampleFrame *outputs,
	int numParams, const AudioParamFrame *params, void *userData)
{
	(void)numInputs; (void)inputs; (void)numParams; (void)params; (void)userData;

	if (numOutputs < 1 || outputs[0].numberOfChannels < 2)
		return EM_TRUE;

	float *outL = outputs[0].data;
	float *outR = outputs[0].data + QUANTUM;

	bool mixed = false;
	if (audioMode == 0 && !audioIsPaused && !audioThreadDead)
	{
		/* The main thread holds the lock only for a moment (a note from the keyboard, an edit).
		** Wait for it a little instead of leaving a hole in the sound, which is heard as a click.
		** If it keeps it for long (loading, big sample edits: FT2 goes quiet then too), give up soon.
		*/
		for (int32_t spins = 0; spins < lockSpinLimit; spins++)
		{
			if (__atomic_load_n(&audioLockState, __ATOMIC_RELAXED) != 0)
				continue;

			int32_t expected = 0;
			if (__atomic_compare_exchange_n(&audioLockState, &expected, 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			{
				// check again now that we hold the lock (the main thread may have paused/switched in between)
				if (audioMode == 0 && !audioIsPaused && audioCb != NULL)
				{
					runMixer(interleaved, interleaved16, QUANTUM);
					mixed = true;
				}
				__atomic_store_n(&audioLockState, 0, __ATOMIC_RELEASE);
				break;
			}
		}

		lockSpinLimit = mixed ? LOCK_SPIN_LONG : LOCK_SPIN_SHORT;
		if (!mixed && audioMode == 0 && !audioIsPaused)
			__atomic_add_fetch(&audioGaps, 1, __ATOMIC_RELAXED);
	}

	if (mixed)
	{
		for (int32_t i = 0; i < QUANTUM; i++)
		{
			outL[i] = interleaved[(i << 1) + 0];
			outR[i] = interleaved[(i << 1) + 1];
		}
		soundGapFadeIn(outL, outR);
	}
	else
	{
		soundGapFadeOut(outL, outR);
	}

	__atomic_add_fetch(&framesRendered, QUANTUM, __ATOMIC_SEQ_CST);
	return EM_TRUE;
}

uint64_t webAudioPerfCounter(void)
{
	if (audioMode != 0)
		return SDL_GetPerformanceCounter(); // main-thread engines

	// worklet engine: runs on the audio thread (no performance.now() there)
	uint32_t seq;
	uint64_t counter, frames;
	do
	{
		seq = __atomic_load_n(&anchorSeq, __ATOMIC_ACQUIRE);
		counter = __atomic_load_n(&anchorCounter, __ATOMIC_SEQ_CST); // (atomic: plain loads could be done after the check below on ARM)
		frames = __atomic_load_n(&anchorFrames, __ATOMIC_SEQ_CST);
	}
	while ((seq & 1) || seq != __atomic_load_n(&anchorSeq, __ATOMIC_ACQUIRE));

	const int64_t framesSince = (int64_t)(__atomic_load_n(&framesRendered, __ATOMIC_SEQ_CST) - frames);
	return counter + (uint64_t)(((double)framesSince * (double)perfFreq) / audioRate);
}

static void updateAudioClockAnchor(void) // main thread, once per frame
{
	__atomic_add_fetch(&anchorSeq, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&anchorCounter, SDL_GetPerformanceCounter(), __ATOMIC_SEQ_CST);
	__atomic_store_n(&anchorFrames, __atomic_load_n(&framesRendered, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&anchorSeq, 1, __ATOMIC_ACQ_REL);
}

static void workletDone(int32_t ok)
{
	if (ok)
		audioReady = 1;
	else
		audioFailed = 1;

	EM_ASM({ if (globalThis.BT2.onWorkletReady) globalThis.BT2.onWorkletReady(); });
}

static void onProcessorCreated(EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success, void *userData)
{
	(void)userData;
	if (!success)
	{
		workletDone(0);
		return;
	}

	int outputChannelCounts[1] = { 2 };
	EmscriptenAudioWorkletNodeCreateOptions options;
	options.numberOfInputs = 0;
	options.numberOfOutputs = 1;
	options.outputChannelCounts = outputChannelCounts;

	EMSCRIPTEN_AUDIO_WORKLET_NODE_T node = emscripten_create_wasm_audio_worklet_node(ctx, "bt2-mixer", &options, processAudio, NULL);
	EM_ASM({
		const node = emscriptenGetAudioObject($0);
		globalThis.BT2.workletNode = node;
		node.addEventListener('processorerror', () => globalThis.BT2.workletFailed()); // the audio thread stopped (an error in the mixer)
	}, node);
	workletDone(1);
}

static void onAudioThreadStarted(EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success, void *userData)
{
	(void)userData;
	if (!success)
	{
		workletDone(0);
		return;
	}

	WebAudioWorkletProcessorCreateOptions opts;
	memset(&opts, 0, sizeof (opts));
	opts.name = "bt2-mixer";
	emscripten_create_wasm_audio_worklet_processor_async(ctx, &opts, onProcessorCreated, NULL);
}

EMSCRIPTEN_KEEPALIVE void webStartWorkletThread(void) // called from JS when switching to the worklet engine
{
	if (workletStarted || audioCtx == 0)
	{
		if (audioReady || audioFailed)
			EM_ASM({ if (globalThis.BT2.onWorkletReady) globalThis.BT2.onWorkletReady(); });
		return;
	}

	workletStarted = 1;
	emscripten_start_wasm_audio_worklet_thread_async(audioCtx, audioThreadStack, sizeof (audioThreadStack), onAudioThreadStarted, NULL);
}

EM_JS(void, bt2_workletFailed, (void), { setTimeout(() => globalThis.BT2.workletFailed(), 0); });

/* The audio thread stopped in the middle of mixing (an error there): flags it sets for a moment would
** stay set, and the main thread waits for them in a few places (pauseMusic() etc.), which would freeze it.
*/
static void clearAudioThreadFlags(void)
{
	replayerBusy = false;
	audio.callbackOngoing = false;
	pattQueueClearing = false;
	chQueueClearing = false;
}

// called from JS when the audio thread has stopped (processorerror): it can never let go of the lock then
EMSCRIPTEN_KEEPALIVE void webAudioThreadDied(void)
{
	audioThreadDead = 1;
	int32_t expected = 2;
	__atomic_compare_exchange_n(&audioLockState, &expected, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
	clearAudioThreadFlags();
}

void webAudioLock(void)
{
	if (mainLockDepth++ == 0)
	{
		double waitStart = 0.0;
		int32_t expected = 0;
		while (!__atomic_compare_exchange_n(&audioLockState, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		{
			// the audio thread holds it for well under a millisecond. If it holds it for a whole second,
			// it has stopped (an error on the audio thread): take over instead of freezing the program,
			// and switch to the compatible engine.
			const double now = emscripten_get_now();
			if (waitStart == 0.0)
			{
				waitStart = now;
			}
			else if (now - waitStart > 1000.0)
			{
				audioThreadDead = 1;
				__atomic_store_n(&audioLockState, 1, __ATOMIC_RELEASE);
				clearAudioThreadFlags();
				bt2_workletFailed();
				break;
			}

			expected = 0;
		}
	}
}

void webAudioUnlock(void)
{
	if (mainLockDepth > 0 && --mainLockDepth == 0)
		__atomic_store_n(&audioLockState, 0, __ATOMIC_RELEASE);
}

// The page makes the AudioContext (BT2.createAudioContext: on Windows it asks for a smaller sound buffer
// than on other devices). 0 if that fails.
EM_JS(int32_t, bt2_createAudioContext, (void), {
	try {
		const ctx = globalThis.BT2.createAudioContext();
		return ctx ? emscriptenRegisterAudioObject(ctx) : 0;
	} catch (e) {
		console.warn('AudioContext', e);
		return 0;
	}
});

static int32_t openEngine(void) // first open: create the AudioContext and attach the wanted engine
{
	audioCtx = bt2_createAudioContext();
	if (audioCtx == 0)
	{
		EmscriptenWebAudioCreateAttributes attr;
		attr.latencyHint = "interactive";
		attr.sampleRate = 0; // use the device's own rate (no resampling = less latency)
		audioCtx = emscripten_create_audio_context(&attr);
	}

	if (audioCtx == 0)
		return 0;

	int32_t mode = bt2_wantedEngine(1);
	if (mode == 0)
	{
		webStartWorkletThread();
		for (int32_t i = 0; i < 1000 && !audioReady && !audioFailed; i++)
			emscripten_sleep(10);

		if (!audioReady)
			mode = 1; // fall back to the compat engine
	}

	audioMode = mode;
	if (mode == 0)
		updateAudioClockAnchor();

	return bt2_attachEngine(audioCtx, mode);
}

#else // build without the audio thread (pages that are not "cross-origin isolated")

EM_ASYNC_JS(int32_t, bt2_attachEngine, (int32_t ctxHandle, int32_t mode), {
	return await globalThis.BT2.openEngine(null, mode);
});

uint64_t webAudioPerfCounter(void)
{
	return SDL_GetPerformanceCounter(); // the mixer always runs on the main thread here
}

static void updateAudioClockAnchor(void) { }

EMSCRIPTEN_KEEPALIVE void webStartWorkletThread(void)
{
	EM_ASM({ if (globalThis.BT2.onWorkletReady) globalThis.BT2.onWorkletReady(); });
}

void webAudioLock(void)
{
	mainLockDepth++;
}

void webAudioUnlock(void)
{
	if (mainLockDepth > 0)
		mainLockDepth--;
}

static int32_t openEngine(void)
{
	int32_t mode = bt2_wantedEngine(0);
	if (mode == 0)
		mode = 1;

	audioMode = mode;
	return bt2_attachEngine(0, mode);
}

#endif

// main-thread engines (compat and SP) call this from JS for every block
EMSCRIPTEN_KEEPALIVE float *webAudioRenderChunk(int32_t frames)
{
	if (frames > CHUNK_MAX)
		frames = CHUNK_MAX;

	if (audioMode == 0 || audioIsPaused || mainLockDepth > 0 || audioCb == NULL)
		memset(chunkBuffer, 0, frames * 2 * sizeof (float));
	else
		runMixer(chunkBuffer, chunkBuffer16, frames);

	return chunkBuffer;
}

// for the compat engine's queue size (it only gets smaller while a song plays, when the program is busiest)
EMSCRIPTEN_KEEPALIVE int32_t webSongIsPlaying(void)
{
	return songPlaying ? 1 : 0;
}

// for the sound test: how many times the low-latency engine had to leave a gap in the sound
EMSCRIPTEN_KEEPALIVE uint32_t webAudioGaps(void)
{
#ifdef BT2_WEB_WORKLET
	return __atomic_load_n(&audioGaps, __ATOMIC_RELAXED);
#else
	return 0;
#endif
}

// called from JS when the engine's delay changes (engine switch, compat queue size), for the visual sync
EMSCRIPTEN_KEEPALIVE void webSetLatencyFrames(int32_t frames)
{
	webAudioSetLatencyFrames(frames);
}

// called from JS when the user picks another engine (the JS side swaps the audio nodes right after)
EMSCRIPTEN_KEEPALIVE void webSetAudioMode(int32_t mode)
{
	webAudioLock();
	audioMode = mode;
	if (mode == 0)
		updateAudioClockAnchor();
	audio.resetSyncTickTimeFlag = true; // line scopes/pattern up with the new engine's timing
	webAudioUnlock();
}

SDL_AudioDeviceID webAudioOpen(SDL_AudioCallback callback, const SDL_AudioSpec *want, SDL_AudioSpec *have)
{
	audioCb = callback;
	perfFreq = SDL_GetPerformanceFrequency();
	*have = *want;

	if (!engineAttached) // one AudioContext for the whole session
	{
		const int32_t rate = openEngine();
		if (rate <= 0)
			return 0;

		audioRate = (uint32_t)rate;
		engineAttached = true;
	}

	// FT2's bit depth setting: 16-bit (the mixer rounds to 16 bits, turned into floats for the browser) or 32-bit
	webAudioLock();
	audioIs16Bit = (want->format == AUDIO_S16) ? 1 : 0;
	webAudioUnlock();

	// FT2's latency setting: the smallest queue for the main-thread engines (the low-latency engine has none)
	bt2_setLatencyChoice(want->samples);

	have->freq = (int)audioRate; // always the device's own rate (no resampling = less delay)
	have->format = audioIs16Bit ? AUDIO_S16 : AUDIO_F32;
	have->channels = 2;
	have->samples = (Uint16)bt2_engineLatencyFrames(); // used to line up the scopes/pattern with the sound

	audioIsPaused = 1;
	return 1;
}

void webAudioClose(void)
{
	webAudioPause(true); // (under the lock: the mixer is surely not running when the buffers are freed after this)
}

void webAudioPause(bool pause)
{
	if (!pause && audioMode == 0)
		updateAudioClockAnchor();

	// under the lock: when this returns, the mixer is not running, and it won't start while paused
	webAudioLock();
	audioIsPaused = pause ? 1 : 0;
	webAudioUnlock();
}

// ------------------------------------------------------------------------------------------------
// keyboard (hardware/Bluetooth keyboards in the browser)

/* The browser sometimes gives no key code for a key (for instance the "<" key next to left Shift
** on Nordic keyboards, keyCode 226). FT2 ignores keys without a key code, so the key would do
** nothing (no key-off). Use the key's position (scancode) instead.
*/
SDL_Keycode webFixKeycode(SDL_Scancode scancode, SDL_Keycode keycode)
{
	// modifier keys: trust the key position (Safari/Chrome on Mac give right Cmd the "context menu" key code)
	if (scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI)
		return (SDL_Keycode)SDL_SCANCODE_TO_KEYCODE(scancode);

	if (keycode != SDLK_UNKNOWN || scancode == SDL_SCANCODE_UNKNOWN)
		return keycode;

	if (scancode == SDL_SCANCODE_NONUSBACKSLASH)
		return SDLK_LESS;

	SDL_Keycode k = SDL_GetKeyFromScancode(scancode);
	if (k == SDLK_UNKNOWN)
		k = SDL_SCANCODE_TO_KEYCODE(scancode);

	return k;
}

static void pushKey(SDL_Scancode scancode, bool down)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof (ev));
	ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.key.windowID = (video.window != NULL) ? SDL_GetWindowID(video.window) : 0;
	ev.key.keysym.scancode = scancode;
	ev.key.keysym.sym = webFixKeycode(scancode, SDL_GetKeyFromScancode(scancode));
	ev.key.keysym.mod = (Uint16)SDL_GetModState();
	SDL_PushEvent(&ev);
}

// one full key press (down + up), e.g. Caps Lock on iPad/Mac where the browser only reports every other press
EMSCRIPTEN_KEEPALIVE void webTapKey(int32_t scancode)
{
	pushKey((SDL_Scancode)scancode, true);
	pushKey((SDL_Scancode)scancode, false);
}

// a key press or release from the page: keys that SDL would drop (no usable "code" from the browser,
// e.g. Delete on some Bluetooth keyboards on iPad). See the keyboard layer in index.html.
EMSCRIPTEN_KEEPALIVE void webKeyEvent(int32_t scancode, int32_t down)
{
	pushKey((SDL_Scancode)scancode, down != 0);
}

/* Called from the page for every key and pointer event, with the modifier keys the browser says
** are held (1 = Ctrl, 2 = Alt, 4 = Shift, 8 = Cmd/Win, 16 = AltGr). Fixes "stuck" modifiers when a
** key-up got lost (app switch, system shortcut), and the fake left Ctrl that Windows sends with
** AltGr (FT2 would then not start "play pattern" with the right Alt key).
*/
EMSCRIPTEN_KEEPALIVE void webSyncModifiers(int32_t flags)
{
	const SDL_Keymod m = SDL_GetModState();
	SDL_Keymod n = m;

	if (flags & 16) // AltGr is held: its "Ctrl" is not a real Ctrl
		n &= ~KMOD_LCTRL;
	else if (!(flags & 1))
		n &= ~KMOD_CTRL;
	else if (!(m & KMOD_CTRL))
		n |= KMOD_LCTRL;

	if (!(flags & 2) && !(flags & 16))
		n &= ~KMOD_ALT;

	if (!(flags & 4))
		n &= ~KMOD_SHIFT;

	if (!(flags & 8))
		n &= ~KMOD_GUI;

	if (n != m)
		SDL_SetModState(n);
}

EM_JS(void, bt2_toggleFullscreen, (void), {
	if (globalThis.BT2 && globalThis.BT2.toggleFullscreen) globalThis.BT2.toggleFullscreen();
});

void webToggleFullscreen(void)
{
	bt2_toggleFullscreen();
}

/* Open a file in the program (module, instrument, sample...) like when a file is dropped on the
** window. The main loop does the loading (it may show dialog boxes), so we only post an event.
*/
EMSCRIPTEN_KEEPALIVE void webLoadFile(const char *path)
{
	if (path == NULL || path[0] == '\0')
		return;

	SDL_Event ev;
	memset(&ev, 0, sizeof (ev));
	ev.type = SDL_DROPFILE;
	ev.drop.file = SDL_strdup(path);
	ev.drop.windowID = (video.window != NULL) ? SDL_GetWindowID(video.window) : 0;
	if (ev.drop.file != NULL && SDL_PushEvent(&ev) <= 0)
		SDL_free(ev.drop.file);
}

EMSCRIPTEN_KEEPALIVE int32_t webSongIsModified(void)
{
	return song.isModified ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int32_t webIsEditingText(void)
{
	return (editor.editTextFlag || ui.sysReqShown) ? 1 : 0;
}

// for the page: does FT2 use the Esc key right now? (a dialog, text being edited, Nibbles, a long job Esc
// stops, Disk Op's delete/rename mode). If not, Esc leaves fullscreen.
EMSCRIPTEN_KEEPALIVE int32_t webEscIsUsed(void)
{
	return (ui.sysReqShown || editor.editTextFlag || editor.NI_Play || editor.busy || mouse.mode != MOUSE_MODE_NORMAL) ? 1 : 0;
}

// for automatic tests: the editor state as JSON
EMSCRIPTEN_KEEPALIVE const char *webDebugState(void)
{
	static char buf[2048];

	const int32_t row = editor.row, ch = cursor.ch, patt = editor.editPattern;
	note_t n;
	memset(&n, 0, sizeof (n));
	if (patt >= 0 && patt < MAX_PATTERNS && pattern[patt] != NULL && row < patternNumRows[patt])
		n = pattern[patt][(row * MAX_CHANNELS) + ch];

	char name[21];
	int32_t j = 0;
	for (int32_t i = 0; i < 20 && song.name[i] != '\0'; i++)
	{
		const char c = song.name[i];
		name[j++] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '?';
	}
	name[j] = '\0';

	const SDL_Keymod mod = SDL_GetModState();
	snprintf(buf, sizeof (buf),
		"{\"row\":%d,\"ch\":%d,\"obj\":%d,\"patt\":%d,\"rows\":%d,\"songPos\":%d,\"playMode\":%d,\"songPlaying\":%d,"
		"\"octave\":%d,\"skip\":%d,\"instr\":%d,\"note\":%d,\"ins\":%d,\"vol\":%d,\"efx\":%d,\"efxData\":%d,"
		"\"lctrl\":%d,\"lalt\":%d,\"lshift\":%d,\"mod\":%d,\"editText\":%d,\"sysReq\":%d,"
		"\"patternEd\":%d,\"diskOp\":%d,\"instEd\":%d,\"smpEd\":%d,\"config\":%d,\"help\":%d,\"about\":%d,\"nibbles\":%d,"
		"\"extended\":%d,\"screenH\":%d,\"numCh\":%d,\"songName\":\"%s\","
		"\"advEdit\":%d,\"transp\":%d,\"trim\":%d,\"instExt\":%d,\"smpExt\":%d,\"cfgScreen\":%d,"
		"\"jump\":[%d,%d,%d,%d],\"markX1\":%d,\"markX2\":%d,\"markY1\":%d,\"markY2\":%d,"
		"\"masterVol\":%d,\"bankOffset\":%d,\"bankSwapped\":%d,\"songLen\":%d,\"pattNum\":%d,\"muted0\":%d,"
		"\"wavShown\":%d,\"wavRendering\":%d,"
		"\"ptnAltLayout\":%d,\"bits16\":%d,\"bufSize\":%d,\"audioFreq\":%d,\"interp\":%d,\"volRamp\":%d,\"boost\":%d,\"haveFreq\":%d,"
		"\"kbShown\":%d}",
		row, ch, cursor.object, patt, (patt >= 0 && patt < MAX_PATTERNS) ? patternNumRows[patt] : 0, song.songPos, playMode,
		songPlaying ? 1 : 0, editor.curOctave, editor.editRowSkip, editor.curInstr, n.note, n.instr, n.vol, n.efx, n.efxData,
		keyb.leftCtrlPressed, keyb.leftAltPressed, keyb.leftShiftPressed, (int32_t)mod, editor.editTextFlag, ui.sysReqShown,
		ui.patternEditorShown, ui.diskOpShown, ui.instEditorShown, ui.sampleEditorShown, ui.configScreenShown,
		ui.helpScreenShown, ui.aboutScreenShown, ui.nibblesShown, ui.extendedPatternEditor, SCREEN_H, song.numChannels, name,
		ui.advEditShown, ui.transposeShown, ui.trimScreenShown, ui.instEditorExtShown, ui.sampleEditorExtShown,
		editor.currConfigScreen, editor.ptnJumpPos[0], editor.ptnJumpPos[1], editor.ptnJumpPos[2], editor.ptnJumpPos[3],
		pattMark.markX1, pattMark.markX2, pattMark.markY1, pattMark.markY2, config.masterVol, editor.instrBankOffset,
		editor.instrBankSwapped, song.songLength, song.pattNum, editor.channelMuted[0],
		ui.wavRendererShown, editor.wavIsRendering,
		config.ptnAlternativeLayout, (config.specialFlags & BITDEPTH_16) ? 1 : 0,
		(config.specialFlags & BUFFSIZE_512) ? 512 : (config.specialFlags & BUFFSIZE_2048) ? 2048 : 1024,
		(int32_t)config.audioFreq, config.interpolation, (config.specialFlags & NO_VOLRAMP_FLAG) ? 0 : 1,
		config.boostLevel, (int32_t)audio.haveFreq, tkIsShown() ? 1 : 0);

	return buf;
}

// ------------------------------------------------------------------------------------------------
// long tasks (WAV rendering): FT2 runs them in a thread, here they run on the main thread

void webLongTaskYield(void)
{
	static uint32_t lastTicks;

	const uint32_t now = SDL_GetTicks();
	if (now - lastTicks < 40)
		return;

	lastTicks = now;

	// like FT2 while busy: input is dropped, but a key press or a click stops the rendering
	SDL_Event ev;
	while (SDL_PollEvent(&ev))
	{
		if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_KEYDOWN)
			editor.stopWavRender = true;
		else if (ev.type == SDL_FINGERUP || ev.type == SDL_WINDOWEVENT)
			tkHandleEvent(&ev); // (on-screen keyboard: releases)
		else if (ev.type == SDL_DROPFILE && ev.drop.file != NULL)
			SDL_free(ev.drop.file);
	}

	handleRedrawing();
	flipFrame(); // shows the progress and gives the browser some time
}

// ------------------------------------------------------------------------------------------------
// FT2 waits in a loop until the mixer has started a voice (Smp. Ed. Wave/Range/Display, filter preview).
// The compatible engines mix on this same thread between frames, so that loop would never end. Wait by
// letting the browser run instead (then the mixer gets its turn), and never for long.

void webWaitVoiceTrigger(volatile uint8_t *status)
{
	const uint32_t start = SDL_GetTicks();
	while ((*status & CS_TRIGGER_VOICE) && (SDL_GetTicks() - start) < 300)
		SDL_Delay(1);
}

// ------------------------------------------------------------------------------------------------
// files

EM_JS(void, bt2_syncFiles, (void), { globalThis.BT2.syncFiles(); });
EM_JS(void, bt2_syncIfChanged, (void), { globalThis.BT2.syncIfChanged(); });
EM_JS(void, bt2_toast, (const char *msg), { if (globalThis.BT2 && globalThis.BT2.toast) globalThis.BT2.toast(UTF8ToString(msg)); });

void webSyncFiles(void)
{
	bt2_syncFiles();
}

static bool pathInside(const char *path, const char *root)
{
	const size_t n = strlen(root);
	return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static bool makeDirs(const char *path) // like "mkdir -p"
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof (tmp), "%s", path);
	for (char *p = tmp + 1; *p != '\0'; p++)
	{
		if (*p == '/')
		{
			*p = '\0';
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);

	struct stat st;
	return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
}

bool bt2DiskOpReadOnlyHere(void)
{
	char cwd[PATH_MAX];
	return getcwd(cwd, sizeof (cwd)) != NULL && bt2ModlandIsMirrorPath(cwd);
}

/* Only /home/web_user is stored in the browser. The Modland library (/Modland) is kept in memory,
** so a file saved there would be gone after a restart. Save it in the user's own copy of that
** folder instead (BT2/Modland/..., where songs opened from Modland are kept too), and say so.
*/
bool bt2DiskOpPrepareSaveDir(bool showFolder)
{
	char cwd[PATH_MAX], dest[PATH_MAX];
	if (getcwd(cwd, sizeof (cwd)) == NULL)
		cwd[0] = '\0';

	if (pathInside(cwd, "/home/web_user"))
		return true;

	const bool fromModland = bt2ModlandIsMirrorPath(cwd);
	if (!fromModland || !bt2ModlandOwnFolderFor(cwd, dest, sizeof (dest)))
		snprintf(dest, sizeof (dest), "%s", WEB_USER_ROOT);

	if (!makeDirs(dest) || chdir(dest) != 0)
	{
		okBox(0, "System message", "Can't save here! Go to one of your own folders first.", NULL);
		return false;
	}

	if (showFolder)
		editor.diskOpReadDir = true; // show the folder the file goes to

	char msg[PATH_MAX + 80];
	snprintf(msg, sizeof (msg), "%sLagrer i %s", fromModland ? "Modland er bare til lesing. " : "",
		dest + strlen("/home/web_user/"));
	bt2_toast(msg);
	return true;
}

void bt2DiskOpConfineDir(void)
{
	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof (cwd)) != NULL && (pathInside(cwd, WEB_USER_ROOT) || pathInside(cwd, WEB_MODLAND_ROOT)))
		return;

	// "../" from the top folder (or "Set path" somewhere else): back to the user's own folders
	if (chdir(WEB_USER_ROOT) != 0)
		makeDirs(WEB_USER_ROOT);
}

// ------------------------------------------------------------------------------------------------
// Modland: the folders, the songs and their rules are in bt2_modland.c (the same as in the Windows program).
// The page downloads (with its progress box) and keeps the list in the browser's cache.

static void initModland(void)
{
	bt2ModlandSetup ml;
	memset(&ml, 0, sizeof (ml));
	snprintf(ml.mirrorRoot, sizeof (ml.mirrorRoot), "%s", WEB_MODLAND_ROOT); // (in memory only)
	snprintf(ml.ownRoot, sizeof (ml.ownRoot), "%s/Modland", WEB_USER_ROOT);
	// (no list file: the page keeps the list)
	bt2ModlandInit(&ml);
}

// the page: allmods.zip -> the Fasttracker 2 lines (malloc'ed; the page frees it). NULL = not a zip file with the list.
EMSCRIPTEN_KEEPALIVE char *webModlandListFromZip(const uint8_t *zip, int32_t len)
{
	size_t outLen = 0;
	return (zip != NULL && len > 0) ? bt2ModlandListFromZip(zip, (size_t)len, &outLen) : NULL;
}

// the page: the list ("size<TAB>Artist/song.xm" lines; malloc'ed by the page, kept or freed here)
EMSCRIPTEN_KEEPALIVE int32_t webModlandUseList(char *text, int32_t len)
{
	if (text != NULL && len <= 0)
	{
		free(text);
		return 0;
	}

	return (text != NULL && bt2ModlandUseList(text, (size_t)len)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int32_t webModlandIsLoaded(void)
{
	return bt2ModlandIsLoaded() ? 1 : 0;
}

EM_ASYNC_JS(int32_t, bt2_modlandLoad, (void), {
	try { return (await globalThis.BT2.modlandLoad()) ? 1 : 0; }
	catch (e) { console.error('modland', e); return 0; }
});

bool bt2ModlandOpen(void)
{
	if (!bt2ModlandIsLoaded() && bt2_modlandLoad() == 0)
		return false; // (the page said why)

	return makeDirs(WEB_MODLAND_ROOT);
}

void bt2DiskOpBeforeList(const char *dir) // (FT2's folder reading: Modland's folders get their songs, empty files, the first time)
{
	if (dir != NULL && bt2ModlandIsMirrorPath(dir))
		bt2ModlandPopulate(dir);
}

const char *bt2ModlandRootU(void)
{
	return WEB_MODLAND_ROOT;
}

const char *bt2HomeRootU(void)
{
	return WEB_USER_ROOT;
}

// "a/../b" or a name in the current folder -> "/x/b"
static void absolutePath(const char *path, char *out, size_t outLen)
{
	char tmp[PATH_MAX * 2];
	if (path[0] == '/')
	{
		snprintf(tmp, sizeof (tmp), "%s", path);
	}
	else
	{
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof (cwd)) == NULL)
			snprintf(cwd, sizeof (cwd), "/");
		snprintf(tmp, sizeof (tmp), "%s/%s", cwd, path);
	}

	size_t o = 0;
	out[0] = '\0';
	for (char *part = strtok(tmp, "/"); part != NULL; part = strtok(NULL, "/"))
	{
		if (strcmp(part, ".") == 0)
			continue;

		if (strcmp(part, "..") == 0)
		{
			while (o > 0 && out[o - 1] != '/')
				o--;
			if (o > 0)
				o--; // (the '/')
			out[o] = '\0';
			continue;
		}

		const int32_t w = snprintf(out + o, outLen - o, "/%s", part);
		if (w < 0 || (size_t)w >= outLen - o)
			break;
		o += (size_t)w;
	}

	if (o == 0)
		snprintf(out, outLen, "/");
}

EM_ASYNC_JS(int32_t, bt2_prepareFile, (const char *path), {
	try { return (await globalThis.BT2.prepareFile(UTF8ToString(path))) ? 1 : 0; }
	catch (e) { console.error('prepareFile', e); return 0; }
});

// a file from the web, with the page's progress box: malloc'ed (with a 0 byte after it), NULL = failed (the page said why)
EM_ASYNC_JS(uint8_t *, bt2_download, (const char *url, const char *label, int32_t *outLen), {
	const data = await globalThis.BT2.download(UTF8ToString(url), UTF8ToString(label));
	if (!data)
		return 0;
	const p = _malloc(data.length + 1);
	if (!p) {
		globalThis.BT2.toast('Kunne ikke lagre filen');
		return 0;
	}
	HEAPU8.set(data, p);
	HEAPU8[p + data.length] = 0;
	HEAP32[outLen >> 2] = data.length;
	return p;
});

/* Before FT2 opens a file: a song in Modland's folders (bt2ModlandPrepareSong: the user's own copy first, else it is
** downloaded the first time and kept in BT2/Modland), and the page's Demo Tracks (downloaded when first opened).
*/
bool bt2PrepareFile(const char *path)
{
	if (path == NULL)
		return true;

	char abs[PATH_MAX];
	absolutePath(path, abs, sizeof (abs));
	if (!bt2ModlandIsMirrorPath(abs))
		return bt2_prepareFile(abs) != 0;

	struct stat st;
	if (stat(abs, &st) != 0 || S_ISDIR(st.st_mode))
		return true; // (FT2 says what is wrong)

	if (!bt2ModlandIsLoaded() && !bt2ModlandOpen())
		return st.st_size > 0; // (the page said why; a song that came before can still be opened)

	bt2ModlandSong *song = (bt2ModlandSong *)malloc(sizeof (bt2ModlandSong));
	if (song == NULL)
		return true;

	char msg[2400];
	const int32_t what = bt2ModlandPrepareSong(abs, song, msg, sizeof (msg));
	if (msg[0] != '\0')
	{
		bt2_toast(msg);
		bt2_syncFiles(); // (a new copy in BT2/Modland)
	}

	if (what != BT2_SONG_DOWNLOAD)
	{
		free(song);
		return what != BT2_SONG_FAILED;
	}

	int32_t len = 0;
	uint8_t *data = bt2_download(song->url, song->label, &len);
	if (data == NULL)
	{
		free(song);
		return false;
	}

	bool ownSaved = false;
	const bool ok = bt2ModlandStoreSong(song, data, (size_t)len, &ownSaved);
	free(data);

	if (ok)
		bt2ModlandStoredMessage(song, ownSaved, msg, sizeof (msg));
	else
		snprintf(msg, sizeof (msg), "Kunne ikke lagre filen");
	bt2_toast(msg);
	free(song);

	bt2_syncFiles();
	return ok;
}

bool bt2PrepareDroppedFile(const char *path)
{
	return bt2PrepareFile(path);
}

// ------------------------------------------------------------------------------------------------
// the search window: the page shows it (the iPad's keyboard needs a real text box), the program finds
// (bt2_search.c, the same as in the Windows program)

// "Find": the page's search window. (The page also opens it right in the tap/click, so the iPad keyboard can come up.)
EM_JS(void, bt2_searchOpen, (void), { if (globalThis.BT2 && globalThis.BT2.search) globalThis.BT2.search.open(); });

void bt2SearchOpen(void)
{
	bt2_searchOpen();
}

EMSCRIPTEN_KEEPALIVE void webDiskOpShowFolder(const char *path, int32_t item); // (below)

static bt2SearchPool ownPool, modPool;
static bool modPoolReady;
static bt2SearchItem **results;
static int32_t numResults;
static char *resultsJson;

// the search window was opened: the user's own files as they are now
EMSCRIPTEN_KEEPALIVE void webSearchOpen(void)
{
	free(results); // (the last results point into the old list)
	results = NULL;
	numResults = 0;
	bt2SearchBuildOwn(&ownPool, WEB_USER_ROOT);
}

// Modland's list is there: its songs and folders are searched too. 0 = not yet.
EMSCRIPTEN_KEEPALIVE int32_t webSearchModland(void)
{
	if (!modPoolReady && bt2ModlandIsLoaded())
	{
		bt2SearchBuildModland(&modPool);
		modPoolReady = true;
	}

	return modPoolReady ? 1 : 0;
}

// where: 0 all, 1 own files, 2 Modland. what: 0 all, 1 songs, 2 folders, 3 instruments/samples. Returns the number of hits.
EMSCRIPTEN_KEEPALIVE int32_t webSearchRun(const char *query, int32_t where, int32_t what)
{
	free(results);
	results = NULL;
	numResults = bt2SearchRun((query != NULL) ? query : "", where, what, &ownPool, modPoolReady ? &modPool : NULL, &results);
	return numResults;
}

static bool jsonAdd(char **buf, size_t *len, size_t *cap, const char *text, size_t n)
{
	if (*len + n + 1 > *cap)
	{
		size_t c = (*cap > 0) ? *cap : 65536;
		while (c < *len + n + 1)
			c *= 2;

		char *bigger = (char *)realloc(*buf, c);
		if (bigger == NULL)
			return false;
		*buf = bigger;
		*cap = c;
	}

	memcpy(*buf + *len, text, n);
	*len += n;
	(*buf)[*len] = '\0';
	return true;
}

static bool jsonString(char **buf, size_t *len, size_t *cap, const char *s)
{
	if (!jsonAdd(buf, len, cap, "\"", 1))
		return false;

	for (const uint8_t *p = (const uint8_t *)s; *p != '\0'; p++)
	{
		char e[8];
		size_t n = 0;
		if (*p == '"' || *p == '\\')
		{
			e[0] = '\\';
			e[1] = (char)*p;
			n = 2;
		}
		else if (*p < 0x20)
		{
			n = (size_t)snprintf(e, sizeof (e), "\\u%04x", (unsigned)*p);
		}
		else
		{
			e[0] = (char)*p;
			n = 1;
		}

		if (!jsonAdd(buf, len, cap, e, n))
			return false;
	}

	return jsonAdd(buf, len, cap, "\"", 1);
}

// hits [from, from + count) for the page: [{"r": path, "k": kind, "m": Modland, "z": size, "c": songs in a folder}]
EMSCRIPTEN_KEEPALIVE const char *webSearchResults(int32_t from, int32_t count)
{
	free(resultsJson);
	resultsJson = NULL;

	size_t len = 0, cap = 0;
	bool ok = jsonAdd(&resultsJson, &len, &cap, "[", 1);
	for (int32_t i = from; ok && i >= 0 && i < numResults && i < from + count; i++)
	{
		const bt2SearchItem *it = results[i];
		char nums[96];
		ok = jsonAdd(&resultsJson, &len, &cap, (i > from) ? ",{\"r\":" : "{\"r\":", (i > from) ? 6 : 5) &&
			jsonString(&resultsJson, &len, &cap, it->rel);
		if (ok)
		{
			const int32_t n = snprintf(nums, sizeof (nums), ",\"k\":%d,\"m\":%d,\"z\":%lld,\"c\":%d}", (int)it->kind,
				it->modland ? 1 : 0, (long long)it->size, (int)it->count);
			ok = jsonAdd(&resultsJson, &len, &cap, nums, (size_t)n);
		}
	}

	if (!ok || !jsonAdd(&resultsJson, &len, &cap, "]", 1))
	{
		free(resultsJson);
		resultsJson = strdup("[]");
	}

	return (resultsJson != NULL) ? resultsJson : "[]";
}

// a hit was chosen: a song, instrument or sample is opened (Modland's are downloaded first), a folder is shown in Disk Op.
// showFolder: the hit's folder instead. 0 = it wasn't found.
EMSCRIPTEN_KEEPALIVE int32_t webSearchAct(int32_t index, int32_t showFolder, int32_t what)
{
	if (index < 0 || index >= numResults)
		return 0;

	const bt2SearchItem *it = results[index];
	char folder[PATH_MAX], file[PATH_MAX];
	int32_t item = BT2_ITEM_MODULE;
	if (!bt2SearchTarget(it, what, WEB_USER_ROOT, folder, sizeof (folder), file, sizeof (file), &item))
	{
		bt2_toast("Fant ikke mappa");
		return 0;
	}

	if (it->kind == BT2_KIND_FOLDER || showFolder)
		webDiskOpShowFolder(folder, item);
	else
		webLoadFile(file); // (like a dropped file)

	return 1;
}

// ------------------------------------------------------------------------------------------------
// Imp. and dropped files: the page gives the files, bt2_import.c decides (the same as in the Windows program).
// A zip file comes one file at a time (the page unpacks with the browser), so a big one is never in this memory all at once.

static bt2Import *pageImport;
static bt2ImportZip *pageZip;
static bt2ZipEntry *zipList; // (the zip file's table of contents, from the page)
static int32_t zipCount, zipCap;
static bool zipListBad;
static char importFirst[PATH_MAX];

static void importToast(const char *text)
{
	bt2_toast(text);
}

static void endPageZip(void)
{
	bt2ImportZipEnd(pageZip);
	pageZip = NULL;
	free(zipList);
	zipList = NULL;
	zipCount = zipCap = 0;
	zipListBad = false;
}

// count: how many files will come. 0 = failed.
EMSCRIPTEN_KEEPALIVE int32_t webImportBegin(int32_t count)
{
	if (pageImport != NULL) // (one that wasn't ended)
	{
		endPageZip();
		bt2ImportEnd(pageImport);
		pageImport = NULL;
	}

	// into the folder Disk Op. is in, if it is one of the user's own (else the BT2 folder)
	char cwd[PATH_MAX];
	const bool here = getcwd(cwd, sizeof (cwd)) != NULL && pathInside(cwd, WEB_USER_ROOT);
	pageImport = bt2ImportBegin(here ? cwd : WEB_USER_ROOT, WEB_USER_ROOT, here, importToast, NULL, NULL, NULL, count);
	importFirst[0] = '\0';
	return (pageImport != NULL) ? 1 : 0;
}

// one file (data NULL: it couldn't be read). keepFolders: a file from a dropped folder, name = "folder/sub/file"
EMSCRIPTEN_KEEPALIVE void webImportData(const char *name, const uint8_t *data, int32_t len, int32_t keepFolders)
{
	if (pageImport == NULL || name == NULL || len < 0)
		return;

	if (keepFolders)
		bt2ImportCopy(pageImport, name, data, (size_t)len);
	else
		bt2ImportData(pageImport, name, data, (size_t)len);
}

// a zip file: first its table of contents, entry i = 0, 1, 2 ... (the name in UTF-8 with '/' between folders, a folder
// ends with '/'; the size and CRC-32 as the zip file says)
EMSCRIPTEN_KEEPALIVE void webImportZipAdd(int32_t i, const char *name, int32_t dir, uint32_t size, uint32_t crc)
{
	if (i == 0)
		endPageZip(); // (a new zip file: anything left from one that wasn't ended goes)

	if (i != zipCount)
	{
		zipListBad = true; // (they must come in order)
		return;
	}

	if (zipCount == zipCap)
	{
		const int32_t cap = (zipCap > 0) ? zipCap * 2 : 256;
		bt2ZipEntry *bigger = (bt2ZipEntry *)realloc(zipList, (size_t)cap * sizeof (bt2ZipEntry));
		if (bigger == NULL)
		{
			zipListBad = true;
			return;
		}

		zipList = bigger;
		zipCap = cap;
	}

	bt2ZipEntry *e = &zipList[zipCount++];
	memset(e, 0, sizeof (bt2ZipEntry));
	if (name == NULL || strlen(name) >= sizeof (e->name))
		zipListBad = true; // (the page leaves such names out, like bt2ZipList does)
	else
		snprintf(e->name, sizeof (e->name), "%s", name);

	e->dir = (dir != 0);
	e->size = size;
	e->crc = crc;
}

// then this (the zip file's name). 0 = nothing more to do (the message is given).
EMSCRIPTEN_KEEPALIVE int32_t webImportZipBegin(const char *zipName)
{
	pageZip = bt2ImportZipBegin(pageImport, (zipName != NULL) ? zipName : "", zipListBad ? NULL : zipList, zipListBad ? 0 : zipCount);
	if (pageZip == NULL)
		endPageZip();

	return (pageZip != NULL) ? 1 : 0;
}

// 1 = the page should unpack entry i and give it with webImportZipEntry
EMSCRIPTEN_KEEPALIVE int32_t webImportZipWants(int32_t i)
{
	return bt2ImportZipWants(pageZip, i) ? 1 : 0;
}

// entry i, unpacked by the page (data NULL: it couldn't be). Checked like bt2ZipExtract does: the size and the CRC-32.
EMSCRIPTEN_KEEPALIVE void webImportZipEntry(int32_t i, const uint8_t *data, int32_t len)
{
	if (pageZip == NULL || i < 0 || i >= zipCount)
		return;

	const bt2ZipEntry *e = &zipList[i];
	if (data != NULL && (len < 0 || (uint32_t)len != e->size || bt2Crc32(data, (size_t)len) != e->crc))
		data = NULL; // (damaged)

	bt2ImportZipEntry(pageZip, i, data, (data != NULL) ? (size_t)len : 0);
}

// entry i still packed ("deflate"), from a browser that can't unpack: unpacked here
EMSCRIPTEN_KEEPALIVE void webImportZipEntryPacked(int32_t i, const uint8_t *packed, int32_t packedLen)
{
	if (pageZip == NULL || i < 0 || i >= zipCount)
		return;

	const bt2ZipEntry *e = &zipList[i];
	uint8_t *out = (packed != NULL && packedLen >= 0) ? (uint8_t *)malloc((size_t)e->size + 1) : NULL;
	size_t got = 0;
	if (out != NULL && !(bt2Inflate(packed, (size_t)packedLen, out, e->size, &got) && got == e->size))
	{
		free(out);
		out = NULL;
	}

	webImportZipEntry(i, out, (out != NULL) ? (int32_t)e->size : 0);
	free(out);
}

EMSCRIPTEN_KEEPALIVE void webImportZipEnd(void) // (the zip file's message, e.g. a backup's)
{
	endPageZip();
}

// the message, and the number of files written
EMSCRIPTEN_KEEPALIVE int32_t webImportEnd(void)
{
	endPageZip(); // (a zip file that wasn't ended)
	if (pageImport == NULL)
		return 0;

	snprintf(importFirst, sizeof (importFirst), "%s", bt2ImportFirstFile(pageImport));
	const int32_t n = bt2ImportEnd(pageImport);
	pageImport = NULL;
	return n;
}

EMSCRIPTEN_KEEPALIVE const char *webImportFirstFile(void) // the first file written (not from a zip), for opening it
{
	return importFirst;
}

EMSCRIPTEN_KEEPALIVE void webImportDone(void) // called from JS when imported files have been written
{
	editor.diskOpReadDir = true; // refresh the Disk Op. file list
}

EMSCRIPTEN_KEEPALIVE const char *webGetExportPath(void) // called from JS (Export button)
{
	return webDiskOpGetFilePath();
}

static void makeDir(const char *path)
{
	mkdir(path, 0777); // fails harmlessly if it exists
}

static void setDefaultPath(char *configPath, const char *path)
{
	// use the default folder if no folder is stored, or the stored one doesn't exist anymore
	struct stat st;
	if (configPath[0] == '\0' || stat(configPath, &st) != 0 || !S_ISDIR(st.st_mode))
	{
		strncpy(configPath, path, 79);
		configPath[79] = '\0';
	}
}

void webInit(void) // after the config has been loaded
{
	initModland();

	// the user's own folders (stored in the browser, kept between sessions)
	const char *home = "/home/web_user";
	makeDir("/home/web_user/BT2");
	makeDir("/home/web_user/BT2/Modules");
	makeDir("/home/web_user/BT2/Instruments");
	makeDir("/home/web_user/BT2/Samples");
	makeDir("/home/web_user/BT2/Patterns");
	makeDir("/home/web_user/BT2/Tracks");
	(void)home;

	setDefaultPath(config.modulesPath,  "/home/web_user/BT2/Modules");
	setDefaultPath(config.instrPath,    "/home/web_user/BT2/Instruments");
	setDefaultPath(config.samplesPath,  "/home/web_user/BT2/Samples");
	setDefaultPath(config.patternsPath, "/home/web_user/BT2/Patterns");
	setDefaultPath(config.tracksPath,   "/home/web_user/BT2/Tracks");

	// the browser/iPad shows its own pointer (or none for touch), the FT2 pointer would get stuck
	config.specialFlags2 |= HARDWARE_MOUSE;

	/* One-time switch to the FT2-like defaults (blue palette, linear interpolation like the
	** original, volume ramping) for people who opened an earlier test version.
	*/
	struct stat st;
	if (stat("/home/web_user/BT2/.defaults-v1", &st) != 0)
	{
		config.cfg_StdPalNum = PAL_BLUES;
		setPalette(palTable[PAL_BLUES], false);
		config.interpolation = INTERPOLATION_LINEAR;
		audio.interpolationType = INTERPOLATION_LINEAR;
		config.specialFlags &= ~NO_VOLRAMP_FLAG;
		audioSetVolRamp(true);

		FILE *f = fopen("/home/web_user/BT2/.defaults-v1", "wb");
		if (f != NULL)
			fclose(f);
	}

	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
}

static config_t lastSavedConfig;
static uint32_t frameCounter, settingsCounter;

// settings (colours, interpolation, Disk Op. folders ...): stored right after a change. Updates of the
// app never touch them (they are the user's own file, FT2.CFG, in the browser's storage).
static void saveSettingsIfChanged(void)
{
	webDiskOpStorePathsInConfig();
	config.specialFlags2 |= HARDWARE_MOUSE;

	if (memcmp(&lastSavedConfig, &config, sizeof (config_t)) != 0 && !editor.editTextFlag)
	{
		saveConfig(CONFIG_HIDE_ERRORS);
		memcpy(&lastSavedConfig, &config, sizeof (config_t));
	}
}

// from the page, when the app goes to the background or is closed
EMSCRIPTEN_KEEPALIVE void webSaveSettingsNow(void)
{
	if (editor.mainLoopOngoing)
		saveSettingsIfChanged();
}

// FT2's colours for the page's own windows (the search window looks like the rest of the program)
EMSCRIPTEN_KEEPALIVE int32_t webPaletteRGB(int32_t index)
{
	if (index < 0 || index >= PAL_NUM)
		return 0;

	return (int32_t)(video.palette[index] & 0xFFFFFF);
}

// the search window: "show this folder in Disk Op." (done in the main loop, see webHandlePageRequests)
static char pendingFolder[PATH_MAX];
static int32_t pendingFolderItem = -1;

EMSCRIPTEN_KEEPALIVE void webDiskOpShowFolder(const char *path, int32_t item)
{
	snprintf(pendingFolder, sizeof (pendingFolder), "%s", path);
	pendingFolderItem = item;
}

void webHandlePageRequests(void)
{
	if (pendingFolder[0] == '\0' || ui.sysReqShown)
		return;

	if (!ui.diskOpShown)
		showDiskOpScreen();

	// the right kind of files (songs, instruments, samples), then the folder
	if (pendingFolderItem >= 0 && pendingFolderItem != getDiskOpItem())
	{
		if (pendingFolderItem == DISKOP_ITEM_INSTR)
			rbDiskOpInstr();
		else if (pendingFolderItem == DISKOP_ITEM_SAMPLE)
			rbDiskOpSample();
		else
			rbDiskOpModule();
	}

	if (chdir(pendingFolder) != 0)
		bt2_toast("Fant ikke mappa");

	editor.diskOpReadDir = true;
	pendingFolder[0] = '\0';
}

void webAfterSetup(void)
{
	memcpy(&lastSavedConfig, &config, sizeof (config_t));
	EM_ASM({ globalThis.BT2.appStarted(); });
}

EM_JS(void, bt2_frame, (int32_t rx, int32_t ry, int32_t rw, int32_t rh, int32_t diskOpShown, int32_t screenH), {
	globalThis.BT2.frame(rx, ry, rw, rh, diskOpShown, screenH);
});

void webFrame(void)
{
	if (audioMode == 0)
		updateAudioClockAnchor();

	// the scopes move at FT2's 64 Hz, whatever the screen's frame rate is
	static double lastScopeTime, scopeSteps;
	const double now = emscripten_get_now();
	if (lastScopeTime == 0.0)
		lastScopeTime = now - (1000.0 / SCOPE_HZ);

	scopeSteps += (now - lastScopeTime) * (SCOPE_HZ / 1000.0);
	lastScopeTime = now;
	if (scopeSteps > 4.0)
		scopeSteps = 4.0; // after a pause (app in the background): don't race to catch up

	const int32_t steps = (int32_t)scopeSteps;
	scopeSteps -= steps;
	webScopesTick(steps);

	// tell the page where the FT2 screen is (for the Import/Export buttons in Disk Op.)
	bt2_frame(video.renderX, video.renderY, video.renderW, video.renderH, webDiskOpIsShown() ? 1 : 0, SCREEN_H);

	// settings: about twice a second, check if they changed (then they are saved and stored at once)
	if (++settingsCounter >= 30)
	{
		settingsCounter = 0;
		saveSettingsIfChanged();
	}

	// every ~3 seconds: store anything that changed (files are also stored right after they are written)
	if (++frameCounter >= 180)
	{
		frameCounter = 0;
		if (!editor.wavIsRendering) // (not while a WAV file is being written: it would be copied again and again)
			bt2_syncIfChanged();
	}
}

#endif // __EMSCRIPTEN__
