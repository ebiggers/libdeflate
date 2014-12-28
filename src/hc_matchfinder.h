/*
 * hc_matchfinder.h
 *
 * This is a Hash Chain (hc) based matchfinder.
 *
 * The data structure is a hash table where each hash bucket contains a linked
 * list of sequences, referenced by position.
 *
 * For each sequence (position) in the input, the first 3 bytes are hashed and
 * that sequence (position) is prepended to the appropriate linked list in the
 * hash table.  Since the sequences are inserted in order, each list is always
 * sorted by increasing match offset.
 *
 * At the same time as inserting a sequence, we may search the linked list for
 * matches with that sequence.  At each step, the length of the match is
 * computed.  The search ends when the sequences get too far away (outside of
 * the sliding window), or when the list ends (in the code this is the same
 * check as "too far away"), or when 'max_search_depth' positions have been
 * searched, or when a match of at least 'nice_len' bytes has been found.
 */

#pragma once

#include "lz_extend.h"
#include "lz_hash3.h"
#include "matchfinder_common.h"
#include "unaligned.h"

#ifndef HC_MATCHFINDER_HASH_ORDER
#  if MATCHFINDER_WINDOW_ORDER < 14
#    define HC_MATCHFINDER_HASH_ORDER 14
#  else
#    define HC_MATCHFINDER_HASH_ORDER 15
#  endif
#endif

#define HC_MATCHFINDER_HASH_LENGTH	(1UL << HC_MATCHFINDER_HASH_ORDER)

#define HC_MATCHFINDER_TOTAL_LENGTH	\
	(HC_MATCHFINDER_HASH_LENGTH + MATCHFINDER_WINDOW_SIZE)

struct hc_matchfinder {
	union {
		pos_t mf_data[HC_MATCHFINDER_TOTAL_LENGTH];
		struct {
			pos_t hash_tab[HC_MATCHFINDER_HASH_LENGTH];
			pos_t next_tab[MATCHFINDER_WINDOW_SIZE];
		};
	};
} _aligned_attribute(MATCHFINDER_ALIGNMENT);

static inline void
hc_matchfinder_init(struct hc_matchfinder *mf)
{
	matchfinder_init(mf->hash_tab, HC_MATCHFINDER_HASH_LENGTH);
}

#if MATCHFINDER_IS_SLIDING
static inline void
hc_matchfinder_slide_window(struct hc_matchfinder *mf)
{
	matchfinder_rebase(mf->mf_data, HC_MATCHFINDER_TOTAL_LENGTH);
}
#endif

/*
 * Find the longest match longer than 'best_len'.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base
 *	Pointer to the next byte in the input buffer to process _at the last
 *	time hc_matchfinder_init() or hc_matchfinder_slide_window() was called_.
 * @in_next
 *	Pointer to the next byte in the input buffer to process.  This is the
 *	pointer to the bytes being matched against.
 * @best_len
 *	Require a match at least this long.
 * @max_len
 *	Maximum match length to return.
 * @nice_len
 *	Stop searching if a match of at least this length is found.
 * @max_search_depth
 *	Limit on the number of potential matches to consider.
 * @offset_ret
 *	The match offset is returned here.
 *
 * Return the length of the match found, or 'best_len' if no match longer than
 * 'best_len' was found.
 */
static inline unsigned
hc_matchfinder_longest_match(struct hc_matchfinder * const restrict mf,
			     const u8 * const in_base,
			     const u8 * const in_next,
			     unsigned best_len,
			     const unsigned max_len,
			     const unsigned nice_len,
			     const unsigned max_search_depth,
			     unsigned *offset_ret)
{
	unsigned depth_remaining = max_search_depth;
	const u8 *best_matchptr = best_matchptr; /* uninitialized */
	const u8 *matchptr;
	unsigned len;
	unsigned hash;
	pos_t cur_match;
	u32 first_3_bytes;

	/* Insert the current sequence into the appropriate hash chain.  */
	if (unlikely(max_len < LZ_HASH_REQUIRED_NBYTES))
		goto out;
	first_3_bytes = load_u24_unaligned(in_next);
	hash = lz_hash3_u24(first_3_bytes, HC_MATCHFINDER_HASH_ORDER);
	cur_match = mf->hash_tab[hash];
	mf->next_tab[in_next - in_base] = cur_match;
	mf->hash_tab[hash] = in_next - in_base;

	if (unlikely(best_len >= max_len))
		goto out;

	/* Search the appropriate hash chain for matches.  */

	if (!(matchfinder_match_in_window(cur_match, in_base, in_next)))
		goto out;

	if (best_len < 3) {
		for (;;) {
			/* No length 3 match found yet.
			 * Check the first 3 bytes.  */
			matchptr = &in_base[cur_match];

			if (load_u24_unaligned(matchptr) == first_3_bytes)
				break;

			/* Not a match; keep trying.  */
			cur_match = mf->next_tab[
					matchfinder_slot_for_match(cur_match)];
			if (!matchfinder_match_in_window(cur_match,
							 in_base, in_next))
				goto out;
			if (!--depth_remaining)
				goto out;
		}

		/* Found a length 3 match.  */
		best_matchptr = matchptr;
		best_len = lz_extend(in_next, best_matchptr, 3, max_len);
		if (best_len >= nice_len)
			goto out;
		cur_match = mf->next_tab[matchfinder_slot_for_match(cur_match)];
		if (!matchfinder_match_in_window(cur_match, in_base, in_next))
			goto out;
		if (!--depth_remaining)
			goto out;
	}

	for (;;) {
		for (;;) {
			matchptr = &in_base[cur_match];

			/* Already found a length 3 match.  Try for a longer match;
			 * start by checking the last 2 bytes and the first 4 bytes.  */
		#if UNALIGNED_ACCESS_IS_FAST
			if ((load_u32_unaligned(matchptr + best_len - 3) ==
			     load_u32_unaligned(in_next + best_len - 3)) &&
			    (load_u32_unaligned(matchptr) ==
			     load_u32_unaligned(in_next)))
		#else
			if (matchptr[best_len] == in_next[best_len])
		#endif
				break;

			cur_match = mf->next_tab[matchfinder_slot_for_match(cur_match)];
			if (!matchfinder_match_in_window(cur_match, in_base, in_next))
				goto out;
			if (!--depth_remaining)
				goto out;
		}

		if (UNALIGNED_ACCESS_IS_FAST)
			len = 4;
		else
			len = 0;
		len = lz_extend(in_next, matchptr, len, max_len);
		if (len > best_len) {
			best_len = len;
			best_matchptr = matchptr;
			if (best_len >= nice_len)
				goto out;
		}
		cur_match = mf->next_tab[matchfinder_slot_for_match(cur_match)];
		if (!matchfinder_match_in_window(cur_match, in_base, in_next))
			goto out;
		if (!--depth_remaining)
			goto out;
	}
out:
	*offset_ret = in_next - best_matchptr;
	return best_len;
}

/*
 * Advance the match-finder, but don't search for matches.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base
 *	Pointer to the next byte in the input buffer to process _at the last
 *	time hc_matchfinder_init() or hc_matchfinder_slide_window() was called_.
 * @in_next
 *	Pointer to the next byte in the input buffer to process.
 * @in_end
 *	Pointer to the end of the input buffer.
 * @count
 *	Number of bytes to skip; must be > 0.
 */
static inline void
hc_matchfinder_skip_positions(struct hc_matchfinder * restrict mf,
			      const u8 *in_base,
			      const u8 *in_next,
			      const u8 *in_end,
			      unsigned count)
{
	unsigned hash;

	if (unlikely(in_next + count >= in_end - LZ_HASH_REQUIRED_NBYTES))
		return;

	do {
		hash = lz_hash3(in_next, HC_MATCHFINDER_HASH_ORDER);
		mf->next_tab[in_next - in_base] = mf->hash_tab[hash];
		mf->hash_tab[hash] = in_next - in_base;
		in_next++;
	} while (--count);
}
