/* BerntTracker (Windows): sound output through an ASIO driver, e.g. ASIO4ALL.
**
** ASIO drivers talk to the sound card more directly than Windows' own sound paths. On some laptops
** (for example ThinkPad X1 Carbon, Intel Smart Sound) that is the only way to get a low latency:
** Windows' shared and exclusive modes stay slow there, while ASIO4ALL gives about 10 ms.
**
** Used when "Exclusive" is ticked in Config > Audio and an ASIO driver is installed (ASIO4ALL is
** preferred). If it can't be used (no driver, another program has it), the WASAPI exclusive mode and then
** FT2's normal output are tried, as before. FT2's mixer is the same: its callback is called from the
** driver's own thread, and the samples are converted to the driver's format.
**
** The ASIO interface below follows Steinberg's ASIO SDK (dual licensed, GPLv3 or proprietary, since
** October 2025): the same functions in the same order, so drivers can be called directly.
*/

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <wchar.h>
#include "ft2_asio.h"

// ------------------------------------------------------------------------------------------------
// ASIO interface (as in Steinberg's SDK: asio.h, iasiodrv.h)

typedef long ASIOBool;
typedef long ASIOError;
typedef long ASIOSampleType;
typedef double ASIOSampleRate;

struct ASIOSamples { unsigned long hi, lo; };
struct ASIOTimeStamp { unsigned long hi, lo; };
struct ASIOClockSource { long index, associatedChannel, associatedGroup; ASIOBool isCurrentSource; char name[32]; };
struct ASIOChannelInfo { long channel; ASIOBool isInput; ASIOBool isActive; long channelGroup; ASIOSampleType type; char name[32]; };
struct ASIOBufferInfo { ASIOBool isInput; long channelNum; void *buffers[2]; };
struct ASIOTime;

struct ASIOCallbacks
{
	void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
	void (*sampleRateDidChange)(ASIOSampleRate sRate);
	long (*asioMessage)(long selector, long value, void *message, double *opt);
	ASIOTime *(*bufferSwitchTimeInfo)(ASIOTime *params, long doubleBufferIndex, ASIOBool directProcess);
};

struct IASIO : public IUnknown
{
	virtual ASIOBool init(void *sysHandle) = 0;
	virtual void getDriverName(char *name) = 0;
	virtual long getDriverVersion() = 0;
	virtual void getErrorMessage(char *string) = 0;
	virtual ASIOError start() = 0;
	virtual ASIOError stop() = 0;
	virtual ASIOError getChannels(long *numInputChannels, long *numOutputChannels) = 0;
	virtual ASIOError getLatencies(long *inputLatency, long *outputLatency) = 0;
	virtual ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity) = 0;
	virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
	virtual ASIOError getSampleRate(ASIOSampleRate *sampleRate) = 0;
	virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
	virtual ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) = 0;
	virtual ASIOError setClockSource(long reference) = 0;
	virtual ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) = 0;
	virtual ASIOError getChannelInfo(ASIOChannelInfo *info) = 0;
	virtual ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks) = 0;
	virtual ASIOError disposeBuffers() = 0;
	virtual ASIOError controlPanel() = 0;
	virtual ASIOError future(long selector, void *opt) = 0;
	virtual ASIOError outputReady() = 0;
};

enum
{
	ASE_OK = 0,
	ASE_SUCCESS = 0x3f4847a0
};

enum // asioMessage selectors
{
	kAsioSelectorSupported = 1,
	kAsioEngineVersion = 2,
	kAsioResetRequest = 3,
	kAsioBufferSizeChange = 4,
	kAsioResyncRequest = 5,
	kAsioLatenciesChanged = 6,
	kAsioSupportsTimeInfo = 7,
	kAsioSupportsTimeCode = 8,
	kAsioOverload = 15
};

enum // sample types (little-endian ones: Windows)
{
	ASIOSTInt16LSB = 16,
	ASIOSTInt24LSB = 17,
	ASIOSTInt32LSB = 18,
	ASIOSTFloat32LSB = 19,
	ASIOSTFloat64LSB = 20,
	ASIOSTInt32LSB16 = 24,
	ASIOSTInt32LSB18 = 25,
	ASIOSTInt32LSB20 = 26,
	ASIOSTInt32LSB24 = 27
};

// ------------------------------------------------------------------------------------------------

static IASIO *driver;
static ASIOBufferInfo bufferInfo[2];
static ASIOCallbacks callbacks;
static ASIOSampleType sampleType;
static long blockFrames;
static bool driverInitialized, buffersCreated, started, postOutput, comInitialized, active, lockReady, mix16;
static CRITICAL_SECTION lock;
static volatile LONG pausedFlag, resetFlag;
static SDL_AudioCallback mixCallback;
static uint8_t *mixBuffer;
static int32_t rate;
static long outLatencyFrames; // (what the driver says its output adds, in samples: for the start screen)
static char driverName[64], statusText[32], infoText[160];

// the installed ASIO drivers are listed in the registry: HKLM\SOFTWARE\ASIO\<name> (value "CLSID")
static bool findDriver(CLSID *clsid, char *name, size_t nameSize)
{
	HKEY asioKey;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &asioKey) != ERROR_SUCCESS)
		return false;

	bool found = false;
	for (int32_t pass = 0; pass < 2 && !found; pass++) // ASIO4ALL first, then any other
	{
		for (DWORD i = 0; !found; i++)
		{
			wchar_t keyName[256];
			DWORD keyNameLen = 256;
			if (RegEnumKeyExW(asioKey, i, keyName, &keyNameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
				break;

			if (pass == 0 && wcsstr(keyName, L"ASIO4ALL") == NULL)
				continue;

			HKEY driverKey;
			if (RegOpenKeyExW(asioKey, keyName, 0, KEY_READ, &driverKey) != ERROR_SUCCESS)
				continue;

			wchar_t clsidText[64];
			memset(clsidText, 0, sizeof (clsidText));
			DWORD clsidSize = sizeof (clsidText) - sizeof (wchar_t), type = 0; // (room for the ending zero)
			if (RegQueryValueExW(driverKey, L"CLSID", NULL, &type, (LPBYTE)clsidText, &clsidSize) == ERROR_SUCCESS &&
				type == REG_SZ && SUCCEEDED(CLSIDFromString(clsidText, clsid)))
			{
				WideCharToMultiByte(CP_UTF8, 0, keyName, -1, name, (int)nameSize, NULL, NULL);
				found = true;
			}

			RegCloseKey(driverKey);
		}
	}

	RegCloseKey(asioKey);
	return found;
}

bool winAsioInstalled(void)
{
	CLSID clsid;
	char name[64];
	return findDriver(&clsid, name, sizeof (name));
}

// FT2's block (16-bit or float, stereo interleaved) -> the driver's two channel buffers
static void writeChannel(void *dst, int32_t c)
{
	const int16_t *s16 = (const int16_t *)mixBuffer;
	const float *sF = (const float *)mixBuffer;

	for (long i = 0; i < blockFrames; i++)
	{
		const int32_t in16 = mix16 ? s16[(i << 1) + c] : 0;
		const double in = mix16 ? (double)in16 * (1.0 / 32768.0) : (double)sF[(i << 1) + c];

		switch (sampleType)
		{
			case ASIOSTInt16LSB:
			{
				int32_t v = mix16 ? in16 : (int32_t)(in * 32767.0);
				((int16_t *)dst)[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
			}
			break;

			case ASIOSTInt24LSB:
			{
				int32_t v = mix16 ? in16 * 256 : (int32_t)(in * 8388607.0);
				if (v > 8388607) v = 8388607; else if (v < -8388608) v = -8388608;
				uint8_t *out = (uint8_t *)dst + i * 3;
				out[0] = (uint8_t)v;
				out[1] = (uint8_t)(v >> 8);
				out[2] = (uint8_t)(v >> 16);
			}
			break;

			case ASIOSTInt32LSB:
			{
				double v = mix16 ? (double)in16 * 65536.0 : in * 2147483647.0;
				if (v > 2147483647.0) v = 2147483647.0; else if (v < -2147483648.0) v = -2147483648.0;
				((int32_t *)dst)[i] = (int32_t)v;
			}
			break;

			case ASIOSTFloat32LSB: ((float *)dst)[i] = (float)in; break;
			case ASIOSTFloat64LSB: ((double *)dst)[i] = in; break;

			default: // 32-bit containers with 16/18/20/24 bits of sound in the low bits
			{
				const double scale = (sampleType == ASIOSTInt32LSB16) ? 32767.0 : (sampleType == ASIOSTInt32LSB18) ? 131071.0 :
					(sampleType == ASIOSTInt32LSB20) ? 524287.0 : 8388607.0;
				double v = in * scale;
				if (v > scale) v = scale; else if (v < -scale - 1.0) v = -scale - 1.0;
				((int32_t *)dst)[i] = (int32_t)v;
			}
			break;
		}
	}
}

// ------------------------------------------------------------------------------------------------
// called by the driver, on its own thread

static void bufferSwitch(long index, ASIOBool directProcess)
{
	(void)directProcess;
	if (index < 0 || index > 1 || mixBuffer == NULL)
		return;

	EnterCriticalSection(&lock);
	if (!pausedFlag && mixCallback != NULL)
		mixCallback(NULL, mixBuffer, (int)(blockFrames * (mix16 ? 4 : 8)));
	else
		memset(mixBuffer, 0, (size_t)blockFrames * 8);
	LeaveCriticalSection(&lock);

	writeChannel(bufferInfo[0].buffers[index], 0);
	writeChannel(bufferInfo[1].buffers[index], 1);

	if (postOutput)
		driver->outputReady(); // (the driver can send the block at once)
}

static ASIOTime *bufferSwitchTimeInfo(ASIOTime *params, long index, ASIOBool directProcess)
{
	bufferSwitch(index, directProcess);
	return params;
}

static void sampleRateDidChange(ASIOSampleRate sRate)
{
	(void)sRate;
	InterlockedExchange(&resetFlag, 1); // (the program opens the sound again)
}

static long asioMessage(long selector, long value, void *message, double *opt)
{
	(void)message; (void)opt;

	switch (selector)
	{
		case kAsioSelectorSupported:
			return (value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioResyncRequest ||
				value == kAsioLatenciesChanged || value == kAsioBufferSizeChange || value == kAsioOverload) ? 1 : 0;

		case kAsioEngineVersion: return 2;
		case kAsioResetRequest: InterlockedExchange(&resetFlag, 1); return 1; // (e.g. new settings in the driver's panel)
		case kAsioBufferSizeChange: InterlockedExchange(&resetFlag, 1); return 1;
		case kAsioResyncRequest: return 1;
		case kAsioLatenciesChanged: return 1;
		case kAsioOverload: return 1;
		default: return 0; // (no time info / time code: plain bufferSwitch is used)
	}
}

// ------------------------------------------------------------------------------------------------

// the buffer size nearest to the chosen one, that the driver allows
static long chooseBufferSize(long wanted, long minSize, long maxSize, long preferred, long granularity)
{
	if (minSize <= 0 || maxSize < minSize || granularity == 0)
		return preferred; // (the driver allows one size only)

	if (wanted < minSize) wanted = minSize;
	if (wanted > maxSize) wanted = maxSize;

	if (granularity == -1) // powers of two
	{
		long size = minSize;
		while (size < wanted && size * 2 <= maxSize)
			size *= 2;
		return size;
	}

	long size = minSize + ((wanted - minSize + granularity - 1) / granularity) * granularity;
	return (size > maxSize) ? maxSize : size;
}

static void fail(const char *status, const char *info)
{
	snprintf(statusText, sizeof (statusText), "%s", status);
	snprintf(infoText, sizeof (infoText), "ASIO: %s", info);
	winAsioClose();
}

SDL_AudioDeviceID winAsioOpen(SDL_AudioCallback callback, const SDL_AudioSpec *want, SDL_AudioSpec *have, int32_t bufferSamples)
{
	winAsioClose();

	if (!lockReady)
	{
		InitializeCriticalSection(&lock);
		lockReady = true;
	}

	CLSID clsid;
	if (!findDriver(&clsid, driverName, sizeof (driverName)))
	{
		snprintf(statusText, sizeof (statusText), "no ASIO");
		snprintf(infoText, sizeof (infoText), "ASIO: no ASIO driver installed (e.g. ASIO4ALL)");
		return 0;
	}

	const HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	comInitialized = SUCCEEDED(co);

	// (ASIO drivers use their class id as the interface id too)
	if (FAILED(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid, (void **)&driver)) || driver == NULL)
	{
		driver = NULL;
		fail("n/a", "the driver couldn't be loaded");
		return 0;
	}

	if (!driver->init((void *)GetDesktopWindow()))
	{
		char msg[128] = "";
		driver->getErrorMessage(msg);
		msg[sizeof (msg) - 1] = '\0';
		char info[160];
		snprintf(info, sizeof (info), "%s couldn't start (%s)", driverName, msg[0] ? msg : "busy?");
		fail("busy", info);
		return 0;
	}
	driverInitialized = true;

	long numIn = 0, numOut = 0;
	if (driver->getChannels(&numIn, &numOut) != ASE_OK || numOut < 2)
	{
		fail("n/a", "the driver has no stereo output");
		return 0;
	}

	// FT2's rate if the driver can do it, else the driver's own
	ASIOSampleRate sr = 0.0;
	if (driver->canSampleRate((ASIOSampleRate)want->freq) == ASE_OK && driver->setSampleRate((ASIOSampleRate)want->freq) == ASE_OK)
		sr = want->freq;
	else if (driver->getSampleRate(&sr) != ASE_OK || sr < 8000.0 || sr > 192000.0)
		sr = 0.0;

	if (sr == 0.0)
	{
		fail("n/a", "no usable sample rate");
		return 0;
	}
	rate = (int32_t)(sr + 0.5);

	long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
	if (driver->getBufferSize(&minSize, &maxSize, &preferred, &granularity) != ASE_OK || preferred <= 0)
	{
		fail("n/a", "the driver gives no buffer size");
		return 0;
	}

	// v0.32: the buffer chosen on the start screen, 32-512 samples (the nearest the driver allows)
	const long wanted = bufferSamples;
	blockFrames = chooseBufferSize(wanted, minSize, maxSize, preferred, granularity);
	if (blockFrames <= 0 || blockFrames > 16384)
	{
		fail("n/a", "unusable buffer size");
		return 0;
	}

	// the two first outputs, and their sample format (both the same)
	ASIOChannelInfo ch;
	memset(&ch, 0, sizeof (ch));
	ch.channel = 0;
	ch.isInput = 0;
	if (driver->getChannelInfo(&ch) != ASE_OK)
	{
		fail("n/a", "no channel info");
		return 0;
	}

	sampleType = ch.type;
	if (sampleType != ASIOSTInt16LSB && sampleType != ASIOSTInt24LSB && sampleType != ASIOSTInt32LSB &&
		sampleType != ASIOSTFloat32LSB && sampleType != ASIOSTFloat64LSB && sampleType != ASIOSTInt32LSB16 &&
		sampleType != ASIOSTInt32LSB18 && sampleType != ASIOSTInt32LSB20 && sampleType != ASIOSTInt32LSB24)
	{
		fail("n/a", "unknown sample format");
		return 0;
	}

	mixCallback = callback;
	mix16 = (want->format == AUDIO_S16);
	mixBuffer = (uint8_t *)calloc((size_t)blockFrames, 8); // (stereo, up to 32-bit float)
	if (mixBuffer == NULL)
	{
		fail("n/a", "out of memory");
		return 0;
	}

	pausedFlag = 1; // (starts paused, like SDL)
	resetFlag = 0;

	memset(bufferInfo, 0, sizeof (bufferInfo));
	for (int32_t i = 0; i < 2; i++)
	{
		bufferInfo[i].isInput = 0;
		bufferInfo[i].channelNum = i;
	}

	callbacks.bufferSwitch = bufferSwitch;
	callbacks.sampleRateDidChange = sampleRateDidChange;
	callbacks.asioMessage = asioMessage;
	callbacks.bufferSwitchTimeInfo = bufferSwitchTimeInfo;

	if (driver->createBuffers(bufferInfo, 2, blockFrames, &callbacks) != ASE_OK)
	{
		fail("n/a", "the driver couldn't make its buffers");
		return 0;
	}
	buffersCreated = true;

	postOutput = (driver->outputReady() == ASE_OK);

	long inLatency = 0, outLatency = 0;
	driver->getLatencies(&inLatency, &outLatency);
	outLatencyFrames = (outLatency > 0 && outLatency < 65536) ? outLatency : 0;

	active = true; // (before start: the driver may call bufferSwitch right away)
	if (driver->start() != ASE_OK)
	{
		fail("n/a", "the driver couldn't start");
		return 0;
	}
	started = true;

	const double blockMs = (blockFrames * 1000.0) / rate;
	snprintf(statusText, sizeof (statusText), "ASIO %.1fms", blockMs);
	snprintf(infoText, sizeof (infoText), "ASIO: %s, %dHz, %ld samples (%.2fms), output latency %.1fms",
		driverName, rate, blockFrames, blockMs, (outLatency * 1000.0) / rate);

	*have = *want;
	have->freq = rate;
	have->channels = 2;
	have->samples = (Uint16)blockFrames;
	have->size = (Uint32)blockFrames * (mix16 ? 4 : 8);

	return WIN_ASIO_DEVICE_ID;
}

void winAsioClose(void)
{
	if (driver != NULL)
	{
		if (started)
			driver->stop();
		if (buffersCreated)
			driver->disposeBuffers();

		driver->Release();
		driver = NULL;
	}

	started = buffersCreated = driverInitialized = postOutput = false;
	active = false;
	mixCallback = NULL;

	if (mixBuffer != NULL)
	{
		free(mixBuffer);
		mixBuffer = NULL;
	}

	if (comInitialized)
	{
		CoUninitialize();
		comInitialized = false;
	}
}

void winAsioLock(void)
{
	if (lockReady)
		EnterCriticalSection(&lock);
}

void winAsioUnlock(void)
{
	if (lockReady)
		LeaveCriticalSection(&lock);
}

void winAsioPause(bool pause)
{
	// (with the lock: when this returns, FT2's callback isn't running and won't run while paused, as with SDL)
	if (!lockReady)
		return;

	EnterCriticalSection(&lock);
	InterlockedExchange(&pausedFlag, pause ? 1 : 0);
	LeaveCriticalSection(&lock);
}

bool winAsioActive(void)
{
	return active;
}

bool winAsioResetRequested(void)
{
	return active && resetFlag != 0;
}

const char *winAsioStatus(void)
{
	return statusText;
}

const char *winAsioInfo(void)
{
	return infoText;
}

const char *winAsioDriverName(void)
{
	return driverName;
}

int32_t winAsioOutputLatencyFrames(void)
{
	return active ? (int32_t)outLatencyFrames : 0;
}

#endif
