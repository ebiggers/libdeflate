/*
 * arm/adler32_impl.h - ARM implementations of Adler-32 checksum algorithm
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

#ifndef LIB_ARM_ADLER32_IMPL_H
#define LIB_ARM_ADLER32_IMPL_H

#include "cpu_features.h"

#if HAVE_NEON_INTRIN && CPU_IS_LITTLE_ENDIAN()
#  define FUNCNAME		adler32_neon
#  define FUNCNAME_CHUNK	adler32_neon_chunk
#  define IMPL_ALIGNMENT	16
#  define IMPL_SEGMENT_LEN	64
/* Prevent unsigned overflow of the 16-bit precision byte counters */
#  define IMPL_MAX_CHUNK_LEN	(64 * (0xFFFF / 0xFF))
#  if HAVE_NEON_NATIVE
#    define ATTRIBUTES
#  else
#    ifdef __arm__
#      define ATTRIBUTES	__attribute__((target("fpu=neon")))
#    else
#      define ATTRIBUTES	__attribute__((target("+simd")))
#    endif
#  endif
#  include <arm_neon.h>
static forceinline ATTRIBUTES void
adler32_neon_chunk(const uint8x16_t *p, const uint8x16_t * const end,
		   u32 *s1, u32 *s2)
{
	const uint16x8_t mults_a = { 64, 63, 62, 61, 60, 59, 58, 57, };
	const uint16x8_t mults_b = { 56, 55, 54, 53, 52, 51, 50, 49, };
	const uint16x8_t mults_c = { 48, 47, 46, 45, 44, 43, 42, 41, };
	const uint16x8_t mults_d = { 40, 39, 38, 37, 36, 35, 34, 33, };
	const uint16x8_t mults_e = { 32, 31, 30, 29, 28, 27, 26, 25, };
	const uint16x8_t mults_f = { 24, 23, 22, 21, 20, 19, 18, 17, };
	const uint16x8_t mults_g = { 16, 15, 14, 13, 12, 11, 10,  9, };
	const uint16x8_t mults_h = {  8,  7,  6,  5,  4,  3,  2,  1, };

	uint32x4_t v_s1 = { 0, 0, 0, 0 };
	uint32x4_t v_s2 = { 0, 0, 0, 0 };
	/*
	 * v_byte_sums_* contain the sum of the bytes at index i across all
	 * 64-byte segments, for each index 0..63.
	 */
	uint16x8_t v_byte_sums_a = { 0, 0, 0, 0, 0, 0, 0, 0 };
	uint16x8_t v_byte_sums_b = { 0, 0, 0, 0, 0, 0, 0, 0 };
	uint16x8_t v_byte_sums_c = { 0, 0, 0, 0, 0, 0, 0, 0 };
	uint16x8_t v_byte_sums_d = { 0, 0, 0, 0, 0, 0, 0, 0 };
	uint16x8_t v_byte_sums_e = { 0, 0, 0, 0, 0, 0, 0, 0 };
	uint16x8_t v_byte_sums_f = { 0, 0, 0, 0, 0, 0, 0, 0 };
	uint16x8_t v_byte_sums_g = { 0, 0, 0, 0, 0, 0, 0, 0 };
	uint16x8_t v_byte_sums_h = { 0, 0, 0, 0, 0, 0, 0, 0 };

	do {
		/* Load the next 64 bytes. */
		const uint8x16_t bytes1 = *p++;
		const uint8x16_t bytes2 = *p++;
		const uint8x16_t bytes3 = *p++;
		const uint8x16_t bytes4 = *p++;
		uint16x8_t tmp;

		/*
		 * Accumulate the previous s1 counters into the s2 counters.
		 * The needed multiplication by 64 is delayed to later.
		 */
		v_s2 += v_s1;

		/*
		 * Add the 64 bytes to their corresponding v_byte_sums counters,
		 * while also accumulating the sums of each adjacent set of 4
		 * bytes into v_s1.
		 */
		tmp = vpaddlq_u8(bytes1);
		v_byte_sums_a = vaddw_u8(v_byte_sums_a, vget_low_u8(bytes1));
		v_byte_sums_b = vaddw_u8(v_byte_sums_b, vget_high_u8(bytes1));
		tmp = vpadalq_u8(tmp, bytes2);
		v_byte_sums_c = vaddw_u8(v_byte_sums_c, vget_low_u8(bytes2));
		v_byte_sums_d = vaddw_u8(v_byte_sums_d, vget_high_u8(bytes2));
		tmp = vpadalq_u8(tmp, bytes3);
		v_byte_sums_e = vaddw_u8(v_byte_sums_e, vget_low_u8(bytes3));
		v_byte_sums_f = vaddw_u8(v_byte_sums_f, vget_high_u8(bytes3));
		tmp = vpadalq_u8(tmp, bytes4);
		v_byte_sums_g = vaddw_u8(v_byte_sums_g, vget_low_u8(bytes4));
		v_byte_sums_h = vaddw_u8(v_byte_sums_h, vget_high_u8(bytes4));
		v_s1 = vpadalq_u16(v_s1, tmp);

	} while (p != end);

	/* s2 = 64*s2 + (64*bytesum0 + 63*bytesum1 + ... + 1*bytesum63) */
#ifdef __arm__
#  define umlal2(a, b, c)  vmlal_u16((a), vget_high_u16(b), vget_high_u16(c))
#else
#  define umlal2	   vmlal_high_u16
#endif
	v_s2 = vqshlq_n_u32(v_s2, 6);
	v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_a), vget_low_u16(mults_a));
	v_s2 = umlal2(v_s2, v_byte_sums_a, mults_a);
	v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_b), vget_low_u16(mults_b));
	v_s2 = umlal2(v_s2, v_byte_sums_b, mults_b);
	v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_c), vget_low_u16(mults_c));
	v_s2 = umlal2(v_s2, v_byte_sums_c, mults_c);
	v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_d), vget_low_u16(mults_d));
	v_s2 = umlal2(v_s2, v_byte_sums_d, mults_d);
	v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_e), vget_low_u16(mults_e));
	v_s2 = umlal2(v_s2, v_byte_sums_e, mults_e);
	v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_f), vget_low_u16(mults_f));
	v_s2 = umlal2(v_s2, v_byte_sums_f, mults_f);
	v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_g), vget_low_u16(mults_g));
	v_s2 = umlal2(v_s2, v_byte_sums_g, mults_g);
	v_s2 = vmlal_u16(v_s2, vget_low_u16(v_byte_sums_h), vget_low_u16(mults_h));
	v_s2 = umlal2(v_s2, v_byte_sums_h, mults_h);
#undef umlal2

	/* Horizontal sum to finish up */
	*s1 += v_s1[0] + v_s1[1] + v_s1[2] + v_s1[3];
	*s2 += v_s2[0] + v_s2[1] + v_s2[2] + v_s2[3];
}
#  include "../adler32_vec_template.h"
#  if HAVE_NEON_NATIVE
#    define DEFAULT_IMPL	adler32_neon
#  else
static inline adler32_func_t
arch_select_adler32_func(void)
{
	if (HAVE_NEON(get_arm_cpu_features()))
		return adler32_neon;
	return NULL;
}
#    define arch_select_adler32_func	arch_select_adler32_func
#  endif /* !HAVE_NEON_NATIVE */
#endif /* HAVE_NEON_INTRIN && CPU_IS_LITTLE_ENDIAN() */

#endif /* LIB_ARM_ADLER32_IMPL_H */
