/*
 * arm/crc32_impl.h - ARM implementations of the gzip CRC-32 algorithm
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

#ifndef LIB_ARM_CRC32_IMPL_H
#define LIB_ARM_CRC32_IMPL_H

#include "cpu_features.h"

/* crc32_arm_pmullx4() */
#if HAVE_PMULL_INTRIN && CPU_IS_LITTLE_ENDIAN()
#  define SUFFIX			 _pmullx4
#  define crc32_arm_pmullx4	crc32_arm_pmullx4
#  define ENABLE_CRC		0
#  define ENABLE_PMULL		1
#  define ENABLE_EOR3		0
#  define PMULL_STRIDE_VECS	4
#  if HAVE_PMULL_NATIVE
#    define ATTRIBUTES
#  else
#    ifdef __arm__
#      define ATTRIBUTES    __attribute__((target("fpu=crypto-neon-fp-armv8")))
#    else
#      ifdef __clang__
#        define ATTRIBUTES  __attribute__((target("crypto")))
#      else
#        define ATTRIBUTES  __attribute__((target("+crypto")))
#      endif
#    endif
#  endif
#  include "crc32_template.h"
#endif

/* crc32_arm_crc() */
#if HAVE_CRC32_INTRIN
#  define SUFFIX			 _crc
#  define crc32_arm_crc		crc32_arm_crc
#  if HAVE_CRC32_NATIVE
#    define ATTRIBUTES
#  else
#    ifdef __arm__
#      ifdef __clang__
#        define ATTRIBUTES	__attribute__((target("armv8-a,crc")))
#      else
#        define ATTRIBUTES	__attribute__((target("arch=armv8-a+crc")))
#      endif
#    else
#      ifdef __clang__
#        define ATTRIBUTES	__attribute__((target("crc")))
#      else
#        define ATTRIBUTES	__attribute__((target("+crc")))
#      endif
#    endif
#  endif
#  define ENABLE_CRC	1
#  define ENABLE_PMULL	0
#  include "crc32_template.h"
#endif

/* crc32_arm_pmullx12_crc() */
#if defined(__aarch64__) && CPU_IS_LITTLE_ENDIAN() && \
	(HAVE_PMULL_INTRIN && HAVE_CRC32_INTRIN) && \
	((HAVE_PMULL_NATIVE && HAVE_CRC32_NATIVE) || \
	 (HAVE_PMULL_TARGET && HAVE_CRC32_TARGET))
#  define SUFFIX				 _pmullx12_crc
#  define crc32_arm_pmullx12_crc	crc32_arm_pmullx12_crc
#  if HAVE_PMULL_NATIVE && HAVE_CRC32_NATIVE
#    define ATTRIBUTES
#  else
#    ifdef __clang__
#      define ATTRIBUTES  __attribute__((target("crypto,crc")))
#    else
#      define ATTRIBUTES  __attribute__((target("arch=armv8-a+crypto+crc")))
#    endif
#  endif
#  define ENABLE_CRC		1
#  define ENABLE_PMULL		1
#  define ENABLE_EOR3		0
#  define PMULL_STRIDE_VECS	12
#  include "crc32_template.h"
#endif

/* crc32_arm_pmullx12_crc_eor3() */
#if defined(__aarch64__) && CPU_IS_LITTLE_ENDIAN() && \
	(HAVE_PMULL_INTRIN && HAVE_CRC32_INTRIN) && \
	((HAVE_PMULL_NATIVE && HAVE_CRC32_NATIVE && HAVE_SHA3_NATIVE) || \
	 (HAVE_PMULL_TARGET && HAVE_CRC32_TARGET && HAVE_SHA3_TARGET))
#  define SUFFIX				 _pmullx12_crc_eor3
#  define crc32_arm_pmullx12_crc_eor3	crc32_arm_pmullx12_crc_eor3
#  if HAVE_PMULL_NATIVE && HAVE_CRC32_NATIVE && HAVE_SHA3_NATIVE
#    define ATTRIBUTES
#  else
#    ifdef __clang__
#      define ATTRIBUTES  __attribute__((target("crypto,crc,sha3")))
#    else
#      define ATTRIBUTES  __attribute__((target("arch=armv8.2-a+crypto+crc+sha3")))
#    endif
#  endif
#  define ENABLE_CRC		1
#  define ENABLE_PMULL		1
#  define ENABLE_EOR3		1
#  define PMULL_STRIDE_VECS	12
#  include "crc32_template.h"
#endif

/*
 * On the Apple M1 processor, a PMULL implementation is significantly faster
 * than a CRC scalar implementation, provided that PMULL_STRIDE_VECS is large
 * enough.  Increasing it all the way to 12 is helpful.
 */
#define PREFER_PMULL_TO_CRC	0
#ifdef __APPLE__
#  include <TargetConditionals.h>
#  if TARGET_OS_OSX
#    undef PREFER_PMULL_TO_CRC
#    define PREFER_PMULL_TO_CRC	1
#  endif
#endif

/*
 * If the best implementation is statically available, use it unconditionally.
 * Otherwise choose the best implementation at runtime.
 */
#if PREFER_PMULL_TO_CRC && \
	defined(crc32_arm_pmullx12_crc_eor3) && \
	HAVE_PMULL_NATIVE && HAVE_CRC32_NATIVE && HAVE_SHA3_NATIVE
#  define DEFAULT_IMPL	crc32_arm_pmullx12_crc_eor3
#elif !PREFER_PMULL_TO_CRC && defined(crc32_arm_crc) && HAVE_CRC32_NATIVE
#  define DEFAULT_IMPL	crc32_arm_crc
#else
static inline crc32_func_t
arch_select_crc32_func(void)
{
	const u32 features MAYBE_UNUSED = get_arm_cpu_features();

#if PREFER_PMULL_TO_CRC && defined(crc32_arm_pmullx12_crc_eor3)
	if (HAVE_PMULL(features) && HAVE_CRC32(features) && HAVE_SHA3(features))
		return crc32_arm_pmullx12_crc_eor3;
#endif
#if PREFER_PMULL_TO_CRC && defined(crc32_arm_pmullx12_crc)
	if (HAVE_PMULL(features) && HAVE_CRC32(features))
		return crc32_arm_pmullx12_crc;
#endif
#ifdef crc32_arm_crc
	if (HAVE_CRC32(features))
		return crc32_arm_crc;
#endif
#ifdef crc32_arm_pmullx4
	if (HAVE_PMULL(features))
		return crc32_arm_pmullx4;
#endif
	return NULL;
}
#define arch_select_crc32_func	arch_select_crc32_func
#endif

#endif /* LIB_ARM_CRC32_IMPL_H */
