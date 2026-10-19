/* BerntTracker (Windows): the program updates itself from the web. See bt2_winupdate.h.
**
** Only these files are ever written, deleted or renamed, all in the program's own folder:
**   <the program>.exe, <the program>.exe.new, <the program>.exe.old,
**   SDL2.dll, SDL2.dll.new, SDL2.dll.old
** Nothing else on the computer is touched.
*/

#if defined _WIN32 && !defined __EMSCRIPTEN__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <SDL2/SDL.h> // (SDL_GetVersion)
#include "bt2_winupdate.h"
#include "bt2_version.h"

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
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
#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(s) (((LONG)(s)) >= 0)
#endif

// where the Windows program is on the web (BT2_UPDATE_URL can point somewhere else, for testing)
#define DEFAULT_BASE_URL L"https://bt2-tracker.github.io/BT2/windows/"

#define MAX_FILES 4
#define MAX_FILE_SIZE (64 * 1024 * 1024)

typedef struct remoteFile_t
{
	char path[128]; // the name on the web
	uint64_t size;
	char sha256[65];
	wchar_t localPath[MAX_PATH]; // the file it replaces here
	bool differs;
} remoteFile;

static CRITICAL_SECTION lock;
static bool lockReady;
static HANDLE worker;
static volatile LONG cancelFlag, swapping;
static bt2UpdateInfo info;
static remoteFile files[MAX_FILES];
static int32_t numFiles;
static char remoteSdlVersion[32]; // the SDL2.dll the new program needs ("2.32.10")
static wchar_t exePath[MAX_PATH], exeDir[MAX_PATH], baseUrl[512];
static HINTERNET session;

static void setState(int32_t state, double progress)
{
	EnterCriticalSection(&lock);
	info.state = state;
	info.progress = progress;
	info.changeCount++;
	LeaveCriticalSection(&lock);
}

static bool findPaths(void)
{
	const DWORD n = GetModuleFileNameW(NULL, exePath, MAX_PATH);
	if (n == 0 || n >= MAX_PATH - 8)
		return false;

	wcscpy(exeDir, exePath);
	wchar_t *slash = wcsrchr(exeDir, L'\\');
	if (slash == NULL)
		return false;

	*slash = L'\0';
	return true;
}

static void pathPlus(wchar_t *out, const wchar_t *path, const wchar_t *ending)
{
	_snwprintf(out, MAX_PATH + 15, L"%ls%ls", path, ending);
	out[MAX_PATH + 15] = L'\0';
}

static void dllPath(wchar_t *out)
{
	_snwprintf(out, MAX_PATH - 1, L"%ls\\SDL2.dll", exeDir);
	out[MAX_PATH - 1] = L'\0';
}

void bt2UpdateEarlyStartup(void)
{
	// started by an update: wait until the old program has closed (it held the sound card and the files)
	wchar_t pidText[32];
	const DWORD n = GetEnvironmentVariableW(L"BT2_WAIT_PID", pidText, 32);
	if (n > 0 && n < 32)
	{
		SetEnvironmentVariableW(L"BT2_WAIT_PID", NULL);

		const DWORD pid = (DWORD)wcstoul(pidText, NULL, 10);
		if (pid != 0 && pid != GetCurrentProcessId())
		{
			HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
			if (h != NULL)
			{
				WaitForSingleObject(h, 15000);
				CloseHandle(h);
			}
		}
	}

	if (!findPaths())
		return;

	// leftovers from an update: the old program files, and unfinished downloads (only the program's own files)
	wchar_t p[MAX_PATH + 16], dll[MAX_PATH];
	dllPath(dll);

	pathPlus(p, exePath, L".old"); DeleteFileW(p);
	pathPlus(p, exePath, L".new"); DeleteFileW(p);
	pathPlus(p, dll, L".old"); DeleteFileW(p);
	pathPlus(p, dll, L".new"); DeleteFileW(p);
}

// ------------------------------------------------------------------------------------------------
// SHA-256 (Windows' own)

typedef struct sha256_t
{
	BCRYPT_ALG_HANDLE alg;
	BCRYPT_HASH_HANDLE hash;
	uint8_t *obj;
} sha256;

static bool shaInit(sha256 *c)
{
	memset(c, 0, sizeof (sha256));
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&c->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
	{
		c->alg = NULL;
		return false;
	}

	DWORD objLen = 0, got = 0;
	if (BCRYPT_SUCCESS(BCryptGetProperty(c->alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof (objLen), &got, 0)) && objLen > 0)
	{
		c->obj = (uint8_t *)malloc(objLen);
		if (c->obj != NULL && BCRYPT_SUCCESS(BCryptCreateHash(c->alg, &c->hash, c->obj, objLen, NULL, 0, 0)))
			return true;
	}

	free(c->obj);
	c->obj = NULL;
	c->hash = NULL;
	BCryptCloseAlgorithmProvider(c->alg, 0);
	c->alg = NULL;
	return false;
}

static void shaAdd(sha256 *c, const void *data, ULONG len)
{
	if (c->hash != NULL && len > 0)
		BCryptHashData(c->hash, (PUCHAR)data, len, 0);
}

static void shaEnd(sha256 *c, char hex[65])
{
	uint8_t digest[32];
	memset(digest, 0, sizeof (digest));
	hex[0] = '\0';

	if (c->hash != NULL && BCRYPT_SUCCESS(BCryptFinishHash(c->hash, digest, 32, 0)))
	{
		for (int32_t i = 0; i < 32; i++)
			sprintf(&hex[i * 2], "%02x", digest[i]);
		hex[64] = '\0';
	}

	if (c->hash != NULL) BCryptDestroyHash(c->hash);
	if (c->alg != NULL) BCryptCloseAlgorithmProvider(c->alg, 0);
	free(c->obj);
	memset(c, 0, sizeof (sha256));
}

static bool shaFile(const wchar_t *path, char hex[65], uint64_t *size)
{
	hex[0] = '\0';
	*size = 0;

	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return false;

	sha256 c;
	if (!shaInit(&c))
	{
		CloseHandle(f);
		return false;
	}

	static uint8_t buf[65536]; // (only the worker thread uses this)
	DWORD got = 0;
	bool ok = true;
	for (;;)
	{
		if (!ReadFile(f, buf, sizeof (buf), &got, NULL))
		{
			ok = false;
			break;
		}

		if (got == 0)
			break;

		shaAdd(&c, buf, got);
		*size += got;
	}

	CloseHandle(f);
	shaEnd(&c, hex);
	return ok && hex[0] != '\0';
}

// ------------------------------------------------------------------------------------------------
// HTTPS (Windows' own, WinHTTP: uses the computer's proxy settings and certificates)

typedef bool (*sinkFunc)(void *ctx, const uint8_t *data, DWORD len);

enum { HTTP_OK = 0, HTTP_OFFLINE = 1, HTTP_FAILED = 2 };

static bool isNetworkError(DWORD err)
{
	return err == 12007 /* NAME_NOT_RESOLVED */ || err == 12029 /* CANNOT_CONNECT */ || err == 12002 /* TIMEOUT */ ||
		err == 12030 /* CONNECTION_ERROR */ || err == 12031 /* CONNECTION_RESET */ || err == 12180 /* AUTODETECTION_FAILED */;
}

static int32_t httpGet(const wchar_t *url, sinkFunc sink, void *ctx)
{
	wchar_t host[256], path[2048], extra[1024];
	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof (uc));
	uc.dwStructSize = sizeof (uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2048;
	uc.lpszExtraInfo = extra;
	uc.dwExtraInfoLength = 1024;
	if (!WinHttpCrackUrl(url, 0, 0, &uc))
		return HTTP_FAILED;

	wchar_t object[3100];
	_snwprintf(object, 3099, L"%ls%ls", path, extra);
	object[3099] = L'\0';

	if (session == NULL)
	{
		session = WinHttpOpen(L"Bernttracker II", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (session == NULL) // (Windows before 8.1)
			session = WinHttpOpen(L"Bernttracker II", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (session == NULL)
			return HTTP_OFFLINE;

		WinHttpSetTimeouts(session, 10000, 10000, 20000, 30000);

		DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
		if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof (protocols)))
		{
			protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
			WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof (protocols));
		}
	}

	HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
	if (conn == NULL)
		return HTTP_OFFLINE;

	HINTERNET req = WinHttpOpenRequest(conn, L"GET", object, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		(uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
	if (req == NULL)
	{
		WinHttpCloseHandle(conn);
		return HTTP_FAILED;
	}

	WinHttpAddRequestHeaders(req, L"Cache-Control: no-cache\r\nPragma: no-cache", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

	int32_t result = HTTP_FAILED;
	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, NULL))
	{
		result = isNetworkError(GetLastError()) ? HTTP_OFFLINE : HTTP_FAILED;
	}
	else
	{
		DWORD status = 0, len = sizeof (status);
		WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

		if (status == 200)
		{
			static uint8_t buf[65536]; // (only the worker thread uses this)
			result = HTTP_OK;

			for (;;)
			{
				DWORD avail = 0, got = 0;
				if (cancelFlag || !WinHttpQueryDataAvailable(req, &avail))
				{
					result = HTTP_FAILED;
					break;
				}

				if (avail == 0)
					break;

				if (avail > sizeof (buf))
					avail = sizeof (buf);

				if (!WinHttpReadData(req, buf, avail, &got))
				{
					result = HTTP_FAILED;
					break;
				}

				if (got == 0)
					break;

				if (!sink(ctx, buf, got))
				{
					result = HTTP_FAILED;
					break;
				}
			}
		}
	}

	WinHttpCloseHandle(req);
	WinHttpCloseHandle(conn);
	return result;
}

// ------------------------------------------------------------------------------------------------
// version.json (a small JSON reader: just enough for this file)

typedef struct json_t
{
	const char *p, *end;
	int32_t depth;
} json;

static void jsonSpace(json *j)
{
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t'))
		j->p++;
}

static bool jsonString(json *j, char *out, size_t outSize)
{
	jsonSpace(j);
	if (j->p >= j->end || *j->p != '"')
		return false;
	j->p++;

	size_t n = 0;
	while (j->p < j->end && *j->p != '"')
	{
		char ch = *j->p++;
		if (ch == '\\')
		{
			if (j->p >= j->end)
				return false;

			const char e = *j->p++;
			if (e == 'u')
			{
				if (j->end - j->p < 4)
					return false;

				char hex[5];
				memcpy(hex, j->p, 4);
				hex[4] = '\0';
				j->p += 4;
				const unsigned v = (unsigned)strtoul(hex, NULL, 16);

				// (as UTF-8)
				char u[3];
				int32_t ul = 0;
				if (v < 0x80) { u[0] = (char)v; ul = 1; }
				else if (v < 0x800) { u[0] = (char)(0xC0 | (v >> 6)); u[1] = (char)(0x80 | (v & 0x3F)); ul = 2; }
				else { u[0] = (char)(0xE0 | (v >> 12)); u[1] = (char)(0x80 | ((v >> 6) & 0x3F)); u[2] = (char)(0x80 | (v & 0x3F)); ul = 3; }

				for (int32_t i = 0; i < ul; i++)
				{
					if (out != NULL && n + 1 < outSize)
						out[n++] = u[i];
				}
				continue;
			}

			ch = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r' : (e == 'b') ? '\b' : (e == 'f') ? '\f' : e;
		}

		if (out != NULL && n + 1 < outSize)
			out[n++] = ch;
	}

	if (j->p >= j->end)
		return false;

	j->p++; // (the closing quote)
	if (out != NULL)
		out[n] = '\0';

	return true;
}

static bool jsonNumber(json *j, double *v)
{
	jsonSpace(j);
	char tmp[40];
	size_t n = 0;
	while (j->p < j->end && n < sizeof (tmp) - 1 && strchr("+-0123456789.eE", *j->p) != NULL)
		tmp[n++] = *j->p++;
	tmp[n] = '\0';

	if (n == 0)
		return false;

	*v = strtod(tmp, NULL);
	return true;
}

static bool jsonSkipValue(json *j)
{
	jsonSpace(j);
	if (j->p >= j->end || ++j->depth > 32)
		return false;

	bool ok = true;
	const char c = *j->p;
	if (c == '"')
	{
		ok = jsonString(j, NULL, 0);
	}
	else if (c == '{' || c == '[')
	{
		const char close = (c == '{') ? '}' : ']';
		j->p++;
		jsonSpace(j);
		if (j->p < j->end && *j->p == close)
		{
			j->p++;
		}
		else
		{
			for (;;)
			{
				if (c == '{')
				{
					if (!jsonString(j, NULL, 0))
					{
						ok = false;
						break;
					}

					jsonSpace(j);
					if (j->p >= j->end || *j->p != ':')
					{
						ok = false;
						break;
					}
					j->p++;
				}

				if (!jsonSkipValue(j))
				{
					ok = false;
					break;
				}

				jsonSpace(j);
				if (j->p < j->end && *j->p == ',')
				{
					j->p++;
					continue;
				}

				if (j->p < j->end && *j->p == close)
				{
					j->p++;
					break;
				}

				ok = false;
				break;
			}
		}
	}
	else if (c == 't' || c == 'f' || c == 'n')
	{
		while (j->p < j->end && *j->p >= 'a' && *j->p <= 'z')
			j->p++;
	}
	else
	{
		double d;
		ok = jsonNumber(j, &d);
	}

	j->depth--;
	return ok;
}

static bool jsonExpect(json *j, char ch)
{
	jsonSpace(j);
	if (j->p >= j->end || *j->p != ch)
		return false;

	j->p++;
	return true;
}

static bool isHex64(const char *s)
{
	if (strlen(s) != 64)
		return false;

	for (int32_t i = 0; i < 64; i++)
	{
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	}

	return true;
}

static bool endsWithNoCase(const char *s, const char *ending)
{
	const size_t a = strlen(s), b = strlen(ending);
	return a >= b && _stricmp(s + a - b, ending) == 0;
}

static bool parseFileEntry(json *j, remoteFile *f)
{
	memset(f, 0, sizeof (remoteFile));
	if (!jsonExpect(j, '{'))
		return false;

	jsonSpace(j);
	if (j->p < j->end && *j->p == '}')
	{
		j->p++;
		return true;
	}

	for (;;)
	{
		char key[32];
		if (!jsonString(j, key, sizeof (key)) || !jsonExpect(j, ':'))
			return false;

		if (strcmp(key, "path") == 0)
		{
			if (!jsonString(j, f->path, sizeof (f->path)))
				return false;
		}
		else if (strcmp(key, "sha256") == 0)
		{
			if (!jsonString(j, f->sha256, sizeof (f->sha256)))
				return false;
		}
		else if (strcmp(key, "size") == 0)
		{
			double d;
			if (!jsonNumber(j, &d) || d < 1.0 || d > MAX_FILE_SIZE)
				return false;
			f->size = (uint64_t)d;
		}
		else if (!jsonSkipValue(j))
		{
			return false;
		}

		jsonSpace(j);
		if (j->p < j->end && *j->p == ',')
		{
			j->p++;
			continue;
		}

		return jsonExpect(j, '}');
	}
}

static bool parseVersionJson(const char *text, size_t len)
{
	json j = { text, text + len, 0 };
	numFiles = 0;
	info.remoteVersion[0] = info.remoteDate[0] = '\0';
	remoteSdlVersion[0] = '\0';

	if (!jsonExpect(&j, '{'))
		return false;

	bool haveExe = false;
	for (;;)
	{
		char key[32];
		if (!jsonString(&j, key, sizeof (key)) || !jsonExpect(&j, ':'))
			return false;

		if (strcmp(key, "version") == 0)
		{
			if (!jsonString(&j, info.remoteVersion, sizeof (info.remoteVersion)))
				return false;
		}
		else if (strcmp(key, "date") == 0)
		{
			if (!jsonString(&j, info.remoteDate, sizeof (info.remoteDate)))
				return false;
		}
		else if (strcmp(key, "sdl") == 0)
		{
			if (!jsonString(&j, remoteSdlVersion, sizeof (remoteSdlVersion)))
				return false;
		}
		else if (strcmp(key, "files") == 0)
		{
			if (!jsonExpect(&j, '['))
				return false;

			jsonSpace(&j);
			if (j.p < j.end && *j.p == ']')
			{
				j.p++;
			}
			else
			{
				for (;;)
				{
					remoteFile f;
					if (!parseFileEntry(&j, &f))
						return false;

					// only the program and SDL2.dll, with a plain name, are used
					const bool plain = strchr(f.path, '/') == NULL && strchr(f.path, '\\') == NULL && strstr(f.path, "..") == NULL;
					if (plain && isHex64(f.sha256) && f.size > 0 && numFiles < MAX_FILES)
					{
						if (endsWithNoCase(f.path, ".exe") && !haveExe)
						{
							wcscpy(f.localPath, exePath);
							files[numFiles++] = f;
							haveExe = true;
						}
						else if (_stricmp(f.path, "SDL2.dll") == 0)
						{
							dllPath(f.localPath);
							files[numFiles++] = f;
						}
					}

					jsonSpace(&j);
					if (j.p < j.end && *j.p == ',')
					{
						j.p++;
						continue;
					}

					if (!jsonExpect(&j, ']'))
						return false;
					break;
				}
			}
		}
		else if (!jsonSkipValue(&j))
		{
			return false;
		}

		jsonSpace(&j);
		if (j.p < j.end && *j.p == ',')
		{
			j.p++;
			continue;
		}

		if (!jsonExpect(&j, '}'))
			return false;
		break;
	}

	return haveExe && info.remoteVersion[0] != '\0';
}

// ------------------------------------------------------------------------------------------------
// looking for a new version

typedef struct memSink_t
{
	char *data;
	size_t len, cap;
} memSink;

static bool memSinkAdd(void *ctx, const uint8_t *data, DWORD len)
{
	memSink *m = (memSink *)ctx;
	if (m->len + len + 1 > m->cap)
		return false; // (version.json is small: anything bigger is wrong)

	memcpy(m->data + m->len, data, len);
	m->len += len;
	m->data[m->len] = '\0';
	return true;
}

static DWORD WINAPI checkThread(LPVOID arg)
{
	(void)arg;

	wchar_t url[700];
	_snwprintf(url, 699, L"%lsversion.json?t=%llu", baseUrl, (unsigned long long)GetTickCount64());
	url[699] = L'\0';

	memSink m;
	m.cap = 256 * 1024;
	m.len = 0;
	m.data = (char *)malloc(m.cap);
	if (m.data == NULL)
	{
		setState(BT2_UPD_CHECK_FAILED, 0.0);
		return 0;
	}

	const int32_t r = httpGet(url, memSinkAdd, &m);
	if (r != HTTP_OK)
	{
		free(m.data);
		setState((r == HTTP_OFFLINE) ? BT2_UPD_OFFLINE : BT2_UPD_CHECK_FAILED, 0.0);
		return 0;
	}

	EnterCriticalSection(&lock);
	const bool parsed = parseVersionJson(m.data, m.len);
	LeaveCriticalSection(&lock);
	free(m.data);

	if (!parsed)
	{
		setState(BT2_UPD_CHECK_FAILED, 0.0);
		return 0;
	}

	/* The same program as on the web? It compares its own file, so any change counts.
	** SDL2.dll is only replaced when the new program needs another SDL version: the program won't start
	** with the wrong one, so the one in use is right for this program (a different build of the same
	** SDL version, e.g. Visual Studio's, is left alone).
	*/
	SDL_version sdl;
	SDL_GetVersion(&sdl);
	char localSdl[32];
	snprintf(localSdl, sizeof (localSdl), "%d.%d.%d", sdl.major, sdl.minor, sdl.patch);

	bool anyDiffers = false;
	for (int32_t i = 0; i < numFiles; i++)
	{
		if (_stricmp(files[i].path, "SDL2.dll") == 0 && remoteSdlVersion[0] != '\0')
		{
			files[i].differs = (strcmp(remoteSdlVersion, localSdl) != 0) || GetFileAttributesW(files[i].localPath) == INVALID_FILE_ATTRIBUTES;
		}
		else
		{
			char hex[65];
			uint64_t size = 0;
			const bool haveLocal = shaFile(files[i].localPath, hex, &size);
			files[i].differs = !haveLocal || size != files[i].size || strcmp(hex, files[i].sha256) != 0;
		}

		if (files[i].differs)
			anyDiffers = true;
	}

	setState(anyDiffers ? BT2_UPD_AVAILABLE : BT2_UPD_LATEST, 0.0);
	return 0;
}

static void startWorker(LPTHREAD_START_ROUTINE func)
{
	if (worker != NULL)
	{
		// (the last job sets its result just before it ends: give it a moment to finish)
		if (WaitForSingleObject(worker, 2000) != WAIT_OBJECT_0)
			return; // (still working)

		CloseHandle(worker);
		worker = NULL;
	}

	cancelFlag = 0;
	worker = CreateThread(NULL, 0, func, NULL, 0, NULL);
}

void bt2UpdateStartCheck(void)
{
	if (!lockReady)
	{
		InitializeCriticalSection(&lock);
		lockReady = true;
	}

	if (exePath[0] == L'\0' && !findPaths())
	{
		setState(BT2_UPD_CHECK_FAILED, 0.0);
		return;
	}

	const DWORD n = GetEnvironmentVariableW(L"BT2_UPDATE_URL", baseUrl, 500);
	if (n == 0 || n >= 500)
		wcscpy(baseUrl, DEFAULT_BASE_URL);

	setState(BT2_UPD_CHECKING, 0.0);
	startWorker(checkThread);
}

void bt2UpdateGetInfo(bt2UpdateInfo *out)
{
	if (!lockReady)
	{
		memset(out, 0, sizeof (bt2UpdateInfo));
		out->state = BT2_UPD_CHECK_FAILED;
		return;
	}

	EnterCriticalSection(&lock);
	*out = info;
	LeaveCriticalSection(&lock);
}

// ------------------------------------------------------------------------------------------------
// the update

typedef struct fileSink_t
{
	HANDLE f;
	sha256 sha;
	uint64_t written, expected, doneBefore, total;
} fileSink;

static bool fileSinkAdd(void *ctx, const uint8_t *data, DWORD len)
{
	fileSink *s = (fileSink *)ctx;
	if (s->written + len > s->expected)
		return false;

	DWORD wrote = 0;
	if (!WriteFile(s->f, data, len, &wrote, NULL) || wrote != len)
		return false;

	shaAdd(&s->sha, data, len);
	s->written += len;

	const double p = (s->total > 0) ? (double)(s->doneBefore + s->written) / (double)s->total : 0.0;
	EnterCriticalSection(&lock);
	if (p - info.progress >= 0.01 || p >= 1.0)
	{
		info.progress = p;
		info.changeCount++;
	}
	LeaveCriticalSection(&lock);
	return true;
}

static void urlEncodeName(const char *name, wchar_t *out, size_t outLen)
{
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)name; *p != '\0' && n + 4 < outLen; p++)
	{
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' || *p == '~')
		{
			out[n++] = (wchar_t)*p;
		}
		else
		{
			static const char hexDigits[] = "0123456789ABCDEF";
			out[n++] = L'%';
			out[n++] = (wchar_t)hexDigits[*p >> 4];
			out[n++] = (wchar_t)hexDigits[*p & 15];
		}
	}

	out[n] = L'\0';
}

// (OneDrive or a virus checker can hold a file for a moment)
static bool moveFileRetry(const wchar_t *from, const wchar_t *to)
{
	for (int32_t i = 0; i < 15; i++)
	{
		if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;

		const DWORD err = GetLastError();
		if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
			return false;

		Sleep(200);
	}

	return false;
}

static DWORD WINAPI downloadThread(LPVOID arg)
{
	(void)arg;

	uint64_t total = 0;
	for (int32_t i = 0; i < numFiles; i++)
	{
		if (files[i].differs)
			total += files[i].size;
	}

	setState(BT2_UPD_DOWNLOADING, 0.0);

	// 1. download each new file next to the one it replaces ("<name>.new"), and check it
	uint64_t done = 0;
	for (int32_t i = 0; i < numFiles; i++)
	{
		remoteFile *rf = &files[i];
		if (!rf->differs)
			continue;

		wchar_t tmp[MAX_PATH + 16];
		pathPlus(tmp, rf->localPath, L".new");

		fileSink s;
		memset(&s, 0, sizeof (s));
		s.f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (s.f == INVALID_HANDLE_VALUE)
		{
			const DWORD err = GetLastError();
			setState((err == ERROR_ACCESS_DENIED) ? BT2_UPD_NO_ACCESS : BT2_UPD_FAILED, 0.0);
			return 0;
		}

		if (!shaInit(&s.sha))
		{
			CloseHandle(s.f);
			DeleteFileW(tmp);
			setState(BT2_UPD_FAILED, 0.0);
			return 0;
		}

		s.expected = rf->size;
		s.doneBefore = done;
		s.total = total;

		wchar_t name[400], url[1000];
		urlEncodeName(rf->path, name, 400);
		_snwprintf(url, 999, L"%ls%ls?v=%.16hs", baseUrl, name, rf->sha256);
		url[999] = L'\0';

		const int32_t r = httpGet(url, fileSinkAdd, &s);
		FlushFileBuffers(s.f);
		CloseHandle(s.f);

		char hex[65];
		shaEnd(&s.sha, hex);

		if (r != HTTP_OK || s.written != rf->size || strcmp(hex, rf->sha256) != 0)
		{
			DeleteFileW(tmp);
			setState(BT2_UPD_FAILED, 0.0);
			return 0;
		}

		done += rf->size;
	}

	// 2. swap: the old file becomes "<name>.old" (removed at the next start), the new one takes its place
	InterlockedExchange(&swapping, 1);

	int32_t swapped[MAX_FILES];
	int32_t numSwapped = 0;
	bool ok = true;

	for (int32_t i = 0; i < numFiles && ok; i++)
	{
		remoteFile *rf = &files[i];
		if (!rf->differs)
			continue;

		wchar_t tmp[MAX_PATH + 16], old[MAX_PATH + 16];
		pathPlus(tmp, rf->localPath, L".new");
		pathPlus(old, rf->localPath, L".old");
		DeleteFileW(old);

		const bool hadOld = (GetFileAttributesW(rf->localPath) != INVALID_FILE_ATTRIBUTES);
		if (hadOld && !moveFileRetry(rf->localPath, old))
		{
			ok = false;
			break;
		}

		if (!moveFileRetry(tmp, rf->localPath))
		{
			if (hadOld)
				moveFileRetry(old, rf->localPath); // (put it back)
			ok = false;
			break;
		}

		swapped[numSwapped++] = i;
	}

	if (!ok) // put back what was already swapped, so the program stays as it was
	{
		for (int32_t k = numSwapped - 1; k >= 0; k--)
		{
			remoteFile *rf = &files[swapped[k]];
			wchar_t old[MAX_PATH + 16], tmp[MAX_PATH + 16];
			pathPlus(old, rf->localPath, L".old");
			pathPlus(tmp, rf->localPath, L".new");
			if (GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES)
			{
				moveFileRetry(rf->localPath, tmp);
				moveFileRetry(old, rf->localPath);
			}
		}

		for (int32_t i = 0; i < numFiles; i++)
		{
			wchar_t tmp[MAX_PATH + 16];
			pathPlus(tmp, files[i].localPath, L".new");
			DeleteFileW(tmp);
		}

		InterlockedExchange(&swapping, 0);
		setState(BT2_UPD_NO_ACCESS, 0.0);
		return 0;
	}

	InterlockedExchange(&swapping, 0);
	setState(BT2_UPD_RESTARTING, 1.0);
	return 0;
}

void bt2UpdateStartDownload(void)
{
	if (!lockReady)
		return;

	bt2UpdateInfo now;
	bt2UpdateGetInfo(&now);
	if (now.state != BT2_UPD_AVAILABLE && now.state != BT2_UPD_FAILED)
		return;

	startWorker(downloadThread);
}

bool bt2UpdateLaunchNew(void)
{
	if (exePath[0] == L'\0')
		return false;

	// (the new program waits until this one has closed, see bt2UpdateEarlyStartup())
	wchar_t pid[32];
	_snwprintf(pid, 31, L"%lu", (unsigned long)GetCurrentProcessId());
	pid[31] = L'\0';
	SetEnvironmentVariableW(L"BT2_WAIT_PID", pid);

	wchar_t cmd[MAX_PATH + 8];
	_snwprintf(cmd, MAX_PATH + 7, L"\"%ls\"", exePath);
	cmd[MAX_PATH + 7] = L'\0';

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof (si));
	memset(&pi, 0, sizeof (pi));
	si.cb = sizeof (si);

	const BOOL ok = CreateProcessW(exePath, cmd, NULL, NULL, FALSE, 0, NULL, exeDir, &si, &pi);
	SetEnvironmentVariableW(L"BT2_WAIT_PID", NULL);

	if (ok)
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}

	return ok ? true : false;
}

void bt2UpdateShutdown(void)
{
	if (worker != NULL)
	{
		// a download is stopped (its unfinished file is removed at the next start), a swap is let finish
		InterlockedExchange(&cancelFlag, 1);
		const DWORD waitMs = swapping ? INFINITE : 3000;
		WaitForSingleObject(worker, waitMs);
		while (swapping)
			Sleep(20);

		CloseHandle(worker);
		worker = NULL;
	}

	if (session != NULL)
	{
		WinHttpCloseHandle(session);
		session = NULL;
	}
}

#endif
