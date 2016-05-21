/*
 * adler32.c - Adler-32 checksum algorithm
 *
 * Written in 2014-2015 by Eric Biggers <ebiggers3@gmail.com>
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

#include "adler32.h"

/*
 * The Adler-32 divisor, or "base", value.
 */
#define DIVISOR 65521

/*
 * MAX_BYTES_PER_CHUNK is the most bytes that can be processed without the
 * possibility of s2 overflowing when it is represented as an unsigned 32-bit
 * integer.  This value was computed using the following Python script:
 *
 *	divisor = 65521
 *	count = 0
 *	s1 = divisor - 1
 *	s2 = divisor - 1
 *	while True:
 *		s1 += 0xFF
 *		s2 += s1
 *		if s2 > 0xFFFFFFFF:
 *			break
 *		count += 1
 *	print(count)
 *
 * Note that to get the correct worst-case value, we must assume that every byte
 * has value 0xFF and that s1 and s2 started with the highest possible values
 * modulo the divisor.
 */
#define MAX_BYTES_PER_CHUNK	5552

/* Number of bytes to process per loop iteration  */
#define UNROLL_FACTOR	4

u32
adler32(const void *buffer, size_t size)
{
	u32 s1 = 1;
	u32 s2 = 0;
	const u8 *p = buffer;
	const u8 *end = p + size;
	while (p != end) {
		size_t chunk_size = MIN(end - p, MAX_BYTES_PER_CHUNK);
		const u8 *chunk_end = p + chunk_size;

	#if UNROLL_FACTOR > 1
		size_t num_unrolled_iterations = chunk_size / UNROLL_FACTOR;
		while (num_unrolled_iterations--) {
			s1 += *p++;
			s2 += s1;
		#if UNROLL_FACTOR >= 2
			s1 += *p++;
			s2 += s1;
		#endif
		#if UNROLL_FACTOR >= 3
			s1 += *p++;
			s2 += s1;
		#endif
		#if UNROLL_FACTOR >= 4
			s1 += *p++;
			s2 += s1;
		#endif
		#if UNROLL_FACTOR >= 5
			s1 += *p++;
			s2 += s1;
		#endif
		#if UNROLL_FACTOR >= 6
			s1 += *p++;
			s2 += s1;
		#endif
		#if UNROLL_FACTOR >= 7
			s1 += *p++;
			s2 += s1;
		#endif
		#if UNROLL_FACTOR >= 8
			s1 += *p++;
			s2 += s1;
		#endif
			STATIC_ASSERT(UNROLL_FACTOR <= 8);
		}
	#endif /* UNROLL_FACTOR > 1 */
		while (p != chunk_end) {
			s1 += *p++;
			s2 += s1;
		}
		s1 %= DIVISOR;
		s2 %= DIVISOR;
	}
	return (s2 << 16) | s1;
}
