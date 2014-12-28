/*
 * lz_extend.h
 *
 * Fast match extension for Lempel-Ziv matchfinding.
 */

#pragma once

#include "bitops.h"
#include "unaligned.h"

/*
 * Return the number of bytes at @matchptr that match the bytes at @strptr, up
 * to a maximum of @max_len.  Initially, @start_len bytes are matched.
 */
static inline unsigned
lz_extend(const u8 * const strptr, const u8 * const matchptr,
	  const unsigned start_len, const unsigned max_len)
{
	unsigned len = start_len;
	machine_word_t v_word;

	if (UNALIGNED_ACCESS_IS_FAST) {

		if (likely(max_len - len >= 4 * WORDSIZE)) {

		#define COMPARE_WORD_STEP					\
			v_word = load_word_unaligned(&matchptr[len]) ^		\
				 load_word_unaligned(&strptr[len]);		\
			if (v_word != 0)					\
				goto word_differs;				\
			len += WORDSIZE;					\

			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
		#undef COMPARE_WORD_STEP
		}

		while (len + WORDSIZE <= max_len) {
			v_word = load_word_unaligned(&matchptr[len]) ^
				 load_word_unaligned(&strptr[len]);
			if (v_word != 0)
				goto word_differs;
			len += WORDSIZE;
		}
	}

	while (len < max_len && matchptr[len] == strptr[len])
		len++;
	return len;

word_differs:
	if (CPU_IS_LITTLE_ENDIAN)
		len += (ffsw(v_word) >> 3);
	else
		len += (flsw(v_word) >> 3);
	return len;
}
