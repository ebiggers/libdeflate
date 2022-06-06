/*
 * common_defs.h
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

#ifndef COMMON_COMMON_DEFS_H
#define COMMON_COMMON_DEFS_H

#ifndef _MSC_VER
#  include <stdbool.h>
#  include <stdlib.h>	/* for _byteswap_*() */
#endif
#include <stddef.h>	/* for size_t */
#include <stdint.h>
#ifndef FREESTANDING
#  include <string.h>	/* for memcpy() */
#endif

/* ========================================================================== */
/*                              Type definitions                              */
/* ========================================================================== */

/* Fixed-width integer types */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* bool */
#ifdef _MSC_VER
 /*
  * Old versions of MSVC (e.g. VS2010) don't have the C99 header stdbool.h.
  * Beware: the below replacement isn't fully standard, since normally any value
  * != 0 should be implicitly cast to a bool with value 1... but that doesn't
  * happen if bool is really just an 'int'.
  */
   typedef int bool;
#  define true	1
#  define false	0
#endif

/* ssize_t, if not available in <sys/types.h> */
#ifdef _MSC_VER
#  ifdef _WIN64
     typedef long long ssize_t;
#  else
     typedef long ssize_t;
#  endif
#endif

/*
 * Word type of the target architecture.  Use 'size_t' instead of
 * 'unsigned long' to account for platforms such as Windows that use 32-bit
 * 'unsigned long' on 64-bit architectures.
 */
typedef size_t machine_word_t;

/* Number of bytes in a word */
#define WORDBYTES	((int)sizeof(machine_word_t))

/* Number of bits in a word */
#define WORDBITS	(8 * WORDBYTES)

/* ========================================================================== */
/*                         Optional compiler features                         */
/* ========================================================================== */

/* Compiler version checks.  Only use when absolutely necessary. */
#if defined(__GNUC__) && !defined(__clang__) && !defined(__INTEL_COMPILER)
#  define GCC_PREREQ(major, minor)		\
	(__GNUC__ > (major) ||			\
	 (__GNUC__ == (major) && __GNUC_MINOR__ >= (minor)))
#else
#  define GCC_PREREQ(major, minor)	0
#endif
#ifdef __clang__
#  ifdef __apple_build_version__
#    define CLANG_PREREQ(major, minor, apple_version)	\
	(__apple_build_version__ >= (apple_version))
#  else
#    define CLANG_PREREQ(major, minor, apple_version)	\
	(__clang_major__ > (major) ||			\
	 (__clang_major__ == (major) && __clang_minor__ >= (minor)))
#  endif
#else
#  define CLANG_PREREQ(major, minor, apple_version)	0
#endif

/*
 * Macros to check for compiler support for attributes and builtins.  clang
 * implements these macros, but gcc doesn't, so generally any use of one of
 * these macros must also be combined with a gcc version check.
 */
#ifndef __has_attribute
#  define __has_attribute(attribute)	0
#endif
#ifndef __has_builtin
#  define __has_builtin(builtin)	0
#endif

/* LIBEXPORT - export a function from a shared library */
#ifdef _WIN32
#  define LIBEXPORT		__declspec(dllexport)
#elif defined(__GNUC__)
#  define LIBEXPORT		__attribute__((visibility("default")))
#else
#  define LIBEXPORT
#endif

/* inline - suggest that a function be inlined */
#ifdef _MSC_VER
#  define inline		__inline
#endif /* else assume 'inline' is usable as-is */

/* forceinline - force a function to be inlined, if possible */
#ifdef __GNUC__
#  define forceinline		inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define forceinline		__forceinline
#else
#  define forceinline		inline
#endif

/* MAYBE_UNUSED - mark a function or variable as maybe unused */
#ifdef __GNUC__
#  define MAYBE_UNUSED		__attribute__((unused))
#else
#  define MAYBE_UNUSED
#endif

/* restrict - hint that writes only occur through the given pointer */
#ifdef __GNUC__
#  define restrict		__restrict__
#else
/* Don't use MSVC's __restrict; it has nonstandard behavior. */
#  define restrict
#endif

/* likely(expr) - hint that an expression is usually true */
#ifdef __GNUC__
#  define likely(expr)		__builtin_expect(!!(expr), 1)
#else
#  define likely(expr)		(expr)
#endif

/* unlikely(expr) - hint that an expression is usually false */
#ifdef __GNUC__
#  define unlikely(expr)	__builtin_expect(!!(expr), 0)
#else
#  define unlikely(expr)	(expr)
#endif

/* prefetchr(addr) - prefetch into L1 cache for read */
#ifdef __GNUC__
#  define prefetchr(addr)	__builtin_prefetch((addr), 0)
#else
#  define prefetchr(addr)
#endif

/* prefetchw(addr) - prefetch into L1 cache for write */
#ifdef __GNUC__
#  define prefetchw(addr)	__builtin_prefetch((addr), 1)
#else
#  define prefetchw(addr)
#endif

/*
 * _aligned_attribute(n) - declare that the annotated variable, or variables of
 * the annotated type, must be aligned on n-byte boundaries.
 */
#undef _aligned_attribute
#ifdef __GNUC__
#  define _aligned_attribute(n)	__attribute__((aligned(n)))
#endif

/* Does the compiler support the 'target' function attribute? */
#define COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE \
	(GCC_PREREQ(4, 4) || __has_attribute(target))

#if COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE

#  if defined(__i386__) || defined(__x86_64__)

#    define COMPILER_SUPPORTS_PCLMUL_TARGET	\
	(GCC_PREREQ(4, 4) || __has_builtin(__builtin_ia32_pclmulqdq128))

#    define COMPILER_SUPPORTS_AVX_TARGET	\
	(GCC_PREREQ(4, 6) || __has_builtin(__builtin_ia32_maxps256))

#    define COMPILER_SUPPORTS_BMI2_TARGET	\
	(GCC_PREREQ(4, 7) || __has_builtin(__builtin_ia32_pdep_di))

#    define COMPILER_SUPPORTS_AVX2_TARGET	\
	(GCC_PREREQ(4, 7) || __has_builtin(__builtin_ia32_psadbw256))

#    define COMPILER_SUPPORTS_AVX512BW_TARGET	\
	(GCC_PREREQ(5, 1) || __has_builtin(__builtin_ia32_psadbw512))

	/*
	 * Prior to gcc 4.9 (r200349) and clang 3.8 (r239883), x86 intrinsics
	 * not available in the main target could not be used in 'target'
	 * attribute functions.  Unfortunately clang has no feature test macro
	 * for this so we have to check its version.
	 */
#    if GCC_PREREQ(4, 9) || CLANG_PREREQ(3, 8, 7030000)
#      define COMPILER_SUPPORTS_SSE2_TARGET_INTRINSICS	1
#      define COMPILER_SUPPORTS_PCLMUL_TARGET_INTRINSICS	\
		COMPILER_SUPPORTS_PCLMUL_TARGET
#      define COMPILER_SUPPORTS_AVX2_TARGET_INTRINSICS	\
		COMPILER_SUPPORTS_AVX2_TARGET
#      define COMPILER_SUPPORTS_AVX512BW_TARGET_INTRINSICS	\
		COMPILER_SUPPORTS_AVX512BW_TARGET
#    endif

#  elif defined(__arm__) || defined(__aarch64__)

    /*
     * Determine whether NEON and crypto intrinsics are supported.
     *
     * With gcc prior to 6.1, (r230411 for arm32, r226563 for arm64), neither
     * was available unless enabled in the main target.
     *
     * But even after that, to include <arm_neon.h> (which contains both the
     * basic NEON intrinsics and the crypto intrinsics) the main target still
     * needs to have:
     *   - gcc: hardware floating point support
     *   - clang: NEON support (but not necessarily crypto support)
     */
#    if (GCC_PREREQ(6, 1) && defined(__ARM_FP)) || \
        (defined(__clang__) && defined(__ARM_NEON))
#      define COMPILER_SUPPORTS_NEON_TARGET_INTRINSICS 1
       /*
        * The crypto intrinsics are broken on arm32 with clang, even when using
        * -mfpu=crypto-neon-fp-armv8, because clang's <arm_neon.h> puts them
        * behind __aarch64__.  Undefine __ARM_FEATURE_CRYPTO in that case...
        */
#      if defined(__clang__) && defined(__arm__)
#        undef __ARM_FEATURE_CRYPTO
#      elif __has_builtin(__builtin_neon_vmull_p64) || !defined(__clang__)
#        define COMPILER_SUPPORTS_PMULL_TARGET_INTRINSICS 1
#      endif
#    endif

     /*
      * Determine whether ARM CRC32 intrinsics are supported.
      *
      * This support has been affected by several gcc bugs, which we must avoid
      * by only allowing gcc versions that have the corresponding fixes.  First,
      * gcc commit 943766d37ae4 ("[arm] Fix use of CRC32 intrinsics with Armv8-a
      * and hard-float"), i.e. gcc 8.4+, 9.3+, 10.1+, or 11+, is needed.
      * Second, gcc commit c1cdabe3aab8 ("arm: reorder assembler architecture
      * directives [PR101723]"), i.e. gcc 9.5+, 10.4+, 11.3+, or 12+, is needed
      * when binutils is 2.34 or later, due to
      * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=104439.  We use the second
      * set of prerequisites, as they are stricter and we have no way to detect
      * the binutils version in C source without requiring a configure script.
      *
      * Yet another gcc bug makes arm_acle.h sometimes not define the crc
      * functions even when the corresponding builtins are available.  However,
      * we work around this later when including arm_acle.h.
      *
      * Things are a bit easier with clang -- we can just check whether the
      * crc builtins are available.  However, clang's arm_acle.h is broken in
      * the same way as gcc's, which we work around later in the same way.
      */
#    if GCC_PREREQ(11, 3) || \
        (GCC_PREREQ(10, 4) && !GCC_PREREQ(11, 0)) || \
        (GCC_PREREQ(9, 5) && !GCC_PREREQ(10, 0)) || \
        (defined(__clang__) && __has_builtin(__builtin_arm_crc32b))
#      define COMPILER_SUPPORTS_CRC32_TARGET_INTRINSICS 1
#    endif

#  endif /* __arm__ || __aarch64__ */

#endif /* COMPILER_SUPPORTS_TARGET_FUNCTION_ATTRIBUTE */

/*
 * Prior to gcc 5.1 and clang 3.9, emmintrin.h only defined vectors of signed
 * integers (e.g. __v4si), not vectors of unsigned integers (e.g.  __v4su).  But
 * we need the unsigned ones in order to avoid signed integer overflow, which is
 * undefined behavior.  Add the missing definitions for the unsigned ones if
 * needed.
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

/* Which targets are supported with the 'target' function attribute? */
#ifndef COMPILER_SUPPORTS_BMI2_TARGET
#  define COMPILER_SUPPORTS_BMI2_TARGET 0
#endif
#ifndef COMPILER_SUPPORTS_AVX_TARGET
#  define COMPILER_SUPPORTS_AVX_TARGET 0
#endif
#ifndef COMPILER_SUPPORTS_AVX512BW_TARGET
#  define COMPILER_SUPPORTS_AVX512BW_TARGET 0
#endif

/*
 * Which targets are supported with the 'target' function attribute and have
 * intrinsics that work within 'target'-ed functions?
 */
#ifndef COMPILER_SUPPORTS_SSE2_TARGET_INTRINSICS
#  define COMPILER_SUPPORTS_SSE2_TARGET_INTRINSICS 0
#endif
#ifndef COMPILER_SUPPORTS_PCLMUL_TARGET_INTRINSICS
#  define COMPILER_SUPPORTS_PCLMUL_TARGET_INTRINSICS 0
#endif
#ifndef COMPILER_SUPPORTS_AVX2_TARGET_INTRINSICS
#  define COMPILER_SUPPORTS_AVX2_TARGET_INTRINSICS 0
#endif
#ifndef COMPILER_SUPPORTS_AVX512BW_TARGET_INTRINSICS
#  define COMPILER_SUPPORTS_AVX512BW_TARGET_INTRINSICS 0
#endif
#ifndef COMPILER_SUPPORTS_NEON_TARGET_INTRINSICS
#  define COMPILER_SUPPORTS_NEON_TARGET_INTRINSICS 0
#endif
#ifndef COMPILER_SUPPORTS_PMULL_TARGET_INTRINSICS
#  define COMPILER_SUPPORTS_PMULL_TARGET_INTRINSICS 0
#endif
#ifndef COMPILER_SUPPORTS_CRC32_TARGET_INTRINSICS
#  define COMPILER_SUPPORTS_CRC32_TARGET_INTRINSICS 0
#endif

/* ========================================================================== */
/*                          Miscellaneous macros                              */
/* ========================================================================== */

#define ARRAY_LEN(A)		(sizeof(A) / sizeof((A)[0]))
#define MIN(a, b)		((a) <= (b) ? (a) : (b))
#define MAX(a, b)		((a) >= (b) ? (a) : (b))
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define STATIC_ASSERT(expr)	((void)sizeof(char[1 - 2 * !(expr)]))
#define ALIGN(n, a)		(((n) + (a) - 1) & ~((a) - 1))

/* ========================================================================== */
/*                           Endianness handling                              */
/* ========================================================================== */

/*
 * CPU_IS_LITTLE_ENDIAN() - 1 if the CPU is little endian, or 0 if it is big
 * endian.  When possible this is a compile-time macro that can be used in
 * preprocessor conditionals.  As a fallback, a generic method is used that
 * can't be used in preprocessor conditionals but should still be optimized out.
 */
#if defined(__BYTE_ORDER__) /* gcc v4.6+ and clang */
#  define CPU_IS_LITTLE_ENDIAN()  (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#elif defined(_MSC_VER)
#  define CPU_IS_LITTLE_ENDIAN()  true
#else
static forceinline bool CPU_IS_LITTLE_ENDIAN(void)
{
	union {
		u32 w;
		u8 b;
	} u;

	u.w = 1;
	return u.b;
}
#endif

/* bswap16(v) - swap the bytes of a 16-bit integer */
static forceinline u16 bswap16(u16 v)
{
#if GCC_PREREQ(4, 8) || __has_builtin(__builtin_bswap16)
	return __builtin_bswap16(v);
#elif defined(_MSC_VER)
	return _byteswap_ushort(v);
#else
	return (v << 8) | (v >> 8);
#endif
}

/* bswap32(v) - swap the bytes of a 32-bit integer */
static forceinline u32 bswap32(u32 v)
{
#if GCC_PREREQ(4, 3) || __has_builtin(__builtin_bswap32)
	return __builtin_bswap32(v);
#elif defined(_MSC_VER)
	return _byteswap_ulong(v);
#else
	return ((v & 0x000000FF) << 24) |
	       ((v & 0x0000FF00) << 8) |
	       ((v & 0x00FF0000) >> 8) |
	       ((v & 0xFF000000) >> 24);
#endif
}

/* bswap64(v) - swap the bytes of a 64-bit integer */
static forceinline u64 bswap64(u64 v)
{
#if GCC_PREREQ(4, 3) || __has_builtin(__builtin_bswap64)
	return __builtin_bswap64(v);
#elif defined(_MSC_VER)
	return _byteswap_uint64(v);
#else
	return ((v & 0x00000000000000FF) << 56) |
	       ((v & 0x000000000000FF00) << 40) |
	       ((v & 0x0000000000FF0000) << 24) |
	       ((v & 0x00000000FF000000) << 8) |
	       ((v & 0x000000FF00000000) >> 8) |
	       ((v & 0x0000FF0000000000) >> 24) |
	       ((v & 0x00FF000000000000) >> 40) |
	       ((v & 0xFF00000000000000) >> 56);
#endif
}

#define le16_bswap(v) (CPU_IS_LITTLE_ENDIAN() ? (v) : bswap16(v))
#define le32_bswap(v) (CPU_IS_LITTLE_ENDIAN() ? (v) : bswap32(v))
#define le64_bswap(v) (CPU_IS_LITTLE_ENDIAN() ? (v) : bswap64(v))
#define be16_bswap(v) (CPU_IS_LITTLE_ENDIAN() ? bswap16(v) : (v))
#define be32_bswap(v) (CPU_IS_LITTLE_ENDIAN() ? bswap32(v) : (v))
#define be64_bswap(v) (CPU_IS_LITTLE_ENDIAN() ? bswap64(v) : (v))

/* ========================================================================== */
/*                          Unaligned memory accesses                         */
/* ========================================================================== */

/*
 * UNALIGNED_ACCESS_IS_FAST() - 1 if unaligned memory accesses can be performed
 * efficiently on the target platform, otherwise 0.
 */
#if defined(__GNUC__) && \
	(defined(__x86_64__) || defined(__i386__) || \
	 defined(__ARM_FEATURE_UNALIGNED) || defined(__powerpc64__) || \
	 /*
	  * For all compilation purposes, WebAssembly behaves like any other CPU
	  * instruction set. Even though WebAssembly engine might be running on
	  * top of different actual CPU architectures, the WebAssembly spec
	  * itself permits unaligned access and it will be fast on most of those
	  * platforms, and simulated at the engine level on others, so it's
	  * worth treating it as a CPU architecture with fast unaligned access.
	  */ defined(__wasm__))
#  define UNALIGNED_ACCESS_IS_FAST	1
#elif defined(_MSC_VER)
#  define UNALIGNED_ACCESS_IS_FAST	1
#else
#  define UNALIGNED_ACCESS_IS_FAST	0
#endif

/*
 * Implementing unaligned memory accesses using memcpy() is portable, and it
 * usually gets optimized appropriately by modern compilers.  I.e., each
 * memcpy() of 1, 2, 4, or WORDBYTES bytes gets compiled to a load or store
 * instruction, not to an actual function call.
 *
 * We no longer use the "packed struct" approach to unaligned accesses, as that
 * is nonstandard, has unclear semantics, and doesn't receive enough testing
 * (see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94994).
 *
 * arm32 with __ARM_FEATURE_UNALIGNED in gcc 5 and earlier is a known exception
 * where memcpy() generates inefficient code
 * (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=67366).  However, we no longer
 * consider that one case important enough to maintain different code for.
 * If you run into it, please just use a newer version of gcc (or use clang).
 */

#ifdef FREESTANDING
#  define MEMCOPY	__builtin_memcpy
#else
#  define MEMCOPY	memcpy
#endif

#define DEFINE_UNALIGNED_TYPE(type)				\
static forceinline type						\
load_##type##_unaligned(const void *p)				\
{								\
	type v;							\
								\
	MEMCOPY(&v, p, sizeof(v));				\
	return v;						\
}								\
								\
static forceinline void						\
store_##type##_unaligned(type v, void *p)			\
{								\
	MEMCOPY(p, &v, sizeof(v));				\
}

DEFINE_UNALIGNED_TYPE(u16)
DEFINE_UNALIGNED_TYPE(u32)
DEFINE_UNALIGNED_TYPE(u64)
DEFINE_UNALIGNED_TYPE(machine_word_t)

#undef MEMCOPY

#define load_word_unaligned	load_machine_word_t_unaligned
#define store_word_unaligned	store_machine_word_t_unaligned

/* Unaligned loads with endianness conversion */

static forceinline u16
get_unaligned_le16(const u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST)
		return le16_bswap(load_u16_unaligned(p));
	else
		return ((u16)p[1] << 8) | p[0];
}

static forceinline u16
get_unaligned_be16(const u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST)
		return be16_bswap(load_u16_unaligned(p));
	else
		return ((u16)p[0] << 8) | p[1];
}

static forceinline u32
get_unaligned_le32(const u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST)
		return le32_bswap(load_u32_unaligned(p));
	else
		return ((u32)p[3] << 24) | ((u32)p[2] << 16) |
			((u32)p[1] << 8) | p[0];
}

static forceinline u32
get_unaligned_be32(const u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST)
		return be32_bswap(load_u32_unaligned(p));
	else
		return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
			((u32)p[2] << 8) | p[3];
}

static forceinline u64
get_unaligned_le64(const u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST)
		return le64_bswap(load_u64_unaligned(p));
	else
		return ((u64)p[7] << 56) | ((u64)p[6] << 48) |
			((u64)p[5] << 40) | ((u64)p[4] << 32) |
			((u64)p[3] << 24) | ((u64)p[2] << 16) |
			((u64)p[1] << 8) | p[0];
}

static forceinline machine_word_t
get_unaligned_leword(const u8 *p)
{
	STATIC_ASSERT(WORDBITS == 32 || WORDBITS == 64);
	if (WORDBITS == 32)
		return get_unaligned_le32(p);
	else
		return get_unaligned_le64(p);
}

/* Unaligned stores with endianness conversion */

static forceinline void
put_unaligned_le16(u16 v, u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST) {
		store_u16_unaligned(le16_bswap(v), p);
	} else {
		p[0] = (u8)(v >> 0);
		p[1] = (u8)(v >> 8);
	}
}

static forceinline void
put_unaligned_be16(u16 v, u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST) {
		store_u16_unaligned(be16_bswap(v), p);
	} else {
		p[0] = (u8)(v >> 8);
		p[1] = (u8)(v >> 0);
	}
}

static forceinline void
put_unaligned_le32(u32 v, u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST) {
		store_u32_unaligned(le32_bswap(v), p);
	} else {
		p[0] = (u8)(v >> 0);
		p[1] = (u8)(v >> 8);
		p[2] = (u8)(v >> 16);
		p[3] = (u8)(v >> 24);
	}
}

static forceinline void
put_unaligned_be32(u32 v, u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST) {
		store_u32_unaligned(be32_bswap(v), p);
	} else {
		p[0] = (u8)(v >> 24);
		p[1] = (u8)(v >> 16);
		p[2] = (u8)(v >> 8);
		p[3] = (u8)(v >> 0);
	}
}

static forceinline void
put_unaligned_le64(u64 v, u8 *p)
{
	if (UNALIGNED_ACCESS_IS_FAST) {
		store_u64_unaligned(le64_bswap(v), p);
	} else {
		p[0] = (u8)(v >> 0);
		p[1] = (u8)(v >> 8);
		p[2] = (u8)(v >> 16);
		p[3] = (u8)(v >> 24);
		p[4] = (u8)(v >> 32);
		p[5] = (u8)(v >> 40);
		p[6] = (u8)(v >> 48);
		p[7] = (u8)(v >> 56);
	}
}

static forceinline void
put_unaligned_leword(machine_word_t v, u8 *p)
{
	STATIC_ASSERT(WORDBITS == 32 || WORDBITS == 64);
	if (WORDBITS == 32)
		put_unaligned_le32(v, p);
	else
		put_unaligned_le64(v, p);
}

/* ========================================================================== */
/*                         Bit manipulation functions                         */
/* ========================================================================== */

/*
 * Bit Scan Reverse (BSR) - find the 0-based index (relative to the least
 * significant end) of the *most* significant 1 bit in the input value.  The
 * input value must be nonzero!
 */

static forceinline unsigned
bsr32(u32 v)
{
#ifdef __GNUC__
	return 31 - __builtin_clz(v);
#elif defined(_MSC_VER)
	_BitScanReverse(&v, v);
	return v;
#else
	unsigned i = 0;

	while ((v >>= 1) != 0)
		i++;
	return i;
#endif
}

static forceinline unsigned
bsr64(u64 v)
{
#ifdef __GNUC__
	return 63 - __builtin_clzll(v);
#elif defined(_MSC_VER) && defined(_M_X64)
	_BitScanReverse64(&v, v);
	return v;
#else
	unsigned i = 0;

	while ((v >>= 1) != 0)
		i++;
	return i;
#endif
}

static forceinline unsigned
bsrw(machine_word_t v)
{
	STATIC_ASSERT(WORDBITS == 32 || WORDBITS == 64);
	if (WORDBITS == 32)
		return bsr32(v);
	else
		return bsr64(v);
}

/*
 * Bit Scan Forward (BSF) - find the 0-based index (relative to the least
 * significant end) of the *least* significant 1 bit in the input value.  The
 * input value must be nonzero!
 */

static forceinline unsigned
bsf32(u32 v)
{
#ifdef __GNUC__
	return __builtin_ctz(v);
#elif defined(_MSC_VER)
	_BitScanForward(&v, v);
	return v;
#else
	unsigned i = 0;

	for (; (v & 1) == 0; v >>= 1)
		i++;
	return i;
#endif
}

static forceinline unsigned
bsf64(u64 v)
{
#ifdef __GNUC__
	return __builtin_ctzll(v);
#elif defined(_MSC_VER) && defined(_M_X64)
	_BitScanForward64(&v, v);
	return v;
#else
	unsigned i = 0;

	for (; (v & 1) == 0; v >>= 1)
		i++;
	return i;
#endif
}

static forceinline unsigned
bsfw(machine_word_t v)
{
	STATIC_ASSERT(WORDBITS == 32 || WORDBITS == 64);
	if (WORDBITS == 32)
		return bsf32(v);
	else
		return bsf64(v);
}

/*
 * rbit32(v): reverse the bits in a 32-bit integer.  This doesn't have a
 * fallback implementation; use '#ifdef rbit32' to check if this is available.
 */
#undef rbit32
#if defined(__GNUC__) && defined(__arm__) && \
	(__ARM_ARCH >= 7 || (__ARM_ARCH == 6 && defined(__ARM_ARCH_6T2__)))
static forceinline u32
rbit32(u32 v)
{
	__asm__("rbit %0, %1" : "=r" (v) : "r" (v));
	return v;
}
#define rbit32 rbit32
#elif defined(__GNUC__) && defined(__aarch64__)
static forceinline u32
rbit32(u32 v)
{
	__asm__("rbit %w0, %w1" : "=r" (v) : "r" (v));
	return v;
}
#define rbit32 rbit32
#endif

#endif /* COMMON_COMMON_DEFS_H */
