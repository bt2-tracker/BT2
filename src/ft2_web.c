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
#include "ft2_keyboard.h"
#include "ft2_replayer.h"
#include "ft2_pattern_ed.h"
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
static int32_t mainLockDepth;
static float chunkBuffer[CHUNK_MAX * 2];
static bool engineAttached;

EM_JS(int32_t, bt2_wantedEngine, (int32_t hasWorklet), {
	return globalThis.BT2.wantedEngineCode(hasWorklet);
});

EM_JS(int32_t, bt2_engineLatencyFrames, (void), {
	return globalThis.BT2.engineLatencyFrames();
});

#ifdef BT2_WEB_WORKLET

#define QUANTUM 128

static EMSCRIPTEN_WEBAUDIO_T audioCtx;
static volatile int32_t audioLockState; // 0 = free, 1 = main thread, 2 = audio thread
static volatile int32_t audioReady, audioFailed, workletStarted;
static float interleaved[QUANTUM * 2];
static volatile uint64_t framesRendered;
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

	int32_t expected = 0;
	if (audioMode == 0 && !audioIsPaused &&
		__atomic_compare_exchange_n(&audioLockState, &expected, 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
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
	EM_ASM({ globalThis.BT2.workletNode = emscriptenGetAudioObject($0); }, node);
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

static int32_t openEngine(void) // first open: create the AudioContext and attach the wanted engine
{
	EmscriptenWebAudioCreateAttributes attr;
	attr.latencyHint = "interactive";
	attr.sampleRate = 0; // use the device's own rate (no resampling = less latency)
	audioCtx = emscripten_create_audio_context(&attr);
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
		audioCb(NULL, (Uint8 *)chunkBuffer, frames * 2 * sizeof (float));

	return chunkBuffer;
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

	have->freq = (int)audioRate;
	have->format = AUDIO_F32;
	have->channels = 2;
	have->samples = (Uint16)bt2_engineLatencyFrames(); // used to line up the scopes/pattern with the sound

	audioIsPaused = 1;
	return 1;
}

void webAudioClose(void)
{
	audioIsPaused = 1; // the context is kept for the next open
}

void webAudioPause(bool pause)
{
	if (!pause && audioMode == 0)
		updateAudioClockAnchor();

	audioIsPaused = pause ? 1 : 0;
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
		"\"wavShown\":%d,\"wavRendering\":%d}",
		row, ch, cursor.object, patt, (patt >= 0 && patt < MAX_PATTERNS) ? patternNumRows[patt] : 0, song.songPos, playMode,
		songPlaying ? 1 : 0, editor.curOctave, editor.editRowSkip, editor.curInstr, n.note, n.instr, n.vol, n.efx, n.efxData,
		keyb.leftCtrlPressed, keyb.leftAltPressed, keyb.leftShiftPressed, (int32_t)mod, editor.editTextFlag, ui.sysReqShown,
		ui.patternEditorShown, ui.diskOpShown, ui.instEditorShown, ui.sampleEditorShown, ui.configScreenShown,
		ui.helpScreenShown, ui.aboutScreenShown, ui.nibblesShown, ui.extendedPatternEditor, SCREEN_H, song.numChannels, name,
		ui.advEditShown, ui.transposeShown, ui.trimScreenShown, ui.instEditorExtShown, ui.sampleEditorExtShown,
		editor.currConfigScreen, editor.ptnJumpPos[0], editor.ptnJumpPos[1], editor.ptnJumpPos[2], editor.ptnJumpPos[3],
		pattMark.markX1, pattMark.markX2, pattMark.markY1, pattMark.markY2, config.masterVol, editor.instrBankOffset,
		editor.instrBankSwapped, song.songLength, song.pattNum, editor.channelMuted[0],
		ui.wavRendererShown, editor.wavIsRendering);

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
	}

	handleRedrawing();
	flipFrame(); // shows the progress and gives the browser some time
}

// ------------------------------------------------------------------------------------------------
// files

EM_JS(void, bt2_syncFiles, (void), { globalThis.BT2.syncFiles(); });

void webSyncFiles(void)
{
	bt2_syncFiles();
}

EM_ASYNC_JS(int32_t, bt2_prepareFile, (const char *path), {
	try { return (await globalThis.BT2.prepareFile(UTF8ToString(path))) ? 1 : 0; }
	catch (e) { console.error('prepareFile', e); return 0; }
});

bool webPrepareFile(const char *path)
{
	return bt2_prepareFile(path) != 0;
}

EM_ASYNC_JS(int32_t, bt2_modlandPrepare, (void), {
	try { return (await globalThis.BT2.modlandPrepare()) ? 1 : 0; }
	catch (e) { console.error('modland', e); return 0; }
});

EM_ASYNC_JS(int32_t, bt2_modlandSearch, (const char *query), {
	try { return await globalThis.BT2.modlandSearch(UTF8ToString(query)); }
	catch (e) { console.error('modland search', e); return -1; }
});

bool webModlandPrepare(void)
{
	return bt2_modlandPrepare() != 0;
}

int32_t webModlandSearch(const char *query)
{
	return bt2_modlandSearch(query);
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

EM_JS(void, bt2_frame, (int32_t rx, int32_t ry, int32_t rw, int32_t rh, int32_t diskOpShown, int32_t screenH), {
	globalThis.BT2.frame(rx, ry, rw, rh, diskOpShown, screenH);
});

void webFrame(void)
{
	if (audioMode == 0)
		updateAudioClockAnchor();

	webScopesTick();

	// tell the page where the FT2 screen is (for the Import/Export buttons in Disk Op.)
	bt2_frame(video.renderX, video.renderY, video.renderW, video.renderH, webDiskOpIsShown() ? 1 : 0, SCREEN_H);

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
