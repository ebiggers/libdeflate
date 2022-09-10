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

#if defined(__i386__) || defined(__x86_64__)

#if COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE
#  undef HAVE_DYNAMIC_X86_CPU_FEATURES
#  define HAVE_DYNAMIC_X86_CPU_FEATURES	1
#endif

#define X86_CPU_FEATURE_SSE2		0x00000001
#define X86_CPU_FEATURE_PCLMUL		0x00000002
#define X86_CPU_FEATURE_AVX		0x00000004
#define X86_CPU_FEATURE_AVX2		0x00000008
#define X86_CPU_FEATURE_BMI2		0x00000010

#define HAVE_SSE2(features)	(HAVE_SSE2_NATIVE     || ((features) & X86_CPU_FEATURE_SSE2))
#define HAVE_PCLMUL(features)	(HAVE_PCLMUL_NATIVE   || ((features) & X86_CPU_FEATURE_PCLMUL))
#define HAVE_AVX(features)	(HAVE_AVX_NATIVE      || ((features) & X86_CPU_FEATURE_AVX))
#define HAVE_AVX2(features)	(HAVE_AVX2_NATIVE     || ((features) & X86_CPU_FEATURE_AVX2))
#define HAVE_BMI2(features)	(HAVE_BMI2_NATIVE     || ((features) & X86_CPU_FEATURE_BMI2))

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
#define HAVE_TARGET_INTRINSICS \
	(HAVE_DYNAMIC_X86_CPU_FEATURES && \
	 (GCC_PREREQ(4, 9) || CLANG_PREREQ(3, 8, 7030000)))

/*
 * Before gcc 5.1 and clang 3.9, emmintrin.h only defined vectors of signed
 * integers (e.g. __v4si), not vectors of unsigned integers (e.g. __v4su).  We
 * need the unsigned ones to avoid signed integer overflow, which is undefined
 * behavior.  Add the missing definitions for the unsigned ones if needed.
 */
#if (GCC_PREREQ(4, 0) && !GCC_PREREQ(5, 1)) || \
	(defined(__clang__) && !CLANG_PREREQ(3, 9, 8020000)) || \
	defined(__INTEL_COMPILER)
typedef unsigned long long  __v2du __attribute__((__vector_size__(16)));
typedef unsigned int        __v4su __attribute__((__vector_size__(16)));
typedef unsigned short      __v8hu __attribute__((__vector_size__(16)));
typedef unsigned char      __v16qu __attribute__((__vector_size__(16)));
typedef unsigned long long  __v4du __attribute__((__vector_size__(32)));
typedef unsigned int        __v8su __attribute__((__vector_size__(32)));
typedef unsigned short     __v16hu __attribute__((__vector_size__(32)));
typedef unsigned char      __v32qu __attribute__((__vector_size__(32)));
#endif
#ifdef __INTEL_COMPILER
typedef int   __v16si __attribute__((__vector_size__(64)));
typedef short __v32hi __attribute__((__vector_size__(64)));
typedef char  __v64qi __attribute__((__vector_size__(64)));
#endif

/* SSE2 */
#ifdef __SSE2__
#  define HAVE_SSE2_NATIVE	1
#else
#  define HAVE_SSE2_NATIVE	0
#endif
#define HAVE_SSE2_TARGET	HAVE_DYNAMIC_X86_CPU_FEATURES
#define HAVE_SSE2_INTRIN \
	(HAVE_SSE2_NATIVE || (HAVE_SSE2_TARGET && HAVE_TARGET_INTRINSICS))

/* PCLMUL */
#ifdef __PCLMUL__
#  define HAVE_PCLMUL_NATIVE	1
#else
#  define HAVE_PCLMUL_NATIVE	0
#endif
#define HAVE_PCLMUL_TARGET \
	(HAVE_DYNAMIC_X86_CPU_FEATURES && \
	 (GCC_PREREQ(4, 4) || __has_builtin(__builtin_ia32_pclmulqdq128)))
#define HAVE_PCLMUL_INTRIN \
	(HAVE_PCLMUL_NATIVE || (HAVE_PCLMUL_TARGET && HAVE_TARGET_INTRINSICS))

/* AVX */
#ifdef __AVX__
#  define HAVE_AVX_NATIVE	1
#else
#  define HAVE_AVX_NATIVE	0
#endif
#define HAVE_AVX_TARGET \
	(HAVE_DYNAMIC_X86_CPU_FEATURES && \
	 (GCC_PREREQ(4, 6) || __has_builtin(__builtin_ia32_maxps256)))
#define HAVE_AVX_INTRIN \
	(HAVE_AVX_NATIVE || (HAVE_AVX_TARGET && HAVE_TARGET_INTRINSICS))

/* AVX2 */
#ifdef __AVX2__
#  define HAVE_AVX2_NATIVE	1
#else
#  define HAVE_AVX2_NATIVE	0
#endif
#define HAVE_AVX2_TARGET \
	(HAVE_DYNAMIC_X86_CPU_FEATURES && \
	 (GCC_PREREQ(4, 7) || __has_builtin(__builtin_ia32_psadbw256)))
#define HAVE_AVX2_INTRIN \
	(HAVE_AVX2_NATIVE || (HAVE_AVX2_TARGET && HAVE_TARGET_INTRINSICS))

/* BMI2 */
#ifdef __BMI2__
#  define HAVE_BMI2_NATIVE	1
#else
#  define HAVE_BMI2_NATIVE	0
#endif
#define HAVE_BMI2_TARGET \
	(HAVE_DYNAMIC_X86_CPU_FEATURES && \
	 (GCC_PREREQ(4, 7) || __has_builtin(__builtin_ia32_pdep_di)))
#define HAVE_BMI2_INTRIN \
	(HAVE_BMI2_NATIVE || (HAVE_BMI2_TARGET && HAVE_TARGET_INTRINSICS))

#endif /* __i386__ || __x86_64__ */

#endif /* LIB_X86_CPU_FEATURES_H */
