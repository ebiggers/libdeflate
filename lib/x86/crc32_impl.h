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

/* PCLMUL implementation */
#if HAVE_PCLMUL_INTRIN
#  define SUFFIX			 _pclmul
#  define crc32_x86_pclmul	crc32_x86_pclmul
#  if HAVE_PCLMUL_NATIVE
#    define ATTRIBUTES
#  else
#    define ATTRIBUTES		__attribute__((target("pclmul")))
#  endif
#  include "crc32_template.h"
#endif

/*
 * PCLMUL/AVX implementation.  Although the PCLMUL-optimized CRC-32 function
 * doesn't use any AVX intrinsics specifically, it can benefit a lot from being
 * compiled for an AVX target: on Skylake, ~16700 MB/s vs. ~10100 MB/s.  This is
 * probably related to the PCLMULQDQ instructions being assembled in the newer
 * three-operand form rather than the older two-operand form.
 */
#if HAVE_PCLMUL_INTRIN && \
	(HAVE_PCLMUL_NATIVE && HAVE_AVX_NATIVE) || \
	(HAVE_PCLMUL_TARGET && HAVE_AVX_TARGET)
#  define SUFFIX			 _pclmul_avx
#  define crc32_x86_pclmul_avx	crc32_x86_pclmul_avx
#  if HAVE_PCLMUL_NATIVE && HAVE_AVX_NATIVE
#    define ATTRIBUTES
#  else
#    define ATTRIBUTES		__attribute__((target("pclmul,avx")))
#  endif
#  include "crc32_template.h"
#endif

/*
 * If the best implementation is statically available, use it unconditionally.
 * Otherwise choose the best implementation at runtime.
 */
#if defined(crc32_x86_pclmul_avx) && HAVE_PCLMUL_NATIVE && HAVE_AVX_NATIVE
#define DEFAULT_IMPL	crc32_x86_pclmul_avx
#else
static inline crc32_func_t
arch_select_crc32_func(void)
{
	const u32 features MAYBE_UNUSED = get_x86_cpu_features();

#ifdef crc32_x86_pclmul_avx
	if (HAVE_PCLMUL(features) && HAVE_AVX(features))
		return crc32_x86_pclmul_avx;
#endif
#ifdef crc32_x86_pclmul
	if (HAVE_PCLMUL(features))
		return crc32_x86_pclmul;
#endif
	return NULL;
}
#define arch_select_crc32_func	arch_select_crc32_func
#endif

#endif /* LIB_X86_CRC32_IMPL_H */
