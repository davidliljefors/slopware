// MurmurHash3 was written by Austin Appleby, and is placed in the public domain.
// https://github.com/aappleby/smhasher

#pragma once

#include "types.h"
#include "string.h"

static inline u32 murmur3_rotl32(u32 x, i8 r) { return (x << r) | (x >> (32 - r)); }
static inline u64 murmur3_rotl64(u64 x, i8 r) { return (x << r) | (x >> (64 - r)); }

static inline u64 murmur3_fmix64(u64 k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	k *= 0xc4ceb9fe1a85ec53ULL;
	k ^= k >> 33;
	return k;
}

static u64 murmurhash3_64(const void* key, i32 len, u64 seed = 0)
{
	const u8* data = (const u8*)key;
	const i32 nblocks = len / 16;

	u64 h1 = seed;
	u64 h2 = seed;

	const u64 c1 = 0x87c37b91114253d5ULL;
	const u64 c2 = 0x4cf5ad432745937fULL;

	const u64* blocks = (const u64*)data;
	for (i32 i = 0; i < nblocks; i++) {
		u64 k1, k2;
		memcpy(&k1, &blocks[i * 2 + 0], sizeof(u64));
		memcpy(&k2, &blocks[i * 2 + 1], sizeof(u64));

		k1 *= c1; k1  = murmur3_rotl64(k1, 31); k1 *= c2; h1 ^= k1;
		h1 = murmur3_rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;

		k2 *= c2; k2  = murmur3_rotl64(k2, 33); k2 *= c1; h2 ^= k2;
		h2 = murmur3_rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
	}

	const u8* tail = data + nblocks * 16;
	u64 k1 = 0;
	u64 k2 = 0;

	switch (len & 15) {
	case 15: k2 ^= (u64)tail[14] << 48;
	case 14: k2 ^= (u64)tail[13] << 40;
	case 13: k2 ^= (u64)tail[12] << 32;
	case 12: k2 ^= (u64)tail[11] << 24;
	case 11: k2 ^= (u64)tail[10] << 16;
	case 10: k2 ^= (u64)tail[ 9] << 8; 
	case  9: k2 ^= (u64)tail[ 8] << 0;
		k2 *= c2; k2  = murmur3_rotl64(k2, 33); k2 *= c1; h2 ^= k2;
	case  8: k1 ^= (u64)tail[ 7] << 56;
	case  7: k1 ^= (u64)tail[ 6] << 48;
	case  6: k1 ^= (u64)tail[ 5] << 40;
	case  5: k1 ^= (u64)tail[ 4] << 32;
	case  4: k1 ^= (u64)tail[ 3] << 24;
	case  3: k1 ^= (u64)tail[ 2] << 16;
	case  2: k1 ^= (u64)tail[ 1] << 8; 
	case  1: k1 ^= (u64)tail[ 0] << 0;
		k1 *= c1; k1  = murmur3_rotl64(k1, 31); k1 *= c2; h1 ^= k1;
	}

	h1 ^= len; h2 ^= len;
	h1 += h2; h2 += h1;
	h1 = murmur3_fmix64(h1);
	h2 = murmur3_fmix64(h2);
	h1 += h2;

	return h1;
}
