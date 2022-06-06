/*
 * x86/crc32_template.h - "template" for x86-optimized gzip CRC-32 algorithm
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
 * This file is a "template" for instantiating crc32_x86 functions with
 * different target attributes.  The "parameters" are:
 *
 * ATTRIBUTES:
 *	Target function attributes to use.
 * SUFFIX:
 *	Name suffix to append to "crc32_x86" and any helper functions.
 *
 * The implementation used is CRC folding with carryless multiplication
 * instructions (PCLMULQDQ).  The x86 crc32 instruction cannot be used, as it is
 * for a different polynomial (not the gzip one).  For an explanation of CRC
 * folding with carryless multiplication instructions, see
 * scripts/gen_crc32_multipliers.c and the following paper:
 *
 *	"Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction"
 *	https://www.intel.com/content/dam/www/public/us/en/documents/white-papers/fast-crc-computation-generic-polynomials-pclmulqdq-paper.pdf
 */

#include <immintrin.h>

static forceinline ATTRIBUTES __m128i
ADD_SUFFIX(fold_vec)(__m128i src, __m128i dst, __v2di multipliers)
{
	/*
	 * Note: the immediate constant for PCLMULQDQ specifies which 64-bit
	 * halves of the 128-bit vectors to multiply:
	 *
	 * 0x00 means low halves (higher degree polynomial terms for us)
	 * 0x11 means high halves (lower degree polynomial terms for us)
	 */
	return dst ^ _mm_clmulepi64_si128(src, multipliers, 0x00) ^
		_mm_clmulepi64_si128(src, multipliers, 0x11);
}
#define fold_vec	ADD_SUFFIX(fold_vec)

static u32 ATTRIBUTES MAYBE_UNUSED
ADD_SUFFIX(crc32_x86)(u32 crc, const u8 *p, size_t len)
{
	const __v2di multipliers_8 = (__v2di)CRC32_8VECS_MULTS;
	const __v2di multipliers_4 = (__v2di)CRC32_4VECS_MULTS;
	const __v2di multipliers_2 = (__v2di)CRC32_2VECS_MULTS;
	const __v2di multipliers_1 = (__v2di)CRC32_1VECS_MULTS;
	const __v2di final_multiplier = (__v2di){ CRC32_FINAL_MULT };
	const __m128i mask32 = (__m128i)(__v4si){ 0xFFFFFFFF };
	const __v2di barrett_reduction_constants = (__v2di)CRC32_BARRETT_CONSTANTS;
	const __m128i *vp;
	__m128i v0, v1, v2, v3, v4, v5, v6, v7;

	if ((uintptr_t)p % 16) {
		size_t align = MIN(len, -(uintptr_t)p % 16);

		crc = crc32_slice1(crc, p, align);
		p += align;
		len -= align;
	}
	vp = (const __m128i *)p;
	if (len < 32)
		goto scalar;

	v0 = *vp++;
	/*
	 * Fold the current CRC into the next 32 message bits.  If A(x) is the
	 * polynomial for the message prefix processed already and B(x) is the
	 * polynomial for the next 32 bits, then the combined polynomial is
	 * A(x)*x^32 + B(x).  Conveniently, by definition the CRC already has an
	 * extra factor of x^32 included, i.e. it is A(x)*x^32 mod G(x) rather
	 * than just A(x) mod G(x).  So we just need to add (xor) it to B(x) to
	 * get a polynomial congruent with the combined polynomial.
	 */
	v0 ^= (__m128i)(__v4si){ crc };

	if (len >= 256) {
		v1 = *vp++;
		v2 = *vp++;
		v3 = *vp++;
		v4 = *vp++;
		v5 = *vp++;
		v6 = *vp++;
		v7 = *vp++;
		len -= 128;

		/* Fold 128 bytes at a time. */
		do {
			v0 = fold_vec(v0, *vp++, multipliers_8);
			v1 = fold_vec(v1, *vp++, multipliers_8);
			v2 = fold_vec(v2, *vp++, multipliers_8);
			v3 = fold_vec(v3, *vp++, multipliers_8);
			v4 = fold_vec(v4, *vp++, multipliers_8);
			v5 = fold_vec(v5, *vp++, multipliers_8);
			v6 = fold_vec(v6, *vp++, multipliers_8);
			v7 = fold_vec(v7, *vp++, multipliers_8);
			len -= 128;
		} while (len >= 128);

		/* Fold v0..v7 into v0. */
		v4 = fold_vec(v0, v4, multipliers_4);
		v5 = fold_vec(v1, v5, multipliers_4);
		v6 = fold_vec(v2, v6, multipliers_4);
		v7 = fold_vec(v3, v7, multipliers_4);
		v6 = fold_vec(v4, v6, multipliers_2);
		v7 = fold_vec(v5, v7, multipliers_2);
		v0 = fold_vec(v6, v7, multipliers_1);
	} else {
		len -= 16;
	}

	/* Fold 16 bytes at a time. */
	while (len >= 16) {
		v0 = fold_vec(v0, *vp++, multipliers_1);
		len -= 16;
	}

	/*
	 * Fold 128 => 96 bits.  This also implicitly appends 32 zero bits,
	 * which is equivalent to multiplying by x^32.  This is needed because
	 * the CRC is defined as M(x)*x^32 mod G(x), not just M(x) mod G(x).
	 */
	v0 = _mm_srli_si128(v0, 8) ^
	     _mm_clmulepi64_si128(v0, multipliers_1, 0x10);

	/* Fold 96 => 64 bits. */
	v0 = _mm_srli_si128(v0, 4) ^
	     _mm_clmulepi64_si128(v0 & mask32, final_multiplier, 0x00);

        /*
	 * Reduce 64 => 32 bits using Barrett reduction.
	 *
	 * Let M(x) = A(x)*x^32 + B(x) be the remaining message.  The goal is to
	 * compute R(x) = M(x) mod G(x).  Since degree(B(x)) < degree(G(x)):
	 *
	 *	R(x) = (A(x)*x^32 + B(x)) mod G(x)
	 *	     = (A(x)*x^32) mod G(x) + B(x)
	 *
	 * Then, by the Division Algorithm there exists a unique q(x) such that:
	 *
	 *	A(x)*x^32 mod G(x) = A(x)*x^32 - q(x)*G(x)
	 *
	 * Since the left-hand side is of maximum degree 31, the right-hand side
	 * must be too.  This implies that we can apply 'mod x^32' to the
	 * right-hand side without changing its value:
	 *
	 *	(A(x)*x^32 - q(x)*G(x)) mod x^32 = q(x)*G(x) mod x^32
	 *
	 * Note that '+' is equivalent to '-' in polynomials over GF(2).
	 *
	 * We also know that:
	 *
	 *	              / A(x)*x^32 \
	 *	q(x) = floor (  ---------  )
	 *	              \    G(x)   /
	 *
	 * To compute this efficiently, we can multiply the top and bottom by
	 * x^32 and move the division by G(x) to the top:
	 *
	 *	              / A(x) * floor(x^64 / G(x)) \
	 *	q(x) = floor (  -------------------------  )
	 *	              \           x^32            /
	 *
	 * Note that floor(x^64 / G(x)) is a constant.
	 *
	 * So finally we have:
	 *
	 *	                          / A(x) * floor(x^64 / G(x)) \
	 *	R(x) = B(x) + G(x)*floor (  -------------------------  )
	 *	                          \           x^32            /
	 */
	v1 = _mm_clmulepi64_si128(v0 & mask32, barrett_reduction_constants, 0x00);
	v1 = _mm_clmulepi64_si128(v1 & mask32, barrett_reduction_constants, 0x10);
	crc = _mm_cvtsi128_si32(_mm_srli_si128(v0 ^ v1, 4));
	p = (const u8 *)vp;
	/* Process up to 15 bytes left over at the end. */
scalar:
	return crc32_slice1(crc, p, len);
}

#undef SUFFIX
#undef ATTRIBUTES
#undef fold_vec
