/*
 * util.h - useful types, macros, and compiler/platform-specific definitions
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler.h"

/* Definitions of fixed-width integers, 'bool', 'size_t', and 'machine_word_t'  */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/*
 * Type of a machine word.  'unsigned long' would be logical, but that is only
 * 32 bits on x86_64 Windows.  The same applies to 'uint_fast32_t'.  So the best
 * we can do without a bunch of #ifdefs appears to be 'size_t'.
 */
typedef size_t machine_word_t;

#define WORDSIZE	sizeof(machine_word_t)

/* STATIC_ASSERT() - verify the truth of an expression at compilation time.  */
#if __STDC_VERSION__ >= 201112L
#  define STATIC_ASSERT(expr)	_Static_assert((expr), "")
#else
#  define STATIC_ASSERT(expr)	((void)sizeof(char[1 - 2 * !(expr)]))
#endif

/* ARRAY_LEN() - get the number of entries in an array object  */
#define ARRAY_LEN(A) (sizeof(A) / sizeof((A)[0]))

/* MIN() - calculate the minimum of two variables.  Arguments may be evaluted
 * multiple times.  */
#define MIN(a, b)	((a) <= (b) ? (a) : (b))
