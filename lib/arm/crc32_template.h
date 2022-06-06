/*
 * arm/crc32_template.h - "template" for ARM-optimized gzip CRC-32 algorithm
 *
 * Copyright 2022 Eric Biggers
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
 * This file is a "template" for instantiating crc32_arm functions with various
 * supported instruction sets and stride lengths.  The "parameters" are:
 *
 * ATTRIBUTES:
 *	Target function attributes to use.  This must match the instructions
 *	enabled by the ENABLE_* parameters.
 * SUFFIX:
 *	Name suffix to append to "crc32_arm" and any helper functions.
 * ENABLE_PMULL:
 *	Enable CRC folding with pmull instructions.
 * PMULL_STRIDE_VECS:
 *	[If ENABLE_PMULL] Number of vectors processed per iteration.
 * ENABLE_EOR3:
 *	[If ENABLE_PMULL] Enable eor3 instructions.
 * ENABLE_CRC:
 *	Enable crc32 instructions.  This can be given on its own, in which case
 *	all data will be processed with crc32 instructions.  Alternatively, it
 *	can be combined with ENABLE_PMULL, in which case crc32 instructions will
 *	only be used at the beginning and end of the data buffer to process any
 *	data that isn't properly aligned for the pmull main loop.
 *
 * For an explanation of CRC folding with carryless multiplication instructions,
 * see scripts/gen_crc32_multipliers.c and the following paper:
 *
 *	"Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction"
 *	https://www.intel.com/content/dam/www/public/us/en/documents/white-papers/fast-crc-computation-generic-polynomials-pclmulqdq-paper.pdf
 */

#if ENABLE_CRC
#  include <arm_acle.h>
#endif

#if ENABLE_PMULL
#include <arm_neon.h>

#define PMULL_STRIDE_BYTES	(PMULL_STRIDE_VECS * 16)

#if PMULL_STRIDE_VECS == 1
#  define MULTIPLIERS	CRC32_1VECS_MULTS
#  define FOLD_TO_1_VEC /* no-op */
#elif PMULL_STRIDE_VECS == 2
#  define MULTIPLIERS	CRC32_2VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v0 = fold_vec(v0, v1, multipliers_1);
#elif PMULL_STRIDE_VECS == 3
#  define MULTIPLIERS	CRC32_3VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v1 = fold_vec(v0, v1, multipliers_1);		\
	v0 = fold_vec(v1, v2, multipliers_1);
#elif PMULL_STRIDE_VECS == 4
#  define MULTIPLIERS	CRC32_4VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v2 = fold_vec(v0, v2, multipliers_2);		\
	v3 = fold_vec(v1, v3, multipliers_2);		\
	v0 = fold_vec(v2, v3, multipliers_1);
#elif PMULL_STRIDE_VECS == 5
#  define MULTIPLIERS	CRC32_5VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v3 = fold_vec(v0, v3, multipliers_3);		\
	v4 = fold_vec(v1, v4, multipliers_3);		\
	v3 = fold_vec(v2, v3, multipliers_1);		\
	v0 = fold_vec(v3, v4, multipliers_1);
#elif PMULL_STRIDE_VECS == 6
#  define MULTIPLIERS	CRC32_6VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v3 = fold_vec(v0, v3, multipliers_3);		\
	v4 = fold_vec(v1, v4, multipliers_3);		\
	v5 = fold_vec(v2, v5, multipliers_3);		\
	v4 = fold_vec(v3, v4, multipliers_1);		\
	v0 = fold_vec(v4, v5, multipliers_1);
#elif PMULL_STRIDE_VECS == 7
#  define MULTIPLIERS	CRC32_7VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v4 = fold_vec(v0, v4, multipliers_4);		\
	v5 = fold_vec(v1, v5, multipliers_4);		\
	v6 = fold_vec(v2, v6, multipliers_4);		\
	v5 = fold_vec(v3, v5, multipliers_2);		\
	v6 = fold_vec(v4, v6, multipliers_2);		\
	v0 = fold_vec(v5, v6, multipliers_1);
#elif PMULL_STRIDE_VECS == 8
#  define MULTIPLIERS	CRC32_8VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v4 = fold_vec(v0, v4, multipliers_4);		\
	v5 = fold_vec(v1, v5, multipliers_4);		\
	v6 = fold_vec(v2, v6, multipliers_4);		\
	v7 = fold_vec(v3, v7, multipliers_4);		\
	v6 = fold_vec(v4, v6, multipliers_2);		\
	v7 = fold_vec(v5, v7, multipliers_2);		\
	v0 = fold_vec(v6, v7, multipliers_1);
#elif PMULL_STRIDE_VECS == 9
#  define MULTIPLIERS	CRC32_9VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v4 = fold_vec(v0, v4, multipliers_4);		\
	v5 = fold_vec(v1, v5, multipliers_4);		\
	v6 = fold_vec(v2, v6, multipliers_4);		\
	v7 = fold_vec(v3, v7, multipliers_4);		\
	v6 = fold_vec(v4, v6, multipliers_2);		\
	v7 = fold_vec(v5, v7, multipliers_2);		\
	v8 = fold_vec(v6, v8, multipliers_2);		\
	v0 = fold_vec(v7, v8, multipliers_1);
#elif PMULL_STRIDE_VECS == 10
#  define MULTIPLIERS	CRC32_10VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v5 = fold_vec(v0, v5, multipliers_5);		\
	v6 = fold_vec(v1, v6, multipliers_5);		\
	v7 = fold_vec(v2, v7, multipliers_5);		\
	v8 = fold_vec(v3, v8, multipliers_5);		\
	v9 = fold_vec(v4, v9, multipliers_5);		\
	v8 = fold_vec(v5, v8, multipliers_3);		\
	v9 = fold_vec(v6, v9, multipliers_3);		\
	v8 = fold_vec(v7, v8, multipliers_1);		\
	v0 = fold_vec(v8, v9, multipliers_1);
#elif PMULL_STRIDE_VECS == 11
#  define MULTIPLIERS	CRC32_11VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v5 = fold_vec(v0, v5, multipliers_5);		\
	v6 = fold_vec(v1, v6, multipliers_5);		\
	v7 = fold_vec(v2, v7, multipliers_5);		\
	v8 = fold_vec(v3, v8, multipliers_5);		\
	v9 = fold_vec(v4, v9, multipliers_5);		\
	v8 = fold_vec(v5, v8, multipliers_3);		\
	v9 = fold_vec(v6, v9, multipliers_3);		\
	v10 = fold_vec(v7, v10, multipliers_3);	\
	v9 = fold_vec(v8, v9, multipliers_1);		\
	v0 = fold_vec(v9, v10, multipliers_1);
#elif PMULL_STRIDE_VECS == 12
#  define MULTIPLIERS	CRC32_12VECS_MULTS
#  define FOLD_TO_1_VEC					\
	v6 = fold_vec(v0, v6, multipliers_6);		\
	v7 = fold_vec(v1, v7, multipliers_6);		\
	v8 = fold_vec(v2, v8, multipliers_6);		\
	v9 = fold_vec(v3, v9, multipliers_6);		\
	v10 = fold_vec(v4, v10, multipliers_6);	\
	v11 = fold_vec(v5, v11, multipliers_6);	\
	v9 = fold_vec(v6, v9, multipliers_3);		\
	v10 = fold_vec(v7, v10, multipliers_3);	\
	v11 = fold_vec(v8, v11, multipliers_3);	\
	v10 = fold_vec(v9, v10, multipliers_1);	\
	v0 = fold_vec(v10, v11, multipliers_1);
#else
#  error "Unsupported PMULL_STRIDE_VECS"
#endif

static forceinline ATTRIBUTES poly128_t
ADD_SUFFIX(clmul_high)(poly64x2_t a, poly64x2_t b)
{
#ifdef __clang__
	/*
	 * Use inline asm to ensure that pmull2 is really used.  This works
	 * around clang bug https://github.com/llvm/llvm-project/issues/52868.
	 */
	poly128_t res;

	__asm__("pmull2 %0.1q, %1.2d, %2.2d" : "=w" (res) : "w" (a), "w" (b));
	return res;
#else
	return vmull_high_p64(a, b);
#endif
}
#define clmul_high	ADD_SUFFIX(clmul_high)

static forceinline ATTRIBUTES uint8x16_t
ADD_SUFFIX(eor3)(uint8x16_t a, uint8x16_t b, uint8x16_t c)
{
#if ENABLE_EOR3
#if HAVE_SHA3_INTRIN
	return veor3q_u8(a, b, c);
#else
	uint8x16_t res;

	__asm__("eor3 %0.16b, %1.16b, %2.16b, %3.16b"
		: "=w" (res) : "w" (a), "w" (b), "w" (c));
	return res;
#endif
#else /* ENABLE_EOR3 */
	return a ^ b ^ c;
#endif /* !ENABLE_EOR3 */
}
#define eor3	ADD_SUFFIX(eor3)

static forceinline ATTRIBUTES poly64x2_t
ADD_SUFFIX(fold_vec)(poly64x2_t src, poly64x2_t dst, poly64x2_t multipliers)
{
	return (poly64x2_t)eor3((uint8x16_t)dst,
				(uint8x16_t)vmull_p64(src[0], multipliers[0]),
				(uint8x16_t)clmul_high(src, multipliers));
}
#define fold_vec	ADD_SUFFIX(fold_vec)

#elif defined(ENABLE_CRC)
/* ENABLE_CRC && !ENABLE_PMULL */

/*
 * When CRC instructions are enabled and PMULL is not, all the data will be
 * processed with CRC instructions.  The naive algorithm is serial: the input to
 * each crc32x instruction depends on the output of the previous one.  To take
 * advantage of CPUs that can execute multiple crc32x in parallel, we checksum
 * adjacent chunks of the data simultaneously, then combine the resulting CRCs.
 * The chunk size must not be too small, as combining CRCs is fairly expensive.
 */
#define CHUNK_LEN	((size_t)16384)
#define NUM_CHUNKS	4
#define CHUNK_MULT1	CRC32_COMBINE_16384B_CHUNK_MULT_1
#define CHUNK_MULT2	CRC32_COMBINE_16384B_CHUNK_MULT_2
#define CHUNK_MULT3	CRC32_COMBINE_16384B_CHUNK_MULT_3

/*
 * Combine the CRCs for 4 adjacent chunks of length CHUNK_LEN by computing
 * (crc0*CHUNK_MULT3 + crc1*CHUNK_MULT2 + crc2*CHUNK_MULT1 + crc3) mod G(x).
 * Combining CRCs is fairly expensive; we try to reduce the cost slightly by
 * interleaving the 3 independent multiplications and reductions.
 */
static forceinline ATTRIBUTES u32
ADD_SUFFIX(combine_crcs)(u32 crc0, u32 crc1, u32 crc2, u32 crc3)
{
	u32 res0 = 0, res1 = 0, res2 = 0;
	int i;

	for (i = 0; i < 32; i++) {
		if (CHUNK_MULT3 & (0x80000000 >> i))
			res0 ^= crc0;
		if (CHUNK_MULT2 & (0x80000000 >> i))
			res1 ^= crc1;
		if (CHUNK_MULT1 & (0x80000000 >> i))
			res2 ^= crc2;
		/* Multiply by x. */
		crc0 = (crc0 >> 1) ^ ((crc0 & 1) ? 0xEDB88320 : 0);
		crc1 = (crc1 >> 1) ^ ((crc1 & 1) ? 0xEDB88320 : 0);
		crc2 = (crc2 >> 1) ^ ((crc2 & 1) ? 0xEDB88320 : 0);
	}
	return res0 ^ res1 ^ res2 ^ crc3;
}
#define combine_crcs	ADD_SUFFIX(combine_crcs)

#endif /* ENABLE_CRC */

static u32 ATTRIBUTES MAYBE_UNUSED
ADD_SUFFIX(crc32_arm)(u32 crc, const u8 *p, size_t len)
{
	size_t align MAYBE_UNUSED;
#if ENABLE_PMULL
	const poly64x2_t multipliers = (poly64x2_t)MULTIPLIERS;
	const poly64x2_t multipliers_6 MAYBE_UNUSED = (poly64x2_t)CRC32_6VECS_MULTS;
	const poly64x2_t multipliers_5 MAYBE_UNUSED = (poly64x2_t)CRC32_5VECS_MULTS;
	const poly64x2_t multipliers_4 MAYBE_UNUSED = (poly64x2_t)CRC32_4VECS_MULTS;
	const poly64x2_t multipliers_3 MAYBE_UNUSED = (poly64x2_t)CRC32_3VECS_MULTS;
	const poly64x2_t multipliers_2 MAYBE_UNUSED = (poly64x2_t)CRC32_2VECS_MULTS;
	const poly64x2_t multipliers_1 MAYBE_UNUSED = (poly64x2_t)CRC32_1VECS_MULTS;
	const poly64x2_t zeroes = (poly64x2_t){ 0 };
	const poly64x2_t mask32 = (poly64x2_t){ 0xFFFFFFFF };
	const poly64x2_t *vp;
	poly64x2_t v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11;

	/*
	 * If the length is very short, skip the PMULL code.  Specifically, if
	 * crc32 instructions are available, skip PMULL if we can't use the main
	 * PMULL loop.  Otherwise only skip PMULL when we can't use it at all.
	 */
	if (len < 15 + 2 * (ENABLE_CRC ? PMULL_STRIDE_BYTES : 16))
		goto scalar;
	/* Align p to a 16-byte boundary. */
	align = (uintptr_t)p & 15;
	if (align) {
		align = 16 - align;
		len -= align;
	#if ENABLE_CRC
		if (align & 1)
			crc = __crc32b(crc, *p++);
		if (align & 2) {
			crc = __crc32h(crc, le16_bswap(*(u16 *)p));
			p += 2;
		}
		if (align & 4) {
			crc = __crc32w(crc, le32_bswap(*(u32 *)p));
			p += 4;
		}
		if (align & 8) {
			crc = __crc32d(crc, le64_bswap(*(u64 *)p));
			p += 8;
		}
	#else /* ENABLE_CRC */
		crc = crc32_slice1(crc, p, align);
		p += align;
	#endif /* !ENABLE_CRC */
	}
	vp = (const poly64x2_t *)p;

	v0 = *vp++ ^ (poly64x2_t){ crc };
	if (ENABLE_CRC || len >= 2 * PMULL_STRIDE_BYTES) {
		/*
		 * There is enough data for at least one iteration of the main
		 * loop, either because we checked len at the very beginning (if
		 * ENABLE_CRC) or because we checked len just above (otherwise).
		 */
		if (PMULL_STRIDE_VECS >= 2)
			v1 = *vp++;
		if (PMULL_STRIDE_VECS >= 3)
			v2 = *vp++;
		if (PMULL_STRIDE_VECS >= 4)
			v3 = *vp++;
		if (PMULL_STRIDE_VECS >= 5)
			v4 = *vp++;
		if (PMULL_STRIDE_VECS >= 6)
			v5 = *vp++;
		if (PMULL_STRIDE_VECS >= 7)
			v6 = *vp++;
		if (PMULL_STRIDE_VECS >= 8)
			v7 = *vp++;
		if (PMULL_STRIDE_VECS >= 9)
			v8 = *vp++;
		if (PMULL_STRIDE_VECS >= 10)
			v9 = *vp++;
		if (PMULL_STRIDE_VECS >= 11)
			v10 = *vp++;
		if (PMULL_STRIDE_VECS >= 12)
			v11 = *vp++;
		len -= PMULL_STRIDE_BYTES;
		/* Fold PMULL_STRIDE_BYTES at a time. */
		do {
			v0 = fold_vec(v0, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 2)
				v1 = fold_vec(v1, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 3)
				v2 = fold_vec(v2, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 4)
				v3 = fold_vec(v3, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 5)
				v4 = fold_vec(v4, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 6)
				v5 = fold_vec(v5, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 7)
				v6 = fold_vec(v6, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 8)
				v7 = fold_vec(v7, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 9)
				v8 = fold_vec(v8, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 10)
				v9 = fold_vec(v9, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 11)
				v10 = fold_vec(v10, *vp++, multipliers);
			if (PMULL_STRIDE_VECS >= 12)
				v11 = fold_vec(v11, *vp++, multipliers);
			len -= PMULL_STRIDE_BYTES;
		} while (len >= PMULL_STRIDE_BYTES);

		/* Fold v0..v{PMULL_STRIDE_VECS-1} into v0. */
		FOLD_TO_1_VEC

		/*
		 * If !ENABLE_CRC, the scalar part will be slow, so use PMULL as
		 * much as possible by folding 16 bytes at a time.
		 */
		if (!ENABLE_CRC) {
			while (len >= 16) {
				v0 = fold_vec(v0, *vp++, multipliers_1);
				len -= 16;
			}
		}
	} else {
		/*
		 * !ENABLE_CRC && len >= 32 && len < 2 * PMULL_STRIDE_BYTES.
		 * Since !ENABLE_CRC, the scalar part will be slow, so use PMULL
		 * as much as possible by folding 16 bytes at a time.
		 */
		len -= 16;
		do {
			v0 = fold_vec(v0, *vp++, multipliers_1);
			len -= 16;
		} while (len >= 16);
	}

	/*
	 * Fold 128 => 96 bits.  This also implicitly appends 32 zero bits,
	 * which is equivalent to multiplying by x^32.  This is needed because
	 * the CRC is defined as M(x)*x^32 mod G(x), not just M(x) mod G(x).
	 */
	v0 = (poly64x2_t)vextq_u8((uint8x16_t)v0, (uint8x16_t)zeroes, 8) ^
	     (poly64x2_t)vmull_p64(v0[0], CRC32_1VECS_MULT_2);

	/* Fold 96 => 64 bits. */
	v0 = (poly64x2_t)vextq_u8((uint8x16_t)v0, (uint8x16_t)zeroes, 4) ^
	     (poly64x2_t)vmull_p64((v0 & mask32)[0], CRC32_FINAL_MULT);

	/* Reduce 64 => 32 bits using Barrett reduction. */
	v1 = (poly64x2_t)vmull_p64((v0 & mask32)[0], CRC32_BARRETT_CONSTANT_1);
	v1 = (poly64x2_t)vmull_p64((v1 & mask32)[0], CRC32_BARRETT_CONSTANT_2);
	crc = ((uint32x4_t)(v0 ^ v1))[1];
	p = (const u8 *)vp;
scalar:
#endif /* !ENABLE_PMULL */
	/* Process the rest using scalar instructions. */
#if ENABLE_CRC
	if (len >= 64) {
		/* Align p to an 8-byte boundary. */
		align = (uintptr_t)p & 7;
		if (align) {
			align = 8 - align;
			len -= align;
			if (align & 1)
				crc = __crc32b(crc, *p++);
			if (align & 2) {
				crc = __crc32h(crc, le16_bswap(*(u16 *)p));
				p += 2;
			}
			if (align & 4) {
				crc = __crc32w(crc, le32_bswap(*(u32 *)p));
				p += 4;
			}
		}
	#if !ENABLE_PMULL && NUM_CHUNKS > 1
		/*
		 * Chunked implementation to take advantage of instruction-level
		 * parallelism
		 */
		while (len >= NUM_CHUNKS * CHUNK_LEN) {
			u32 crc1 = 0, crc2 = 0, crc3 = 0;
			const u64 *wp = (const u64 *)p;
			size_t i;

			STATIC_ASSERT(NUM_CHUNKS == 4);
			STATIC_ASSERT(CHUNK_LEN % (2 * 8) == 0);
			for (i = 0; i < CHUNK_LEN / 8; ) {
				crc  = __crc32d(crc,  le64_bswap(wp[i + 0*CHUNK_LEN/8]));
				crc1 = __crc32d(crc1, le64_bswap(wp[i + 1*CHUNK_LEN/8]));
				crc2 = __crc32d(crc2, le64_bswap(wp[i + 2*CHUNK_LEN/8]));
				crc3 = __crc32d(crc3, le64_bswap(wp[i + 3*CHUNK_LEN/8]));
				i++;
				crc  = __crc32d(crc,  le64_bswap(wp[i + 0*CHUNK_LEN/8]));
				crc1 = __crc32d(crc1, le64_bswap(wp[i + 1*CHUNK_LEN/8]));
				crc2 = __crc32d(crc2, le64_bswap(wp[i + 2*CHUNK_LEN/8]));
				crc3 = __crc32d(crc3, le64_bswap(wp[i + 3*CHUNK_LEN/8]));
				i++;
			}
			crc = combine_crcs(crc, crc1, crc2, crc3);
			p += NUM_CHUNKS * CHUNK_LEN;
			len -= NUM_CHUNKS * CHUNK_LEN;
		}
	#endif /* !ENABLE_PMULL && NUM_CHUNKS > 1 */

		while (len >= 64) {
			crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 0)));
			crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 8)));
			crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 16)));
			crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 24)));
			crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 32)));
			crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 40)));
			crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 48)));
			crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 56)));
			p += 64;
			len -= 64;
		}
	}
	if (len & 32) {
		crc = __crc32d(crc, get_unaligned_le64(p));
		crc = __crc32d(crc, get_unaligned_le64(p + 8));
		crc = __crc32d(crc, get_unaligned_le64(p + 16));
		crc = __crc32d(crc, get_unaligned_le64(p + 24));
		p += 32;
	}
	if (len & 16) {
		crc = __crc32d(crc, get_unaligned_le64(p));
		crc = __crc32d(crc, get_unaligned_le64(p + 8));
		p += 16;
	}
	if (len & 8) {
		crc = __crc32d(crc, get_unaligned_le64(p));
		p += 8;
	}
	if (len & 4) {
		crc = __crc32w(crc, get_unaligned_le32(p));
		p += 4;
	}
	if (len & 2) {
		crc = __crc32h(crc, get_unaligned_le16(p));
		p += 2;
	}
	if (len & 1)
		crc = __crc32b(crc, *p);
	return crc;
#else /* ENABLE_CRC */
	return crc32_slice1(crc, p, len);
#endif /* !ENABLE_CRC */
}

#undef SUFFIX
#undef ATTRIBUTES
#undef ENABLE_CRC
#undef ENABLE_PMULL
#undef ENABLE_EOR3
#undef PMULL_STRIDE_VECS

#undef PMULL_STRIDE_BYTES
#undef MULTIPLIERS
#undef FOLD_TO_1_VEC

#undef clmul_high
#undef eor3
#undef fold_vec
#undef combine_crcs
