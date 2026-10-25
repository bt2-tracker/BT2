/* BerntTracker (web, v0.35): MIDI keyboards in the web version, through the browser's Web MIDI.
**
** FT2's MIDI code (ft2_midi.c) talks to RtMidi; on the web these RtMidi functions are made here instead, on top
** of the page's BT2.midi (web/index.html), so Config > MIDI input and MIDI recording work as in the Windows program.
** Web MIDI is in Chrome, Edge and Firefox (PC/Mac/Android). Safari (also every browser on iPad, they all use Safari
** underneath) doesn't have it yet: then the MIDI page says so (see drawMidiInputList() in ft2_midi.c).
**
** The browser asks the user before a page may use MIDI. So the page only asks when it has been allowed before, or
** when the MIDI input page in Config is opened (see webMidiRequest()).
*/

#if defined __EMSCRIPTEN__ && defined HAS_MIDI

#include <emscripten.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "rtmidi/rtmidi_c.h"
#include "ft2_header.h"
#include "ft2_structs.h"
#include "ft2_midi.h"

static struct RtMidiWrapper wrapper;
static RtMidiCCallback callback;
static void *callbackUser;
static bool created;
static int32_t openPort = -1;

EM_JS(int, bt2_midiCount, (void), { return (globalThis.BT2 && BT2.midi) ? BT2.midi.count() : 0; });
EM_JS(int, bt2_midiName, (int i, char *out, int outLen), {
	const name = (globalThis.BT2 && BT2.midi) ? BT2.midi.name(i) : '';
	if (out && outLen > 0) stringToUTF8(name, out, outLen);
	return lengthBytesUTF8(name) + 1;
});
EM_JS(void, bt2_midiOpen, (int i), { if (globalThis.BT2 && BT2.midi) BT2.midi.open(i); });
EM_JS(void, bt2_midiInit, (void), { if (globalThis.BT2 && BT2.midi) BT2.midi.init(); });
EM_JS(void, bt2_midiRequest, (void), { if (globalThis.BT2 && BT2.midi) BT2.midi.request(); });
EM_JS(int, bt2_midiState, (void), { return (globalThis.BT2 && BT2.midi) ? BT2.midi.state() : 0; });

RtMidiInPtr rtmidi_in_create_default(void)
{
	memset(&wrapper, 0, sizeof (wrapper));
	wrapper.ok = true;
	created = true;
	bt2_midiInit(); // (the page gets the list of MIDI inputs, if it is allowed already)
	return &wrapper;
}

void rtmidi_in_free(RtMidiInPtr device)
{
	(void)device;
	created = false;
	callback = NULL;
	openPort = -1;
	bt2_midiOpen(-1);
}

unsigned int rtmidi_get_port_count(RtMidiPtr device)
{
	(void)device;
	return (unsigned int)bt2_midiCount();
}

int rtmidi_get_port_name(RtMidiPtr device, unsigned int portNumber, char *bufOut, int *bufLen)
{
	const int need = bt2_midiName((int)portNumber, NULL, 0);
	if (device != NULL)
		device->ok = true;

	if (bufOut == NULL)
	{
		if (bufLen != NULL)
			*bufLen = need;
		return need;
	}

	const int len = (bufLen != NULL && *bufLen > 0) ? *bufLen : need;
	bt2_midiName((int)portNumber, bufOut, len + 1); // (ft2_midi.c allocates *bufLen + 1 bytes)
	return need;
}

void rtmidi_open_port(RtMidiPtr device, unsigned int portNumber, const char *portName)
{
	(void)portName;
	if ((int)portNumber >= bt2_midiCount())
	{
		device->ok = false;
		return;
	}

	openPort = (int32_t)portNumber;
	bt2_midiOpen(openPort);
	device->ok = true;
}

void rtmidi_close_port(RtMidiPtr device)
{
	(void)device;
	openPort = -1;
	bt2_midiOpen(-1);
}

void rtmidi_in_set_callback(RtMidiInPtr device, RtMidiCCallback cb, void *userData)
{
	callback = cb;
	callbackUser = userData;
	device->ok = true;
}

void rtmidi_in_cancel_callback(RtMidiInPtr device)
{
	(void)device;
	callback = NULL;
}

void rtmidi_in_ignore_types(RtMidiInPtr device, bool midiSysex, bool midiTime, bool midiSense)
{
	(void)device; (void)midiSysex; (void)midiTime; (void)midiSense; // (the page only passes notes, controllers, pitch bend)
}

// from the page: a message from the open MIDI input (on the main thread, between frames)
EMSCRIPTEN_KEEPALIVE void webMidiMessage(int32_t b0, int32_t b1, int32_t b2, int32_t len)
{
	if (callback == NULL || len < 2 || len > 3)
		return;

	const unsigned char msg[3] = { (unsigned char)b0, (unsigned char)b1, (unsigned char)b2 };
	callback(0.0, msg, (size_t)len, callbackUser);
}

// from the page: the list of MIDI inputs changed (allowed now, or a keyboard was plugged in or out)
EMSCRIPTEN_KEEPALIVE void webMidiPortsChanged(void)
{
	if (!created)
		return;

	webMidiReopen(); // (ft2_midi.c: the device from the config, or the first one)
	midi.rescanDevicesFlag = true; // (Config > MIDI input shows the new list)
}

void webMidiRequest(void)
{
	bt2_midiRequest();
}

int32_t webMidiState(void) // 0 = no Web MIDI in this browser, 1 = not allowed (yet), 2 = ready
{
	return bt2_midiState();
}

#else
typedef int prevent_compiler_warning; // (empty file when not the web version)
#endif
