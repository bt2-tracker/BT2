/* BerntTracker: what the search window finds. See bt2_search.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bt2_search.h"
#include "bt2_modland.h"
#include "bt2_fs.h"

#define MAX_OWN_ITEMS 200000
#define MAX_DEPTH 24
#define BLOCK_SIZE (1024 * 1024)

// ------------------------------------------------------------------------------------------------
// small letters

static uint32_t lowerCp(uint32_t c)
{
	if (c >= 'A' && c <= 'Z') return c + 32;
	if (c < 0xC0) return c;
	if (c <= 0xDE) return (c == 0xD7) ? c : c + 32;
	if (c == 0x130) return 'i';
	if (c >= 0x100 && c <= 0x137) return (c & 1) ? c : c + 1;
	if (c >= 0x139 && c <= 0x148) return (c & 1) ? c + 1 : c;
	if (c >= 0x14A && c <= 0x177) return (c & 1) ? c : c + 1;
	if (c == 0x178) return 0xFF;
	if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c + 1 : c;
	if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
	if (c >= 0x410 && c <= 0x42F) return c + 32;
	if (c >= 0x400 && c <= 0x40F) return c + 80;
	return c;
}

void bt2Utf8Lower(const char *in, char *out, size_t outLen)
{
	if (outLen == 0)
		return;

	const uint8_t *p = (const uint8_t *)in;
	size_t o = 0;
	while (*p != '\0')
	{
		uint32_t c = *p;
		int32_t n = 0;
		if (c >= 0xF0 && c < 0xF8) { n = 3; c &= 0x07; }
		else if (c >= 0xE0) { n = 2; c &= 0x0F; }
		else if (c >= 0xC0) { n = 1; c &= 0x1F; }

		bool ok = true;
		for (int32_t i = 1; i <= n; i++)
		{
			if ((p[i] & 0xC0) != 0x80)
			{
				ok = false;
				break;
			}
			c = (c << 6) | (p[i] & 0x3F);
		}

		if (!ok || c >= 0x80000000u) // (not UTF-8: the byte as it is)
		{
			if (o + 1 >= outLen) break;
			out[o++] = (char)*p++;
			continue;
		}

		const uint32_t l = lowerCp(c);
		char buf[4];
		size_t bn;
		if (l < 0x80) { buf[0] = (char)l; bn = 1; }
		else if (l < 0x800) { buf[0] = (char)(0xC0 | (l >> 6)); buf[1] = (char)(0x80 | (l & 0x3F)); bn = 2; }
		else if (l < 0x10000) { buf[0] = (char)(0xE0 | (l >> 12)); buf[1] = (char)(0x80 | ((l >> 6) & 0x3F)); buf[2] = (char)(0x80 | (l & 0x3F)); bn = 3; }
		else { buf[0] = (char)(0xF0 | (l >> 18)); buf[1] = (char)(0x80 | ((l >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((l >> 6) & 0x3F)); buf[3] = (char)(0x80 | (l & 0x3F)); bn = 4; }

		if (o + bn >= outLen) break;
		memcpy(out + o, buf, bn);
		o += bn;
		p += n + 1;
	}

	out[o] = '\0';
}

// ------------------------------------------------------------------------------------------------
// kinds

uint8_t bt2SearchKindOf(const char *name)
{
	static const char *const songs[] = { "xm", "mod", "s3m", "it", "stm", "stk", "nst", "digi", "bem", "mtm", "669", "wow", NULL };
	static const char *const sounds[] = { "xi", "xp", "xt", "wav", "aif", "aiff", "iff", "8svx", "raw", "flac", "ogg", "mp3", "brr", "pat", "sf2", NULL };

	const char *dot = strrchr(name, '.');
	if (dot == NULL || strchr(dot, '/') != NULL)
		return BT2_KIND_OTHER;

	char ext[8];
	size_t n = strlen(dot + 1);
	if (n == 0 || n >= sizeof (ext))
		return BT2_KIND_OTHER;

	for (size_t i = 0; i <= n; i++)
	{
		char c = dot[1 + i];
		ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}

	for (int32_t i = 0; songs[i] != NULL; i++)
		if (!strcmp(ext, songs[i])) return BT2_KIND_SONG;

	for (int32_t i = 0; sounds[i] != NULL; i++)
		if (!strcmp(ext, sounds[i])) return BT2_KIND_SOUND;

	return BT2_KIND_OTHER;
}

// ------------------------------------------------------------------------------------------------
// the pools

static char *poolText(bt2SearchPool *p, const char *s, size_t len)
{
	if (len + 1 > BLOCK_SIZE)
		return NULL;

	if (p->numBlocks == 0 || p->blockUsed + len + 1 > p->blockSize)
	{
		char **blocks = (char **)realloc(p->blocks, (size_t)(p->numBlocks + 1) * sizeof (char *));
		if (blocks == NULL)
			return NULL;
		p->blocks = blocks;

		char *b = (char *)malloc(BLOCK_SIZE);
		if (b == NULL)
			return NULL;

		p->blocks[p->numBlocks++] = b;
		p->blockUsed = 0;
		p->blockSize = BLOCK_SIZE;
	}

	char *out = p->blocks[p->numBlocks - 1] + p->blockUsed;
	memcpy(out, s, len);
	out[len] = '\0';
	p->blockUsed += len + 1;
	return out;
}

static bt2SearchItem *poolAdd(bt2SearchPool *p, const char *rel, size_t relLen)
{
	if (p->count >= p->cap)
	{
		const int32_t cap = (p->cap > 0) ? p->cap * 2 : 1024;
		bt2SearchItem *items = (bt2SearchItem *)realloc(p->items, (size_t)cap * sizeof (bt2SearchItem));
		if (items == NULL)
			return NULL;
		p->items = items;
		p->cap = cap;
	}

	char *r = poolText(p, rel, relLen);
	if (r == NULL)
		return NULL;

	char lowBuf[4096];
	bt2Utf8Lower(r, lowBuf, sizeof (lowBuf));
	char *l = poolText(p, lowBuf, strlen(lowBuf));
	if (l == NULL)
		return NULL;

	bt2SearchItem *it = &p->items[p->count++];
	memset(it, 0, sizeof (bt2SearchItem));
	it->rel = r;
	it->low = l;

	const char *slash = strrchr(r, '/');
	it->name = (slash != NULL) ? slash + 1 : r;
	it->dirLen = (slash != NULL) ? (int32_t)(slash - r) : 0;

	const char *lslash = strrchr(l, '/');
	it->lowName = (lslash != NULL) ? lslash + 1 : l;
	return it;
}

void bt2SearchFreePool(bt2SearchPool *p)
{
	for (int32_t i = 0; i < p->numBlocks; i++)
		free(p->blocks[i]);
	free(p->blocks);
	free(p->items);
	memset(p, 0, sizeof (bt2SearchPool));
}

static void walk(bt2SearchPool *p, const char *abs, const char *rel, int32_t depth)
{
	if (depth > MAX_DEPTH)
		return;

	bt2DirIter *d = bt2FsDirOpen(abs);
	if (d == NULL)
		return;

	const char *name;
	bool isDir;
	int64_t size;
	while (p->count < MAX_OWN_ITEMS && bt2FsDirNext(d, &name, &isDir, &size))
	{
		if (name[0] == '.')
			continue; // (the program's own files, like .demo-seen.json)

		char r[2048], a[2048];
		if ((size_t)snprintf(r, sizeof (r), "%s%s%s", rel, (rel[0] != '\0') ? "/" : "", name) >= sizeof (r))
			continue;
		bt2PathJoin(a, sizeof (a), abs, name);

		bt2SearchItem *it = poolAdd(p, r, strlen(r));
		if (it == NULL)
			break;

		it->kind = isDir ? BT2_KIND_FOLDER : bt2SearchKindOf(name);
		it->size = isDir ? 0 : size;

		if (isDir)
			walk(p, a, r, depth + 1);
	}

	bt2FsDirClose(d);
}

void bt2SearchBuildOwn(bt2SearchPool *p, const char *userRoot)
{
	bt2SearchFreePool(p);
	walk(p, userRoot, "", 0);
}

typedef struct folderSlot_t
{
	const char *key;
	uint32_t len;
	int32_t count;
} folderSlot;

static uint32_t hashBytes(const char *s, uint32_t len)
{
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < len; i++)
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	return h;
}

void bt2SearchBuildModland(bt2SearchPool *p)
{
	bt2SearchFreePool(p);

	const int32_t n = bt2ModlandFileCount();
	if (n <= 0)
		return;

	// the folders and how many songs are in each (also in their sub-folders)
	uint32_t cap = 8192;
	while (cap < (uint32_t)n * 2)
		cap *= 2;

	folderSlot *slots = (folderSlot *)calloc(cap, sizeof (folderSlot));
	int32_t numFolders = 0;

	for (int32_t i = 0; i < n; i++)
	{
		size_t len;
		const char *path = bt2ModlandFilePath(i, &len);
		if (len >= 4000)
			continue;

		bt2SearchItem *it = poolAdd(p, path, len);
		if (it == NULL)
			break;

		const uint8_t kind = bt2SearchKindOf(it->name);
		it->kind = (kind == BT2_KIND_OTHER) ? BT2_KIND_SONG : kind;
		it->size = bt2ModlandFileSize(i);
		it->modland = true;

		if (slots == NULL)
			continue;

		// every folder above the song gets one more ("A/B/c.xm": "A/B" and "A")
		for (int32_t d = it->dirLen; d > 0;)
		{
			uint32_t h = hashBytes(it->rel, (uint32_t)d) & (cap - 1);
			while (slots[h].key != NULL && !(slots[h].len == (uint32_t)d && memcmp(slots[h].key, it->rel, (size_t)d) == 0))
				h = (h + 1) & (cap - 1);

			if (slots[h].key == NULL)
			{
				if ((uint32_t)numFolders * 2 >= cap)
					break; // (full: no more folders are counted)

				slots[h].key = it->rel;
				slots[h].len = (uint32_t)d;
				numFolders++;
			}
			slots[h].count++;

			while (d > 0 && it->rel[d - 1] != '/')
				d--;
			if (d > 0)
				d--; // (the folder above)
		}
	}

	if (slots != NULL)
	{
		for (uint32_t h = 0; h < cap; h++)
		{
			if (slots[h].key == NULL)
				continue;

			bt2SearchItem *it = poolAdd(p, slots[h].key, slots[h].len);
			if (it == NULL)
				break;

			it->kind = BT2_KIND_FOLDER;
			it->count = slots[h].count;
			it->modland = true;
		}
		free(slots);
	}
}

// ------------------------------------------------------------------------------------------------
// searching

static int compareResults(const void *a, const void *b)
{
	const bt2SearchItem *x = *(const bt2SearchItem *const *)a, *y = *(const bt2SearchItem *const *)b;
	if (x->rank != y->rank)
		return (x->rank < y->rank) ? -1 : 1;

	if (x->modland != y->modland)
		return x->modland ? 1 : -1; // (own files first)

	const int c = strcmp(x->low, y->low);
	if (c != 0)
		return c;

	return strcmp(x->rel, y->rel); // ("Jungle" and "jungle": always in the same order)
}

int32_t bt2SearchRun(const char *query, int32_t where, int32_t what, const bt2SearchPool *own, const bt2SearchPool *mod,
	bt2SearchItem ***results)
{
	*results = NULL;

	// the words, in small letters
	char low[1024];
	bt2Utf8Lower(query, low, sizeof (low));

	char *words[64];
	int32_t numWords = 0;
	for (char *p = low; *p != '\0' && numWords < 64;)
	{
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			*p++ = '\0';
		if (*p == '\0')
			break;

		words[numWords++] = p;
		while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
			p++;
	}

	const bt2SearchPool *pools[2];
	int32_t numPools = 0;
	if (where != BT2_WHERE_MODLAND && own != NULL) pools[numPools++] = own;
	if (where != BT2_WHERE_OWN && mod != NULL) pools[numPools++] = mod;

	int32_t total = 0;
	for (int32_t i = 0; i < numPools; i++)
		total += pools[i]->count;

	bt2SearchItem **res = (bt2SearchItem **)malloc((size_t)(total + 1) * sizeof (bt2SearchItem *));
	if (res == NULL)
		return 0;

	const uint8_t wantKind = (what == BT2_WHAT_SONG) ? BT2_KIND_SONG : (what == BT2_WHAT_FOLDER) ? BT2_KIND_FOLDER : BT2_KIND_SOUND;

	int32_t n = 0;
	for (int32_t pi = 0; pi < numPools; pi++)
	{
		const bt2SearchPool *pool = pools[pi];
		for (int32_t i = 0; i < pool->count; i++)
		{
			bt2SearchItem *it = &pool->items[i];
			if ((what == BT2_WHAT_ALL) ? (it->kind == BT2_KIND_OTHER) : (it->kind != wantKind))
				continue;

			bool ok = true;
			for (int32_t w = 0; w < numWords; w++)
			{
				if (strstr(it->low, words[w]) == NULL)
				{
					ok = false;
					break;
				}
			}
			if (!ok)
				continue;

			// best first: all words in the name itself, the name starting with the first word, folders, own files
			int32_t rank = 0;
			for (int32_t w = 0; w < numWords; w++)
			{
				if (strstr(it->lowName, words[w]) == NULL)
				{
					rank += 4;
					break;
				}
			}
			if (numWords > 0 && strncmp(it->lowName, words[0], strlen(words[0])) != 0)
				rank += 2;
			if (it->kind != BT2_KIND_FOLDER)
				rank += 1;

			it->rank = rank + ((it->modland && numWords == 0) ? 8 : 0);
			res[n++] = it;
		}
	}

	qsort(res, (size_t)n, sizeof (bt2SearchItem *), compareResults);
	*results = res;
	return n;
}

// ------------------------------------------------------------------------------------------------
// where a hit leads (the same in the Windows program and the web version)

static bool endsWithXi(const char *name)
{
	const size_t n = strlen(name);
	return n >= 3 && name[n - 3] == '.' && (name[n - 2] == 'x' || name[n - 2] == 'X') && (name[n - 1] == 'i' || name[n - 1] == 'I');
}

static void toSystemSeparators(char *path)
{
#ifdef _WIN32
	for (char *p = path; *p != '\0'; p++)
	{
		if (*p == '/')
			*p = BT2_SEP;
	}
#else
	(void)path;
#endif
}

bool bt2SearchTarget(const bt2SearchItem *it, int32_t what, const char *userRoot, char *folder, size_t folderLen,
	char *file, size_t fileLen, int32_t *diskOpItem)
{
	if (it == NULL || folderLen == 0 || fileLen == 0)
		return false;

	folder[0] = '\0';
	file[0] = '\0';

	// the folder (on Modland or in the BT2 folder): the hit itself, or the folder it is in
	char rel[4096];
	if (it->kind == BT2_KIND_FOLDER)
		snprintf(rel, sizeof (rel), "%s", it->rel);
	else
		snprintf(rel, sizeof (rel), "%.*s", (int)it->dirLen, it->rel);

	if (it->modland)
	{
		// (made and filled here; the names here can differ from Modland's)
		if (!bt2ModlandMaterialize(rel, folder, folderLen))
			return false;

		if (it->kind != BT2_KIND_FOLDER && !bt2ModlandMaterialize(it->rel, file, fileLen))
			return false;
	}
	else
	{
		if (rel[0] == '\0')
			snprintf(folder, folderLen, "%s", userRoot);
		else
			bt2PathJoin(folder, folderLen, userRoot, rel);
		toSystemSeparators(folder);

		if (it->kind != BT2_KIND_FOLDER)
			bt2PathJoin(file, fileLen, folder, it->name);
	}

	// the kind of files Disk Op. shows for it
	if (it->kind == BT2_KIND_SOUND)
		*diskOpItem = endsWithXi(it->name) ? BT2_ITEM_INSTR : BT2_ITEM_SAMPLE;
	else
		*diskOpItem = (what == BT2_WHAT_SOUND) ? BT2_ITEM_SAMPLE : BT2_ITEM_MODULE;

	return true;
}
