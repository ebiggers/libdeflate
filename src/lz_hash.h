/*
 * lz_hash.h
 *
 * Hashing for Lempel-Ziv matchfinding.
 */

#ifndef _LZ_HASH_H
#define _LZ_HASH_H

#include "unaligned.h"

/*
 * The hash function: given a sequence prefix held in the low-order bits of a
 * 32-bit value, multiply by a carefully-chosen large constant.  Discard any
 * bits of the product that don't fit in a 32-bit value, but take the
 * next-highest @num_bits bits of the product as the hash value, as those have
 * the most randomness.
 */
static inline u32
lz_hash(u32 seq, unsigned num_bits)
{
	return (u32)(seq * 0x1E35A7BD) >> (32 - num_bits);
}

/*
 * Hash the 3-byte sequence beginning at @p, producing a hash of length
 * @num_bits bits.  At least LZ_HASH3_REQUIRED_NBYTES bytes of data must be
 * available at @p; note that this may be more than 3.
 */
static inline u32
lz_hash_3_bytes(const u8 *p, unsigned num_bits)
{
	u32 seq = load_u24_unaligned(p);
	if (num_bits >= 24)
		return seq;
	return lz_hash(seq, num_bits);
}

#define LZ_HASH3_REQUIRED_NBYTES LOAD_U24_REQUIRED_NBYTES

#endif /* _LZ_HASH_H */
