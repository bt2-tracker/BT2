/* BerntTracker: the parts of downloading that are the same everywhere. See bt2_net.h. */

#include <stdio.h>
#include <string.h>
#include "bt2_net.h"

void bt2NetErrorText(const bt2NetResult *res, char *out, size_t outLen)
{
	if (outLen == 0)
		return;

	switch (res->error)
	{
		case BT2_NET_OK:
		case BT2_NET_CANCELLED:
			out[0] = '\0';
		break;

		case BT2_NET_OFFLINE:
			snprintf(out, outLen, "Ingen nett \xE2\x80\x93 pr\xC3\xB8v igjen n\xC3\xA5r du er p\xC3\xA5 nett");
		break;

		case BT2_NET_TIMEOUT:
			snprintf(out, outLen, "Nettet svarer ikke \xE2\x80\x93 pr\xC3\xB8v igjen");
		break;

		case BT2_NET_HTTP:
			snprintf(out, outLen, "Kunne ikke hente filen (HTTP %d)", (int)res->httpStatus);
		break;

		case BT2_NET_INCOMPLETE:
			snprintf(out, outLen, "Kunne ikke hente filen (ufullstendig (%llu av %llu byte))",
				(unsigned long long)res->got, (unsigned long long)res->total);
		break;

		case BT2_NET_TOO_BIG:
			snprintf(out, outLen, "Kunne ikke hente filen (for stor)");
		break;

		default:
			snprintf(out, outLen, "Kunne ikke hente filen (feil %u)", (unsigned)res->sysError);
		break;
	}
}

void bt2UrlEncodePath(const char *path, char *out, size_t outLen)
{
	static const char hex[] = "0123456789ABCDEF";
	if (outLen == 0)
		return;

	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++)
	{
		const unsigned char c = *p;
		const int keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
			strchr("-_.!~*'()", c) != NULL; // (what encodeURIComponent keeps, and the '/' between the parts)

		if (keep)
		{
			if (o + 1 >= outLen)
				break;
			out[o++] = (char)c;
		}
		else
		{
			if (o + 3 >= outLen)
				break;
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}

	out[o] = '\0';
}

#if !defined _WIN32 && !defined BT2_NET_TEST
// other systems have no download code here: the web version downloads with the page's own code (BT2.download in
// index.html), Linux and macOS can't download yet
bool bt2NetGet(const char *url, size_t maxLen, volatile int32_t *cancel, bt2NetProgressFunc progress, void *ud,
	uint8_t **data, size_t *len, bt2NetResult *res)
{
	memset(res, 0, sizeof (*res));
	res->error = BT2_NET_FAILED;
	*data = NULL;
	*len = 0;
	return false;

	(void)url;
	(void)maxLen;
	(void)cancel;
	(void)progress;
	(void)ud;
}
#endif
