/*
 * adler32_impl.h
 *
 * Written in 2016 by Eric Biggers <ebiggers3@gmail.com>
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

/* Template for vectorized Adler-32 implementations */
static u32 ATTRIBUTES
FUNCNAME(const void *buffer, size_t size)
{
	u32 s1 = 1;
	u32 s2 = 0;
	const u8 *p = buffer;
	const u8 * const end = p + size;
	const uintptr_t endv = (uintptr_t)end & ~31;

	/* Proceed until the buffer is 32-byte aligned. */
	if (p != end && ((uintptr_t)p & 31)) {
		do {
			s1 += *p++;
			s2 += s1;
		} while (p != end && ((uintptr_t)p & 31));
		s1 %= DIVISOR;
		s2 %= DIVISOR;
	}

	/* While there are still 32-byte aligned vectors remaining... */
	while ((uintptr_t)p < endv) {

		/* Process a chunk which is guaranteed not to overflow s2. */
		size_t chunk_size = MIN(endv - (uintptr_t)p,
					MAX_BYTES_PER_CHUNK & ~31);
		const u8 *chunk_end = p + chunk_size;

		s2 += s1 * chunk_size;
		s2 %= DIVISOR;
		{
	#ifdef ADLER32_TARGET_AVX2
		/*
		 * AVX2 implementation.  This works like the SSE2 implementation
		 * below, except:
		 *
		 * - Vectors are 32 bytes instead of 16 bytes.
		 *
		 * - We use "Packed Multiply and Add Unsigned Byte to Signed
		 *   Word" (_mm256_maddubs_epi16()) to directly do the
		 *   multiplication of the bytes with their multipliers without
		 *   having to expand the bytes to 16-bit integers first.
		 */
		const __m256i zeroes = _mm256_setzero_si256();
		const __v32qi multipliers = (__v32qi) { 32, 31, 30, 29, 28, 27, 26, 25,
							24, 23, 22, 21, 20, 19, 18, 17,
							16, 15, 14, 13, 12, 11, 10, 9,
							8,  7,  6,  5,  4,  3,  2,  1 };
		const __v16hi ones = (__v16hi)_mm256_set1_epi16(1);
		__v8si v_s1 = (__v8si)zeroes;
		__v8si v_s2 = (__v8si)zeroes;
		do {
			__v32qi bytes = *(const __v32qi *)p;
			__v16hi sums = (__v16hi)_mm256_maddubs_epi16(
						(__m256i)bytes, (__m256i)multipliers);
			v_s2 += (__v8si)_mm256_slli_epi32((__m256i)v_s1, 5);
			v_s1 += (__v8si)_mm256_sad_epu8((__m256i)bytes, zeroes);
			v_s2 += (__v8si)_mm256_madd_epi16((__m256i)sums, (__m256i)ones);
			p += 32;
		} while (p != chunk_end);

		v_s1 = (__v8si)_mm256_hadd_epi32((__m256i)v_s1, zeroes);
		v_s1 = (__v8si)_mm256_hadd_epi32((__m256i)v_s1, zeroes);
		s1 += v_s1[0] + v_s1[4];

		v_s2 = (__v8si)_mm256_hadd_epi32((__m256i)v_s2, zeroes);
		v_s2 = (__v8si)_mm256_hadd_epi32((__m256i)v_s2, zeroes);
		s2 += v_s2[0] + v_s2[4];

	#elif defined(ADLER32_TARGET_SSE2)
		/* SSE2 implementation */
		const __m128i zeroes = _mm_setzero_si128();
		const __v8hi multipliers_a = (__v8hi) { 32, 31, 30, 29, 28, 27, 26, 25 };
		const __v8hi multipliers_b = (__v8hi) { 24, 23, 22, 21, 20, 19, 18, 17 };
		const __v8hi multipliers_c = (__v8hi) { 16, 15, 14, 13, 12, 11, 10, 9  };
		const __v8hi multipliers_d = (__v8hi) { 8,  7,  6,  5,  4,  3,  2,  1  };

		/* s1 counters: 32-bit, sum of bytes */
		__v4si v_s1 = (__v4si)zeroes;

		/* s2 counters: 32-bit, sum of s1 values */
		__v4si v_s2 = (__v4si)zeroes;
		__v4si v_s2_a = (__v4si)zeroes;
		__v4si v_s2_b = (__v4si)zeroes;
		do {
			/* Load the next 32 bytes. */
			__v16qi bytes1 = *(const __v16qi *)p;
			__v16qi bytes2 = *(const __v16qi *)(p + 16);

			/* Expand the bytes into 16-bit integers. */
			__v8hi bytes_a = (__v8hi)_mm_unpacklo_epi8((__m128i)bytes1, zeroes);
			__v8hi bytes_b = (__v8hi)_mm_unpackhi_epi8((__m128i)bytes1, zeroes);
			__v8hi bytes_c = (__v8hi)_mm_unpacklo_epi8((__m128i)bytes2, zeroes);
			__v8hi bytes_d = (__v8hi)_mm_unpackhi_epi8((__m128i)bytes2, zeroes);

			/*
			 * s2 update (part 1): multiply the s1 counters by 32
			 * and add them to the s2 counters.  This accounts for
			 * all the additions of existing s1 values into s2 which
			 * need to happen over the 32 bytes currently being
			 * processed.
			 */
			v_s2_a += (__v4si)_mm_slli_epi32((__m128i)v_s1, 5);

			/*
			 * s1 update: use "Packed Sum of Absolute Differences"
			 * to add the bytes horizontally with 8 bytes per sum.
			 * Then add the sums to the s1 counters.
			 */
			v_s1 += (__v4si)_mm_sad_epu8((__m128i)bytes1, zeroes);
			v_s1 += (__v4si)_mm_sad_epu8((__m128i)bytes2, zeroes);

			/*
			 * s2 update (part 2): use "Packed Multiply and Add Word
			 * to Doubleword" to multiply the expanded bytes by the
			 * multipliers and sum adjacent pairs into 32-bit sums.
			 * Then add those sums to the s2 counters.
			 */
			v_s2_a += (__v4si)_mm_madd_epi16((__m128i)bytes_a, (__m128i)multipliers_a);
			v_s2_a += (__v4si)_mm_madd_epi16((__m128i)bytes_b, (__m128i)multipliers_b);
			v_s2_b += (__v4si)_mm_madd_epi16((__m128i)bytes_c, (__m128i)multipliers_c);
			v_s2_b += (__v4si)_mm_madd_epi16((__m128i)bytes_d, (__m128i)multipliers_d);

			p += 32;
		} while (p != chunk_end);

		v_s2 += v_s2_a + v_s2_b;

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
