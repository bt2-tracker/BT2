/* BerntTracker (Windows): sound output in WASAPI exclusive mode.
**
** FT2's normal Windows output (SDL with DirectSound) goes through Windows' own sound mixer, which adds its
** buffers and the sound card's effects: often 40-60 ms before a note is heard. In exclusive mode the program
** talks to the sound card directly, in the card's smallest block size (often ~3 ms): typically 5-10 ms in
** all. While it is on, other programs can't play sound on the same sound card.
**
** Used when "Exclusive" is ticked in Config > Audio (standard) and the sound card allows it. Otherwise (the
** card is busy, Windows doesn't allow it, no sample format fits) FT2's normal output is used, as before.
** FT2's mixer is the same: its callback is called from here instead of from SDL, in FT2's own 16-bit or
** 32-bit float format, and the samples are then converted to the sound card's format.
**
** Everything that talks to the sound card (opening, filling blocks, closing) happens on one thread of its
** own, with Windows' "Pro Audio" priority. The program only starts it, waits until it says whether the
** sound card could be opened, and stops it again.
*/

#ifdef _WIN32

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ft2_wasapi.h"

// (own copies, so nothing extra has to be linked; same values as in Windows' headers)
static const CLSID BT2_CLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID BT2_IID_IMMDeviceEnumerator = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID BT2_IID_IAudioClient = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID BT2_IID_IAudioRenderClient = { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };
static const GUID BT2_SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID BT2_SUBTYPE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const PROPERTYKEY BT2_PKEY_Device_FriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

#define BT2_STREAMFLAGS_EVENTCALLBACK 0x00040000
#define BT2_E_DEVICE_INVALIDATED ((HRESULT)0x88890004L)
#define BT2_E_DEVICE_IN_USE ((HRESULT)0x8889000AL)
#define BT2_E_EXCLUSIVE_NOT_ALLOWED ((HRESULT)0x8889000EL)
#define BT2_E_NOT_ALIGNED ((HRESULT)0x88890019L)
#define BT2_SPEAKER_FRONT_LEFT 0x1
#define BT2_SPEAKER_FRONT_RIGHT 0x2

#define MIN_PERIOD_100NS 13333 // ~1.3 ms (64 samples at 48 kHz): the smallest block we ask for
#define MAX_LOSSES 3 // the sound card stopped this many times in a row: use the normal output for this session

typedef struct
{
	int32_t bits, validBits;
	bool isFloat;
} sampleType_t;

// sample formats to try, best first (FT2's 16-bit setting: 16-bit first, so the sound is exactly FT2's)
static const sampleType_t typesFor16[] = { { 16, 16, false }, { 32, 24, false }, { 32, 32, false }, { 24, 24, false }, { 32, 32, true } };
static const sampleType_t typesFor32[] = { { 32, 32, true }, { 32, 32, false }, { 32, 24, false }, { 24, 24, false }, { 16, 16, false } };
#define NUM_TYPES 5

// what the program asks for (set before the sound thread starts)
static char wantDevice[512];
static int32_t wantRate, wantBufferSetting;
static SDL_AudioCallback mixCallback;
static bool mix16;

// the sound thread's own (only it touches these, until it has stopped)
static IMMDevice *dev;
static IAudioClient *client;
static IAudioRenderClient *render;
static HANDLE blockEvent;
static uint8_t *mixBuffer;

// shared
static HANDLE thread, readyEvent;
static CRITICAL_SECTION lock;
static bool lockReady, active;
static volatile LONG quitThread, pausedFlag, lostFlag, openOk;
static int32_t lossCount;
static uint32_t blockFrames;
static int32_t devChannels, devBits, devValidBits, devRate;
static bool devFloat;
static char statusText[32], infoText[160];

// ------------------------------------------------------------------------------------------------
// opening the sound card (on the sound thread)

static bool activateClient(void)
{
	if (client != NULL)
	{
		IAudioClient_Release(client);
		client = NULL;
	}

	return SUCCEEDED(IMMDevice_Activate(dev, &BT2_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client)) && client != NULL;
}

// the device FT2 uses (by its name in Config > Audio), else Windows' default sound output
static IMMDevice *findDevice(IMMDeviceEnumerator *enumerator, const char *deviceName)
{
	IMMDevice *found = NULL;

	if (deviceName != NULL && deviceName[0] != '\0')
	{
		IMMDeviceCollection *list = NULL;
		if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender, DEVICE_STATE_ACTIVE, &list)) && list != NULL)
		{
			UINT count = 0;
			IMMDeviceCollection_GetCount(list, &count);

			for (UINT i = 0; i < count && found == NULL; i++)
			{
				IMMDevice *d = NULL;
				if (FAILED(IMMDeviceCollection_Item(list, i, &d)) || d == NULL)
					continue;

				IPropertyStore *props = NULL;
				if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &props)) && props != NULL)
				{
					PROPVARIANT name;
					PropVariantInit(&name);
					if (SUCCEEDED(IPropertyStore_GetValue(props, &BT2_PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
					{
						char utf8[512];
						if (WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, utf8, sizeof (utf8), NULL, NULL) > 0 &&
							strcmp(utf8, deviceName) == 0)
						{
							found = d;
							d = NULL;
						}
					}
					PropVariantClear(&name);
					IPropertyStore_Release(props);
				}

				if (d != NULL)
					IMMDevice_Release(d);
			}

			IMMDeviceCollection_Release(list);
		}
	}

	if (found == NULL)
		IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &found);

	return found;
}

static void makeFormat(WAVEFORMATEXTENSIBLE *f, int32_t rate, int32_t channels, DWORD channelMask, const sampleType_t *t)
{
	memset(f, 0, sizeof (WAVEFORMATEXTENSIBLE));
	f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	f->Format.nChannels = (WORD)channels;
	f->Format.nSamplesPerSec = (DWORD)rate;
	f->Format.wBitsPerSample = (WORD)t->bits;
	f->Format.nBlockAlign = (WORD)(channels * (t->bits / 8));
	f->Format.nAvgBytesPerSec = (DWORD)rate * f->Format.nBlockAlign;
	f->Format.cbSize = (WORD)(sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX));
	f->Samples.wValidBitsPerSample = (WORD)t->validBits;
	f->dwChannelMask = channelMask;
	f->SubFormat = t->isFloat ? BT2_SUBTYPE_FLOAT : BT2_SUBTYPE_PCM;
}

// Initialize in exclusive mode with one block of 'period'. Some sound cards want the block a little
// different: they say how big, and then we open again with that (as Microsoft's documentation shows).
static HRESULT initExclusive(WAVEFORMATEXTENSIBLE *f, REFERENCE_TIME period)
{
	HRESULT hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE, BT2_STREAMFLAGS_EVENTCALLBACK,
		period, period, (WAVEFORMATEX *)f, NULL);

	if (hr == BT2_E_NOT_ALIGNED)
	{
		UINT32 frames = 0;
		if (FAILED(IAudioClient_GetBufferSize(client, &frames)) || frames == 0)
			return hr;

		period = (REFERENCE_TIME)((10000000.0 * frames / f->Format.nSamplesPerSec) + 0.5);
		if (!activateClient())
			return E_FAIL;

		hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE, BT2_STREAMFLAGS_EVENTCALLBACK,
			period, period, (WAVEFORMATEX *)f, NULL);
	}

	return hr;
}

static void setFailure(HRESULT hr)
{
	if (hr == BT2_E_DEVICE_IN_USE)
	{
		strcpy(statusText, "busy");
		strcpy(infoText, "Exclusive mode: the sound card is busy (another program), using normal output");
	}
	else if (hr == BT2_E_EXCLUSIVE_NOT_ALLOWED)
	{
		strcpy(statusText, "blocked");
		strcpy(infoText, "Exclusive mode: not allowed in Windows' sound settings, using normal output");
	}
	else
	{
		strcpy(statusText, "n/a");
		strcpy(infoText, "Exclusive mode: the sound card can't do it, using normal output");
	}
}

static bool openSoundCard(void)
{
	IMMDeviceEnumerator *enumerator = NULL;
	if (FAILED(CoCreateInstance(&BT2_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &BT2_IID_IMMDeviceEnumerator,
		(void **)&enumerator)) || enumerator == NULL)
	{
		setFailure(E_FAIL);
		return false;
	}

	dev = findDevice(enumerator, wantDevice);
	IMMDeviceEnumerator_Release(enumerator);
	if (dev == NULL || !activateClient())
	{
		setFailure(E_FAIL);
		return false;
	}

	// the sound card's own format (for its rate, and its channels if it has no plain stereo)
	int32_t mixRate = 48000, mixChannels = 2;
	DWORD mixMask = 0;
	WAVEFORMATEX *mixFormat = NULL;
	if (SUCCEEDED(IAudioClient_GetMixFormat(client, &mixFormat)) && mixFormat != NULL)
	{
		mixRate = (int32_t)mixFormat->nSamplesPerSec;
		mixChannels = mixFormat->nChannels;
		if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			mixMask = ((WAVEFORMATEXTENSIBLE *)mixFormat)->dwChannelMask;
		CoTaskMemFree(mixFormat);
	}

	// block size: FT2's "Smaller latency" = the sound card's smallest, "Medium" ~10 ms, "Bigger" ~20 ms
	REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
	IAudioClient_GetDevicePeriod(client, &defaultPeriod, &minPeriod);
	if (minPeriod <= 0)
		minPeriod = 30000;
	if (minPeriod < MIN_PERIOD_100NS)
		minPeriod = MIN_PERIOD_100NS;

	REFERENCE_TIME period = minPeriod;
	if (wantBufferSetting >= 2048 && period < 200000)
		period = 200000;
	else if (wantBufferSetting >= 1024 && wantBufferSetting < 2048 && period < 100000)
		period = 100000;

	// FT2's rate first, then the sound card's own, then the usual ones; stereo first
	const int32_t rates[4] = { wantRate, mixRate, 48000, 44100 };
	const int32_t channelsToTry[2] = { 2, (mixChannels > 2) ? mixChannels : 0 };
	const sampleType_t *types = mix16 ? typesFor16 : typesFor32;

	HRESULT lastError = E_FAIL;
	bool opened = false, stop = false;
	WAVEFORMATEXTENSIBLE fmt;

	for (int32_t c = 0; c < 2 && !opened && !stop; c++)
	{
		const int32_t channels = channelsToTry[c];
		if (channels == 0)
			continue;

		const DWORD mask = (channels == 2) ? (BT2_SPEAKER_FRONT_LEFT | BT2_SPEAKER_FRONT_RIGHT) : mixMask;

		for (int32_t r = 0; r < 4 && !opened && !stop; r++)
		{
			const int32_t rate = rates[r];
			if (rate < 8000 || rate > 192000)
				continue;

			bool triedBefore = false;
			for (int32_t k = 0; k < r; k++)
			{
				if (rates[k] == rate)
					triedBefore = true;
			}
			if (triedBefore)
				continue;

			for (int32_t t = 0; t < NUM_TYPES && !opened && !stop; t++)
			{
				makeFormat(&fmt, rate, channels, mask, &types[t]);
				if (IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX *)&fmt, NULL) != S_OK)
					continue;

				lastError = initExclusive(&fmt, period);
				if (SUCCEEDED(lastError))
					opened = true;
				else if (lastError == BT2_E_DEVICE_IN_USE || lastError == BT2_E_EXCLUSIVE_NOT_ALLOWED || !activateClient())
					stop = true; // (busy or not allowed: no point in trying other formats)
			}
		}
	}

	if (!opened)
	{
		setFailure(lastError);
		return false;
	}

	devChannels = fmt.Format.nChannels;
	devBits = fmt.Format.wBitsPerSample;
	devValidBits = fmt.Samples.wValidBitsPerSample;
	devFloat = (memcmp(&fmt.SubFormat, &BT2_SUBTYPE_FLOAT, sizeof (GUID)) == 0);
	devRate = (int32_t)fmt.Format.nSamplesPerSec;

	blockFrames = 0;
	blockEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (FAILED(IAudioClient_GetBufferSize(client, &blockFrames)) || blockFrames == 0 || blockFrames > 65535 ||
		blockEvent == NULL || FAILED(IAudioClient_SetEventHandle(client, blockEvent)) ||
		FAILED(IAudioClient_GetService(client, &BT2_IID_IAudioRenderClient, (void **)&render)) || render == NULL)
	{
		setFailure(E_FAIL);
		return false;
	}

	mixBuffer = (uint8_t *)calloc(blockFrames, 8); // (stereo, up to 32-bit float)
	if (mixBuffer == NULL)
	{
		setFailure(E_FAIL);
		return false;
	}

	const double blockMs = (blockFrames * 1000.0) / devRate;
	snprintf(statusText, sizeof (statusText), "%.1fms", blockMs);
	snprintf(infoText, sizeof (infoText), "Exclusive mode: on, %dHz, %d-bit%s, %u samples (%.2fms)",
		devRate, devValidBits, devFloat ? " float" : "", (unsigned)blockFrames, blockMs);

	return true;
}

static void closeSoundCard(void)
{
	if (client != NULL)
		IAudioClient_Stop(client);

	if (render != NULL) { IAudioRenderClient_Release(render); render = NULL; }
	if (client != NULL) { IAudioClient_Release(client); client = NULL; }
	if (dev != NULL) { IMMDevice_Release(dev); dev = NULL; }
	if (mixBuffer != NULL) { free(mixBuffer); mixBuffer = NULL; }
	// (blockEvent is closed by winExclusiveClose(), after this thread has ended: it may still signal it)
}

// ------------------------------------------------------------------------------------------------
// playing (on the sound thread)

static inline int32_t clampToInt32(double v)
{
	if (v >= 2147483647.0) return INT32_MAX;
	if (v <= -2147483648.0) return INT32_MIN;
	return (int32_t)v;
}

// FT2's block (16-bit or float, stereo) -> the sound card's format (extra channels get silence)
static void convertBlock(const uint8_t *src, BYTE *dst, uint32_t frames)
{
	const int16_t *s16 = (const int16_t *)src;
	const float *sF = (const float *)src;
	const int32_t bytes = devBits / 8;

	memset(dst, 0, (size_t)frames * devChannels * bytes);

	for (uint32_t i = 0; i < frames; i++)
	{
		for (int32_t c = 0; c < 2; c++)
		{
			BYTE *out = dst + ((size_t)i * devChannels + c) * bytes;
			const int32_t in16 = mix16 ? s16[(i << 1) + c] : 0;
			const float inF = mix16 ? (float)in16 * (1.0f / 32768.0f) : sF[(i << 1) + c];

			if (devFloat)
			{
				*(float *)out = inF;
			}
			else if (devBits == 16)
			{
				int32_t v = mix16 ? in16 : (int32_t)(inF * 32767.0f);
				if (v > INT16_MAX) v = INT16_MAX; else if (v < INT16_MIN) v = INT16_MIN;
				*(int16_t *)out = (int16_t)v;
			}
			else if (devBits == 32) // (24 or 32 valid bits: both are left-justified in 32 bits)
			{
				*(int32_t *)out = mix16 ? (int32_t)((uint32_t)in16 << 16) : clampToInt32(inF * 2147483647.0);
			}
			else // 24-bit packed
			{
				int32_t v = mix16 ? in16 * 256 : (int32_t)(inF * 8388607.0f);
				if (v > 8388607) v = 8388607; else if (v < -8388608) v = -8388608;
				out[0] = (BYTE)v;
				out[1] = (BYTE)(v >> 8);
				out[2] = (BYTE)(v >> 16);
			}
		}
	}
}

static bool fillBlock(void)
{
	BYTE *data = NULL;
	HRESULT hr = IAudioRenderClient_GetBuffer(render, blockFrames, &data);
	if (FAILED(hr) || data == NULL)
		return hr != BT2_E_DEVICE_INVALIDATED; // (the sound card went away: stop, the program opens the sound again)

	EnterCriticalSection(&lock);
	if (!pausedFlag && mixCallback != NULL)
		mixCallback(NULL, mixBuffer, (int)(blockFrames * (mix16 ? 4 : 8)));
	else
		memset(mixBuffer, 0, (size_t)blockFrames * 8);
	LeaveCriticalSection(&lock);

	convertBlock(mixBuffer, data, blockFrames);

	hr = IAudioRenderClient_ReleaseBuffer(render, blockFrames, 0);
	return hr != BT2_E_DEVICE_INVALIDATED;
}

typedef HANDLE (WINAPI *AvSetMmThreadCharacteristicsW_t)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI *AvRevertMmThreadCharacteristics_t)(HANDLE);

static DWORD WINAPI soundThread(LPVOID unused)
{
	(void)unused;
	const HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);

	// Windows' "Pro Audio" priority for this thread (as music programs do), else simply the highest
	HANDLE mmcss = NULL;
	AvRevertMmThreadCharacteristics_t avRevert = NULL;
	HMODULE avrt = LoadLibraryW(L"avrt.dll");
	if (avrt != NULL)
	{
		AvSetMmThreadCharacteristicsW_t avSet = (AvSetMmThreadCharacteristicsW_t)(void *)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
		avRevert = (AvRevertMmThreadCharacteristics_t)(void *)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
		DWORD taskIndex = 0;
		if (avSet != NULL)
			mmcss = avSet(L"Pro Audio", &taskIndex);
	}
	if (mmcss == NULL)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	// open, and tell the program how it went
	bool ok = SUCCEEDED(co) && openSoundCard();
	InterlockedExchange(&openOk, ok ? 1 : 0);
	SetEvent(readyEvent);

	// the first block goes in before starting (else the start of the sound would be a glitch)
	if (ok)
		ok = fillBlock() && SUCCEEDED(IAudioClient_Start(client));

	int32_t timeouts = 0;
	uint32_t blocksPlayed = 0;
	while (ok && !quitThread)
	{
		const DWORD w = WaitForSingleObject(blockEvent, 500);
		if (quitThread)
			break;

		if (w != WAIT_OBJECT_0)
		{
			if (++timeouts >= 4) // no request from the sound card for 2 seconds: it has stopped
				ok = false;
			continue;
		}

		timeouts = 0;
		ok = fillBlock();

		if (++blocksPlayed == (uint32_t)(devRate * 10) / blockFrames) // played fine for 10 seconds
			lossCount = 0;
	}

	if (openOk && !quitThread)
		InterlockedExchange(&lostFlag, 1); // (the sound card stopped: the main loop sees it and opens the sound again)

	closeSoundCard();

	if (mmcss != NULL && avRevert != NULL)
		avRevert(mmcss);
	if (avrt != NULL)
		FreeLibrary(avrt);
	if (SUCCEEDED(co))
		CoUninitialize();

	return 0;
}

// ------------------------------------------------------------------------------------------------
// called by the program (ft2_audio.c)

SDL_AudioDeviceID winExclusiveOpen(const char *deviceName, SDL_AudioCallback callback, const SDL_AudioSpec *want,
	SDL_AudioSpec *have, int32_t bufferSetting)
{
	winExclusiveClose();

	if (!lockReady)
	{
		InitializeCriticalSection(&lock);
		lockReady = true;
	}

	if (lossCount >= MAX_LOSSES) // (it keeps stopping on this computer: the normal output for the rest of the session)
	{
		strcpy(statusText, "error");
		strcpy(infoText, "Exclusive mode: the sound card stopped several times, using normal output");
		return 0;
	}

	snprintf(wantDevice, sizeof (wantDevice), "%s", (deviceName != NULL) ? deviceName : "");
	wantRate = want->freq;
	wantBufferSetting = bufferSetting;
	mixCallback = callback;
	mix16 = (want->format == AUDIO_S16);

	quitThread = 0;
	lostFlag = 0;
	openOk = 0;
	pausedFlag = 1; // (starts paused, like SDL)

	readyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (readyEvent == NULL)
		return 0;

	thread = CreateThread(NULL, 0, soundThread, NULL, 0, NULL);
	if (thread == NULL)
	{
		CloseHandle(readyEvent);
		readyEvent = NULL;
		return 0;
	}

	// wait until the sound thread says whether the sound card could be opened (if not, it has said why)
	const DWORD w = WaitForSingleObject(readyEvent, 10000);
	if (w != WAIT_OBJECT_0 || !openOk)
	{
		if (w != WAIT_OBJECT_0)
			setFailure(E_FAIL);

		winExclusiveClose();
		return 0;
	}

	*have = *want;
	have->freq = devRate;
	have->channels = 2;
	have->samples = (Uint16)blockFrames;
	have->size = blockFrames * (mix16 ? 4 : 8);

	active = true;
	return WIN_EXCLUSIVE_DEVICE_ID;
}

void winExclusiveClose(void)
{
	bool threadEnded = true;
	if (thread != NULL)
	{
		InterlockedExchange(&quitThread, 1);
		if (blockEvent != NULL)
			SetEvent(blockEvent); // (it also wakes up by itself within half a second)

		threadEnded = (WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
		CloseHandle(thread);
		thread = NULL;
	}

	// (if the thread somehow didn't end, leave its handles alone: it still owns them)
	if (threadEnded)
	{
		if (blockEvent != NULL)
		{
			CloseHandle(blockEvent);
			blockEvent = NULL;
		}

		if (readyEvent != NULL)
		{
			CloseHandle(readyEvent);
			readyEvent = NULL;
		}
	}
	else
	{
		blockEvent = NULL;
		readyEvent = NULL;
	}

	if (active && lostFlag)
		lossCount++;

	active = false;
	mixCallback = NULL;
}

void winExclusiveLock(void)
{
	if (lockReady)
		EnterCriticalSection(&lock);
}

void winExclusiveUnlock(void)
{
	if (lockReady)
		LeaveCriticalSection(&lock);
}

void winExclusivePause(bool pause)
{
	// (with the lock: when this returns, FT2's callback isn't running and won't run while paused, as with SDL)
	if (!lockReady)
		return;

	EnterCriticalSection(&lock);
	InterlockedExchange(&pausedFlag, pause ? 1 : 0);
	LeaveCriticalSection(&lock);
}

bool winExclusiveActive(void)
{
	return active;
}

bool winExclusiveDeviceLost(void)
{
	return active && lostFlag != 0;
}

const char *winExclusiveStatus(void)
{
	return statusText;
}

const char *winExclusiveInfo(void)
{
	return infoText;
}

#endif
