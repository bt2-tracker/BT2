#pragma once

/* BerntTracker: reading zip files (the Modland list, and zip files the user imports), without any
** library: a small "inflate" (RFC 1951) and the zip file's own table of contents.
*/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct bt2ZipEntry_t
{
	char name[512];         // as stored (UTF-8 or old DOS letters), folders end with '/'
	bool dir;
	uint16_t method;        // 0 = stored, 8 = deflate
	uint32_t packedSize, size, crc, localOffset;
} bt2ZipEntry;

// all entries (*count of them) in a zip file in memory; free() the list. NULL = not a zip file we can read.
bt2ZipEntry *bt2ZipList(const uint8_t *zip, size_t zipLen, int32_t *count);

// one entry's contents (*outLen bytes, malloc'ed, with an extra 0 byte at the end); NULL = damaged or unsupported
uint8_t *bt2ZipExtract(const uint8_t *zip, size_t zipLen, const bt2ZipEntry *e, size_t *outLen);

// raw deflate data -> out (outLen bytes expected). true = all good.
bool bt2Inflate(const uint8_t *in, size_t inLen, uint8_t *out, size_t outLen, size_t *written);

uint32_t bt2Crc32(const uint8_t *data, size_t len);
