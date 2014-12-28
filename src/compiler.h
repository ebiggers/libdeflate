/*
 * compiler.h
 *
 * Compiler and platform-specific definitions.
 */

#pragma once

#ifdef __GNUC__
#  include "compiler-gcc.h"
#else
#  warning "Unrecognized compiler.  Please add a header file for your compiler."
#endif

#ifndef LIBEXPORT
#  define LIBEXPORT
#endif

#ifndef BUILD_BUG_ON
#  define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
#endif

#ifndef likely
#  define likely(expr) (expr)
#endif

#ifndef unlikely
#  define unlikely(expr) (expr)
#endif

#ifndef prefetch
#  define prefetch(addr)
#endif

#ifndef _aligned_attribute
#  error "missing required definition of _aligned_attribute"
#endif

#ifndef _packed_attribute
#  error "missing required definition of _packed_attribute"
#endif

#ifndef CPU_IS_BIG_ENDIAN
#  error "missing required endianness definition"
#endif

#define CPU_IS_LITTLE_ENDIAN (!CPU_IS_BIG_ENDIAN)

#ifndef UNALIGNED_ACCESS_SPEED
#  warning "assuming unaligned accesses are not allowed"
#  define UNALIGNED_ACCESS_SPEED 0
#endif

#define UNALIGNED_ACCESS_IS_ALLOWED	(UNALIGNED_ACCESS_SPEED >= 1)
#define UNALIGNED_ACCESS_IS_FAST	(UNALIGNED_ACCESS_SPEED >= 2)
#define UNALIGNED_ACCESS_IS_VERY_FAST	(UNALIGNED_ACCESS_SPEED >= 3)

#if !defined(min) || !defined(max) || !defined(swap)
#  error "missing required definitions of min(), max(), and swap() macros"
#endif
