/*
 * find_min_cost_path_template.h - template for deflate_find_min_cost_path()
 *
 * Copyright 2016 Eric Biggers
 * Copyright 2026 Google LLC
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

/*
 * This file is a template parameterized by FUNCNAME, ATTRIBUTES, and
 * deflate_min_packed_cost.  It is included multiple times in the same .c file
 * with different "arguments", so it intentionally has no include guard.
 *
 * It implements the core part of deflate_find_min_cost_path(): finding the
 * minimum-cost path over the graph of match/literal choices for a block.
 *
 * For each position 'i' from 'block_length - 1' to 0, it decides which literal
 * or match to choose at position 'i' to reach end-of-block in the cheapest way.
 * It saves the choice into optimum_items[i] and the corresponding cost to
 * end-of-block into cost_to_end[i].
 *
 * The main bottleneck is considering matches; there can be many match lengths
 * at each position that need to be considered.  To minimize the overhead of
 * this step and enable fast SIMD implementations, we pack costs and lengths
 * together as follows.  First, we preprocess the bare length costs
 * 'c->p.n.costs.length' to create an array 'length_costs', containing:
 *
 *	(c->p.n.costs.length[len] << 9) | len
 *
 * That is, its high 23 bits contain the length's cost, and its low 9
 * (OPTIMUM_OFFSET_SHIFT in the code) bits contain the length.
 *
 * Then, in 'cost_to_end[i]' we store 'cost[i]' (the minimum cost to reach
 * end-of-block from position 'i') shifted left by 9, using the high 23 bits:
 *
 *	cost[i] << 9
 *
 * To consider multiple lengths (with the same offset) at position 'i', we just
 * sum the corresponding 'length_costs[len]' and 'cost_to_end[i + len]' entries,
 * giving:
 *
 *	((c->p.n.costs.length[len] + cost[i + len]) << 9) | len
 *
 * ... and then take the minimum of those to get:
 *
 *	(best_cost << 9) | best_len
 *
 * This vectorizes well and results in a 'u32' that has both the best cost and
 * the length that gave that cost, without needing to track them separately.
 *
 * However, this scheme gives only 23 bits for the cost, which isn't enough for
 * the entire block.  (In theory, a block's cost can reach 'MAX_BLOCK_LENGTH *
 * MAX_LITERAL_COST', which is just over 2^26 and thus needs 27 bits.)
 *
 * To solve this, we take advantage of the limited local variation in 'cost':
 * over a span of at most DEFLATE_MAX_MATCH_LEN bytes, choosing a literal at
 * each byte costs at most MAX_MATCH_SPAN_COST, so for
 * 'i < j <= MIN(block_length, i + DEFLATE_MAX_MATCH_LEN)',
 * 'cost[i] - cost[j] <= MAX_MATCH_SPAN_COST'.
 *
 * Therefore, we maintain a base cost 'base_cost' (initially 0) and work with
 * costs relative to 'base_cost': 'rel[j] = cost[j] - base_cost'
 * ('best_cost_to_end' in the code).  To avoid having to update 'cost_to_end[]'
 * when 'base_cost' changes, 'cost_to_end[j]' still stores '(cost[j] << 9) mod
 * 2^32', and '-base_cost' is folded into 'length_costs[len]' instead:
 *
 *	(((c->p.n.costs.length[len] - base_cost) << 9) mod 2^32) | len
 *
 * Summing 'length_costs[len]' and 'cost_to_end[i + len]' mod 2^32 then gives:
 *
 *	(((rel[i + len] + c->p.n.costs.length[len]) << 9) mod 2^32) | len
 *
 * As long as every "live" entry 'j' ('i < j <= i + DEFLATE_MAX_MATCH_LEN')
 * satisfies:
 *
 *	0 <= rel[j] < REBASE_THRESHOLD
 *
 * ... each candidate's 'rel[j] + c->p.n.costs.length[len]' lies in
 * '[0, REBASE_THRESHOLD + MAX_LENGTH_COST)', which fits in 23 bits without
 * wrapping around.  Thus the unsigned 32-bit minimum finds the exact minimum
 * relative cost, breaking ties in favor of the smallest length.
 *
 * To maintain '0 <= rel[j] < REBASE_THRESHOLD':
 *
 *   - Initially, 'base_cost = 0', 'cost_to_end[block_length] = 0', and the
 *     sentinel entries past 'block_length' contain 'sentinel << 9' where
 *     'MAX_MATCH_SPAN_COST <= sentinel < REBASE_THRESHOLD'.  This makes any
 *     match extending to a sentinel have a cost to end-of-block greater than
 *     the cost of just choosing literals to end-of-block, so it's never chosen.
 *
 *   - At each position 'i', 'rel[i]' is computed from nonnegative symbol costs
 *     and live 'rel[j] >= 0', so 'rel[i] >= 0'.
 *
 *   - If 'rel[i] >= REBASE_THRESHOLD', we rebase by setting
 *     'base_cost = cost[i] - REBASE_MARGIN' (so the new 'rel[i]' is
 *     'REBASE_MARGIN < REBASE_THRESHOLD').  Increasing 'base_cost' only
 *     decreases 'rel[j]' for other entries, preserving their upper bound.  And
 *     because 'MAX_MATCH_SPAN_COST < REBASE_THRESHOLD <= cost[i] <=
 *     (block_length - i) * MAX_LITERAL_COST', position 'i' is more than
 *     DEFLATE_MAX_MATCH_LEN bytes before 'block_length', so no sentinel entry
 *     is live anymore.  Thus every entry 'j > i' still live after position 'i'
 *     satisfies 'cost[j] >= cost[i] - MAX_MATCH_SPAN_COST >= cost[i] -
 *     REBASE_MARGIN = base_cost', preserving 'rel[j] >= 0'.
 *
 * For safety, REBASE_MARGIN and REBASE_THRESHOLD are set to non-tight bounds.
 */
static ATTRIBUTES u32
FUNCNAME(struct libdeflate_compressor *c, const u32 block_length,
	 const struct lz_match *cache_ptr)
{
	u32 best_cost_to_end = 0; /* relative to base_cost */
	u32 base_cost = 0;
	u32 length_costs[DEFLATE_MAX_MATCH_LEN + 1 + COST_SIMD_PADDING];
	size_t i;

	/*
	 * Initialize length_costs[] from c->p.n.costs.length.  Disable loop
	 * unrolling on clang to avoid excessive code size.
	 */
#ifdef __clang__
#  pragma clang loop unroll(disable)
#endif
	for (i = DEFLATE_MIN_MATCH_LEN; i <= DEFLATE_MAX_MATCH_LEN; i++)
		length_costs[i] = (u32)((c->p.n.costs.length[i] <<
					 OPTIMUM_OFFSET_SHIFT) | i);

	/*
	 * Initialize padding entries to accommodate SIMD loads.  The value
	 * doesn't matter, since the SIMD code masks out these lanes.
	 */
	for (; i < ARRAY_LEN(length_costs); i++)
		length_costs[i] = 0;

	/*
	 * For each position 'i' from 'block_length - 1' to 0, find the cheapest
	 * way to reach end-of-block from position 'i', and set cost_to_end[i]
	 * and optimum_items[i] accordingly.
	 */
	i = block_length;
	do {
		unsigned num_matches;
		u32 literal;
		u32 best_item;

		cache_ptr--;

		num_matches = cache_ptr->length;
		literal = cache_ptr->offset;

		/* It's always possible to choose a literal. */
		best_cost_to_end += c->p.n.costs.literal[literal];
		best_item = (literal << OPTIMUM_OFFSET_SHIFT) | 1;

		i--;

		/*
		 * Also consider matches, if there are any.  They are available
		 * at cache_ptr - num_matches, sorted in order of strictly
		 * increasing length.
		 *
		 * We consider all possible match lengths that are available,
		 * including ones generated by truncating a longer match.
		 * However, we consider only the smallest offset for which each
		 * length is available, since it's very rare for an offset that
		 * isn't the smallest to be best.
		 */
		if (num_matches) {
			const struct lz_match *match = cache_ptr - num_matches;
			size_t len = DEFLATE_MIN_MATCH_LEN;

			do {
				size_t match_len = match->length;
				u32 offset = match->offset;
				u8 offset_slot;
				u32 packed_best;
				u32 match_cost; /* relative to base_cost */

				/*
				 * Consider matches at 'offset' with lengths
				 * 'len' through 'match_len'.
				 */
				ASSERT(offset >= 1 &&
				       offset <= DEFLATE_MAX_MATCH_OFFSET);
				ASSERT(len >= DEFLATE_MIN_MATCH_LEN &&
				       len <= match_len &&
				       match_len <= DEFLATE_MAX_MATCH_LEN &&
				       i + match_len + COST_SIMD_PADDING <
					       ARRAY_LEN(c->p.n.cost_to_end));
				offset_slot = c->p.n.offset_slot_full[offset];
				packed_best = deflate_min_packed_cost(
						length_costs,
						&c->p.n.cost_to_end[i],
						len, match_len);
				match_cost = c->p.n.costs.offset_slot[offset_slot] +
					     (packed_best >> OPTIMUM_OFFSET_SHIFT);
				if (match_cost < best_cost_to_end) {
					best_cost_to_end = match_cost;
					best_item = (packed_best & OPTIMUM_LEN_MASK) |
						    (offset << OPTIMUM_OFFSET_SHIFT);
				}
				len = match_len + 1;
			} while (++match != cache_ptr);
			cache_ptr -= num_matches;
		}
		/*
		 * Store the absolute cost shifted left by OPTIMUM_OFFSET_SHIFT.
		 * This truncates the cost to its low 23 bits, which is safe, as
		 * explained in this function's comment.
		 */
		c->p.n.cost_to_end[i] =
			(best_cost_to_end + base_cost) << OPTIMUM_OFFSET_SHIFT;
		c->p.n.optimum_items[i] = best_item;

		/*
		 * Increment base_cost if best_cost_to_end has reached
		 * REBASE_THRESHOLD.  See this function's comment.
		 */
		if (unlikely(best_cost_to_end >= REBASE_THRESHOLD)) {
			u32 delta = best_cost_to_end - REBASE_MARGIN;
			size_t len;

			best_cost_to_end = REBASE_MARGIN;
			base_cost += delta;
			delta <<= OPTIMUM_OFFSET_SHIFT;
			/* Disable unrolling on clang to avoid code bloat. */
#ifdef __clang__
#  pragma clang loop unroll(disable)
#endif
			for (len = DEFLATE_MIN_MATCH_LEN;
			     len <= DEFLATE_MAX_MATCH_LEN; len++)
				length_costs[len] -= delta;
		}
	} while (i != 0);

	/* Return the absolute cost of the entire block. */
	return best_cost_to_end + base_cost;
}

#undef FUNCNAME
#undef ATTRIBUTES
#undef deflate_min_packed_cost
