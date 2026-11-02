/*
 * arm/find_min_cost_path.h - NEON implementation of deflate_find_min_cost_path
 *
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

#ifndef LIB_ARM_FIND_MIN_COST_PATH_H
#define LIB_ARM_FIND_MIN_COST_PATH_H

#include "cpu_features.h"

#if HAVE_NEON_INTRIN && CPU_IS_LITTLE_ENDIAN()

#if HAVE_NEON_NATIVE
#  define ATTRIBUTES
#elif defined(__clang__)
#  define ATTRIBUTES	_target_attribute("simd")
#else
#  define ATTRIBUTES	_target_attribute("+simd")
#endif

/*
 * For each position 'i' from 'block_length - 1' to 0, compute cost_to_end[i]
 * and optimum_items[i] by considering the literal and matches at that position.
 *
 * This matches the algorithm of deflate_find_min_cost_path_default(), but this
 * uses NEON instructions to consider multiple match lengths more efficiently.
 * The output is identical to the default version.  For more detailed comments
 * on the algorithm, see deflate_find_min_cost_path_default().
 */
static void ATTRIBUTES
deflate_find_min_cost_path_neon(struct libdeflate_compressor *c,
				const u32 block_length,
				const struct lz_match *cache_ptr)
{
	u32 i = block_length;

	do {
		unsigned num_matches;
		u32 literal;
		u32 best_cost_to_end;
		u32 best_item;

		cache_ptr--;
		num_matches = cache_ptr->length;
		literal = cache_ptr->offset;

		best_cost_to_end = c->p.n.costs.literal[literal] +
				   c->p.n.cost_to_end[i];
		best_item = (literal << OPTIMUM_OFFSET_SHIFT) | 1;
		i--;

		if (num_matches) {
			const struct lz_match *match = cache_ptr - num_matches;
			u32 len = DEFLATE_MIN_MATCH_LEN;

			do {
				u32 offset = match->offset;
				u32 offset_cost = deflate_get_offset_cost(c, offset);
				u32 cost_to_end;

				if (len < match->length) {
					static const u32 tail_masks[7] = {
						0, 0, 0, 0,
						~OPTIMUM_LEN_MASK,
						~OPTIMUM_LEN_MASK,
						~OPTIMUM_LEN_MASK,
					};
					static const u32 lane_ids[4] = {
						0, 1, 2, 3,
					};
					const u32 base_cost =
						c->p.n.cost_to_end[i + 1] - 65536;
					uint32x4_t v_len = vdupq_n_u32(
						len - (base_cost << OPTIMUM_OFFSET_SHIFT));
					uint32x4_t v_best = vaddq_u32(
						vld1q_u32(&c->p.n.costs.length[len]),
						vld1q_u32(&c->p.n.cost_to_end[i + len]));
					u32 best;

					STATIC_ASSERT(DEFLATE_MAX_MATCH_LEN *
						      MAX_LITLEN_CODEWORD_LEN *
						      BIT_COST < 65536);
					STATIC_ASSERT(DEFLATE_MAX_MATCH_LEN <=
						      OPTIMUM_LEN_MASK);

					v_best = vaddq_u32(vshlq_n_u32(v_best, OPTIMUM_OFFSET_SHIFT),
							   v_len);
					if (likely(len + 3 >= match->length)) {
						v_best = vorrq_u32(
							v_best,
							vld1q_u32(&tail_masks[3 + len - match->length]));
					} else {
						const uint32x4_t v_step =
							vdupq_n_u32(4);

						for (len += 4; len + 3 <= match->length; len += 4) {
							uint32x4_t v_cost;

							v_len = vaddq_u32(
								v_len, v_step);
							v_cost = vaddq_u32(
								vld1q_u32(&c->p.n.costs.length[len]),
								vld1q_u32(&c->p.n.cost_to_end[i + len]));
							v_cost = vaddq_u32(
								vshlq_n_u32(v_cost, OPTIMUM_OFFSET_SHIFT),
								v_len);
							v_best = vminq_u32(
								v_cost, v_best);
						}
						if (len <= match->length) {
							uint32x4_t v_cost;

							len = match->length - 3;
							v_len = vdupq_n_u32(
								len - (base_cost << OPTIMUM_OFFSET_SHIFT));
							v_cost = vaddq_u32(
								vld1q_u32(&c->p.n.costs.length[len]),
								vld1q_u32(&c->p.n.cost_to_end[i + len]));
							v_cost = vaddq_u32(
								vshlq_n_u32(v_cost, OPTIMUM_OFFSET_SHIFT),
								v_len);
							v_best = vminq_u32(v_cost, v_best);
						}
					}
					v_best = vaddq_u32(v_best,
							   vld1q_u32(lane_ids));
					best = vminvq_u32(v_best);
					cost_to_end = offset_cost + base_cost +
						      (best >> OPTIMUM_OFFSET_SHIFT);
					len = (best & OPTIMUM_LEN_MASK);
				} else {
					cost_to_end = offset_cost +
						c->p.n.costs.length[len] +
						c->p.n.cost_to_end[i + len];
				}
				if (cost_to_end < best_cost_to_end) {
					best_cost_to_end = cost_to_end;
					best_item = len | (offset << OPTIMUM_OFFSET_SHIFT);
				}
				len = match->length + 1;
			} while (++match != cache_ptr);
			cache_ptr -= num_matches;
		}
		c->p.n.cost_to_end[i] = best_cost_to_end;
		c->p.n.optimum_items[i] = best_item;
	} while (i != 0);
}
#undef ATTRIBUTES

#define deflate_find_min_cost_path_arch deflate_find_min_cost_path_arch
static void
deflate_find_min_cost_path_arch(struct libdeflate_compressor *c,
				const u32 block_length,
				const struct lz_match *cache_ptr)
{
	if (HAVE_NEON(get_arm_cpu_features()))
		deflate_find_min_cost_path_neon(c, block_length, cache_ptr);
	else
		deflate_find_min_cost_path_default(c, block_length, cache_ptr);
}
#endif /* HAVE_NEON_INTRIN && CPU_IS_LITTLE_ENDIAN() */

#endif /* LIB_ARM_FIND_MIN_COST_PATH_H */
