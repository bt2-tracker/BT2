// BerntTracker: web (browser) version glue. Only compiled with Emscripten.
//
// Two audio modes:
//  BT2_WEB_WORKLET defined: FT2's mixer runs directly on the browser's audio thread
//                           (Wasm AudioWorklet, 128 samples per block = lowest latency).
//                           Needs a "cross-origin isolated" page (SharedArrayBuffer).
//  otherwise ("compat"):    the mixer runs on the main thread and small blocks are queued
//                           to a plain AudioWorklet. Works everywhere, a bit more latency.

#ifdef __EMSCRIPTEN__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <emscripten.h>
#ifdef BT2_WEB_WORKLET
#include <emscripten/webaudio.h>
#endif
#include "ft2_header.h"
#include "ft2_web.h"
#include "ft2_config.h"
#include "ft2_structs.h"
#include "ft2_video.h"
#include "ft2_diskop.h"
#include "ft2_audio.h"
#include "scopes/ft2_scopes.h"
#include "ft2_palette.h"
#include "ft2_tables.h"
#include "mixer/ft2_mix.h"

void webScopesTick(void); // ft2_scopes.c
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

static SDL_AudioCallback audioCb;
static uint32_t audioRate = 48000;
static uint64_t perfFreq = 1000000;

#ifdef BT2_WEB_WORKLET

#define QUANTUM 128

static EMSCRIPTEN_WEBAUDIO_T audioCtx;
static volatile int32_t audioLockState; // 0 = free, 1 = main thread, 2 = audio thread
static int32_t mainLockDepth;
static volatile int32_t audioIsPaused = 1, audioReady, audioFailed;
static float interleaved[QUANTUM * 2];
static volatile uint64_t framesRendered;
static volatile uint32_t anchorSeq;
static volatile uint64_t anchorCounter, anchorFrames;
static uint8_t audioThreadStack[512 * 1024] __attribute__((aligned(16)));

static EM_BOOL processAudio(int numInputs, const AudioSampleFrame *inputs, int numOutputs, AudioSampleFrame *outputs,
	int numParams, const AudioParamFrame *params, void *userData)
{
	(void)numInputs; (void)inputs; (void)numParams; (void)params; (void)userData;

	if (numOutputs < 1 || outputs[0].numberOfChannels < 2)
		return EM_TRUE;

	float *outL = outputs[0].data;
	float *outR = outputs[0].data + QUANTUM;

	int32_t expected = 0;
	if (!audioIsPaused && __atomic_compare_exchange_n(&audioLockState, &expected, 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
	{
		audioCb(NULL, (Uint8 *)interleaved, QUANTUM * 2 * sizeof (float));
		__atomic_store_n(&audioLockState, 0, __ATOMIC_RELEASE);

		for (int32_t i = 0; i < QUANTUM; i++)
		{
			outL[i] = interleaved[(i << 1) + 0];
			outR[i] = interleaved[(i << 1) + 1];
		}
	}
	else
	{
		memset(outL, 0, QUANTUM * sizeof (float));
		memset(outR, 0, QUANTUM * sizeof (float));
	}

	__atomic_add_fetch(&framesRendered, QUANTUM, __ATOMIC_SEQ_CST);
	return EM_TRUE;
}

uint64_t webAudioPerfCounter(void) // runs on the audio thread (no performance.now() there)
{
	uint32_t seq;
	uint64_t counter, frames;
	do
	{
		seq = __atomic_load_n(&anchorSeq, __ATOMIC_ACQUIRE);
		counter = anchorCounter;
		frames = anchorFrames;
	}
	while ((seq & 1) || seq != __atomic_load_n(&anchorSeq, __ATOMIC_ACQUIRE));

	const int64_t framesSince = (int64_t)(__atomic_load_n(&framesRendered, __ATOMIC_SEQ_CST) - frames);
	return counter + (uint64_t)(((double)framesSince * (double)perfFreq) / audioRate);
}

static void updateAudioClockAnchor(void) // main thread, once per frame
{
	__atomic_add_fetch(&anchorSeq, 1, __ATOMIC_ACQ_REL);
	anchorCounter = SDL_GetPerformanceCounter();
	anchorFrames = __atomic_load_n(&framesRendered, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&anchorSeq, 1, __ATOMIC_ACQ_REL);
}

static void onProcessorCreated(EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success, void *userData)
{
	(void)userData;
	if (!success)
	{
		audioFailed = 1;
		return;
	}

	int outputChannelCounts[1] = { 2 };
	EmscriptenAudioWorkletNodeCreateOptions options;
	options.numberOfInputs = 0;
	options.numberOfOutputs = 1;
	options.outputChannelCounts = outputChannelCounts;

	EMSCRIPTEN_AUDIO_WORKLET_NODE_T node = emscripten_create_wasm_audio_worklet_node(ctx, "bt2-mixer", &options, processAudio, NULL);
	EM_ASM({
		var ctx = emscriptenGetAudioObject($0);
		var node = emscriptenGetAudioObject($1);
		node.connect(ctx.destination);
		globalThis.BT2.audioStarted(ctx, 'worklet', node);
	}, ctx, node);

	audioReady = 1;
}

static void onAudioThreadStarted(EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success, void *userData)
{
	(void)userData;
	if (!success)
	{
		audioFailed = 1;
		return;
	}

	WebAudioWorkletProcessorCreateOptions opts;
	memset(&opts, 0, sizeof (opts));
	opts.name = "bt2-mixer";
	emscripten_create_wasm_audio_worklet_processor_async(ctx, &opts, onProcessorCreated, NULL);
}

SDL_AudioDeviceID webAudioOpen(SDL_AudioCallback callback, const SDL_AudioSpec *want, SDL_AudioSpec *have)
{
	static bool contextCreated;

	audioCb = callback;
	perfFreq = SDL_GetPerformanceFrequency();
	*have = *want;

	if (!contextCreated) // one AudioContext + audio thread for the whole session
	{
		EmscriptenWebAudioCreateAttributes attr;
		attr.latencyHint = "interactive";
		attr.sampleRate = 0; // use the device's own rate (no resampling = less latency)
		audioCtx = emscripten_create_audio_context(&attr);
		if (audioCtx == 0)
			return 0;

		audioRate = (uint32_t)EM_ASM_INT({ return emscriptenGetAudioObject($0).sampleRate; }, audioCtx);
		emscripten_start_wasm_audio_worklet_thread_async(audioCtx, audioThreadStack, sizeof (audioThreadStack), onAudioThreadStarted, NULL);

		for (int32_t i = 0; i < 1000 && !audioReady && !audioFailed; i++)
			emscripten_sleep(10);

		if (!audioReady)
			return 0;

		contextCreated = true;
	}

	// total output latency estimate (one block + what the browser reports), used for visual sync
	const int32_t latencyFrames = EM_ASM_INT({
		var ctx = emscriptenGetAudioObject($0);
		var l = (ctx.baseLatency || 0) + (ctx.outputLatency || 0);
		if (!(l > 0)) l = 256 / ctx.sampleRate;
		return Math.round(l * ctx.sampleRate);
	}, audioCtx);

	have->freq = (int)audioRate;
	have->format = AUDIO_F32;
	have->channels = 2;
	have->samples = (Uint16)(QUANTUM + latencyFrames);

	audioIsPaused = 1;
	return 1;
}

void webAudioClose(void)
{
	audioIsPaused = 1; // the context and audio thread are kept for the next open
}

void webAudioPause(bool pause)
{
	if (!pause)
		updateAudioClockAnchor();

	audioIsPaused = pause ? 1 : 0;
}

void webAudioLock(void)
{
	if (mainLockDepth++ == 0)
	{
		int32_t expected = 0;
		while (!__atomic_compare_exchange_n(&audioLockState, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			expected = 0; // the audio thread holds it for well under a millisecond
	}
}

void webAudioUnlock(void)
{
	if (mainLockDepth > 0 && --mainLockDepth == 0)
		__atomic_store_n(&audioLockState, 0, __ATOMIC_RELEASE);
}

#else // compat audio: mixer on the main thread, blocks queued to a JS AudioWorklet

#define CHUNK_MAX 1024

static bool audioIsPaused = true;
static int32_t mainLockDepth;
static float chunkBuffer[CHUNK_MAX * 2];

EMSCRIPTEN_KEEPALIVE float *webAudioRenderChunk(int32_t frames) // called from JS
{
	if (frames > CHUNK_MAX)
		frames = CHUNK_MAX;

	if (audioIsPaused || mainLockDepth > 0 || audioCb == NULL)
		memset(chunkBuffer, 0, frames * 2 * sizeof (float));
	else
		audioCb(NULL, (Uint8 *)chunkBuffer, frames * 2 * sizeof (float));

	return chunkBuffer;
}

EM_ASYNC_JS(int32_t, bt2_openCompatAudio, (void), {
	return await globalThis.BT2.openCompatAudio();
});

EM_JS(int32_t, bt2_compatLatencyFrames, (void), {
	return globalThis.BT2.compatLatencyFrames();
});

uint64_t webAudioPerfCounter(void)
{
	return SDL_GetPerformanceCounter(); // the mixer runs on the main thread here
}

SDL_AudioDeviceID webAudioOpen(SDL_AudioCallback callback, const SDL_AudioSpec *want, SDL_AudioSpec *have)
{
	audioCb = callback;
	perfFreq = SDL_GetPerformanceFrequency();
	*have = *want;

	const int32_t rate = bt2_openCompatAudio();
	if (rate <= 0)
		return 0;

	audioRate = (uint32_t)rate;
	have->freq = rate;
	have->format = AUDIO_F32;
	have->channels = 2;
	have->samples = (Uint16)bt2_compatLatencyFrames();

	audioIsPaused = true;
	return 1;
}

void webAudioClose(void)
{
	audioIsPaused = true;
}

void webAudioPause(bool pause)
{
	audioIsPaused = pause;
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

#endif

// ------------------------------------------------------------------------------------------------
// files

EM_JS(void, bt2_syncFiles, (void), { globalThis.BT2.syncFiles(); });

void webSyncFiles(void)
{
	bt2_syncFiles();
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
static uint32_t frameCounter;

void webAfterSetup(void)
{
	memcpy(&lastSavedConfig, &config, sizeof (config_t));
	EM_ASM({ globalThis.BT2.appStarted(); });
}

EM_JS(void, bt2_frame, (int32_t rx, int32_t ry, int32_t rw, int32_t rh, int32_t diskOpShown), {
	globalThis.BT2.frame(rx, ry, rw, rh, diskOpShown);
});

void webFrame(void)
{
#ifdef BT2_WEB_WORKLET
	updateAudioClockAnchor();
#endif

	webScopesTick();

	// tell the page where the FT2 screen is (for the Import/Export buttons in Disk Op.)
	bt2_frame(video.renderX, video.renderY, video.renderW, video.renderH, webDiskOpIsShown() ? 1 : 0);

	// every ~3 seconds: remember Disk Op. folders, save config if it changed
	if (++frameCounter >= 180)
	{
		frameCounter = 0;

		webDiskOpStorePathsInConfig();
		config.specialFlags2 |= HARDWARE_MOUSE;

		if (memcmp(&lastSavedConfig, &config, sizeof (config_t)) != 0 && !editor.editTextFlag)
		{
			saveConfig(CONFIG_HIDE_ERRORS);
			memcpy(&lastSavedConfig, &config, sizeof (config_t));
		}

		webSyncFiles();
	}
}

#endif // __EMSCRIPTEN__
