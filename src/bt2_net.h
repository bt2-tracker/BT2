#pragma once

/* BerntTracker: downloading a file from the web (Modland). The Windows program uses Windows' own
** HTTPS (WinHTTP: the computer's proxy settings and certificates), see bt2_winnet.c.
**
** bt2NetGet() waits until the file is there, so it is called from a worker thread. Like the web
** version, a download gives up when no data has come for 20 seconds.
*/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum
{
	BT2_NET_OK = 0,
	BT2_NET_OFFLINE,    // no network (the name couldn't be looked up, no connection)
	BT2_NET_TIMEOUT,    // the network stopped answering
	BT2_NET_HTTP,       // the server answered with an error (httpStatus)
	BT2_NET_INCOMPLETE, // fewer bytes than the server said
	BT2_NET_TOO_BIG,
	BT2_NET_CANCELLED,
	BT2_NET_FAILED      // anything else (sysError)
};

typedef struct bt2NetResult_t
{
	int32_t error;      // BT2_NET_...
	int32_t httpStatus;
	uint32_t sysError;
	uint64_t got, total;
} bt2NetResult;

typedef void (*bt2NetProgressFunc)(void *ud, uint64_t got, uint64_t total); // total = 0: size not known

// url -> *data (malloc'ed, *len bytes, with an extra 0 byte at the end). *cancel != 0 stops it.
bool bt2NetGet(const char *url, size_t maxLen, volatile int32_t *cancel, bt2NetProgressFunc progress, void *ud,
	uint8_t **data, size_t *len, bt2NetResult *res);

// the same messages as the web version (UTF-8)
void bt2NetErrorText(const bt2NetResult *res, char *out, size_t outLen);

// "Fasttracker 2/A b/c?.xm" -> each part like JavaScript's encodeURIComponent, '/' kept
void bt2UrlEncodePath(const char *path, char *out, size_t outLen);
