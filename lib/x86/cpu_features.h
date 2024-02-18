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

#if COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE || defined(_MSC_VER)
#  undef HAVE_DYNAMIC_X86_CPU_FEATURES
#  define HAVE_DYNAMIC_X86_CPU_FEATURES	1
#endif

#define X86_CPU_FEATURE_SSE2		0x00000001
#define X86_CPU_FEATURE_PCLMULQDQ	0x00000002
#define X86_CPU_FEATURE_AVX		0x00000004
#define X86_CPU_FEATURE_AVX2		0x00000008
#define X86_CPU_FEATURE_BMI2		0x00000010
#define X86_CPU_FEATURE_AVX512F		0x00000020
#define X86_CPU_FEATURE_AVX512VL	0x00000040
#define X86_CPU_FEATURE_VPCLMULQDQ	0x00000080

#define HAVE_SSE2(features)	(HAVE_SSE2_NATIVE     || ((features) & X86_CPU_FEATURE_SSE2))
#define HAVE_PCLMULQDQ(features) (HAVE_PCLMULQDQ_NATIVE || ((features) & X86_CPU_FEATURE_PCLMULQDQ))
#define HAVE_AVX(features)	(HAVE_AVX_NATIVE      || ((features) & X86_CPU_FEATURE_AVX))
#define HAVE_AVX2(features)	(HAVE_AVX2_NATIVE     || ((features) & X86_CPU_FEATURE_AVX2))
#define HAVE_BMI2(features)	(HAVE_BMI2_NATIVE     || ((features) & X86_CPU_FEATURE_BMI2))
#define HAVE_AVX512F(features)	(HAVE_AVX512F_NATIVE || ((features) & X86_CPU_FEATURE_AVX512F))
#define HAVE_AVX512VL(features)	(HAVE_AVX512VL_NATIVE || ((features) & X86_CPU_FEATURE_AVX512VL))
#define HAVE_VPCLMULQDQ(features) (HAVE_VPCLMULQDQ_NATIVE || ((features) & X86_CPU_FEATURE_VPCLMULQDQ))

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

/*
 * Prior to gcc 4.9 (r200349) and clang 3.8 (r239883), x86 intrinsics not
 * available in the main target couldn't be used in 'target' attribute
 * functions.  Unfortunately clang has no feature test macro for this, so we
 * have to check its version.
 */
#if HAVE_DYNAMIC_X86_CPU_FEATURES && \
	(GCC_PREREQ(4, 9) || CLANG_PREREQ(3, 8, 7030000) || defined(_MSC_VER))
#  define HAVE_TARGET_INTRINSICS	1
#else
#  define HAVE_TARGET_INTRINSICS	0
#endif

/* SSE2 */
#if defined(__SSE2__) || \
	(defined(_MSC_VER) && \
	 (defined(ARCH_X86_64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)))
#  define HAVE_SSE2_NATIVE	1
#else
#  define HAVE_SSE2_NATIVE	0
#endif
#define HAVE_SSE2_INTRIN	(HAVE_SSE2_NATIVE || HAVE_TARGET_INTRINSICS)

/* PCLMULQDQ */
#if defined(__PCLMUL__) || (defined(_MSC_VER) && defined(__AVX2__))
#  define HAVE_PCLMULQDQ_NATIVE	1
#else
#  define HAVE_PCLMULQDQ_NATIVE	0
#endif
#if HAVE_PCLMULQDQ_NATIVE || (HAVE_TARGET_INTRINSICS && \
			      (GCC_PREREQ(4, 4) || CLANG_PREREQ(3, 2, 0) || \
			       defined(_MSC_VER)))
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
#if HAVE_AVX_NATIVE || (HAVE_TARGET_INTRINSICS && \
			(GCC_PREREQ(4, 6) || CLANG_PREREQ(3, 0, 0) || \
			 defined(_MSC_VER)))
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
#if HAVE_AVX2_NATIVE || (HAVE_TARGET_INTRINSICS && \
			 (GCC_PREREQ(4, 7) || CLANG_PREREQ(3, 1, 0) || \
			  defined(_MSC_VER)))
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
#if HAVE_BMI2_NATIVE || (HAVE_TARGET_INTRINSICS && \
			 (GCC_PREREQ(4, 7) || CLANG_PREREQ(3, 1, 0) || \
			  defined(_MSC_VER)))
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

/* AVX-512F */
#ifdef __AVX512F__
#  define HAVE_AVX512F_NATIVE	1
#else
#  define HAVE_AVX512F_NATIVE	0
#endif
#if HAVE_AVX512F_NATIVE || GCC_PREREQ(5, 1) || CLANG_PREREQ(3, 8, 0) || \
			   defined(_MSC_VER)
#  define HAVE_AVX512F_INTRIN	1
#else
#  define HAVE_AVX512F_INTRIN	0
#endif

/* AVX-512VL */
#ifdef __AVX512VL__
#  define HAVE_AVX512VL_NATIVE	1
#else
#  define HAVE_AVX512VL_NATIVE	0
#endif
#if HAVE_AVX512VL_NATIVE || GCC_PREREQ(5, 1) || CLANG_PREREQ(3, 8, 0) || \
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
#if HAVE_VPCLMULQDQ_NATIVE || (GCC_PREREQ(8, 1) || CLANG_PREREQ(6, 0, 0) || \
			       defined(_MSC_VER))
#  define HAVE_VPCLMULQDQ_INTRIN	1
#else
#  define HAVE_VPCLMULQDQ_INTRIN	0
#endif

#endif /* ARCH_X86_32 || ARCH_X86_64 */

#endif /* LIB_X86_CPU_FEATURES_H */
