/*
 * types.h
 *
 * Definitions of fixed-width integers, 'bool', 'size_t', and 'machine_word_t'.
 */

#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint16_t le16;
typedef uint32_t le32;
typedef uint64_t le64;

typedef uint16_t be16;
typedef uint32_t be32;
typedef uint64_t be64;

/*
 * Type of a machine word.  'unsigned long' would be logical, but that is only
 * 32 bits on x86_64 Windows.  The same applies to 'uint_fast32_t'.  So the best
 * we can do without a bunch of #ifdefs appears to be 'size_t'.
 */
typedef size_t machine_word_t;

#define WORDSIZE	sizeof(machine_word_t)
