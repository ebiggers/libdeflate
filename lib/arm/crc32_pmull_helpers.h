/*
 * arm/crc32_pmull_helpers.h - helper functions for CRC-32 folding with PMULL
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
 * This file is a "template" for instantiating helper functions for CRC folding
 * with pmull instructions.  It accepts the following parameters:
 *
 * SUFFIX:
 *	Name suffix to append to all instantiated functions.
 * ATTRIBUTES:
 *	Target function attributes to use.
 * ENABLE_EOR3:
 *	Use the eor3 instruction (from the sha3 extension).
 */

#include <arm_neon.h>

#undef clmul_high
static forceinline ATTRIBUTES poly128_t
ADD_SUFFIX(clmul_high)(poly64x2_t a, poly64x2_t b)
{
#if defined(__clang__) && defined(__aarch64__)
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

#undef eor3
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

#undef fold_vec
static forceinline ATTRIBUTES uint8x16_t
ADD_SUFFIX(fold_vec)(uint8x16_t src, uint8x16_t dst, poly64x2_t multipliers)
{
	/*
	 * Using vget_low_* instead of vector indexing is necessary to avoid
	 * poor code generation with gcc on arm32.
	 */
	poly128_t a = vmull_p64((poly64_t)vget_low_u8(src),
				(poly64_t)vget_low_p64(multipliers));
	poly128_t b = clmul_high((poly64x2_t)src, multipliers);

	return eor3((uint8x16_t)a, (uint8x16_t)b, dst);
}
#define fold_vec	ADD_SUFFIX(fold_vec)

#undef vtbl
static forceinline ATTRIBUTES uint8x16_t
ADD_SUFFIX(vtbl)(uint8x16_t table, uint8x16_t indices)
{
#ifdef __aarch64__
	return vqtbl1q_u8(table, indices);
#else
	uint8x8x2_t tab2;

	tab2.val[0] = vget_low_u8(table);
	tab2.val[1] = vget_high_u8(table);

	return vcombine_u8(vtbl2_u8(tab2, vget_low_u8(indices)),
			   vtbl2_u8(tab2, vget_high_u8(indices)));
#endif
}
#define vtbl	ADD_SUFFIX(vtbl)

/*
 * Given v containing a 16-byte polynomial, and a pointer 'p' that points to the
 * next '1 <= len <= 15' data bytes, rearrange the concatenation of v and the
 * data into vectors x0 and x1 that contain 'len' bytes and 16 bytes,
 * respectively.  Then fold x0 into x1 and return the result.  Assumes that
 * 'p + len - 16' is in-bounds.
 */
#undef fold_partial_vec
static forceinline ATTRIBUTES MAYBE_UNUSED uint8x16_t
ADD_SUFFIX(fold_partial_vec)(uint8x16_t v, const u8 *p, size_t len,
			     poly64x2_t multipliers_1)
{
	/*
	 * vtbl(v, shift_tab[len..len+15]) left shifts v by 16-len bytes.
	 * vtbl(v, shift_tab[len+16..len+31]) right shifts v by len bytes.
	 */
	static const u8 shift_tab[48] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	};
	const uint8x16_t lshift = vld1q_u8(&shift_tab[len]);
	const uint8x16_t rshift = vld1q_u8(&shift_tab[len + 16]);
	uint8x16_t x0, x1, bsl_mask;

	/* x0 = v left-shifted by '16 - len' bytes */
	x0 = vtbl(v, lshift);

	/* Create a vector of '16 - len' 0x00 bytes, then 'len' 0xff bytes. */
	bsl_mask = (uint8x16_t)vshrq_n_s8((int8x16_t)rshift, 7);

	/*
	 * x1 = the last '16 - len' bytes from v (i.e. v right-shifted by 'len'
	 * bytes) followed by the remaining data.
	 */
	x1 = vbslq_u8(bsl_mask /* 0 bits select from arg3, 1 bits from arg2 */,
		      vld1q_u8(p + len - 16), vtbl(v, rshift));

	return fold_vec(x0, x1, multipliers_1);
}
#define fold_partial_vec	ADD_SUFFIX(fold_partial_vec)
