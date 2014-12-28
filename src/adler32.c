/*
 * adler32.c
 *
 * Adler-32 checksum algorithm.
 */

#include "adler32.h"

u32
adler32(const u8 *buffer, size_t size)
{
	u32 s1 = 1;
	u32 s2 = 0;
	for (size_t i = 0; i < size; i++) {
		s1 = (s1 + buffer[i]) % 65521;
		s2 = (s2 + s1) % 65521;
	}
	return (s2 << 16) | s1;
}
