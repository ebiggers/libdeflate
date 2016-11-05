/*
 * adler32.c - Adler-32 checksum algorithm
 *
 * Originally public domain; changes after 2016-09-07 are copyrighted.
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

#include "x86_cpu_features.h"

#include "libdeflate.h"

/* The Adler-32 divisor, or "base", value. */
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

/* Select the implementations to compile in. */

#define NEED_GENERIC_IMPL 1 /* include generic impl unless overridden */

/* Include the SSE2 implementation? */
#define NEED_SSE2_IMPL 0
#ifdef __SSE2__
#  include <emmintrin.h>
#  undef NEED_SSE2_IMPL
#  define NEED_SSE2_IMPL 1
#  undef NEED_GENERIC_IMPL
#  define NEED_GENERIC_IMPL 0 /* generic impl not needed */
#endif

/* Include the AVX2 implementation? */
#define NEED_AVX2_IMPL 0
#if defined(__AVX2__) || \
	(X86_CPU_FEATURES_ENABLED && COMPILER_SUPPORTS_AVX2_TARGET && \
	 COMPILER_SUPPORTS_TARGET_INTRINSICS)
#  include <immintrin.h>
#  undef NEED_AVX2_IMPL
#  define NEED_AVX2_IMPL 1
#  ifdef __AVX2__ /* compiling for AVX2, i.e. can we assume it's there? */
#    undef NEED_GENERIC_IMPL
#    define NEED_GENERIC_IMPL 0 /* generic impl not needed */
#    undef NEED_SSE2_IMPL
#    define NEED_SSE2_IMPL 0 /* SSE2 impl not needed */
#  endif /* otherwise, we can build an AVX2 version, but we won't know whether
	    we can use it until runtime */
#endif

/* Include the NEON implementation? */
#define NEED_NEON_IMPL 0
#ifdef __ARM_NEON
#  include <arm_neon.h>
#  undef NEED_NEON_IMPL
#  define NEED_NEON_IMPL 1
#  undef NEED_GENERIC_IMPL
#  define NEED_GENERIC_IMPL 0 /* generic impl not needed */
#endif

#define NUM_IMPLS (NEED_GENERIC_IMPL + NEED_SSE2_IMPL + NEED_AVX2_IMPL + \
		   NEED_NEON_IMPL)

/* Define the generic implementation if needed. */
#if NEED_GENERIC_IMPL
static u32 adler32_generic(u32 adler, const void *buffer, size_t size)
{
	u32 s1 = adler & 0xFFFF;
	u32 s2 = adler >> 16;
	const u8 *p = buffer;
	const u8 * const end = p + size;

	while (p != end) {
		size_t chunk_size = MIN(end - p, MAX_BYTES_PER_CHUNK);
		const u8 *chunk_end = p + chunk_size;
		size_t num_unrolled_iterations = chunk_size / 4;

		while (num_unrolled_iterations--) {
			s1 += *p++;
			s2 += s1;
			s1 += *p++;
			s2 += s1;
			s1 += *p++;
			s2 += s1;
			s1 += *p++;
			s2 += s1;
		}
		while (p != chunk_end) {
			s1 += *p++;
			s2 += s1;
		}
		s1 %= DIVISOR;
		s2 %= DIVISOR;
	}

	return (s2 << 16) | s1;
}
#define DEFAULT_IMPL adler32_generic
#endif /* NEED_GENERIC_IMPL */

#define TARGET_SSE2 100
#define TARGET_AVX2 200
#define TARGET_NEON 300

/* Define the SSE2 implementation if needed. */
#if NEED_SSE2_IMPL
#  define FUNCNAME		adler32_sse2
#  define TARGET		TARGET_SSE2
#  define ALIGNMENT_REQUIRED	16
#  define BYTES_PER_ITERATION	32
#  define ATTRIBUTES
#  define DEFAULT_IMPL		adler32_sse2
#  include "adler32_impl.h"
#endif

/* Define the AVX2 implementation if needed. */
#if NEED_AVX2_IMPL
#  define FUNCNAME		adler32_avx2
#  define TARGET		TARGET_AVX2
#  define ALIGNMENT_REQUIRED	32
#  define BYTES_PER_ITERATION	32
#  ifdef __AVX2__
#    define ATTRIBUTES
#    define DEFAULT_IMPL	adler32_avx2
#  else
#    define ATTRIBUTES		__attribute__((target("avx2")))
#  endif
#  include "adler32_impl.h"
#endif

/* Define the NEON implementation if needed. */
#if NEED_NEON_IMPL
#  define FUNCNAME		adler32_neon
#  define TARGET		TARGET_NEON
#  define ALIGNMENT_REQUIRED	16
#  define BYTES_PER_ITERATION	32
#  define ATTRIBUTES
#  define DEFAULT_IMPL		adler32_neon
#  include "adler32_impl.h"
#endif

typedef u32 (*adler32_func_t)(u32, const void *, size_t);

/*
 * If multiple implementations are available, then dispatch among them based on
 * CPU features at runtime.  Otherwise just call the single one directly.
 */
#if NUM_IMPLS == 1
#  define adler32_impl DEFAULT_IMPL
#else
static u32 dispatch(u32, const void *, size_t);

static adler32_func_t adler32_impl = dispatch;

static u32 dispatch(u32 adler, const void *buffer, size_t size)
{
	adler32_func_t f = DEFAULT_IMPL;
#if NEED_AVX2_IMPL && !defined(__AVX2__)
	if (x86_have_cpu_features(X86_CPU_FEATURE_AVX2))
		f = adler32_avx2;
#endif
	adler32_impl = f;
	return adler32_impl(adler, buffer, size);
}
#endif /* NUM_IMPLS != 1 */

LIBDEFLATEAPI u32
libdeflate_adler32(u32 adler, const void *buffer, size_t size)
{
	if (buffer == NULL) /* return initial value */
		return 1;
	return adler32_impl(adler, buffer, size);
}
