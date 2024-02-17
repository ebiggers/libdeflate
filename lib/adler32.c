/*
 * adler32.c - Adler-32 checksum algorithm
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "lib_common.h"

/* The Adler-32 divisor, or "base", value */
#define DIVISOR 65521

/*
 * MAX_CHUNK_LEN is the most bytes that can be processed without the possibility
 * of s2 overflowing when it is represented as an unsigned 32-bit integer.  This
 * value was computed using the following Python script:
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
#define MAX_CHUNK_LEN	5552

static forceinline void MAYBE_UNUSED
adler32_generic_noreduce(u32 *s1_p, u32 *s2_p, const u8 *p, size_t len)
{
	u32 s1 = *s1_p;
	u32 s2 = *s2_p;

	/*
	 * This loop processes four bytes at a time with increased instruction-
	 * level parallelism when compared to the traditional approach of
	 * repeatedly doing 's1 += *p++; s2 += s1'.  It is very similar to how
	 * vectorized implementations (e.g. AVX2) of Adler-32 commonly work.
	 */
	if (len >= 4) {
		u32 s1_sum = 0;
		u32 byte_0_sum = 0;
		u32 byte_1_sum = 0;
		u32 byte_2_sum = 0;
		u32 byte_3_sum = 0;

		do {
			s1_sum += s1;
			s1 += p[0] + p[1] + p[2] + p[3];
			byte_0_sum += p[0];
			byte_1_sum += p[1];
			byte_2_sum += p[2];
			byte_3_sum += p[3];
			p += 4;
			len -= 4;
		} while (len >= 4);
		s2 += (4 * (s1_sum + byte_0_sum)) + (3 * byte_1_sum) +
		      (2 * byte_2_sum) + byte_3_sum;
	}

	/* Process any remainder. */
	for (; len; len--, p++) {
		s1 += *p;
		s2 += s1;
	}
	*s1_p = s1;
	*s2_p = s2;
}

static u32 MAYBE_UNUSED
adler32_generic(u32 adler, const u8 *p, size_t len)
{
	u32 s1 = adler & 0xFFFF;
	u32 s2 = adler >> 16;

	while (len) {
		size_t chunk_len = MIN(len, MAX_CHUNK_LEN);

		adler32_generic_noreduce(&s1, &s2, p, chunk_len);
		p += chunk_len;
		len -= chunk_len;

		s1 %= DIVISOR;
		s2 %= DIVISOR;
	}

	return (s2 << 16) | s1;
}

/* Include architecture-specific implementation(s) if available. */
#undef DEFAULT_IMPL
#undef arch_select_adler32_func
typedef u32 (*adler32_func_t)(u32 adler, const u8 *p, size_t len);
#if defined(ARCH_ARM32) || defined(ARCH_ARM64)
#  include "arm/adler32_impl.h"
#elif defined(ARCH_X86_32) || defined(ARCH_X86_64)
#  include "x86/adler32_impl.h"
#endif

#ifndef DEFAULT_IMPL
#  define DEFAULT_IMPL adler32_generic
#endif

#ifdef arch_select_adler32_func
static u32 dispatch_adler32(u32 adler, const u8 *p, size_t len);

static volatile adler32_func_t adler32_impl = dispatch_adler32;

/* Choose the best implementation at runtime. */
static u32 dispatch_adler32(u32 adler, const u8 *p, size_t len)
{
	adler32_func_t f = arch_select_adler32_func();

	if (f == NULL)
		f = DEFAULT_IMPL;

	adler32_impl = f;
	return f(adler, p, len);
}
#else
/* The best implementation is statically known, so call it directly. */
#define adler32_impl DEFAULT_IMPL
#endif

LIBDEFLATEAPI u32
libdeflate_adler32(u32 adler, const void *buffer, size_t len)
{
	if (buffer == NULL) /* Return initial value. */
		return 1;
	return adler32_impl(adler, buffer, len);
}
