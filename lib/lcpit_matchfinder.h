/*
 * lcpit_matchfinder.h
 *
 * A match-finder for Lempel-Ziv compression based on bottom-up construction and
 * traversal of the Longest Common Prefix (LCP) interval tree.
 *
 * The following copying information applies to this specific source code file:
 *
 * Written in 2014-2015 by Eric Biggers <ebiggers3@gmail.com>
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide via the Creative Commons Zero 1.0 Universal Public Domain
 * Dedication (the "CC0").
 *
 * This software is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the CC0 for more details.
 *
 * You should have received a copy of the CC0 along with this software; if not
 * see <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

#ifndef LIB_LCPIT_MATCHFINDER_H
#define LIB_LCPIT_MATCHFINDER_H

#include "lib_common.h"

struct lcpit_matchfinder {
	bool huge_mode;
	u32 cur_pos;
	u32 *pos_data;
	union {
		u32 *intervals;
		u64 *intervals64;
	};
	u32 min_match_len;
	u32 nice_match_len;
	u32 next[2];
	u32 orig_nice_match_len;
};

struct lz_match {
	u32 length;
	u32 offset;
};

extern u64
lcpit_matchfinder_get_needed_memory(size_t max_bufsize);

extern bool
lcpit_matchfinder_init(struct lcpit_matchfinder *mf, size_t max_bufsize,
		       u32 min_match_len, u32 nice_match_len);

extern void
lcpit_matchfinder_load_buffer(struct lcpit_matchfinder *mf, const u8 *T, u32 n);

extern u32
lcpit_matchfinder_get_matches(struct lcpit_matchfinder *mf,
                              struct lz_match *matches);

extern void
lcpit_matchfinder_skip_bytes(struct lcpit_matchfinder *mf, u32 count);

extern void
lcpit_matchfinder_destroy(struct lcpit_matchfinder *mf);

#endif /* LIB_LCPIT_MATCHFINDER_H */
