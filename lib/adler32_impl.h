/*
 * adler32_impl.h
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

/*
 * This file contains a template for vectorized Adler-32 implementations.
 *
 * The inner loop between reductions modulo 65521 of an unvectorized Adler-32
 * implementation looks something like this:
 *
 *	do {
 * 		s1 += *p;
 * 		s2 += s1;
 *	} while (++p != chunk_end);
 *
 * For vectorized calculation of s1, we only need to sum the input bytes.  They
 * can be accumulated into multiple counters which are eventually summed
 * together.
 *
 * For vectorized calculation of s2, the basic idea is that for each iteration
 * that processes N bytes, we can perform the following vectorizable
 * calculation:
 *
 *	s2 += N*byte_1 + (N-1)*byte_2 + (N-2)*byte_3 + ... + 1*byte_N
 *
 * Or, equivalently, we can sum the byte_1...byte_N for each iteration into N
 * separate counters, then do the multiplications by N...1 just once at the end
 * rather than once per iteration.
 *
 * Also, we must account for how previous bytes will affect s2 by doing the
 * following at beginning of each iteration:
 *
 *	s2 += s1 * N
 *
 * Furthermore, like s1, "s2" can actually be multiple counters which are
 * eventually summed together.
 */

static u32 ATTRIBUTES
FUNCNAME(u32 adler, const void *buffer, size_t size)
{
	u32 s1 = adler & 0xFFFF;
	u32 s2 = adler >> 16;
	const u8 *p = buffer;
	const u8 * const end = p + size;
	const u8 *vend;

	/* Process a byte at a time until the required alignment is reached. */
	if (p != end && (uintptr_t)p % ALIGNMENT_REQUIRED) {
		do {
			s1 += *p++;
			s2 += s1;
		} while (p != end && (uintptr_t)p % ALIGNMENT_REQUIRED);
		s1 %= DIVISOR;
		s2 %= DIVISOR;
	}

	/*
	 * Process "chunks" of bytes using vector instructions.  Chunk sizes are
	 * limited to MAX_BYTES_PER_CHUNK, which guarantees that s1 and s2 never
	 * overflow before being reduced modulo DIVISOR.  For vector processing,
	 * chunks size are also made evenly divisible by BYTES_PER_ITERATION.
	 */
	STATIC_ASSERT(BYTES_PER_ITERATION % ALIGNMENT_REQUIRED == 0);
	vend = end - ((size_t)(end - p) % BYTES_PER_ITERATION);
	while (p != vend) {
		size_t chunk_size;
		const u8 *chunk_end;

		chunk_size = MIN((size_t)(vend - p), MAX_BYTES_PER_CHUNK);
	#if TARGET == TARGET_SSE2
		/* SSE2: the 16-bit precision byte counters must not undergo
		 * *signed* overflow, otherwise the signed multiplication at the
		 * end will not behave as desired. */
		chunk_size = MIN(chunk_size, BYTES_PER_ITERATION * (0x7FFF / 0xFF));
	#elif TARGET == TARGET_NEON
		/* NEON: the 16-bit precision counters must not undergo
		 * *unsigned* overflow. */
		chunk_size = MIN(chunk_size, BYTES_PER_ITERATION * (0xFFFF / 0xFF));
	#endif
		chunk_size -= chunk_size % BYTES_PER_ITERATION;

		chunk_end = p + chunk_size;

		s2 += s1 * chunk_size;
		{
	#if TARGET == TARGET_AVX2
		/* AVX2 implementation */
		const __m256i zeroes = _mm256_setzero_si256();
		const __v32qi multipliers = (__v32qi) { 32, 31, 30, 29, 28, 27, 26, 25,
							24, 23, 22, 21, 20, 19, 18, 17,
							16, 15, 14, 13, 12, 11, 10, 9,
							8,  7,  6,  5,  4,  3,  2,  1 };
		const __v16hi ones = (__v16hi)_mm256_set1_epi16(1);
		__v8si v_s1 = (__v8si)zeroes;
		__v8si v_s1_sums = (__v8si)zeroes;
		__v8si v_s2 = (__v8si)zeroes;
		STATIC_ASSERT(ALIGNMENT_REQUIRED == 32 && BYTES_PER_ITERATION == 32);
		do {
			__m256i bytes = *(const __m256i *)p;
			__v16hi sums = (__v16hi)_mm256_maddubs_epi16(
							bytes, (__m256i)multipliers);
			v_s1_sums += v_s1;
			v_s1 += (__v8si)_mm256_sad_epu8(bytes, zeroes);
			v_s2 += (__v8si)_mm256_madd_epi16((__m256i)sums, (__m256i)ones);
		} while ((p += BYTES_PER_ITERATION) != chunk_end);

		v_s1 = (__v8si)_mm256_hadd_epi32((__m256i)v_s1, zeroes);
		v_s1 = (__v8si)_mm256_hadd_epi32((__m256i)v_s1, zeroes);
		s1 += v_s1[0] + v_s1[4];

		v_s2 += (__v8si)_mm256_slli_epi32((__m256i)v_s1_sums, 5);
		v_s2 = (__v8si)_mm256_hadd_epi32((__m256i)v_s2, zeroes);
		v_s2 = (__v8si)_mm256_hadd_epi32((__m256i)v_s2, zeroes);
		s2 += v_s2[0] + v_s2[4];

	#elif TARGET == TARGET_SSE2
		/* SSE2 implementation */
		const __m128i zeroes = _mm_setzero_si128();

		/* s1 counters: 32-bit, sum of bytes */
		__v4si v_s1 = (__v4si)zeroes;

		/* s2 counters: 32-bit, sum of s1 values */
		__v4si v_s2 = (__v4si)zeroes;

		/*
		 * Thirty-two 16-bit counters for byte sums.  Each accumulates
		 * the bytes that eventually need to be multiplied by a number
		 * 32...1 for addition into s2.
		 */
		__v8hi v_byte_sums_a = (__v8hi)zeroes;
		__v8hi v_byte_sums_b = (__v8hi)zeroes;
		__v8hi v_byte_sums_c = (__v8hi)zeroes;
		__v8hi v_byte_sums_d = (__v8hi)zeroes;

		STATIC_ASSERT(ALIGNMENT_REQUIRED == 16 && BYTES_PER_ITERATION == 32);
		do {
			/* Load the next 32 bytes. */
			const __m128i bytes1 = *(const __m128i *)p;
			const __m128i bytes2 = *(const __m128i *)(p + 16);

			/*
			 * Accumulate the previous s1 counters into the s2
			 * counters.  Logically, this really should be
			 * v_s2 += v_s1 * BYTES_PER_ITERATION, but we can do the
			 * multiplication (or left shift) later.
			 */
			v_s2 += v_s1;

			/*
			 * s1 update: use "Packed Sum of Absolute Differences"
			 * to add the bytes horizontally with 8 bytes per sum.
			 * Then add the sums to the s1 counters.
			 */
			v_s1 += (__v4si)_mm_sad_epu8(bytes1, zeroes);
			v_s1 += (__v4si)_mm_sad_epu8(bytes2, zeroes);

			/*
			 * Also accumulate the bytes into 32 separate counters
			 * that have 16-bit precision.
			 */
			v_byte_sums_a += (__v8hi)_mm_unpacklo_epi8(bytes1, zeroes);
			v_byte_sums_b += (__v8hi)_mm_unpackhi_epi8(bytes1, zeroes);
			v_byte_sums_c += (__v8hi)_mm_unpacklo_epi8(bytes2, zeroes);
			v_byte_sums_d += (__v8hi)_mm_unpackhi_epi8(bytes2, zeroes);

		} while ((p += BYTES_PER_ITERATION) != chunk_end);

		/* Finish calculating the s2 counters. */
		v_s2 = (__v4si)_mm_slli_epi32((__m128i)v_s2, 5);
		v_s2 += (__v4si)_mm_madd_epi16((__m128i)v_byte_sums_a,
					       (__m128i)(__v8hi){ 32, 31, 30, 29, 28, 27, 26, 25 });
		v_s2 += (__v4si)_mm_madd_epi16((__m128i)v_byte_sums_b,
					       (__m128i)(__v8hi){ 24, 23, 22, 21, 20, 19, 18, 17 });
		v_s2 += (__v4si)_mm_madd_epi16((__m128i)v_byte_sums_c,
					       (__m128i)(__v8hi){ 16, 15, 14, 13, 12, 11, 10, 9 });
		v_s2 += (__v4si)_mm_madd_epi16((__m128i)v_byte_sums_d,
					       (__m128i)(__v8hi){ 8,  7,  6,  5,  4,  3,  2,  1 });

		/* Now accumulate what we computed into the real s1 and s2. */
		v_s1 += (__v4si)_mm_shuffle_epi32((__m128i)v_s1, 0x31);
		v_s1 += (__v4si)_mm_shuffle_epi32((__m128i)v_s1, 0x02);
		s1 += _mm_cvtsi128_si32((__m128i)v_s1);

		v_s2 += (__v4si)_mm_shuffle_epi32((__m128i)v_s2, 0x31);
		v_s2 += (__v4si)_mm_shuffle_epi32((__m128i)v_s2, 0x02);
		s2 += _mm_cvtsi128_si32((__m128i)v_s2);

	#elif TARGET == TARGET_NEON
		/* ARM NEON (Advanced SIMD) implementation */
		uint32x4_t v_s1 = (uint32x4_t) { 0, 0, 0, 0 };
		uint32x4_t v_s2 = (uint32x4_t) { 0, 0, 0, 0 };
		uint16x8_t v_byte_sums_a = (uint16x8_t) { 0, 0, 0, 0, 0, 0, 0, 0 };
		uint16x8_t v_byte_sums_b = (uint16x8_t) { 0, 0, 0, 0, 0, 0, 0, 0 };
		uint16x8_t v_byte_sums_c = (uint16x8_t) { 0, 0, 0, 0, 0, 0, 0, 0 };
		uint16x8_t v_byte_sums_d = (uint16x8_t) { 0, 0, 0, 0, 0, 0, 0, 0 };

		STATIC_ASSERT(ALIGNMENT_REQUIRED == 16 && BYTES_PER_ITERATION == 32);
		do {
			const uint8x16_t bytes1 = *(const uint8x16_t *)p;
			const uint8x16_t bytes2 = *(const uint8x16_t *)(p + 16);
			uint16x8_t tmp;

			v_s2 += v_s1;

			tmp = vpaddlq_u8(bytes1);
			tmp = vpadalq_u8(tmp, bytes2);
			v_s1 = vpadalq_u16(v_s1, tmp);

			v_byte_sums_a = vaddw_u8(v_byte_sums_a, vget_low_u8(bytes1));
			v_byte_sums_b = vaddw_u8(v_byte_sums_b, vget_high_u8(bytes1));
			v_byte_sums_c = vaddw_u8(v_byte_sums_c, vget_low_u8(bytes2));
			v_byte_sums_d = vaddw_u8(v_byte_sums_d, vget_high_u8(bytes2));

		} while ((p += BYTES_PER_ITERATION) != chunk_end);

		v_s2 = vqshlq_n_u32(v_s2, 5);
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_a),  (uint16x4_t) { 32, 31, 30, 29 });
		v_s2 = vmlal_u16(v_s2, vget_high_u16(v_byte_sums_a), (uint16x4_t) { 28, 27, 26, 25 });
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_b),  (uint16x4_t) { 24, 23, 22, 21 });
		v_s2 = vmlal_u16(v_s2, vget_high_u16(v_byte_sums_b), (uint16x4_t) { 20, 19, 18, 17 });
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_c),  (uint16x4_t) { 16, 15, 14, 13 });
		v_s2 = vmlal_u16(v_s2, vget_high_u16(v_byte_sums_c), (uint16x4_t) { 12, 11, 10,  9 });
		v_s2 = vmlal_u16(v_s2, vget_low_u16 (v_byte_sums_d), (uint16x4_t) {  8,  7,  6,  5 });
		v_s2 = vmlal_u16(v_s2, vget_high_u16(v_byte_sums_d), (uint16x4_t) {  4,  3,  2,  1 });

		s1 += v_s1[0] + v_s1[1] + v_s1[2] + v_s1[3];
		s2 += v_s2[0] + v_s2[1] + v_s2[2] + v_s2[3];
	#else
	#  error "BUG: unknown target"
	#endif
		}

		s1 %= DIVISOR;
		s2 %= DIVISOR;
	}

	/* Process any remaining bytes. */
	if (p != end) {
		do {
			s1 += *p++;
			s2 += s1;
		} while (p != end);
		s1 %= DIVISOR;
		s2 %= DIVISOR;
	}

	return (s2 << 16) | s1;
}

#undef FUNCNAME
#undef TARGET
#undef ALIGNMENT_REQUIRED
#undef BYTES_PER_ITERATION
#undef ATTRIBUTES
