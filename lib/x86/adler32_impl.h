/*
 * x86/adler32_impl.h - x86 implementations of Adler-32 checksum algorithm
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

#include "cpu_features.h"

/* AVX2 implementation */
#undef DISPATCH_AVX2
#if !defined(DEFAULT_IMPL) &&	\
	(defined(__AVX2__) || (X86_CPU_FEATURES_ENABLED &&	\
			       COMPILER_SUPPORTS_AVX2_TARGET_INTRINSICS))
#  define FUNCNAME		adler32_avx2
#  define FUNCNAME_CHUNK	adler32_avx2_chunk
#  define IMPL_ALIGNMENT	32
#  define IMPL_SEGMENT_SIZE	32
#  define IMPL_MAX_CHUNK_SIZE	MAX_CHUNK_SIZE
#  ifdef __AVX2__
#    define ATTRIBUTES
#    define DEFAULT_IMPL	adler32_avx2
#  else
#    define ATTRIBUTES		__attribute__((target("avx2")))
#    define DISPATCH		1
#    define DISPATCH_AVX2	1
#  endif
#  include <immintrin.h>
static forceinline ATTRIBUTES void
adler32_avx2_chunk(const __m256i *p, const __m256i *const end, u32 *s1, u32 *s2)
{
	const __m256i zeroes = _mm256_setzero_si256();
	const __v32qi multipliers = (__v32qi) { 32, 31, 30, 29, 28, 27, 26, 25,
						24, 23, 22, 21, 20, 19, 18, 17,
						16, 15, 14, 13, 12, 11, 10, 9,
						8,  7,  6,  5,  4,  3,  2,  1 };
	const __v16hi ones = (__v16hi)_mm256_set1_epi16(1);
	__v8si v_s1 = (__v8si)zeroes;
	__v8si v_s1_sums = (__v8si)zeroes;
	__v8si v_s2 = (__v8si)zeroes;

	do {
		__m256i bytes = *p++;
		__v16hi sums = (__v16hi)_mm256_maddubs_epi16(
						bytes, (__m256i)multipliers);
		v_s1_sums += v_s1;
		v_s1 += (__v8si)_mm256_sad_epu8(bytes, zeroes);
		v_s2 += (__v8si)_mm256_madd_epi16((__m256i)sums, (__m256i)ones);
	} while (p != end);

	v_s1 = (__v8si)_mm256_hadd_epi32((__m256i)v_s1, zeroes);
	v_s1 = (__v8si)_mm256_hadd_epi32((__m256i)v_s1, zeroes);
	*s1 += (u32)v_s1[0] + (u32)v_s1[4];

	v_s2 += (__v8si)_mm256_slli_epi32((__m256i)v_s1_sums, 5);
	v_s2 = (__v8si)_mm256_hadd_epi32((__m256i)v_s2, zeroes);
	v_s2 = (__v8si)_mm256_hadd_epi32((__m256i)v_s2, zeroes);
	*s2 += (u32)v_s2[0] + (u32)v_s2[4];
}
#  include "../adler32_vec_template.h"
#endif /* AVX2 implementation */

/* SSE2 implementation */
#undef DISPATCH_SSE2
#if !defined(DEFAULT_IMPL) &&	\
	(defined(__SSE2__) || (X86_CPU_FEATURES_ENABLED &&	\
			       COMPILER_SUPPORTS_SSE2_TARGET_INTRINSICS))
#  define FUNCNAME		adler32_sse2
#  define FUNCNAME_CHUNK	adler32_sse2_chunk
#  define IMPL_ALIGNMENT	16
#  define IMPL_SEGMENT_SIZE	32
/*
 * The 16-bit precision byte counters must not be allowed to undergo *signed*
 * overflow, otherwise the signed multiplications at the end (_mm_madd_epi16)
 * would behave incorrectly.
 */
#  define IMPL_MAX_CHUNK_SIZE	(32 * (0x7FFF / 0xFF))
#  ifdef __SSE2__
#    define ATTRIBUTES
#    define DEFAULT_IMPL	adler32_sse2
#  else
#    define ATTRIBUTES		__attribute__((target("sse2")))
#    define DISPATCH		1
#    define DISPATCH_SSE2	1
#  endif
#  include <emmintrin.h>
static forceinline ATTRIBUTES void
adler32_sse2_chunk(const __m128i *p, const __m128i *const end, u32 *s1, u32 *s2)
{
	const __m128i zeroes = _mm_setzero_si128();

	/* s1 counters: 32-bit, sum of bytes */
	__v4si v_s1 = (__v4si)zeroes;

	/* s2 counters: 32-bit, sum of s1 values */
	__v4si v_s2 = (__v4si)zeroes;

	/*
	 * Thirty-two 16-bit counters for byte sums.  Each accumulates the bytes
	 * that eventually need to be multiplied by a number 32...1 for addition
	 * into s2.
	 */
	__v8hi v_byte_sums_a = (__v8hi)zeroes;
	__v8hi v_byte_sums_b = (__v8hi)zeroes;
	__v8hi v_byte_sums_c = (__v8hi)zeroes;
	__v8hi v_byte_sums_d = (__v8hi)zeroes;

	do {
		/* Load the next 32 bytes */
		const __m128i bytes1 = *p++;
		const __m128i bytes2 = *p++;

		/*
		 * Accumulate the previous s1 counters into the s2 counters.
		 * Logically, this really should be v_s2 += v_s1 * 32, but we
		 * can do the multiplication (or left shift) later.
		 */
		v_s2 += v_s1;

		/*
		 * s1 update: use "Packed Sum of Absolute Differences" to add
		 * the bytes horizontally with 8 bytes per sum.  Then add the
		 * sums to the s1 counters.
		 */
		v_s1 += (__v4si)_mm_sad_epu8(bytes1, zeroes);
		v_s1 += (__v4si)_mm_sad_epu8(bytes2, zeroes);

		/*
		 * Also accumulate the bytes into 32 separate counters that have
		 * 16-bit precision.
		 */
		v_byte_sums_a += (__v8hi)_mm_unpacklo_epi8(bytes1, zeroes);
		v_byte_sums_b += (__v8hi)_mm_unpackhi_epi8(bytes1, zeroes);
		v_byte_sums_c += (__v8hi)_mm_unpacklo_epi8(bytes2, zeroes);
		v_byte_sums_d += (__v8hi)_mm_unpackhi_epi8(bytes2, zeroes);

	} while (p != end);

	/* Finish calculating the s2 counters */
	v_s2 = (__v4si)_mm_slli_epi32((__m128i)v_s2, 5);
	v_s2 += (__v4si)_mm_madd_epi16((__m128i)v_byte_sums_a,
				       (__m128i)(__v8hi){ 32, 31, 30, 29, 28, 27, 26, 25 });
	v_s2 += (__v4si)_mm_madd_epi16((__m128i)v_byte_sums_b,
				       (__m128i)(__v8hi){ 24, 23, 22, 21, 20, 19, 18, 17 });
	v_s2 += (__v4si)_mm_madd_epi16((__m128i)v_byte_sums_c,
				       (__m128i)(__v8hi){ 16, 15, 14, 13, 12, 11, 10, 9 });
	v_s2 += (__v4si)_mm_madd_epi16((__m128i)v_byte_sums_d,
				       (__m128i)(__v8hi){ 8,  7,  6,  5,  4,  3,  2,  1 });

	/* Now accumulate what we computed into the real s1 and s2 */
	v_s1 += (__v4si)_mm_shuffle_epi32((__m128i)v_s1, 0x31);
	v_s1 += (__v4si)_mm_shuffle_epi32((__m128i)v_s1, 0x02);
	*s1 += _mm_cvtsi128_si32((__m128i)v_s1);

	v_s2 += (__v4si)_mm_shuffle_epi32((__m128i)v_s2, 0x31);
	v_s2 += (__v4si)_mm_shuffle_epi32((__m128i)v_s2, 0x02);
	*s2 += _mm_cvtsi128_si32((__m128i)v_s2);
}
#  include "../adler32_vec_template.h"
#endif /* SSE2 implementation */

#ifdef DISPATCH
static inline adler32_func_t
arch_select_adler32_func(void)
{
	u32 features = get_cpu_features();

#ifdef DISPATCH_AVX2
	if (features & X86_CPU_FEATURE_AVX2)
		return adler32_avx2;
#endif
#ifdef DISPATCH_SSE2
	if (features & X86_CPU_FEATURE_SSE2)
		return adler32_sse2;
#endif
	return NULL;
}
#endif /* DISPATCH */
