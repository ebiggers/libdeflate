/*
 * bt_matchfinder.h
 *
 * This is a Binary Tree (bt) based matchfinder.
 *
 * The data structure is a hash table where each hash bucket contains a binary
 * tree of sequences, referenced by position.  The sequences in the binary tree
 * are ordered such that a left child is lexicographically lesser than its
 * parent, and a right child is lexicographically greater than its parent.
 *
 * For each sequence (position) in the input, the first 3 bytes are hashed and
 * the the appropriate binary tree is re-rooted at that sequence (position).
 * Since the sequences are inserted in order, each binary tree maintains the
 * invariant that each child node has greater match offset than its parent.
 *
 * While inserting a sequence, we may search the binary tree for matches with
 * that sequence.  At each step, the length of the match is computed.  The
 * search ends when the sequences get too far away (outside of the sliding
 * window), or when the binary tree ends (in the code this is the same check as
 * "too far away"), or when 'max_search_depth' positions have been searched, or
 * when a match of at least 'nice_len' bytes has been found.
 *
 * Notes:
 *
 *	- Typically, we need to search more nodes to find a given match in a
 *	  binary tree versus in a linked list.  However, a binary tree has more
 *	  overhead than a linked list: it needs to be kept sorted, and the inner
 *	  search loop is more complicated.  As a result, binary trees are best
 *	  suited for compression modes where the potential matches are searched
 *	  more thoroughly.
 *
 *	- Since no attempt is made to keep the binary trees balanced, it's
 *	  essential to have the 'max_search_depth' cutoff.  Otherwise it could
 *	  take quadratic time to run data through the matchfinder.
 */

#pragma once

#include "lz_extend.h"
#include "lz_hash3.h"
#include "matchfinder_common.h"

#ifndef BT_MATCHFINDER_HASH_ORDER
#  if MATCHFINDER_WINDOW_ORDER < 14
#    define BT_MATCHFINDER_HASH_ORDER 14
#  else
#    define BT_MATCHFINDER_HASH_ORDER 15
#  endif
#endif

#define BT_MATCHFINDER_HASH_LENGTH	(1UL << BT_MATCHFINDER_HASH_ORDER)

#define BT_MATCHFINDER_TOTAL_LENGTH	\
	(BT_MATCHFINDER_HASH_LENGTH + (2UL * MATCHFINDER_WINDOW_SIZE))

struct bt_matchfinder {
	union {
		pos_t mf_data[BT_MATCHFINDER_TOTAL_LENGTH];
		struct {
			pos_t hash_tab[BT_MATCHFINDER_HASH_LENGTH];
			pos_t child_tab[2UL * MATCHFINDER_WINDOW_SIZE];
		};
	};
} _aligned_attribute(MATCHFINDER_ALIGNMENT);

static inline void
bt_matchfinder_init(struct bt_matchfinder *mf)
{
	matchfinder_init(mf->hash_tab, BT_MATCHFINDER_HASH_LENGTH);
}

#if MATCHFINDER_IS_SLIDING
static inline void
bt_matchfinder_slide_window(struct bt_matchfinder *mf)
{
	matchfinder_rebase(mf->mf_data, BT_MATCHFINDER_TOTAL_LENGTH);
}
#endif

/*
 * Find matches with the current sequence.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base
 *	Pointer to the next byte in the input buffer to process _at the last
 *	time bt_matchfinder_init() or bt_matchfinder_slide_window() was called_.
 * @in_next
 *	Pointer to the next byte in the input buffer to process.  This is the
 *	pointer to the bytes being matched against.
 * @max_len
 *	Maximum match length to return.
 * @nice_len
 *	Stop searching if a match of at least this length is found.
 * @max_search_depth
 *	Limit on the number of potential matches to consider.
 * @prev_hash
 *	TODO
 * @matches
 *	Space to write the matches that are found.
 *
 * Returns the number of matches found, which may be anywhere from 0 to
 * (nice_len - 3 + 1), inclusively.  The matches are written to @matches in
 * order of strictly increasing length and strictly increasing offset.  The
 * minimum match length is assumed to be 3.
 */
static inline unsigned
bt_matchfinder_get_matches(struct bt_matchfinder * const restrict mf,
			   const u8 * const in_base,
			   const u8 * const in_next,
			   const unsigned max_len,
			   const unsigned nice_len,
			   const unsigned max_search_depth,
			   unsigned long *prev_hash,
			   struct lz_match * const restrict matches)
{
	struct lz_match *lz_matchptr = matches;
	unsigned depth_remaining = max_search_depth;
	unsigned hash;
	pos_t cur_match;
	const u8 *matchptr;
	unsigned best_len;
	pos_t *pending_lt_ptr, *pending_gt_ptr;
	unsigned best_lt_len, best_gt_len;
	unsigned len;
	pos_t *children;

	if (unlikely(max_len < LZ_HASH_REQUIRED_NBYTES + 1))
		return 0;

	hash = *prev_hash;
	*prev_hash = lz_hash3(in_next + 1, BT_MATCHFINDER_HASH_ORDER);
	prefetch(&mf->hash_tab[*prev_hash]);
	cur_match = mf->hash_tab[hash];
	mf->hash_tab[hash] = in_next - in_base;

	best_len = 2;
	pending_lt_ptr = &mf->child_tab[(in_next - in_base) << 1];
	pending_gt_ptr = &mf->child_tab[((in_next - in_base) << 1) + 1];
	best_lt_len = 0;
	best_gt_len = 0;
	for (;;) {
		if (!matchfinder_match_in_window(cur_match,
						 in_base, in_next) ||
		    !depth_remaining--)
		{
			*pending_lt_ptr = MATCHFINDER_INITVAL;
			*pending_gt_ptr = MATCHFINDER_INITVAL;
			return lz_matchptr - matches;
		}

		matchptr = &in_base[cur_match];
		len = min(best_lt_len, best_gt_len);

		children = &mf->child_tab[(unsigned long)
				matchfinder_slot_for_match(cur_match) << 1];

		if (matchptr[len] == in_next[len]) {

			len = lz_extend(in_next, matchptr, len + 1, max_len);

			if (len > best_len) {
				best_len = len;

				lz_matchptr->length = len;
				lz_matchptr->offset = in_next - matchptr;
				lz_matchptr++;

				if (len >= nice_len) {
					*pending_lt_ptr = children[0];
					*pending_gt_ptr = children[1];
					return lz_matchptr - matches;
				}
			}
		}

		if (matchptr[len] < in_next[len]) {
			*pending_lt_ptr = cur_match;
			pending_lt_ptr = &children[1];
			cur_match = *pending_lt_ptr;
			best_lt_len = len;
		} else {
			*pending_gt_ptr = cur_match;
			pending_gt_ptr = &children[0];
			cur_match = *pending_gt_ptr;
			best_gt_len = len;
		}
	}
}

/*
 * Advance the match-finder, but don't search for matches.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base
 *	Pointer to the next byte in the input buffer to process _at the last
 *	time bc_matchfinder_init() or bc_matchfinder_slide_window() was called_.
 * @in_next
 *	Pointer to the next byte in the input buffer to process.
 * @in_end
 *	Pointer to the end of the input buffer.
 * @nice_len
 *	Stop searching if a match of at least this length is found.
 * @max_search_depth
 *	Limit on the number of potential matches to consider.
 * @prev_hash
 *	TODO
 */
static inline void
bt_matchfinder_skip_position(struct bt_matchfinder * const restrict mf,
			     const u8 * const in_base,
			     const u8 * const in_next,
			     const u8 * const in_end,
			     const unsigned nice_len,
			     const unsigned max_search_depth,
			     unsigned long *prev_hash)
{
	unsigned depth_remaining = max_search_depth;
	unsigned hash;
	pos_t cur_match;
	const u8 *matchptr;
	pos_t *pending_lt_ptr, *pending_gt_ptr;
	unsigned best_lt_len, best_gt_len;
	unsigned len;
	pos_t *children;

	if (unlikely(in_end - in_next < LZ_HASH_REQUIRED_NBYTES + 1))
		return;

	hash = *prev_hash;
	*prev_hash = lz_hash3(in_next + 1, BT_MATCHFINDER_HASH_ORDER);
	prefetch(&mf->hash_tab[*prev_hash]);
	cur_match = mf->hash_tab[hash];
	mf->hash_tab[hash] = in_next - in_base;

	depth_remaining = max_search_depth;
	pending_lt_ptr = &mf->child_tab[(in_next - in_base) << 1];
	pending_gt_ptr = &mf->child_tab[((in_next - in_base) << 1) + 1];
	best_lt_len = 0;
	best_gt_len = 0;
	for (;;) {
		if (!matchfinder_match_in_window(cur_match,
						 in_base, in_next) ||
		    !depth_remaining--)
		{
			*pending_lt_ptr = MATCHFINDER_INITVAL;
			*pending_gt_ptr = MATCHFINDER_INITVAL;
			return;
		}

		matchptr = &in_base[cur_match];
		len = min(best_lt_len, best_gt_len);

		children = &mf->child_tab[(unsigned long)
				matchfinder_slot_for_match(cur_match) << 1];

		if (matchptr[len] == in_next[len]) {
			len = lz_extend(in_next, matchptr, len + 1, nice_len);
			if (len == nice_len) {
				*pending_lt_ptr = children[0];
				*pending_gt_ptr = children[1];
				return;
			}
		}

		if (matchptr[len] < in_next[len]) {
			*pending_lt_ptr = cur_match;
			pending_lt_ptr = &children[1];
			cur_match = *pending_lt_ptr;
			best_lt_len = len;
		} else {
			*pending_gt_ptr = cur_match;
			pending_gt_ptr = &children[0];
			cur_match = *pending_gt_ptr;
			best_gt_len = len;
		}
	}
}
