/*
 * gen_crc32_multipliers.c
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
 * This program computes the constant multipliers needed for "folding" over
 * various distances with the gzip CRC-32.  Each such multiplier is x^D mod G(x)
 * for some distance D, in bits, over which the folding is occurring.
 *
 * Folding works as follows: let A(x) be a polynomial (possibly reduced
 * partially or fully mod G(x)) for part of the message, and let B(x) be a
 * polynomial (possibly reduced partially or fully mod G(x)) for a later part of
 * the message.  The unreduced combined polynomial is A(x)*x^D + B(x), where D
 * is the number of bits separating the two parts of the message plus len(B(x)).
 * Since mod G(x) can be applied at any point, x^D mod G(x) can be precomputed
 * and used instead of x^D unreduced.  That allows the combined polynomial to be
 * computed relatively easily in a partially-reduced form A(x)*(x^D mod G(x)) +
 * B(x), with length max(len(A(x)) + 31, len(B(x))).  This does require doing a
 * polynomial multiplication (carryless multiplication).
 *
 * "Folding" in this way can be used for the entire CRC computation except the
 * final reduction to 32 bits; this works well when CPU support for carryless
 * multiplication is available.  It can also be used to combine CRCs of
 * different parts of the message that were computed using a different method.
 *
 * Note that the gzip CRC-32 uses bit-reversed polynomials.  I.e., the low order
 * bits are really the high order polynomial coefficients.
 */

#include <inttypes.h>
#include <stdio.h>

#include "../common_defs.h"

/* The generator polynomial G(x) for the gzip CRC-32 */
#define CRCPOLY		0xEDB88320 /* G(x) without x^32 term */
#define CRCPOLY_FULL	(((u64)CRCPOLY << 1) | 1) /* G(x) */

/* Compute x^D mod G(x) */
static u32
compute_xD_modG(size_t D)
{
	/* Start with x^0 mod G(x) */
	u32 remainder = 0x80000000;

	/* Each iteration, 'remainder' becomes x^i mod G(x) */
	for (size_t i = 1; i <= D; i++)
		remainder = (remainder >> 1) ^ ((remainder & 1) ? CRCPOLY : 0);

	/* Now 'remainder' is x^D mod G(x) */
	return remainder;
}

/* Compute floor(x^95 / G(x)) */
static u64
compute_x95_div_G(void)
{
	/* The quotient, max order 95 - 32 = 63. */
	u64 quotient = 0;

	/*
	 * The x^32 through x^95 terms of the remainder.  This starts at x^95
	 * and is updated through long division.  At the end only the
	 * x^0 through x^31 terms will be nonzero, but those are unneeded.
	 */
	u64 remainder = 0x1;

	for (int i = 0; i < 64; i++) {
		/*
		 * If the x^(95-i) term of remainder is nonzero, add
		 * x^(63-i) * G(x) to cancel it out.  (G(x) has order 32.)
		 */
		if (remainder & (1ULL << i)) {
			quotient |= 1ULL << i;
			remainder ^= (u64)CRCPOLY_FULL << i;
		}
	}

	return quotient;
}

static void
gen_vec_folding_constants(void)
{
	/*
	 * Compute the multipliers needed for CRC-32 folding with carryless
	 * multiplication instructions that operate on the 64-bit halves of
	 * 128-bit segments.  Using the terminology from earlier, for each 64-bit
	 * fold len(A(x)) = 64, and len(B(x)) = 95 since a 64-bit polynomial
	 * multiplied by a 32-bit one produces a 95-bit one.  When A(x) is the
	 * low order polynomial half of a 128-bit segments (high order physical
	 * half), the separation between the message parts is the total length
	 * of the 128-bit segments separating the values.  When A(x) is the high
	 * order polynomial half, the separation is 64 bits greater.
	 */
	for (int i = 1; i <= 32; i++) {
		const int sep_lo = 128 * (i - 1);
		const int sep_hi = sep_lo + 64;
		const int len_B = 95;
		int D;

		/* A(x) = high 64 polynomial bits (low 64 physical bits) */
		D = sep_hi + len_B;
		printf("#define CRC32_X%d_MODG 0x%08"PRIx32" /* x^%d mod G(x) */\n",
		       D, compute_xD_modG(D), D);

		/* A(x) = low 64 polynomial bits (high 64 physical bits) */
		D = sep_lo + len_B;
		printf("#define CRC32_X%d_MODG 0x%08"PRIx32" /* x^%d mod G(x) */\n",
		       D, compute_xD_modG(D), D);
		printf("\n");
	}

	/* Constants for the final 128 => 32 bit reduction */
	printf("#define CRC32_BARRETT_CONSTANT_1 0x%016"PRIx64"ULL /* floor(x^95 / G(x)) */\n",
	       compute_x95_div_G());
	printf("#define CRC32_BARRETT_CONSTANT_2 0x%016"PRIx64"ULL /* G(x) */\n",
	       CRCPOLY_FULL);
}

/* Multipliers for combining the CRCs of separate chunks */
static void
gen_chunk_constants(void)
{
	const size_t num_chunks = 4;
	const size_t table_len = 129;
	const size_t min_chunk_len = 128;

	printf("#define CRC32_NUM_CHUNKS %zu\n", num_chunks);
	printf("#define CRC32_MIN_VARIABLE_CHUNK_LEN %zuUL\n", min_chunk_len);
	printf("#define CRC32_MAX_VARIABLE_CHUNK_LEN %zuUL\n",
	       (table_len - 1) * min_chunk_len);
	printf("\n");
	printf("/* Multipliers for implementations that use a variable chunk length */\n");
	printf("static const u32 crc32_mults_for_chunklen[][CRC32_NUM_CHUNKS - 1] MAYBE_UNUSED = {\n");
	printf("\t{ 0 /* unused row */ },\n");
	for (size_t i = 1; i < table_len; i++) {
		const size_t chunk_len = i*min_chunk_len;

		printf("\t/* chunk_len=%zu */\n", chunk_len);
		printf("\t{ ");
		for (size_t j = num_chunks - 1; j >= 1; j--) {
			const size_t D = (j * 8 * chunk_len) - 33;

			printf("0x%08"PRIx32" /* x^%zu mod G(x) */, ",
			       compute_xD_modG(D), D);
		}
		printf("},\n");
	}
	printf("};\n");
	printf("\n");

	printf("/* Multipliers for implementations that use a large fixed chunk length */\n");
	const size_t fixed_chunk_len = 32768;
	printf("#define CRC32_FIXED_CHUNK_LEN %zuUL\n", fixed_chunk_len);
	for (int j = 1; j < num_chunks; j++) {
		const size_t D = (j * 8 * fixed_chunk_len) - 33;

		printf("#define CRC32_FIXED_CHUNK_MULT_%d 0x%08"PRIx32" /* x^%zu mod G(x) */\n",
		       j, compute_xD_modG(D), D);
	}
}

int
main(void)
{
	printf("/*\n"
	       " * crc32_multipliers.h - constants for CRC-32 folding\n"
	       " *\n"
	       " * THIS FILE WAS GENERATED BY gen_crc32_multipliers.c.  DO NOT EDIT.\n"
	       " */\n"
	       "\n");

	gen_vec_folding_constants();
	printf("\n");
	gen_chunk_constants();
	return 0;
}
