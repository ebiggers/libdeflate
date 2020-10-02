/*
 * x86/matchfinder_impl.h - x86 implementations of matchfinder functions
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

#undef DISPATCH_MATCHFINDER_AVX2
#if (!defined(DEFAULT_MATCHFINDER_INIT) || !defined(DEFAULT_MATCHFINDER_REBASE)) && \
    (defined(__AVX2__) || (X86_CPU_FEATURES_ENABLED &&	\
		COMPILER_SUPPORTS_AVX2_TARGET_INTRINSICS))
#  if MATCHFINDER_ALIGNMENT < 32
#    undef MATCHFINDER_ALIGNMENT
#    define MATCHFINDER_ALIGNMENT 32
#  endif
#  ifdef __AVX2__
#    define ATTRIBUTES
#    define DEFAULT_MATCHFINDER_INIT	matchfinder_init_avx2
#    define DEFAULT_MATCHFINDER_REBASE	matchfinder_rebase_avx2
#  else
#    define ATTRIBUTES					__attribute__((target("avx2")))
#    define DISPATCH_MATCHFINDER_AVX2	1
#    define DISPATCH					1
#  endif
#  include <immintrin.h>
static forceinline ATTRIBUTES bool
matchfinder_init_avx2(mf_pos_t *data, size_t size)
{
	__m256i v, *p;
	size_t n;

	if (size % (sizeof(__m256i) * 4) != 0)
		return false;

	STATIC_ASSERT(sizeof(mf_pos_t) == 2);
	v = _mm256_set1_epi16(MATCHFINDER_INITVAL);
	p = (__m256i *)data;
	n = size / (sizeof(__m256i) * 4);
	do {
		p[0] = v;
		p[1] = v;
		p[2] = v;
		p[3] = v;
		p += 4;
	} while (--n);
	return true;
}

static forceinline bool ATTRIBUTES
matchfinder_rebase_avx2(mf_pos_t *data, size_t size)
{
	__m256i v, *p;
	size_t n;

	if (size % (sizeof(__m256i) * 4) != 0)
		return false;

	STATIC_ASSERT(sizeof(mf_pos_t) == 2);
	v = _mm256_set1_epi16((u16)-MATCHFINDER_WINDOW_SIZE);
	p = (__m256i *)data;
	n = size / (sizeof(__m256i) * 4);
	do {
		/* PADDSW: Add Packed Signed Integers With Signed Saturation  */
		p[0] = _mm256_adds_epi16(p[0], v);
		p[1] = _mm256_adds_epi16(p[1], v);
		p[2] = _mm256_adds_epi16(p[2], v);
		p[3] = _mm256_adds_epi16(p[3], v);
		p += 4;
	} while (--n);
	return true;
}
#endif /* AVX-2 implementation */

#undef DISPATCH_MATCHFINDER_SSE2
#if (!defined(DEFAULT_MATCHFINDER_INIT) || !defined(DEFAULT_MATCHFINDER_REBASE)) && \
	(defined(__SSE2__) || (X86_CPU_FEATURES_ENABLED &&	\
			       COMPILER_SUPPORTS_SSE2_TARGET_INTRINSICS))
#  if MATCHFINDER_ALIGNMENT < 16
#    undef MATCHFINDER_ALIGNMENT
#    define MATCHFINDER_ALIGNMENT 16
#  endif
#  ifdef __AVX2__
#    define ATTRIBUTES
#    define DEFAULT_MATCHFINDER_INIT	matchfinder_init_sse2
#    define DEFAULT_MATCHFINDER_REBASE	matchfinder_rebase_sse2
#  else
#    define ATTRIBUTES					__attribute__((target("avx2")))
#    define DISPATCH_MATCHFINDER_SSE2	1
#    define DISPATCH					1
#  endif
#  define DISPATCH_MATCHFINDER_SSE2	1
#  define DISPATCH					1
#  include <emmintrin.h>
static forceinline ATTRIBUTES bool
matchfinder_init_sse2(mf_pos_t *data, size_t size)
{
	__m128i v, *p;
	size_t n;

	if (size % (sizeof(__m128i) * 4) != 0)
		return false;

	STATIC_ASSERT(sizeof(mf_pos_t) == 2);
	v = _mm_set1_epi16(MATCHFINDER_INITVAL);
	p = (__m128i *)data;
	n = size / (sizeof(__m128i) * 4);
	do {
		p[0] = v;
		p[1] = v;
		p[2] = v;
		p[3] = v;
		p += 4;
	} while (--n);
	return true;
}

static forceinline ATTRIBUTES bool
matchfinder_rebase_sse2(mf_pos_t *data, size_t size)
{
	__m128i v, *p;
	size_t n;

	if (size % (sizeof(__m128i) * 4) != 0)
		return false;

	STATIC_ASSERT(sizeof(mf_pos_t) == 2);
	v = _mm_set1_epi16((u16)-MATCHFINDER_WINDOW_SIZE);
	p = (__m128i *)data;
	n = size / (sizeof(__m128i) * 4);
	do {
		/* PADDSW: Add Packed Signed Integers With Signed Saturation  */
		p[0] = _mm_adds_epi16(p[0], v);
		p[1] = _mm_adds_epi16(p[1], v);
		p[2] = _mm_adds_epi16(p[2], v);
		p[3] = _mm_adds_epi16(p[3], v);
		p += 4;
	} while (--n);
	return true;
}
#endif /* __SSE2__ */

#ifdef DISPATCH
static inline matchfinder_func_t
arch_select_matchfinder_init(void)
{
	u32 features = get_cpu_features();

#ifdef DISPATCH_MATCHFINDER_AVX2
	if (features & X86_CPU_FEATURE_AVX2)
		return matchfinder_init_avx2;
#endif
#ifdef DISPATCH_MATCHFINDER_SSE2
	if (features & X86_CPU_FEATURE_SSE2)
		return matchfinder_init_sse2;
#endif
	return NULL;
}

static inline matchfinder_func_t
arch_select_matchfinder_rebase(void)
{
	u32 features = get_cpu_features();

#ifdef DISPATCH_MATCHFINDER_AVX2
	if (features & X86_CPU_FEATURE_AVX2)
		return matchfinder_rebase_avx2;
#endif
#ifdef DISPATCH_MATCHFINDER_SSE2
	if (features & X86_CPU_FEATURE_SSE2)
		return matchfinder_rebase_sse2;
#endif
	return NULL;
}
#endif /* DISPATCH */
