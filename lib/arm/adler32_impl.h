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

/* Regular NEON implementation */
#if HAVE_NEON_INTRIN && CPU_IS_LITTLE_ENDIAN()
#  define adler32_neon		adler32_neon
#  define FUNCNAME		adler32_neon
#  define FUNCNAME_CHUNK	adler32_neon_chunk
#  define IMPL_ALIGNMENT	16
#  define IMPL_SEGMENT_LEN	512
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
	const uint16x8_t mults_a = { 32, 31, 30, 29, 28, 27, 26, 25, };
	const uint16x8_t mults_b = { 24, 23, 22, 21, 20, 19, 18, 17, };
	const uint16x8_t mults_c = { 16, 15, 14, 13, 12, 11, 10,  9, };
	const uint16x8_t mults_d = {  8,  7,  6,  5,  4,  3,  2,  1, };
	const uint16x8_t thirtytwos = { 32, 32, 32, 32, 32, 32, 32, 32 };

	uint32x4_t v_s1 = { 0 };
	uint32x4_t v_s2 = { 0 };

	do {
		uint16x8_t v_s1_a = { 0 };
		uint16x8_t v_s1_b = { 0 };
		uint16x8_t v_s1_c = { 0 };
		uint16x8_t v_s1_d = { 0 };
		uint16x8_t v_s2_a = { 0 };
		uint16x8_t v_s2_b = { 0 };
		uint16x8_t v_s2_c = { 0 };
		uint16x8_t v_s2_d = { 0 };

		for (int i = 0; i < 16; i++) {
			uint8x16_t bytes1 = *p++;
			uint8x16_t bytes2 = *p++;

			v_s2_a += v_s1_a;
			v_s2_b += v_s1_b;
			v_s2_c += v_s1_c;
			v_s2_d += v_s1_d;
			v_s1_a = vaddw_u8(v_s1_a, vget_low_u8(bytes1));
			v_s1_b = vaddw_u8(v_s1_b, vget_high_u8(bytes1));
			v_s1_c = vaddw_u8(v_s1_c, vget_low_u8(bytes2));
			v_s1_d = vaddw_u8(v_s1_d, vget_high_u8(bytes2));
		}

		v_s2 += vqshlq_n_u32(v_s1, 9);

#ifdef __arm__
#  define umlal2(a, b, c)  vmlal_u16((a), vget_high_u16(b), vget_high_u16(c))
#else
#  define umlal2	   vmlal_high_u16
#endif
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_s2_a), vget_low_u16(thirtytwos));
		v_s2 = umlal2(v_s2, v_s2_a, thirtytwos);
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_s2_b), vget_low_u16(thirtytwos));
		v_s2 = umlal2(v_s2, v_s2_b, thirtytwos);
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_s2_c), vget_low_u16(thirtytwos));
		v_s2 = umlal2(v_s2, v_s2_c, thirtytwos);
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_s2_d), vget_low_u16(thirtytwos));
		v_s2 = umlal2(v_s2, v_s2_d, thirtytwos);

		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_s1_a), vget_low_u16(mults_a));
		v_s2 = umlal2(v_s2, v_s1_a, mults_a);
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_s1_b), vget_low_u16(mults_b));
		v_s2 = umlal2(v_s2, v_s1_b, mults_b);
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_s1_c), vget_low_u16(mults_c));
		v_s2 = umlal2(v_s2, v_s1_c, mults_c);
		v_s2 = vmlal_u16(v_s2, vget_low_u16(v_s1_d), vget_low_u16(mults_d));
		v_s2 = umlal2(v_s2, v_s1_d, mults_d);
#undef umlal2

		v_s1_a = v_s1_a + v_s1_b + v_s1_c + v_s1_d;
		v_s1 = vaddw_u16(v_s1, vget_low_u16(v_s1_a));
		v_s1 = vaddw_u16(v_s1, vget_high_u16(v_s1_a));

	} while (p != end);

	/* Horizontal sum to finish up */
	*s1 += v_s1[0] + v_s1[1] + v_s1[2] + v_s1[3];
	*s2 += v_s2[0] + v_s2[1] + v_s2[2] + v_s2[3];
}
#  include "../adler32_vec_template.h"
#endif /* Regular NEON implementation */

/* NEON+dotprod implementation */
#if HAVE_DOTPROD_INTRIN && CPU_IS_LITTLE_ENDIAN()
#  define adler32_neon_dotprod	adler32_neon_dotprod
#  define FUNCNAME		adler32_neon_dotprod
#  define FUNCNAME_CHUNK	adler32_neon_dotprod_chunk
#  define IMPL_ALIGNMENT	16
#  define IMPL_SEGMENT_LEN	64
#  define IMPL_MAX_CHUNK_LEN	MAX_CHUNK_LEN
#  if HAVE_DOTPROD_NATIVE
#    define ATTRIBUTES
#  else
#    ifdef __clang__
#      define ATTRIBUTES  __attribute__((target("dotprod")))
     /*
      * With gcc, arch=armv8.2-a is needed for dotprod intrinsics, unless the
      * default target is armv8.3-a or later in which case it must be omitted.
      * armv8.3-a or later can be detected by checking for __ARM_FEATURE_JCVT.
      */
#    elif defined(__ARM_FEATURE_JCVT)
#      define ATTRIBUTES  __attribute__((target("+dotprod")))
#    else
#      define ATTRIBUTES  __attribute__((target("arch=armv8.2-a+dotprod")))
#    endif
#  endif
#  include <arm_neon.h>
static forceinline ATTRIBUTES void
adler32_neon_dotprod_chunk(const uint8x16_t *p, const uint8x16_t * const end,
			   u32 *s1, u32 *s2)
{
	const uint8x16_t mults_a = {
		64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49,
	};
	const uint8x16_t mults_b = {
		48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33,
	};
	const uint8x16_t mults_c = {
		32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
	};
	const uint8x16_t mults_d = {
		16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,
	};
	const uint8x16_t ones = {
		 1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1 , 1,  1,
	};
	uint32x4_t v_s1_a = { 0, 0, 0, 0 };
	uint32x4_t v_s1_b = { 0, 0, 0, 0 };
	uint32x4_t v_s1_c = { 0, 0, 0, 0 };
	uint32x4_t v_s1_d = { 0, 0, 0, 0 };
	uint32x4_t v_s2_a = { 0, 0, 0, 0 };
	uint32x4_t v_s2_b = { 0, 0, 0, 0 };
	uint32x4_t v_s2_c = { 0, 0, 0, 0 };
	uint32x4_t v_s2_d = { 0, 0, 0, 0 };
	uint32x4_t v_s1_sums_a = { 0, 0, 0, 0 };
	uint32x4_t v_s1_sums_b = { 0, 0, 0, 0 };
	uint32x4_t v_s1_sums_c = { 0, 0, 0, 0 };
	uint32x4_t v_s1_sums_d = { 0, 0, 0, 0 };
	uint32x4_t v_s1;
	uint32x4_t v_s2;

	do {
		uint8x16_t bytes_a = *p++;
		uint8x16_t bytes_b = *p++;
		uint8x16_t bytes_c = *p++;
		uint8x16_t bytes_d = *p++;

		v_s1_sums_a += v_s1_a;
		v_s1_a = vdotq_u32(v_s1_a, bytes_a, ones);
		v_s2_a = vdotq_u32(v_s2_a, bytes_a, mults_a);

		v_s1_sums_b += v_s1_b;
		v_s1_b = vdotq_u32(v_s1_b, bytes_b, ones);
		v_s2_b = vdotq_u32(v_s2_b, bytes_b, mults_b);

		v_s1_sums_c += v_s1_c;
		v_s1_c = vdotq_u32(v_s1_c, bytes_c, ones);
		v_s2_c = vdotq_u32(v_s2_c, bytes_c, mults_c);

		v_s1_sums_d += v_s1_d;
		v_s1_d = vdotq_u32(v_s1_d, bytes_d, ones);
		v_s2_d = vdotq_u32(v_s2_d, bytes_d, mults_d);
	} while (p != end);

	v_s1 = v_s1_a + v_s1_b + v_s1_c + v_s1_d;
	v_s2 = v_s2_a + v_s2_b + v_s2_c + v_s2_d +
	       vqshlq_n_u32(v_s1_sums_a + v_s1_sums_b +
			    v_s1_sums_c + v_s1_sums_d, 6);
	*s1 += v_s1[0] + v_s1[1] + v_s1[2] + v_s1[3];
	*s2 += v_s2[0] + v_s2[1] + v_s2[2] + v_s2[3];
}
#  include "../adler32_vec_template.h"
#endif /* NEON+dotprod implementation */

#if defined(adler32_neon_dotprod) && HAVE_DOTPROD_NATIVE
#define DEFAULT_IMPL	adler32_neon_dotprod
#else
static inline adler32_func_t
arch_select_adler32_func(void)
{
	const u32 features MAYBE_UNUSED = get_arm_cpu_features();

#ifdef adler32_neon_dotprod
	if (HAVE_NEON(features) && HAVE_DOTPROD(features))
		return adler32_neon_dotprod;
#endif
#ifdef adler32_neon
	if (HAVE_NEON(features))
		return adler32_neon;
#endif
	return NULL;
}
#define arch_select_adler32_func	arch_select_adler32_func
#endif

#endif /* LIB_ARM_ADLER32_IMPL_H */
