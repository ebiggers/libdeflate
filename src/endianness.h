/*
 * endianness.h
 *
 * Macros and inline functions for endianness conversion.
 *
 * This file has no copyright assigned and is placed in the Public Domain.
 */

#pragma once

#include "compiler.h"
#include "types.h"

static inline u16 bswap16(u16 n)
{
#ifdef compiler_bswap16
	return compiler_bswap16(n);
#else
	return (n << 8) | (n >> 8);
#endif
}

static inline u32 bswap32(u32 n)
{
#ifdef compiler_bswap32
	return compiler_bswap32(n);
#else
	return (n << 24) |
	       ((n & 0xFF00) << 8) |
	       ((n & 0xFF0000) >> 8) |
	       (n >> 24);
#endif
}

static inline u64 bswap64(u64 n)
{
#ifdef compiler_bswap64
	return compiler_bswap64(n);
#else
	return (n << 56) |
	       ((n & 0xFF00) << 40) |
	       ((n & 0xFF0000) << 24) |
	       ((n & 0xFF000000) << 8) |
	       ((n & 0xFF00000000) >> 8) |
	       ((n & 0xFF0000000000) >> 24) |
	       ((n & 0xFF000000000000) >> 40) |
	       (n >> 56);
#endif
}

#if CPU_IS_BIG_ENDIAN
#  define cpu_to_le16(n) bswap16(n)
#  define cpu_to_le32(n) bswap32(n)
#  define cpu_to_le64(n) bswap64(n)
#  define le16_to_cpu(n) bswap16(n)
#  define le32_to_cpu(n) bswap32(n)
#  define le64_to_cpu(n) bswap64(n)
#  define cpu_to_be16(n) (n)
#  define cpu_to_be32(n) (n)
#  define cpu_to_be64(n) (n)
#  define be16_to_cpu(n) (n)
#  define be32_to_cpu(n) (n)
#  define be64_to_cpu(n) (n)
#else
#  define cpu_to_le16(n) (n)
#  define cpu_to_le32(n) (n)
#  define cpu_to_le64(n) (n)
#  define le16_to_cpu(n) (n)
#  define le32_to_cpu(n) (n)
#  define le64_to_cpu(n) (n)
#  define cpu_to_be16(n) bswap16(n)
#  define cpu_to_be32(n) bswap32(n)
#  define cpu_to_be64(n) bswap64(n)
#  define be16_to_cpu(n) bswap16(n)
#  define be32_to_cpu(n) bswap32(n)
#  define be64_to_cpu(n) bswap64(n)
#endif
