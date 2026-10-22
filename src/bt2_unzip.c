/* BerntTracker: reading zip files without any library. See bt2_unzip.h.
**
** The inflate part follows the structure of "puff" (Mark Adler's small reference decoder for the
** deflate format, zlib license): slow but simple and careful. Damaged data never writes outside the
** buffers; it only makes the functions return false/NULL.
*/

#include <stdlib.h>
#include <string.h>
#include "bt2_unzip.h"

// ------------------------------------------------------------------------------------------------
// inflate (RFC 1951)

typedef struct inflateState_t
{
	const uint8_t *in;
	size_t inLen, inPos;
	uint32_t bitBuf;
	int32_t bitCnt;
	uint8_t *out;
	size_t outLen, outPos;
	bool err;
} inflateState;

typedef struct huffman_t
{
	int16_t count[16];   // how many codes of each length
	int16_t symbol[288]; // the symbols, in code order
} huffman;

static int32_t getBits(inflateState *s, int32_t need)
{
	uint32_t val = s->bitBuf;
	while (s->bitCnt < need)
	{
		if (s->inPos >= s->inLen)
		{
			s->err = true;
			return 0;
		}

		val |= (uint32_t)s->in[s->inPos++] << s->bitCnt;
		s->bitCnt += 8;
	}

	s->bitBuf = val >> need;
	s->bitCnt -= need;
	return (int32_t)(val & ((1u << need) - 1));
}

static bool storedBlock(inflateState *s)
{
	// (the rest of the current byte is padding)
	s->bitBuf = 0;
	s->bitCnt = 0;

	if (s->inLen - s->inPos < 4)
		return false;

	const uint32_t len = s->in[s->inPos] | ((uint32_t)s->in[s->inPos + 1] << 8);
	const uint32_t nlen = s->in[s->inPos + 2] | ((uint32_t)s->in[s->inPos + 3] << 8);
	s->inPos += 4;

	if (len != (~nlen & 0xFFFF) || len > s->inLen - s->inPos || len > s->outLen - s->outPos)
		return false;

	memcpy(s->out + s->outPos, s->in + s->inPos, len);
	s->inPos += len;
	s->outPos += len;
	return true;
}

static int32_t decodeSymbol(inflateState *s, const huffman *h)
{
	int32_t code = 0, first = 0, index = 0;
	for (int32_t len = 1; len <= 15; len++)
	{
		code |= getBits(s, 1);
		if (s->err)
			return -1;

		const int32_t count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];

		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}

	return -1; // (no such code)
}

// canonical code from the code lengths: 0 = complete, > 0 = incomplete, < 0 = over-subscribed (bad)
static int32_t buildHuffman(huffman *h, const int16_t *length, int32_t n)
{
	int16_t offs[16];

	for (int32_t len = 0; len <= 15; len++)
		h->count[len] = 0;

	for (int32_t sym = 0; sym < n; sym++)
	{
		if (length[sym] < 0 || length[sym] > 15)
			return -1;
		h->count[length[sym]]++;
	}

	if (h->count[0] == n)
		return 0;

	int32_t left = 1;
	for (int32_t len = 1; len <= 15; len++)
	{
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}

	offs[1] = 0;
	for (int32_t len = 1; len < 15; len++)
		offs[len + 1] = offs[len] + h->count[len];

	for (int32_t sym = 0; sym < n; sym++)
	{
		if (length[sym] != 0)
			h->symbol[offs[length[sym]]++] = (int16_t)sym;
	}

	return left;
}

static const int16_t lengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
	115, 131, 163, 195, 227, 258 };
static const int16_t lengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int16_t distBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
	1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const int16_t distExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
	12, 13, 13 };

static bool decodeCodes(inflateState *s, const huffman *lenCode, const huffman *distCode)
{
	for (;;)
	{
		int32_t symbol = decodeSymbol(s, lenCode);
		if (symbol < 0)
			return false;

		if (symbol < 256)
		{
			if (s->outPos >= s->outLen)
				return false;
			s->out[s->outPos++] = (uint8_t)symbol;
		}
		else if (symbol == 256)
		{
			return true; // end of block
		}
		else
		{
			symbol -= 257;
			if (symbol >= 29)
				return false;

			const int32_t len = lengthBase[symbol] + getBits(s, lengthExtra[symbol]);
			const int32_t dsym = decodeSymbol(s, distCode);
			if (s->err || dsym < 0 || dsym >= 30)
				return false;

			const size_t dist = (size_t)distBase[dsym] + (size_t)getBits(s, distExtra[dsym]);
			if (s->err || dist > s->outPos || (size_t)len > s->outLen - s->outPos)
				return false;

			uint8_t *o = s->out + s->outPos;
			const uint8_t *from = o - dist;
			for (int32_t i = 0; i < len; i++) // (byte by byte: the copy may overlap itself)
				o[i] = from[i];
			s->outPos += len;
		}
	}
}

static bool fixedBlock(inflateState *s)
{
	huffman lenCode, distCode;
	int16_t lengths[288];
	int32_t sym = 0;

	for (; sym < 144; sym++) lengths[sym] = 8;
	for (; sym < 256; sym++) lengths[sym] = 9;
	for (; sym < 280; sym++) lengths[sym] = 7;
	for (; sym < 288; sym++) lengths[sym] = 8;
	buildHuffman(&lenCode, lengths, 288);

	for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
	buildHuffman(&distCode, lengths, 30);

	return decodeCodes(s, &lenCode, &distCode);
}

static bool dynamicBlock(inflateState *s)
{
	static const int16_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	int16_t lengths[320];
	huffman lenCode, distCode;

	const int32_t nlen = getBits(s, 5) + 257;
	const int32_t ndist = getBits(s, 5) + 1;
	const int32_t ncode = getBits(s, 4) + 4;
	if (s->err || nlen > 286 || ndist > 30)
		return false;

	int32_t index = 0;
	for (; index < ncode; index++)
		lengths[order[index]] = (int16_t)getBits(s, 3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;

	if (s->err || buildHuffman(&lenCode, lengths, 19) != 0)
		return false; // (the code-length code must be complete)

	index = 0;
	while (index < nlen + ndist)
	{
		int32_t symbol = decodeSymbol(s, &lenCode);
		if (symbol < 0)
			return false;

		if (symbol < 16)
		{
			lengths[index++] = (int16_t)symbol;
		}
		else
		{
			int16_t len = 0;
			int32_t rep;
			if (symbol == 16)
			{
				if (index == 0)
					return false;
				len = lengths[index - 1];
				rep = 3 + getBits(s, 2);
			}
			else if (symbol == 17)
			{
				rep = 3 + getBits(s, 3);
			}
			else
			{
				rep = 11 + getBits(s, 7);
			}

			if (s->err || index + rep > nlen + ndist)
				return false;

			while (rep--)
				lengths[index++] = len;
		}
	}

	if (lengths[256] == 0)
		return false; // (no end-of-block code)

	int32_t err = buildHuffman(&lenCode, lengths, nlen);
	if (err < 0 || (err > 0 && nlen - lenCode.count[0] != 1))
		return false;

	err = buildHuffman(&distCode, lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist - distCode.count[0] != 1))
		return false;

	return decodeCodes(s, &lenCode, &distCode);
}

bool bt2Inflate(const uint8_t *in, size_t inLen, uint8_t *out, size_t outLen, size_t *written)
{
	inflateState s;
	memset(&s, 0, sizeof (s));
	s.in = in;
	s.inLen = inLen;
	s.out = out;
	s.outLen = outLen;

	int32_t last;
	do
	{
		last = getBits(&s, 1);
		const int32_t type = getBits(&s, 2);
		if (s.err)
			return false;

		bool ok;
		if (type == 0) ok = storedBlock(&s);
		else if (type == 1) ok = fixedBlock(&s);
		else if (type == 2) ok = dynamicBlock(&s);
		else ok = false;

		if (!ok || s.err)
			return false;
	}
	while (!last);

	if (written != NULL)
		*written = s.outPos;

	return true;
}

// ------------------------------------------------------------------------------------------------
// CRC-32 (the one zip uses)

static const uint32_t crcTable[256] = // (fixed numbers: the same in every thread)
{
	0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
	0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
	0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
	0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
	0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
	0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
	0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
	0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
	0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
	0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
	0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
	0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
	0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
	0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
	0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
	0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
	0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
	0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
	0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
	0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
	0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
	0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
	0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
	0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
	0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
	0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
	0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
	0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
	0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
	0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
	0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
	0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

uint32_t bt2Crc32(const uint8_t *data, size_t len)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++)
		c = crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);

	return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------------------------------------------
// zip files

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bt2ZipEntry *bt2ZipList(const uint8_t *zip, size_t zipLen, int32_t *count)
{
	*count = 0;
	if (zip == NULL || zipLen < 22)
		return NULL;

	// the "end of central directory" record is at the end, after a comment of at most 65535 bytes
	size_t eocd = (size_t)-1;
	const size_t lowest = (zipLen > 65557) ? zipLen - 65557 : 0;
	for (size_t i = zipLen - 22;; i--)
	{
		if (rd32(zip + i) == 0x06054b50)
		{
			eocd = i;
			break;
		}

		if (i == lowest)
			break;
	}

	if (eocd == (size_t)-1)
		return NULL;

	const uint32_t entries = rd16(zip + eocd + 10);
	size_t p = rd32(zip + eocd + 16);
	if (entries == 0 || p >= zipLen)
		return NULL;

	bt2ZipEntry *list = (bt2ZipEntry *)calloc(entries, sizeof (bt2ZipEntry));
	if (list == NULL)
		return NULL;

	int32_t n = 0;
	for (uint32_t i = 0; i < entries; i++)
	{
		if (p + 46 > zipLen || rd32(zip + p) != 0x02014b50)
			break;

		const uint32_t nameLen = rd16(zip + p + 28), extraLen = rd16(zip + p + 30), commentLen = rd16(zip + p + 32);
		if (p + 46 + nameLen > zipLen)
			break;

		bt2ZipEntry *e = &list[n];
		e->method = (uint16_t)rd16(zip + p + 10);
		e->crc = rd32(zip + p + 16);
		e->packedSize = rd32(zip + p + 20);
		e->size = rd32(zip + p + 24);
		e->localOffset = rd32(zip + p + 42);

		const size_t copyLen = (nameLen < sizeof (e->name) - 1) ? nameLen : sizeof (e->name) - 1;
		memcpy(e->name, zip + p + 46, copyLen);
		e->name[copyLen] = '\0';
		e->dir = (copyLen > 0 && (e->name[copyLen - 1] == '/' || e->name[copyLen - 1] == '\\'));

		// (sizes over 4 GB, "zip64", are not supported: such an entry is left out)
		if (e->packedSize != 0xFFFFFFFFu && e->size != 0xFFFFFFFFu && e->localOffset != 0xFFFFFFFFu && nameLen < sizeof (e->name))
			n++;

		p += 46 + (size_t)nameLen + extraLen + commentLen;
	}

	if (n == 0)
	{
		free(list);
		return NULL;
	}

	*count = n;
	return list;
}

uint8_t *bt2ZipExtract(const uint8_t *zip, size_t zipLen, const bt2ZipEntry *e, size_t *outLen)
{
	*outLen = 0;
	const size_t lh = e->localOffset;
	if (lh + 30 > zipLen || rd32(zip + lh) != 0x04034b50)
		return NULL;

	const size_t dataPos = lh + 30 + rd16(zip + lh + 26) + rd16(zip + lh + 28);
	if (dataPos > zipLen || e->packedSize > zipLen - dataPos)
		return NULL;

	uint8_t *out = (uint8_t *)malloc((size_t)e->size + 1);
	if (out == NULL)
		return NULL;

	size_t got = 0;
	bool ok = false;
	if (e->method == 0)
	{
		if (e->packedSize == e->size)
		{
			memcpy(out, zip + dataPos, e->size);
			got = e->size;
			ok = true;
		}
	}
	else if (e->method == 8)
	{
		ok = bt2Inflate(zip + dataPos, e->packedSize, out, e->size, &got) && got == e->size;
	}

	if (!ok || bt2Crc32(out, got) != e->crc)
	{
		free(out);
		return NULL;
	}

	out[got] = 0;
	*outLen = got;
	return out;
}
