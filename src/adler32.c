/*
 * adler32.c
 *
 * Adler-32 checksum algorithm.
 */

#include "adler32.h"
#include "compiler.h"

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

u32
adler32(const u8 *buffer, size_t size)
{
	u32 s1 = 1;
	u32 s2 = 0;
	const u8 *p = buffer;
	const u8 * const end = p + size;
	while (p != end) {
		const u8 *chunk_end = p + min(end - p,
					      MAX_BYTES_PER_CHUNK);
		do {
			s1 += *p++;
			s2 += s1;
		} while (p != chunk_end);
		s1 %= 65521;
		s2 %= 65521;
	}
	return (s2 << 16) | s1;
}
