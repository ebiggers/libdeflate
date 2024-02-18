/*
 * x86/crc32_impl.h - x86 implementations of the gzip CRC-32 algorithm
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

#ifndef LIB_X86_CRC32_IMPL_H
#define LIB_X86_CRC32_IMPL_H

#include "cpu_features.h"

/* PCLMULQDQ implementation */
#if HAVE_PCLMULQDQ_INTRIN
#  define crc32_x86_pclmulqdq	crc32_x86_pclmulqdq
#  define SUFFIX			 _pclmulqdq
#  if HAVE_PCLMULQDQ_NATIVE
#    define ATTRIBUTES
#  else
#    define ATTRIBUTES		_target_attribute("pclmul")
#  endif
#  define VL			16
#  define FOLD_LESSTHAN16BYTES	0
#  define USE_TERNARYLOGIC	0
#  include "crc32_pclmul_template.h"
#endif

/*
 * PCLMULQDQ/AVX implementation.  Compared to the regular PCLMULQDQ
 * implementation, this still uses 128-bit vectors, but it has two potential
 * benefits.  First, simply compiling against the AVX target can improve
 * performance significantly (e.g. 10100 MB/s to 16700 MB/s on Skylake) without
 * actually using any AVX intrinsics, probably due to the availability of
 * non-destructive VEX-encoded instructions.  Second, AVX support implies SSSE3
 * and SSE4.1 support, and we can use SSSE3 and SSE4.1 intrinsics for efficient
 * handling of partial blocks.  (We *could* compile a variant with
 * PCLMULQDQ+SSE4.1 without AVX, but for simplicity we don't currently bother.)
 *
 * FIXME: with MSVC, this isn't actually compiled with AVX code generation
 * enabled yet.  That would require that this be moved to its own .c file.
 */
#if HAVE_PCLMULQDQ_INTRIN && HAVE_AVX_INTRIN
#  define crc32_x86_pclmulqdq_avx	crc32_x86_pclmulqdq_avx
#  define SUFFIX				 _pclmulqdq_avx
#  if HAVE_PCLMULQDQ_NATIVE && HAVE_AVX_NATIVE
#    define ATTRIBUTES
#  else
#    define ATTRIBUTES		_target_attribute("pclmul,avx")
#  endif
#  define VL			16
#  define FOLD_LESSTHAN16BYTES	1
#  define USE_TERNARYLOGIC	0
#  include "crc32_pclmul_template.h"
#endif

/* VPCLMULQDQ/AVX2 implementation.  Uses 256-bit vectors. */
#if HAVE_VPCLMULQDQ_INTRIN && HAVE_PCLMULQDQ_INTRIN && HAVE_AVX2_INTRIN && \
	/*
	 * This has to be disabled on MSVC because MSVC has a bug where it
	 * incorrectly assumes that VPCLMULQDQ implies AVX-512:
	 * https://developercommunity.visualstudio.com/t/Compiler-incorrectly-assumes-VAES-and-VP/10578785?space=62&q=AVX512&sort=newest
	 */ \
	!(defined(_MSC_VER) && !defined(__clang__))
#  define crc32_x86_vpclmulqdq_avx2	crc32_x86_vpclmulqdq_avx2
#  define SUFFIX				 _vpclmulqdq_avx2
#  if HAVE_VPCLMULQDQ_NATIVE && HAVE_PCLMULQDQ_NATIVE && HAVE_AVX2_NATIVE
#    define ATTRIBUTES
#  else
#    define ATTRIBUTES		_target_attribute("vpclmulqdq,pclmul,avx2")
#  endif
#  define VL			32
#  define FOLD_LESSTHAN16BYTES	1
#  define USE_TERNARYLOGIC	0
#  include "crc32_pclmul_template.h"
#endif

/*
 * VPCLMULQDQ/AVX512VL implementation.  This takes advantage of some AVX-512
 * instructions but uses 256-bit vectors rather than 512-bit.  This can be
 * useful on CPUs where 512-bit vectors cause downclocking.
 */
#if HAVE_VPCLMULQDQ_INTRIN && HAVE_PCLMULQDQ_INTRIN && HAVE_AVX512VL_INTRIN
#  define crc32_x86_vpclmulqdq_avx512vl	crc32_x86_vpclmulqdq_avx512vl
#  define SUFFIX				 _vpclmulqdq_avx512vl
#  if HAVE_VPCLMULQDQ_NATIVE && HAVE_PCLMULQDQ_NATIVE && HAVE_AVX512VL_NATIVE
#    define ATTRIBUTES
#  else
#    define ATTRIBUTES		_target_attribute("vpclmulqdq,pclmul,avx512vl")
#  endif
#  define VL			32
#  define FOLD_LESSTHAN16BYTES	1
#  define USE_TERNARYLOGIC	1
#  include "crc32_pclmul_template.h"
#endif

/* VPCLMULQDQ/AVX512F/AVX512VL implementation.  Uses 512-bit vectors. */
#if HAVE_VPCLMULQDQ_INTRIN && HAVE_PCLMULQDQ_INTRIN && \
	HAVE_AVX512F_INTRIN && HAVE_AVX512VL_INTRIN
#  define crc32_x86_vpclmulqdq_avx512f_avx512vl	crc32_x86_vpclmulqdq_avx512f_avx512vl
#  define SUFFIX					 _vpclmulqdq_avx512f_avx512vl
#if HAVE_VPCLMULQDQ_NATIVE && HAVE_PCLMULQDQ_NATIVE && \
	HAVE_AVX512F_NATIVE && HAVE_AVX512VL_NATIVE
#    define ATTRIBUTES
#  else
#    define ATTRIBUTES		_target_attribute("vpclmulqdq,pclmul,avx512f,avx512vl")
#  endif
#  define VL			64
#  define FOLD_LESSTHAN16BYTES	1
#  define USE_TERNARYLOGIC	1
#  include "crc32_pclmul_template.h"
#endif

/* Choose the best implementation at runtime. */
static inline crc32_func_t
arch_select_crc32_func(void)
{
	const u32 features MAYBE_UNUSED = get_x86_cpu_features();

#ifdef crc32_x86_vpclmulqdq_avx512f_avx512vl
	if (HAVE_VPCLMULQDQ(features) && HAVE_PCLMULQDQ(features) &&
	    HAVE_AVX512F(features) && HAVE_AVX512VL(features))
		return crc32_x86_vpclmulqdq_avx512f_avx512vl;
#endif
#ifdef crc32_x86_vpclmulqdq_avx512vl
	if (HAVE_VPCLMULQDQ(features) && HAVE_PCLMULQDQ(features) &&
	    HAVE_AVX512VL(features))
		return crc32_x86_vpclmulqdq_avx512vl;
#endif
#ifdef crc32_x86_vpclmulqdq_avx2
	if (HAVE_VPCLMULQDQ(features) && HAVE_PCLMULQDQ(features) &&
	    HAVE_AVX2(features))
		return crc32_x86_vpclmulqdq_avx2;
#endif
#ifdef crc32_x86_pclmulqdq_avx
	if (HAVE_PCLMULQDQ(features) && HAVE_AVX(features))
		return crc32_x86_pclmulqdq_avx;
#endif
#ifdef crc32_x86_pclmulqdq
	if (HAVE_PCLMULQDQ(features))
		return crc32_x86_pclmulqdq;
#endif
	return NULL;
}
#define arch_select_crc32_func	arch_select_crc32_func

#endif /* LIB_X86_CRC32_IMPL_H */
