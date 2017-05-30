/*
 * matchfinder_avx2.h - matchfinding routines optimized for Intel AVX2 (Advanced
 * Vector Extensions)
 */

#include <immintrin.h>

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
