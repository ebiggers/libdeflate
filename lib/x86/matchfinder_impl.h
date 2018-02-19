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

#ifdef __AVX2__
#  if MATCHFINDER_ALIGNMENT < 32
#    undef MATCHFINDER_ALIGNMENT
#    define MATCHFINDER_ALIGNMENT 32
#  endif
#  include <immintrin.h>
static forceinline bool
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

static forceinline bool
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
#endif /* __AVX2__ */

#ifdef __SSE2__
#  if MATCHFINDER_ALIGNMENT < 16
#    undef MATCHFINDER_ALIGNMENT
#    define MATCHFINDER_ALIGNMENT 16
#  endif
#  include <emmintrin.h>
static forceinline bool
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

static forceinline bool
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

#undef arch_matchfinder_init
static forceinline bool
arch_matchfinder_init(mf_pos_t *data, size_t size)
{
#ifdef __AVX2__
	if (matchfinder_init_avx2(data, size))
		return true;
#endif
#ifdef __SSE2__
	if (matchfinder_init_sse2(data, size))
		return true;
#endif
	return false;
}

#undef arch_matchfinder_rebase
static forceinline bool
arch_matchfinder_rebase(mf_pos_t *data, size_t size)
{
#ifdef __AVX2__
	if (matchfinder_rebase_avx2(data, size))
		return true;
#endif
#ifdef __SSE2__
	if (matchfinder_rebase_sse2(data, size))
		return true;
#endif
	return false;
}
