/*
 * compiler.h - compiler and platform-specific definitions
 */

#pragma once

/* All macros in this file are optional, but the compiler-specific header should
 * define as many of them as possible.  */
#ifdef __GNUC__
#  include "compiler-gcc.h"
#else
#  warning "Unrecognized compiler.  Please add a header file for your compiler.  Compilation will proceed, but performance may suffer!"
#endif

/* forceinline - force a function to be inlined  */
#ifndef forceinline
#  define forceinline inline
#endif

/* LIBEXPORT - annotate a public API function  */
#ifndef LIBEXPORT
#  define LIBEXPORT
#endif

/* likely() - hint that the expression is usually true  */
#ifndef likely
#  define likely(expr)		(expr)
#endif

/* unlikely() - hint that the expression is usually false  */
#ifndef unlikely
#  define unlikely(expr)	(expr)
#endif

/* prefetchr() - prefetch into L1 cache for read  */
#ifndef prefetchr
#  define prefetchr(addr)
#endif

/* prefetchw() - prefetch into L1 cache for write  */
#ifndef prefetchw
#  define prefetchw(addr)
#endif

/* _aligned_attribute(n) - force instances of a data structure to be aligned on
 * n-byte boundaries  */
#ifndef _aligned_attribute
#endif

/* compiler_fls32() - efficiently find the index of the last (highest) set bit
 * in a nonzero 32-bit integer  */
#ifndef compiler_fls32
#endif

/* compiler_fls64() - efficiently find the index of the last (highest) set bit
 * in a nonzero 64-bit integer  */
#ifndef compiler_fls64
#endif

/* compiler_ffs32() - efficiently find the index of the first (lowest) set bit
 * in a nonzero 32-bit integer  */
#ifndef compiler_ffs32
#endif

/* compiler_ffs64() - efficiently find the index of the first (lowest) set bit
 * in a nonzero 64-bit integer  */
#ifndef compiler_ffs64
#endif

/* compiler_bswap16() - efficiently swap the bytes of a 16-bit integer.  */
#ifndef compiler_bswap16
#endif

/* compiler_bswap32() - efficiently swap the bytes of a 32-bit integer  */
#ifndef compiler_bswap32
#endif

/* compiler_bswap64() - efficiently swap the bytes of a 64-bit integer  */
#ifndef compiler_bswap64
#endif

/* CPU_IS_LITTLE_ENDIAN() - a macro which evaluates to 1 if the CPU is little
 * endian or 0 if it is big endian.  The macro should be defined in a way such
 * that the compiler can evaluate it at compilation time.  If not defined, a
 * fallback is used.  */
#ifndef CPU_IS_LITTLE_ENDIAN
static forceinline int CPU_IS_LITTLE_ENDIAN(void)
{
	union {
		unsigned int v;
		unsigned char b;
	} u;
	u.v = 1;
	return u.b;
}
#define CPU_IS_LITTLE_ENDIAN CPU_IS_LITTLE_ENDIAN
#endif

#define CPU_IS_BIG_ENDIAN() (!CPU_IS_LITTLE_ENDIAN())

/*
 * DEFINE_UNALIGNED_TYPE(type) - this should be a macro that, given an integer
 * type 'type', defines load_type_unaligned() and store_type_unaligned()
 * functions which load and store variables of type 'type' from/to unaligned
 * memory addresses.  If not defined, a fallback is used.
 */
#ifndef DEFINE_UNALIGNED_TYPE

/* Although memcpy() may seem inefficient, it *usually* gets optimized
 * appropriately by modern compilers.  It's portable and is probably the best
 * fallback.  */
#include <string.h>

#define DEFINE_UNALIGNED_TYPE(type)				\
								\
static forceinline type						\
load_##type##_unaligned(const void *p)				\
{								\
	type v;							\
	memcpy(&v, p, sizeof(v));				\
	return v;						\
}								\
								\
static forceinline void						\
store_##type##_unaligned(type v, void *p)			\
{								\
	memcpy(p, &v, sizeof(v));				\
}

#endif /* DEFINE_UNALIGNED_TYPE */

/*
 * UNALIGNED_ACCESS_IS_FAST - this should be defined to 1 if unaligned memory
 * accesses can be performed efficiently on the target platform.
 */
#ifndef UNALIGNED_ACCESS_IS_FAST
#  define UNALIGNED_ACCESS_IS_FAST 0
#endif
