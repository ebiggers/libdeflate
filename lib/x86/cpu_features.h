/*
 * x86/cpu_features.h - feature detection for x86 CPUs
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

#ifndef LIB_X86_CPU_FEATURES_H
#define LIB_X86_CPU_FEATURES_H

#include "../lib_common.h"

#define HAVE_DYNAMIC_X86_CPU_FEATURES	0

#if defined(ARCH_X86_32) || defined(ARCH_X86_64)

#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
#  undef HAVE_DYNAMIC_X86_CPU_FEATURES
#  define HAVE_DYNAMIC_X86_CPU_FEATURES	1
#endif

#define X86_CPU_FEATURE_SSE2		0x00000001
#define X86_CPU_FEATURE_PCLMULQDQ	0x00000002
#define X86_CPU_FEATURE_AVX		0x00000004
#define X86_CPU_FEATURE_AVX2		0x00000008
#define X86_CPU_FEATURE_BMI2		0x00000010
/*
 * ZMM indicates whether 512-bit vectors (zmm registers) should be used.  On
 * some CPUs, to avoid downclocking issues we don't set ZMM even if the CPU
 * supports it, i.e. even if AVX512F is set.  On these CPUs, we may still use
 * AVX-512 instructions, but only with ymm and xmm registers.
 */
#define X86_CPU_FEATURE_ZMM		0x00000020
#define X86_CPU_FEATURE_AVX512F		0x00000040
#define X86_CPU_FEATURE_AVX512BW	0x00000080
#define X86_CPU_FEATURE_AVX512VL	0x00000100
#define X86_CPU_FEATURE_VPCLMULQDQ	0x00000200
#define X86_CPU_FEATURE_AVX512VNNI	0x00000400
#define X86_CPU_FEATURE_AVXVNNI		0x00000800

#define HAVE_SSE2(features)	  (HAVE_SSE2_NATIVE	  || ((features) & X86_CPU_FEATURE_SSE2))
#define HAVE_PCLMULQDQ(features)  (HAVE_PCLMULQDQ_NATIVE  || ((features) & X86_CPU_FEATURE_PCLMULQDQ))
#define HAVE_AVX(features)	  (HAVE_AVX_NATIVE	  || ((features) & X86_CPU_FEATURE_AVX))
#define HAVE_AVX2(features)	  (HAVE_AVX2_NATIVE	  || ((features) & X86_CPU_FEATURE_AVX2))
#define HAVE_BMI2(features)	  (HAVE_BMI2_NATIVE	  || ((features) & X86_CPU_FEATURE_BMI2))
#define HAVE_AVX512F(features)	  (HAVE_AVX512F_NATIVE	  || ((features) & X86_CPU_FEATURE_AVX512F))
#define HAVE_AVX512BW(features)	  (HAVE_AVX512BW_NATIVE	  || ((features) & X86_CPU_FEATURE_AVX512BW))
#define HAVE_AVX512VL(features)	  (HAVE_AVX512VL_NATIVE	  || ((features) & X86_CPU_FEATURE_AVX512VL))
#define HAVE_VPCLMULQDQ(features) (HAVE_VPCLMULQDQ_NATIVE || ((features) & X86_CPU_FEATURE_VPCLMULQDQ))
#define HAVE_AVX512VNNI(features) (HAVE_AVX512VNNI_NATIVE || ((features) & X86_CPU_FEATURE_AVX512VNNI))
#define HAVE_AVXVNNI(features)	  (HAVE_AVXVNNI_NATIVE	  || ((features) & X86_CPU_FEATURE_AVXVNNI))

#if HAVE_DYNAMIC_X86_CPU_FEATURES
#define X86_CPU_FEATURES_KNOWN		0x80000000
extern volatile u32 libdeflate_x86_cpu_features;

void libdeflate_init_x86_cpu_features(void);

static inline u32 get_x86_cpu_features(void)
{
	if (libdeflate_x86_cpu_features == 0)
		libdeflate_init_x86_cpu_features();
	return libdeflate_x86_cpu_features;
}
#else /* HAVE_DYNAMIC_X86_CPU_FEATURES */
static inline u32 get_x86_cpu_features(void) { return 0; }
#endif /* !HAVE_DYNAMIC_X86_CPU_FEATURES */

/* SSE2 */
#if defined(__SSE2__) || \
	(defined(_MSC_VER) && \
	 (defined(ARCH_X86_64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)))
#  define HAVE_SSE2_NATIVE	1
#else
#  define HAVE_SSE2_NATIVE	0
#endif
#if HAVE_SSE2_NATIVE || defined(__GNUC__) || defined(__clang__) || \
			defined(_MSC_VER)
#  define HAVE_SSE2_INTRIN	1
#else
#  define HAVE_SSE2_INTRIN	0
#endif

/* PCLMULQDQ */
#if defined(__PCLMUL__) || (defined(_MSC_VER) && defined(__AVX2__))
#  define HAVE_PCLMULQDQ_NATIVE	1
#else
#  define HAVE_PCLMULQDQ_NATIVE	0
#endif
#if HAVE_PCLMULQDQ_NATIVE || defined(__GNUC__) || defined(__clang__) || \
			     defined(_MSC_VER)
#  define HAVE_PCLMULQDQ_INTRIN	1
#else
#  define HAVE_PCLMULQDQ_INTRIN	0
#endif

/* AVX */
#ifdef __AVX__
#  define HAVE_AVX_NATIVE	1
#else
#  define HAVE_AVX_NATIVE	0
#endif
#if HAVE_AVX_NATIVE || defined(__GNUC__) || defined(__clang__) || \
		       defined(_MSC_VER)
#  define HAVE_AVX_INTRIN	1
#else
#  define HAVE_AVX_INTRIN	0
#endif

/* AVX2 */
#ifdef __AVX2__
#  define HAVE_AVX2_NATIVE	1
#else
#  define HAVE_AVX2_NATIVE	0
#endif
#if HAVE_AVX2_NATIVE || defined(__GNUC__) || defined(__clang__) || \
			defined(_MSC_VER)
#  define HAVE_AVX2_INTRIN	1
#else
#  define HAVE_AVX2_INTRIN	0
#endif

/* BMI2 */
#if defined(__BMI2__) || (defined(_MSC_VER) && defined(__AVX2__))
#  define HAVE_BMI2_NATIVE	1
#else
#  define HAVE_BMI2_NATIVE	0
#endif
#if HAVE_BMI2_NATIVE || defined(__GNUC__) || defined(__clang__) || \
			defined(_MSC_VER)
#  define HAVE_BMI2_INTRIN	1
#else
#  define HAVE_BMI2_INTRIN	0
#endif
/*
 * MSVC from VS2017 (toolset v141) apparently miscompiles the _bzhi_*()
 * intrinsics.  It seems to be fixed in VS2022.
 */
#if defined(_MSC_VER) && _MSC_VER < 1930 /* older than VS2022 (toolset v143) */
#  undef HAVE_BMI2_NATIVE
#  undef HAVE_BMI2_INTRIN
#  define HAVE_BMI2_NATIVE	0
#  define HAVE_BMI2_INTRIN	0
#endif

/* AVX512F */
#ifdef __AVX512F__
#  define HAVE_AVX512F_NATIVE	1
#else
#  define HAVE_AVX512F_NATIVE	0
#endif
#if HAVE_AVX512F_NATIVE || GCC_PREREQ(5, 1) || defined(__clang__) || \
			   defined(_MSC_VER)
#  define HAVE_AVX512F_INTRIN	1
#else
#  define HAVE_AVX512F_INTRIN	0
#endif

/* AVX512BW */
#ifdef __AVX512BW__
#  define HAVE_AVX512BW_NATIVE	1
#else
#  define HAVE_AVX512BW_NATIVE	0
#endif
#if HAVE_AVX512BW_NATIVE || GCC_PREREQ(5, 1) || defined(__clang__) || \
			    defined(_MSC_VER)
#  define HAVE_AVX512BW_INTRIN	1
#else
#  define HAVE_AVX512BW_INTRIN	0
#endif

/* AVX512VL */
#ifdef __AVX512VL__
#  define HAVE_AVX512VL_NATIVE	1
#else
#  define HAVE_AVX512VL_NATIVE	0
#endif
#if HAVE_AVX512VL_NATIVE || GCC_PREREQ(5, 1) || defined(__clang__) || \
			    defined(_MSC_VER)
#  define HAVE_AVX512VL_INTRIN	1
#else
#  define HAVE_AVX512VL_INTRIN	0
#endif

/* VPCLMULQDQ */
#ifdef __VPCLMULQDQ__
#  define HAVE_VPCLMULQDQ_NATIVE	1
#else
#  define HAVE_VPCLMULQDQ_NATIVE	0
#endif
#if HAVE_VPCLMULQDQ_NATIVE || GCC_PREREQ(8, 1) || CLANG_PREREQ(6, 0, 0) || \
			      defined(_MSC_VER)
#  define HAVE_VPCLMULQDQ_INTRIN	1
#else
#  define HAVE_VPCLMULQDQ_INTRIN	0
#endif

/* AVX512VNNI */
#ifdef __AVX512VNNI__
#  define HAVE_AVX512VNNI_NATIVE	1
#else
#  define HAVE_AVX512VNNI_NATIVE	0
#endif
#if HAVE_AVX512VNNI_NATIVE || GCC_PREREQ(8, 1) || CLANG_PREREQ(6, 0, 0) || \
			      defined(_MSC_VER)
#  define HAVE_AVX512VNNI_INTRIN	1
#else
#  define HAVE_AVX512VNNI_INTRIN	0
#endif

/* AVX-VNNI */
#ifdef __AVXVNNI__
#  define HAVE_AVXVNNI_NATIVE	1
#else
#  define HAVE_AVXVNNI_NATIVE	0
#endif
#if HAVE_AVXVNNI_NATIVE || GCC_PREREQ(11, 1) || CLANG_PREREQ(12, 0, 0) || \
			   defined(_MSC_VER)
#  define HAVE_AVXVNNI_INTRIN	1
#else
#  define HAVE_AVXVNNI_INTRIN	0
#endif

#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
#  include <immintrin.h>
#endif
#if defined(_MSC_VER) && defined(__clang__)
  /*
   * With clang in MSVC compatibility mode, immintrin.h incorrectly skips
   * including some sub-headers.
   */
#  include <tmmintrin.h>
#  include <smmintrin.h>
#  include <wmmintrin.h>
#  include <avxintrin.h>
#  include <avx2intrin.h>
#  include <avx512fintrin.h>
#  include <avx512bwintrin.h>
#  include <avx512vlintrin.h>
#  if __has_include(<vpclmulqdqintrin.h>)
#    include <vpclmulqdqintrin.h>
#  endif
#  if __has_include(<avx512vnniintrin.h>)
#    include <avx512vnniintrin.h>
#  endif
#  if __has_include(<avxvnniintrin.h>)
#    include <avxvnniintrin.h>
#  endif
#endif

#endif /* ARCH_X86_32 || ARCH_X86_64 */

#endif /* LIB_X86_CPU_FEATURES_H */
