/*
 * compiler-gcc.h
 *
 * Compiler and platform-specific definitions for the GNU C compiler.
 */

#pragma once

#ifdef __WIN32__
#  define LIBEXPORT __declspec(dllexport)
#else
#  define LIBEXPORT __attribute__((visibility("default")))
#endif

#define likely(expr)		__builtin_expect(!!(expr), 1)
#define unlikely(expr)		__builtin_expect(!!(expr), 0)
#define prefetch(addr)		__builtin_prefetch(addr)
#define inline			inline __attribute__((always_inline))
#define _aligned_attribute(n)	__attribute__((aligned(n)))
#define _packed_attribute	__attribute__((packed))

#define CPU_IS_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

#if defined(__x86_64__) || defined(__i386__)
#  define UNALIGNED_ACCESS_SPEED 3
#elif defined(__ARM_FEATURE_UNALIGNED) && (__ARM_FEATURE_UNALIGNED == 1)
#  define UNALIGNED_ACCESS_SPEED 2
#else
#  define UNALIGNED_ACCESS_SPEED 0
#endif

#define min(a, b)  ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); \
			(_a < _b) ? _a : _b; })

#define max(a, b)  ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); \
			(_a > _b) ? _a : _b; })

#define swap(a, b) ({ __typeof__(a) _a = a; (a) = (b); (b) = _a; })

#if (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3)
#  define compiler_bswap32 __builtin_bswap32
#  define compiler_bswap64 __builtin_bswap64
#endif

#if (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)
#  define compiler_bswap16 __builtin_bswap16
#endif

#define compiler_fls32(n) (31 - __builtin_clz(n))
#define compiler_fls64(n) (63 - __builtin_clzll(n))
#define compiler_ffs32(n) __builtin_ctz(n)
#define compiler_ffs64(n) __builtin_ctzll(n)
