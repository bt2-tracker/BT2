/* BerntTracker (Windows): downloads with WinHTTP (the computer's proxy settings and certificates).
** See bt2_net.h.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include "bt2_net.h"
#include "bt2_fs.h"

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

#define IDLE_MS 20000 // like the web version: no data for 20 seconds = give up

static INIT_ONCE sessionOnce = INIT_ONCE_STATIC_INIT;
static HINTERNET session;

static BOOL CALLBACK openSession(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	session = WinHttpOpen(L"Bernttracker II", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (session == NULL) // (Windows before 8.1)
		session = WinHttpOpen(L"Bernttracker II", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

	if (session != NULL)
	{
		WinHttpSetTimeouts(session, IDLE_MS, IDLE_MS, IDLE_MS, IDLE_MS);

		DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
		if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof (protocols)))
		{
			protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
			WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof (protocols));
		}
	}

	return TRUE;
	(void)once; (void)param; (void)ctx;
}

static void setSysError(bt2NetResult *res, DWORD err)
{
	res->sysError = err;
	if (err == 12007 /* NAME_NOT_RESOLVED */ || err == 12029 /* CANNOT_CONNECT */ || err == 12180 /* AUTODETECTION_FAILED */)
		res->error = BT2_NET_OFFLINE;
	else if (err == 12002 /* TIMEOUT */)
		res->error = BT2_NET_TIMEOUT;
	else
		res->error = BT2_NET_FAILED;
}

bool bt2NetGet(const char *url, size_t maxLen, volatile int32_t *cancel, bt2NetProgressFunc progress, void *ud,
	uint8_t **data, size_t *len, bt2NetResult *res)
{
	*data = NULL;
	*len = 0;
	memset(res, 0, sizeof (bt2NetResult));
	res->error = BT2_NET_FAILED;

	InitOnceExecuteOnce(&sessionOnce, openSession, NULL, NULL);
	if (session == NULL)
	{
		res->error = BT2_NET_OFFLINE;
		return false;
	}

	// (bt2Wide would turn '/' into '\': a URL needs its own conversion)
	const int32_t wn = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
	if (wn <= 0 || wn > 8000)
		return false;

	wchar_t *wurl = (wchar_t *)malloc((size_t)wn * sizeof (wchar_t));
	if (wurl == NULL)
		return false;
	MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, wn);

	wchar_t host[256], path[8192];
	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof (uc));
	uc.dwStructSize = sizeof (uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 8192;
	const BOOL cracked = WinHttpCrackUrl(wurl, 0, 0, &uc);
	free(wurl);
	if (!cracked)
		return false;

	HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
	if (conn == NULL)
	{
		setSysError(res, GetLastError());
		return false;
	}

	HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		(uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
	if (req == NULL)
	{
		setSysError(res, GetLastError());
		WinHttpCloseHandle(conn);
		return false;
	}

	WinHttpAddRequestHeaders(req, L"Cache-Control: no-cache\r\nPragma: no-cache", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

	uint8_t *buf = NULL;
	size_t cap = 0, got = 0;
	bool ok = false;
	DWORD status = 0, slen = sizeof (status), total = 0, tlen = sizeof (total), avail = 0, r = 0;

	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, NULL))
	{
		setSysError(res, GetLastError());
		goto done;
	}

	WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
	if (status != 200)
	{
		res->error = BT2_NET_HTTP;
		res->httpStatus = (int32_t)status;
		goto done;
	}

	if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &tlen, WINHTTP_NO_HEADER_INDEX))
		total = 0;

	res->total = total;
	if (total > maxLen)
	{
		res->error = BT2_NET_TOO_BIG;
		goto done;
	}

	cap = (total > 0) ? (size_t)total + 1 : 65536;
	buf = (uint8_t *)malloc(cap);
	if (buf == NULL)
	{
		res->error = BT2_NET_TOO_BIG;
		goto done;
	}

	if (progress != NULL)
		progress(ud, 0, total);

	for (;;)
	{
		if (cancel != NULL && *cancel)
		{
			res->error = BT2_NET_CANCELLED;
			goto done;
		}

		avail = 0;
		if (!WinHttpQueryDataAvailable(req, &avail))
		{
			setSysError(res, GetLastError());
			goto done;
		}

		if (avail == 0)
			break; // (all of it)

		if (got + avail + 1 > cap)
		{
			size_t newCap = cap * 2;
			while (newCap < got + avail + 1)
				newCap *= 2;

			if (got + avail > maxLen)
			{
				res->error = BT2_NET_TOO_BIG;
				goto done;
			}

			uint8_t *bigger = (uint8_t *)realloc(buf, newCap);
			if (bigger == NULL)
			{
				res->error = BT2_NET_TOO_BIG;
				goto done;
			}

			buf = bigger;
			cap = newCap;
		}

		r = 0;
		if (!WinHttpReadData(req, buf + got, avail, &r))
		{
			setSysError(res, GetLastError());
			goto done;
		}

		if (r == 0)
			break;

		got += r;
		res->got = got;
		if (progress != NULL)
			progress(ud, got, total);
	}

	if (total > 0 && got < total)
	{
		res->error = BT2_NET_INCOMPLETE;
		goto done;
	}

	buf[got] = 0;
	*data = buf;
	*len = got;
	buf = NULL;
	res->error = BT2_NET_OK;
	ok = true;

done:
	free(buf);
	WinHttpCloseHandle(req);
	WinHttpCloseHandle(conn);
	return ok;
}

#endif
