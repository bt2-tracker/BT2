/* BerntTracker: Modland as folders in Disk Op. See bt2_modland.h.
**
** Only these are ever written: the saved list (setup.listFile), the Modland folders and their empty
** song files (setup.mirrorRoot, only made when missing), the downloaded songs there, and new files in
** the user's BT2/Modland folder (never over a file that has anything in it).
*/

#ifndef __EMSCRIPTEN__ // (the web version still does this in index.html)

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL2/SDL.h>
#include "bt2_modland.h"
#include "bt2_unzip.h"
#include "bt2_fs.h"
#include "bt2_search.h"

#define MAX_LIST_ZIP (256u * 1024 * 1024)
#define MAX_LIST_TEXT (512u * 1024 * 1024)
#define MAX_NAME 200 // bytes in one name (like the web version's cleanName)

typedef struct mlFile_t
{
	const char *path; // in the list text, not 0-terminated
	uint32_t len;
	int64_t size;
} mlFile;

typedef struct mlNode_t
{
	const char *name; // the name on Modland (in the list text, not 0-terminated)
	uint32_t nameLen;
	int32_t parent, first, last, next;
	int32_t local; // the name here (in localNames), or -1 when it is the same as on Modland
	bool dir, populated;
} mlNode;

static bt2ModlandSetup setup;
static SDL_mutex *lock;
static char *listText;
static mlFile *files;
static int32_t fileCount;
static mlNode *nodes;
static int32_t nodeCount, nodeCap;
static char *localNames;
static size_t localNamesLen, localNamesCap;
static volatile bool loaded;
static volatile int32_t stopping;
static SDL_atomic_t refreshRunning;

void bt2ModlandInit(const bt2ModlandSetup *s)
{
	setup = *s;
	if (setup.listUrl[0] == '\0') snprintf(setup.listUrl, sizeof (setup.listUrl), "%s", BT2_MODLAND_LIST_URL);
	if (setup.fileUrl[0] == '\0') snprintf(setup.fileUrl, sizeof (setup.fileUrl), "%s", BT2_MODLAND_FILE_URL);

	if (lock == NULL)
		lock = SDL_CreateMutex();
}

const bt2ModlandSetup *bt2ModlandGetSetup(void)
{
	return &setup;
}

bool bt2ModlandIsLoaded(void)
{
	return loaded;
}

// ------------------------------------------------------------------------------------------------
// the list -> folders

static int compareFiles(const void *a, const void *b)
{
	const mlFile *x = (const mlFile *)a, *y = (const mlFile *)b;
	const uint32_t n = (x->len < y->len) ? x->len : y->len;
	const int c = memcmp(x->path, y->path, n);
	if (c != 0)
		return c;

	return (x->len < y->len) ? -1 : (x->len > y->len) ? 1 : 0;
}

static int32_t addNode(int32_t parent, const char *name, uint32_t nameLen, bool dir)
{
	if (nodeCount >= nodeCap)
	{
		const int32_t newCap = (nodeCap > 0) ? nodeCap * 2 : 4096;
		mlNode *bigger = (mlNode *)realloc(nodes, (size_t)newCap * sizeof (mlNode));
		if (bigger == NULL)
			return -1;

		nodes = bigger;
		nodeCap = newCap;
	}

	const int32_t i = nodeCount++;
	mlNode *n = &nodes[i];
	n->name = name;
	n->nameLen = nameLen;
	n->parent = parent;
	n->first = n->last = n->next = -1;
	n->local = -1;
	n->dir = dir;
	n->populated = false;

	if (parent >= 0)
	{
		mlNode *p = &nodes[parent];
		if (p->last >= 0)
			nodes[p->last].next = i;
		else
			p->first = i;
		p->last = i;
	}

	return i;
}

static void makeLocalNames(void);

static bool badPart(const char *s, uint32_t len)
{
	return len == 0 || (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.');
}

bool bt2ModlandUseList(char *text, size_t len)
{
	if (text == NULL)
		return false;

	if (loaded)
	{
		free(text);
		return true;
	}

	// the lines: "size<TAB>Artist/song.xm"
	int32_t lines = 0;
	for (size_t i = 0; i < len; i++)
	{
		if (text[i] == '\n')
			lines++;
	}

	mlFile *f = (mlFile *)malloc(((size_t)lines + 1) * sizeof (mlFile));
	if (f == NULL)
	{
		free(text);
		return false;
	}

	int32_t n = 0;
	for (size_t pos = 0; pos < len;)
	{
		const char *line = text + pos;
		const char *nl = (const char *)memchr(line, '\n', len - pos);
		size_t lineLen = (nl != NULL) ? (size_t)(nl - line) : len - pos;
		pos += lineLen + 1;

		if (lineLen > 0 && line[lineLen - 1] == '\r')
			lineLen--;

		const char *tab = (const char *)memchr(line, '\t', lineLen);
		if (tab == NULL || tab == line)
			continue;

		const size_t pathLen = lineLen - (size_t)(tab + 1 - line);
		if (pathLen == 0 || pathLen > 4000)
			continue;

		f[n].path = tab + 1;
		f[n].len = (uint32_t)pathLen;
		f[n].size = strtoll(line, NULL, 10);
		n++;
	}

	if (n == 0)
	{
		free(f);
		free(text);
		return false;
	}

	qsort(f, (size_t)n, sizeof (mlFile), compareFiles);

	// the folders: with the paths sorted, everything in one folder comes together
	nodeCount = 0;
	addNode(-1, "", 0, true); // (the top: Modland's "Fasttracker 2" folder)

	int32_t stack[64];
	const char *stackName[64];
	uint32_t stackLen[64];
	int32_t depth = 0;
	int32_t kept = 0;

	for (int32_t i = 0; i < n; i++)
	{
		const char *parts[64];
		uint32_t partLen[64];
		int32_t numParts = 0;
		bool ok = true;

		const char *p = f[i].path, *end = p + f[i].len;
		while (p <= end && ok)
		{
			const char *slash = (const char *)memchr(p, '/', (size_t)(end - p));
			const char *partEnd = (slash != NULL) ? slash : end;
			const uint32_t l = (uint32_t)(partEnd - p);

			if (l > 0) // (an empty part, from "//", is skipped)
			{
				if (badPart(p, l) || l > MAX_NAME || numParts >= 63)
					ok = false;
				else
				{
					parts[numParts] = p;
					partLen[numParts] = l;
					numParts++;
				}
			}

			if (slash == NULL)
				break;
			p = slash + 1;
		}

		if (!ok || numParts == 0)
			continue;

		// the folders: keep the ones this path shares with the one before, make the rest
		int32_t k = 0;
		for (; k < numParts - 1; k++)
		{
			if (k < depth && stackLen[k] == partLen[k] && memcmp(stackName[k], parts[k], partLen[k]) == 0)
				continue;
			break;
		}

		depth = k;
		for (; k < numParts - 1; k++)
		{
			const int32_t dir = addNode((k == 0) ? 0 : stack[k - 1], parts[k], partLen[k], true);
			if (dir < 0)
				break;

			stack[k] = dir;
			stackName[k] = parts[k];
			stackLen[k] = partLen[k];
			depth = k + 1;
		}

		if (depth != numParts - 1)
			continue; // (out of memory)

		if (addNode((depth == 0) ? 0 : stack[depth - 1], parts[numParts - 1], partLen[numParts - 1], false) < 0)
			break;

		f[kept++] = f[i];
	}

	if (kept == 0)
	{
		free(f);
		free(text);
		return false;
	}

	makeLocalNames();

	SDL_LockMutex(lock);
	listText = text;
	files = f;
	fileCount = kept;
	loaded = true;
	SDL_UnlockMutex(lock);

	return true;
}

bool bt2ModlandLoadSaved(bool *old)
{
	*old = false;
	if (loaded)
		return true;

	bt2FileInfo info;
	if (!bt2FsInfo(setup.listFile, &info) || info.isDir)
		return false;

	size_t len = 0;
	uint8_t *text = bt2FsReadAll(setup.listFile, &len, MAX_LIST_TEXT);
	if (text == NULL || len == 0)
	{
		free(text);
		return false;
	}

	const int64_t now = (int64_t)time(NULL);
	*old = (now - info.mtime) > (int64_t)BT2_MODLAND_REFRESH_DAYS * 86400;

	return bt2ModlandUseList((char *)text, len);
}

char *bt2ModlandFetchList(volatile int32_t *cancel, bt2NetProgressFunc progress, void *ud, bt2NetResult *res, bool *unreadable, size_t *outLen)
{
	*unreadable = false;
	*outLen = 0;

	uint8_t *zip = NULL;
	size_t zipLen = 0;
	if (!bt2NetGet(setup.listUrl, MAX_LIST_ZIP, cancel, progress, ud, &zip, &zipLen, res))
		return NULL;

	int32_t count = 0;
	bt2ZipEntry *entries = bt2ZipList(zip, zipLen, &count);
	if (entries == NULL)
	{
		free(zip);
		*unreadable = true;
		return NULL;
	}

	// allmods.txt (or else the first file)
	int32_t pick = -1;
	for (int32_t i = 0; i < count && pick < 0; i++)
	{
		const size_t l = strlen(entries[i].name);
		if (!entries[i].dir && l >= 11 && SDL_strcasecmp(entries[i].name + l - 11, "allmods.txt") == 0)
			pick = i;
	}
	if (pick < 0)
		pick = 0;

	size_t allLen = 0;
	uint8_t *all = (entries[pick].size <= MAX_LIST_TEXT) ? bt2ZipExtract(zip, zipLen, &entries[pick], &allLen) : NULL;
	free(entries);
	free(zip);

	if (all == NULL)
	{
		*unreadable = true;
		return NULL;
	}

	// keep the Fasttracker 2 songs: "size<TAB>Fasttracker 2/Artist/song.xm" -> "size<TAB>Artist/song.xm"
	char *out = (char *)malloc(allLen + 1);
	if (out == NULL)
	{
		free(all);
		*unreadable = true;
		return NULL;
	}

	static const char key[] = "\tFasttracker 2/";
	const size_t keyLen = sizeof (key) - 1;
	size_t o = 0;

	for (size_t pos = 0; pos < allLen;)
	{
		const char *line = (const char *)all + pos;
		const char *nl = (const char *)memchr(line, '\n', allLen - pos);
		size_t lineLen = (nl != NULL) ? (size_t)(nl - line) : allLen - pos;
		pos += lineLen + 1;

		if (lineLen > 0 && line[lineLen - 1] == '\r')
			lineLen--;

		// (the first "\tFasttracker 2/" in the line, after the size)
		const char *hit = NULL;
		for (size_t t = 0; t + keyLen <= lineLen; t++)
		{
			if (line[t] == '\t' && memcmp(line + t, key, keyLen) == 0)
			{
				hit = line + t;
				break;
			}
		}

		if (hit == NULL || hit == line)
			continue;

		if (o > 0)
			out[o++] = '\n';

		const size_t sizeLen = (size_t)(hit - line);
		memcpy(out + o, line, sizeLen);
		o += sizeLen;
		out[o++] = '\t';

		const size_t restLen = lineLen - sizeLen - keyLen;
		memcpy(out + o, hit + keyLen, restLen);
		o += restLen;
	}

	free(all);
	out[o] = '\0';

	if (o == 0)
	{
		free(out);
		*unreadable = true;
		return NULL;
	}

	char dir[1024];
	snprintf(dir, sizeof (dir), "%s", setup.listFile);
	char *sep = strrchr(dir, BT2_SEP);
	if (sep != NULL)
	{
		*sep = '\0';
		bt2FsMkdirs(dir);
	}
	bt2FsWriteReplace(setup.listFile, out, o); // (only the program's own list file)

	*outLen = o;
	return out;
}

static int SDLCALL refreshThread(void *arg)
{
	bt2NetResult res;
	bool unreadable;
	size_t len;
	char *text = bt2ModlandFetchList(&stopping, NULL, NULL, &res, &unreadable, &len);
	free(text); // (saved: used the next time)

	SDL_AtomicSet(&refreshRunning, 0);
	return 0;
	(void)arg;
}

void bt2ModlandStartRefresh(void)
{
	if (!SDL_AtomicCAS(&refreshRunning, 0, 1))
		return;

	SDL_Thread *t = SDL_CreateThread(refreshThread, "Modland list", NULL);
	if (t == NULL)
		SDL_AtomicSet(&refreshRunning, 0);
	else
		SDL_DetachThread(t);
}

// ------------------------------------------------------------------------------------------------
// the folders

static void nodeName(int32_t i, char *out, size_t outLen) // the name on Modland
{
	const uint32_t l = (nodes[i].nameLen < outLen - 1) ? nodes[i].nameLen : (uint32_t)(outLen - 1);
	memcpy(out, nodes[i].name, l);
	out[l] = '\0';
}

static void nodeSafeName(int32_t i, char *out, size_t outLen) // the name here
{
	if (nodes[i].local >= 0)
		snprintf(out, outLen, "%s", localNames + nodes[i].local);
	else
		nodeName(i, out, outLen);
}

// names compared the way the system does it: on Windows any case is the same name ("Song.xm" = "SONG.XM",
// also with letters like æ, ø, å and ö)
static void foldName(const char *s, char *out, size_t outLen)
{
#ifdef _WIN32
	bt2Utf8Lower(s, out, outLen);
#else
	snprintf(out, outLen, "%s", s);
#endif
}

#define KEY_LEN 1024

// a set of names (is this name taken in the folder?)
typedef struct nameSet_t
{
	char **slot; // (the names as they are compared: foldName)
	uint32_t cap, count;
} nameSet;

static uint32_t nameHash(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s != '\0'; s++)
		h = (h ^ (uint8_t)*s) * 16777619u;
	return h;
}

static bool nameSetHas(const nameSet *set, const char *name)
{
	if (set->cap == 0)
		return false;

	char key[KEY_LEN];
	foldName(name, key, sizeof (key));
	for (uint32_t h = nameHash(key) & (set->cap - 1); set->slot[h] != NULL; h = (h + 1) & (set->cap - 1))
	{
		if (strcmp(set->slot[h], key) == 0)
			return true;
	}

	return false;
}

static void nameSetAdd(nameSet *set, const char *name)
{
	if ((set->count + 1) * 2 > set->cap)
	{
		// (bigger, and the names put in again)
		const uint32_t cap = (set->cap > 0) ? set->cap * 2 : 64;
		char **slot = (char **)calloc(cap, sizeof (char *));
		if (slot == NULL)
			return;

		for (uint32_t i = 0; i < set->cap; i++)
		{
			if (set->slot[i] == NULL)
				continue;

			uint32_t h = nameHash(set->slot[i]) & (cap - 1);
			while (slot[h] != NULL)
				h = (h + 1) & (cap - 1);
			slot[h] = set->slot[i];
		}

		free(set->slot);
		set->slot = slot;
		set->cap = cap;
	}

	char key[KEY_LEN];
	foldName(name, key, sizeof (key));

	char *copy = (char *)malloc(strlen(key) + 1);
	if (copy == NULL)
		return;
	strcpy(copy, key);

	uint32_t h = nameHash(key) & (set->cap - 1);
	while (set->slot[h] != NULL)
		h = (h + 1) & (set->cap - 1);
	set->slot[h] = copy;
	set->count++;
}

static void nameSetFree(nameSet *set)
{
	for (uint32_t i = 0; i < set->cap; i++)
		free(set->slot[i]);
	free(set->slot);
	set->slot = NULL;
	set->cap = set->count = 0;
}

// the names already in a folder on the disk (so only the missing ones are made: a folder with thousands of artists is quick)
static void nameSetFill(nameSet *set, const char *dir)
{
	memset(set, 0, sizeof (nameSet));

	bt2DirIter *d = bt2FsDirOpen(dir);
	if (d == NULL)
		return;

	const char *name;
	while (bt2FsDirNext(d, &name, NULL, NULL))
		nameSetAdd(set, name);

	bt2FsDirClose(d);
}

static int32_t storeLocal(const char *name)
{
	const size_t len = strlen(name) + 1;
	if (localNamesLen + len > localNamesCap)
	{
		size_t cap = (localNamesCap > 0) ? localNamesCap * 2 : 65536;
		while (cap < localNamesLen + len)
			cap *= 2;

		char *bigger = (char *)realloc(localNames, cap);
		if (bigger == NULL)
			return -1;

		localNames = bigger;
		localNamesCap = cap;
	}

	const int32_t at = (int32_t)localNamesLen;
	memcpy(localNames + localNamesLen, name, len);
	localNamesLen += len;
	return at;
}

// "song.xm" -> "song (2).xm", a folder "Artist" -> "Artist (2)"
static void numberedName(const char *name, bool dir, int32_t k, char *out, size_t outLen)
{
	const char *dot = dir ? NULL : strrchr(name, '.');
	if (dot == NULL || dot == name)
		snprintf(out, outLen, "%s (%d)", name, (int)k);
	else
		snprintf(out, outLen, "%.*s (%d)%s", (int)(dot - name), name, (int)k, dot);
}

/* The names here: Modland's names made safe for this system (bt2CleanName), and different from each other in
** each folder. On Windows "Song.xm" and "song.xm" (or "a?.xm" and "a_.xm") would be the same file, so the
** second one is called "song (2).xm" here. A name that isn't UTF-8 is read as Windows letters (the web address
** still uses Modland's own name).
*/
static void makeLocalNames(void)
{
	localNamesLen = 0;

	char raw[MAX_NAME + 1], utf[MAX_NAME * 3 + 8], clean[MAX_NAME + 8], name[MAX_NAME + 32];
	for (int32_t d = 0; d < nodeCount; d++)
	{
		if (!nodes[d].dir || nodes[d].first < 0)
			continue;

		nameSet used;
		memset(&used, 0, sizeof (used));

		for (int32_t c = nodes[d].first; c >= 0; c = nodes[c].next)
		{
			nodeName(c, raw, sizeof (raw));
			const char *src = raw;
			if (!bt2Utf8Valid(raw, strlen(raw)))
			{
				bt2Cp1252ToUtf8(raw, utf, sizeof (utf));
				src = utf;
			}

			bt2CleanName(src, clean, sizeof (clean), MAX_NAME);
			snprintf(name, sizeof (name), "%s", clean);
			for (int32_t k = 2; nameSetHas(&used, name) && k < 100000; k++)
				numberedName(clean, nodes[c].dir, k, name, sizeof (name));
			nameSetAdd(&used, name);

			nodes[c].local = -1;
			if (strlen(name) != nodes[c].nameLen || memcmp(name, nodes[c].name, nodes[c].nameLen) != 0)
				nodes[c].local = storeLocal(name);
		}

		nameSetFree(&used);
	}
}

// a part of a path here -> the node (the name as it is on the disk; on Windows also in another case, as in a
// path typed in "Set path")
static int32_t childByLocalName(int32_t parent, const char *part, size_t partLen)
{
	char safe[MAX_NAME + 32];
	for (int32_t c = nodes[parent].first; c >= 0; c = nodes[c].next)
	{
		nodeSafeName(c, safe, sizeof (safe));
		if (strlen(safe) == partLen && memcmp(safe, part, partLen) == 0)
			return c;
	}

#ifdef _WIN32
	if (partLen >= KEY_LEN / 4)
		return -1;

	char tmp[KEY_LEN], want[KEY_LEN], key[KEY_LEN];
	memcpy(tmp, part, partLen);
	tmp[partLen] = '\0';
	foldName(tmp, want, sizeof (want));

	for (int32_t c = nodes[parent].first; c >= 0; c = nodes[c].next)
	{
		nodeSafeName(c, safe, sizeof (safe));
		foldName(safe, key, sizeof (key));
		if (strcmp(key, want) == 0)
			return c;
	}
#endif

	return -1;
}

static int32_t childByName(int32_t parent, const char *part, size_t partLen) // (the name on Modland)
{
	for (int32_t c = nodes[parent].first; c >= 0; c = nodes[c].next)
	{
		if (nodes[c].nameLen == partLen && memcmp(nodes[c].name, part, partLen) == 0)
			return c;
	}

	return -1;
}

static bool isSepChar(char c)
{
#ifdef _WIN32
	return c == '\\' || c == '/';
#else
	return c == '/';
#endif
}

// a path here (after the Modland folder) -> its node, or -1
static int32_t findByLocalPath(const char *rel)
{
	int32_t node = 0;
	const char *p = rel;
	while (*p != '\0')
	{
		while (isSepChar(*p))
			p++;
		if (*p == '\0')
			break;

		const char *e = p;
		while (*e != '\0' && !isSepChar(*e))
			e++;

		if (!nodes[node].dir)
			return -1;

		node = childByLocalName(node, p, (size_t)(e - p));
		if (node < 0)
			return -1;

		p = e;
	}

	return node;
}

// the node's path from the top, with the names on Modland ('/') or the names here (separator)
static void nodePath(int32_t node, bool safe, char *out, size_t outLen)
{
	int32_t chain[64], n = 0;
	for (int32_t i = node; i > 0 && n < 64; i = nodes[i].parent)
		chain[n++] = i;

	size_t o = 0;
	out[0] = '\0';
	for (int32_t k = n - 1; k >= 0; k--)
	{
		char name[MAX_NAME + 32];
		if (safe)
			nodeSafeName(chain[k], name, sizeof (name));
		else
			nodeName(chain[k], name, sizeof (name));

		const int32_t w = snprintf(out + o, outLen - o, "%s%s", (o > 0) ? (safe ? BT2_SEP_STR : "/") : "", name);
		if (w < 0 || (size_t)w >= outLen - o)
			break;
		o += (size_t)w;
	}
}

static void populateNode(int32_t node, const char *dir) // (with the lock held)
{
	if (node < 0 || !nodes[node].dir || nodes[node].populated)
		return;

	nodes[node].populated = true;

	nameSet there;
	nameSetFill(&there, dir);

	char path[2048], safe[MAX_NAME + 32];
	for (int32_t c = nodes[node].first; c >= 0; c = nodes[c].next)
	{
		nodeSafeName(c, safe, sizeof (safe));
		if (nameSetHas(&there, safe))
			continue; // (made before)

		bt2PathJoin(path, sizeof (path), dir, safe);
		if (nodes[c].dir)
			bt2FsMkdir(path);
		else
			bt2FsCreateEmpty(path); // (never touches a file that is there: a song that was opened keeps its contents)
	}

	nameSetFree(&there);
}

bool bt2ModlandIsMirrorPath(const char *path)
{
	return setup.mirrorRoot[0] != '\0' && bt2PathInside(path, setup.mirrorRoot);
}

void bt2ModlandPopulate(const char *dir)
{
	if (!loaded)
		return;

	const char *rel = bt2PathAfter(dir, setup.mirrorRoot);
	if (rel == NULL)
		return;

	SDL_LockMutex(lock);
	const int32_t node = findByLocalPath(rel);
	if (node >= 0)
		populateNode(node, dir);
	SDL_UnlockMutex(lock);
}

bool bt2ModlandResolve(const char *file, char *modlandPath, size_t mpLen, char *ownPath, size_t opLen)
{
	if (!loaded)
		return false;

	const char *rel = bt2PathAfter(file, setup.mirrorRoot);
	if (rel == NULL || rel[0] == '\0')
		return false;

	SDL_LockMutex(lock);
	const int32_t node = findByLocalPath(rel);
	bool ok = (node > 0 && !nodes[node].dir);
	if (ok)
	{
		char local[2048];
		nodePath(node, false, modlandPath, mpLen);
		nodePath(node, true, local, sizeof (local));
		bt2PathJoin(ownPath, opLen, setup.ownRoot, local);
	}
	SDL_UnlockMutex(lock);

	return ok;
}

bool bt2ModlandMaterialize(const char *modlandPath, char *out, size_t outLen)
{
	if (!loaded || setup.mirrorRoot[0] == '\0')
		return false;

	SDL_LockMutex(lock);

	char dir[2048], safe[MAX_NAME + 32];
	snprintf(dir, sizeof (dir), "%s", setup.mirrorRoot);
	bt2FsMkdirs(dir);

	int32_t node = 0;
	populateNode(node, dir);

	bool ok = true;
	const char *p = modlandPath;
	while (*p != '\0')
	{
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;

		const char *e = strchr(p, '/');
		if (e == NULL)
			e = p + strlen(p);

		if (!nodes[node].dir) // (nothing is inside a song)
		{
			ok = false;
			break;
		}

		node = childByName(node, p, (size_t)(e - p));
		if (node < 0)
		{
			ok = false;
			break;
		}

		nodeSafeName(node, safe, sizeof (safe));
		char next[2048];
		bt2PathJoin(next, sizeof (next), dir, safe);
		snprintf(dir, sizeof (dir), "%s", next);

		if (nodes[node].dir)
		{
			bt2FsMkdir(dir);
			populateNode(node, dir);
		}
		p = e;
	}

	SDL_UnlockMutex(lock);

	if (ok)
		snprintf(out, outLen, "%s", dir);

	return ok;
}

bool bt2ModlandOwnFolderFor(const char *mirrorDir, char *out, size_t outLen)
{
	const char *rel = bt2PathAfter(mirrorDir, setup.mirrorRoot);
	if (rel == NULL)
		return false;

	if (rel[0] == '\0')
		snprintf(out, outLen, "%s", setup.ownRoot);
	else
		bt2PathJoin(out, outLen, setup.ownRoot, rel);

	return true;
}

void bt2ModlandFileUrl(const char *modlandPath, char *out, size_t outLen)
{
	char enc[6000];
	bt2UrlEncodePath(modlandPath, enc, sizeof (enc));
	snprintf(out, outLen, "%s%s", setup.fileUrl, enc);
}

int32_t bt2ModlandFileCount(void)
{
	return loaded ? fileCount : 0;
}

const char *bt2ModlandFilePath(int32_t i, size_t *len)
{
	*len = files[i].len;
	return files[i].path;
}

int64_t bt2ModlandFileSize(int32_t i)
{
	return files[i].size;
}

void bt2ModlandShutdown(void)
{
	stopping = 1; // (a background download of the list stops; nothing is freed, it may still be running)
}

#endif
