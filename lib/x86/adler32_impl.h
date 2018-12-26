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

/*
 * The following macros horizontally sum the s1 counters and add them to the
 * real s1, and likewise for s2.  They do this via a series of reductions, each
 * of which halves the vector length, until just one counter remains.
 *
 * The s1 reductions don't depend on the s2 reductions and vice versa, so for
 * efficiency they are interleaved.
 */

#define ADLER32_FINISH_VEC_CHUNK_128(s1, s2, v_s1, v_s2)		    \
{									    \
	__v4su s1_last = (v_s1), s2_last = (v_s2);			    \
									    \
	/* 128 => 32 bits */						    \
	s1_last += (__v4su)_mm_shuffle_epi32((__m128i)s1_last, 0x31);	    \
	s2_last += (__v4su)_mm_shuffle_epi32((__m128i)s2_last, 0x31);	    \
	s1_last += (__v4su)_mm_shuffle_epi32((__m128i)s1_last, 0x02);	    \
	s2_last += (__v4su)_mm_shuffle_epi32((__m128i)s2_last, 0x02);	    \
									    \
	*(s1) += (u32)_mm_cvtsi128_si32((__m128i)s1_last);		    \
	*(s2) += (u32)_mm_cvtsi128_si32((__m128i)s2_last);		    \
}

#define ADLER32_FINISH_VEC_CHUNK_256(s1, s2, v_s1, v_s2)		    \
{									    \
	__v4su s1_128bit, s2_128bit;					    \
									    \
	/* 256 => 128 bits */						    \
	s1_128bit = (__v4su)_mm256_extracti128_si256((__m256i)(v_s1), 0) +  \
		    (__v4su)_mm256_extracti128_si256((__m256i)(v_s1), 1);   \
	s2_128bit = (__v4su)_mm256_extracti128_si256((__m256i)(v_s2), 0) +  \
		    (__v4su)_mm256_extracti128_si256((__m256i)(v_s2), 1);   \
									    \
	ADLER32_FINISH_VEC_CHUNK_128((s1), (s2), s1_128bit, s2_128bit);	    \
}

#define ADLER32_FINISH_VEC_CHUNK_512(s1, s2, v_s1, v_s2)		    \
{									    \
	__v8su s1_256bit, s2_256bit;					    \
									    \
	/* 512 => 256 bits */						    \
	s1_256bit = (__v8su)_mm512_extracti64x4_epi64((__m512i)(v_s1), 0) + \
		    (__v8su)_mm512_extracti64x4_epi64((__m512i)(v_s1), 1);  \
	s2_256bit = (__v8su)_mm512_extracti64x4_epi64((__m512i)(v_s2), 0) + \
		    (__v8su)_mm512_extracti64x4_epi64((__m512i)(v_s2), 1);  \
									    \
	ADLER32_FINISH_VEC_CHUNK_256((s1), (s2), s1_256bit, s2_256bit);	    \
}

/* AVX-512BW implementation: like the AVX2 one, but does 64 bytes at a time */
#undef DISPATCH_AVX512BW
#if !defined(DEFAULT_IMPL) &&						\
    /*
     * clang before v3.9 is missing some AVX-512BW intrinsics including
     * _mm512_sad_epu8(), a.k.a. __builtin_ia32_psadbw512.  So just make using
     * AVX-512BW, even when __AVX512BW__ is defined, conditional on
     * COMPILER_SUPPORTS_AVX512BW_TARGET where we check for that builtin.
     */									\
    COMPILER_SUPPORTS_AVX512BW_TARGET &&				\
    (defined(__AVX512BW__) || (X86_CPU_FEATURES_ENABLED &&		\
			       COMPILER_SUPPORTS_AVX512BW_TARGET_INTRINSICS))
#  define FUNCNAME		adler32_avx512bw
#  define FUNCNAME_CHUNK	adler32_avx512bw_chunk
#  define IMPL_ALIGNMENT	64
#  define IMPL_SEGMENT_SIZE	64
#  define IMPL_MAX_CHUNK_SIZE	MAX_CHUNK_SIZE
#  ifdef __AVX512BW__
#    define ATTRIBUTES
#    define DEFAULT_IMPL	adler32_avx512bw
#  else
#    define ATTRIBUTES		__attribute__((target("avx512bw")))
#    define DISPATCH		1
#    define DISPATCH_AVX512BW	1
#  endif
#  include <immintrin.h>
static forceinline ATTRIBUTES void
adler32_avx512bw_chunk(const __m512i *p, const __m512i *const end,
		       u32 *s1, u32 *s2)
{
	const __m512i zeroes = _mm512_setzero_si512();
	__v16si s1_a = (__v16si)zeroes;
	__v16si s1_b = (__v16si)zeroes;
	__v16si s1_c = (__v16si)zeroes;
	__v16si s1_d = (__v16si)zeroes;
	__v16si s2_a = (__v16si)zeroes;
	__v16si s2_b = (__v16si)zeroes;
	__v16si s2_c = (__v16si)zeroes;
	__v16si s2_d = (__v16si)zeroes;

	do {
		const __m128i *p128 = (const __m128i *)p;
		__v16si bytes_a = (__v16si)_mm512_cvtepu8_epi32(p128[0]);
		__v16si bytes_b = (__v16si)_mm512_cvtepu8_epi32(p128[1]);
		__v16si bytes_c = (__v16si)_mm512_cvtepu8_epi32(p128[2]);
		__v16si bytes_d = (__v16si)_mm512_cvtepu8_epi32(p128[3]);

		s2_a += s1_a;
		s2_b += s1_b;
		s2_c += s1_c;
		s2_d += s1_d;

		s1_a += bytes_a;
		s1_b += bytes_b;
		s1_c += bytes_c;
		s1_d += bytes_d;
	} while (++p != end);

	s2_a += s2_c;
	s2_b += s2_d;
	s2_a += s2_b;

	s2_a = (__v16si)_mm512_slli_epi32((__m512i)s2_a, 5);

	s2_a += s1_a * (__v16si){64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49};
	s2_a += s1_b * (__v16si){48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33};
	s2_a += s1_c * (__v16si){32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17};
	s2_a += s1_d * (__v16si){16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1};

	s1_a += s1_c;
	s1_b += s1_d;
	s1_a += s1_b;

	/* Add the counters to the real s1 and s2 */
	ADLER32_FINISH_VEC_CHUNK_512(s1, s2, s1_a, s2_a);
}
#  include "../adler32_vec_template.h"
#endif /* AVX-512BW implementation */

/* AVX2 implementation: like the AVX-512BW one, but does 32 bytes at a time */
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
	__v8su s1_a = (__v8su)zeroes;
	__v8su s1_b = (__v8su)zeroes;
	__v8su s1_c = (__v8su)zeroes;
	__v8su s1_d = (__v8su)zeroes;
	__v8su s2_a = (__v8su)zeroes;
	__v8su s2_b = (__v8su)zeroes;
	__v8su s2_c = (__v8su)zeroes;
	__v8su s2_d = (__v8su)zeroes;
	const __m256i shuf_a = (__m256i)(__v32qi){
		0, -1, -1, -1, 4, -1, -1, -1,  8, -1, -1, -1, 12, -1, -1, -1,
		0, -1, -1, -1, 4, -1, -1, -1,  8, -1, -1, -1, 12, -1, -1, -1
	};
	const __m256i shuf_b = (__m256i)(__v32qi){
		1, -1, -1, -1, 5, -1, -1, -1,  9, -1, -1, -1, 13, -1, -1, -1,
		1, -1, -1, -1, 5, -1, -1, -1,  9, -1, -1, -1, 13, -1, -1, -1
	};
	const __m256i shuf_c = (__m256i)(__v32qi){
		2, -1, -1, -1, 6, -1, -1, -1, 10, -1, -1, -1, 14, -1, -1, -1,
		2, -1, -1, -1, 6, -1, -1, -1, 10, -1, -1, -1, 14, -1, -1, -1
	};
	const __m256i shuf_d = (__m256i)(__v32qi){
		3, -1, -1, -1, 7, -1, -1, -1, 11, -1, -1, -1, 15, -1, -1, -1,
		3, -1, -1, -1, 7, -1, -1, -1, 11, -1, -1, -1, 15, -1, -1, -1
	};

	do {
		__m256i bytes = *p++;
		__v8su bytes_a = (__v8su)_mm256_shuffle_epi8(bytes, shuf_a);
		__v8su bytes_b = (__v8su)_mm256_shuffle_epi8(bytes, shuf_b);
		__v8su bytes_c = (__v8su)_mm256_shuffle_epi8(bytes, shuf_c);
		__v8su bytes_d = (__v8su)_mm256_shuffle_epi8(bytes, shuf_d);

		s2_a += s1_a;
		s2_b += s1_b;
		s2_c += s1_c;
		s2_d += s1_d;

		s1_a += bytes_a;
		s1_b += bytes_b;
		s1_c += bytes_c;
		s1_d += bytes_d;

	} while (p != end);

	s2_a += s2_c;
	s2_b += s2_d;
	s2_a += s2_b;

	s2_a = (__v8su)_mm256_slli_epi32((__m256i)s2_a, 5);

	s2_a += s1_a * (__v8su){ 32, 28, 24, 20, 16, 12, 8, 4 };
	s2_a += s1_b * (__v8su){ 31, 27, 23, 19, 15, 11, 7, 3 };
	s2_a += s1_c * (__v8su){ 30, 26, 22, 18, 14, 10, 6, 2 };
	s2_a += s1_d * (__v8su){ 29, 25, 21, 17, 13,  9, 5, 1 };

	s1_a += s1_c;
	s1_b += s1_d;
	s1_a += s1_b;

	/* Add the counters to the real s1 and s2 */
	ADLER32_FINISH_VEC_CHUNK_256(s1, s2, s1_a, s2_a);
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
	__v4su v_s1 = (__v4su)zeroes;

	/* s2 counters: 32-bit, sum of s1 values */
	__v4su v_s2 = (__v4su)zeroes;

	/*
	 * Thirty-two 16-bit counters for byte sums.  Each accumulates the bytes
	 * that eventually need to be multiplied by a number 32...1 for addition
	 * into s2.
	 */
	__v8hu v_byte_sums_a = (__v8hu)zeroes;
	__v8hu v_byte_sums_b = (__v8hu)zeroes;
	__v8hu v_byte_sums_c = (__v8hu)zeroes;
	__v8hu v_byte_sums_d = (__v8hu)zeroes;

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
		v_s1 += (__v4su)_mm_sad_epu8(bytes1, zeroes);
		v_s1 += (__v4su)_mm_sad_epu8(bytes2, zeroes);

		/*
		 * Also accumulate the bytes into 32 separate counters that have
		 * 16-bit precision.
		 */
		v_byte_sums_a += (__v8hu)_mm_unpacklo_epi8(bytes1, zeroes);
		v_byte_sums_b += (__v8hu)_mm_unpackhi_epi8(bytes1, zeroes);
		v_byte_sums_c += (__v8hu)_mm_unpacklo_epi8(bytes2, zeroes);
		v_byte_sums_d += (__v8hu)_mm_unpackhi_epi8(bytes2, zeroes);

	} while (p != end);

	/* Finish calculating the s2 counters */
	v_s2 = (__v4su)_mm_slli_epi32((__m128i)v_s2, 5);
	v_s2 += (__v4su)_mm_madd_epi16((__m128i)v_byte_sums_a,
				       (__m128i)(__v8hu){ 32, 31, 30, 29, 28, 27, 26, 25 });
	v_s2 += (__v4su)_mm_madd_epi16((__m128i)v_byte_sums_b,
				       (__m128i)(__v8hu){ 24, 23, 22, 21, 20, 19, 18, 17 });
	v_s2 += (__v4su)_mm_madd_epi16((__m128i)v_byte_sums_c,
				       (__m128i)(__v8hu){ 16, 15, 14, 13, 12, 11, 10, 9 });
	v_s2 += (__v4su)_mm_madd_epi16((__m128i)v_byte_sums_d,
				       (__m128i)(__v8hu){ 8,  7,  6,  5,  4,  3,  2,  1 });

	/* Add the counters to the real s1 and s2 */
	ADLER32_FINISH_VEC_CHUNK_128(s1, s2, v_s1, v_s2);
}
#  include "../adler32_vec_template.h"
#endif /* SSE2 implementation */

#ifdef DISPATCH
static inline adler32_func_t
arch_select_adler32_func(void)
{
	u32 features = get_cpu_features();

#ifdef DISPATCH_AVX512BW
	if (features & X86_CPU_FEATURE_AVX512BW)
		return adler32_avx512bw;
#endif
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
