/*
 * x86/find_min_cost_path.h - x86 implementation of deflate_find_min_cost_path
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

#ifndef LIB_X86_FIND_MIN_COST_PATH_H
#define LIB_X86_FIND_MIN_COST_PATH_H

#include "cpu_features.h"

#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)

/*
 * Consider lengths 'start_len' through 'max_len' of a match at position 'i'.
 * For the best one, return its length in *best_len, and add the length cost and
 * cost of the rest of the path to *cost_to_end.
 */
static forceinline void _target_attribute("avx2")
deflate_consider_match_avx2(struct libdeflate_compressor *c, const u32 i,
			    const u32 start_len, const u32 max_len,
			    u32 *best_len, u32 *cost_to_end)
{
	u32 len = start_len;
	static const u32 tail_masks[15] = {
		0, 0, 0, 0, 0, 0, 0, 0,
		~OPTIMUM_LEN_MASK, ~OPTIMUM_LEN_MASK,
		~OPTIMUM_LEN_MASK, ~OPTIMUM_LEN_MASK,
		~OPTIMUM_LEN_MASK, ~OPTIMUM_LEN_MASK,
		~OPTIMUM_LEN_MASK,
	};
	u32 base_cost = c->p.n.cost_to_end[i + 1] - 65536;
	__m256i v_len = _mm256_set1_epi32(len - (base_cost << OPTIMUM_OFFSET_SHIFT));
	__m256i v_best = _mm256_add_epi32(
		_mm256_loadu_si256((const __m256i *)&c->p.n.costs.length[len]),
		_mm256_loadu_si256((const __m256i *)&c->p.n.cost_to_end[i + len]));
	__m128i best128;
	u32 best;

	STATIC_ASSERT(DEFLATE_MAX_MATCH_LEN * MAX_LITLEN_CODEWORD_LEN * BIT_COST < 65536);
	STATIC_ASSERT(DEFLATE_MAX_MATCH_LEN <= OPTIMUM_LEN_MASK);

	v_best = _mm256_add_epi32(_mm256_slli_epi32(v_best, OPTIMUM_OFFSET_SHIFT),
				  v_len);
	if (len + 7 >= max_len) {
		v_best = _mm256_or_si256(v_best,
					 _mm256_loadu_si256((const __m256i *)&tail_masks[7 + len - max_len]));
	} else {
		const __m256i v_step = _mm256_set1_epi32(8);

		for (len += 8; len + 7 <= max_len; len += 8) {
			__m256i v_cost;

			v_len = _mm256_add_epi32(v_len, v_step);
			v_cost = _mm256_add_epi32(
				_mm256_loadu_si256((const __m256i *)&c->p.n.costs.length[len]),
				_mm256_loadu_si256((const __m256i *)&c->p.n.cost_to_end[i + len]));
			v_cost = _mm256_add_epi32(_mm256_slli_epi32(v_cost, OPTIMUM_OFFSET_SHIFT),
						  v_len);
			v_best = _mm256_min_epu32(v_cost, v_best);
		}
		if (len <= max_len) {
			__m256i v_cost;

			len = max_len - 7;
			v_len = _mm256_set1_epi32(len - (base_cost << OPTIMUM_OFFSET_SHIFT));
			v_cost = _mm256_add_epi32(_mm256_loadu_si256((const __m256i *)&c->p.n.costs.length[len]),
						  _mm256_loadu_si256((const __m256i *)&c->p.n.cost_to_end[i + len]));
			v_cost = _mm256_add_epi32(_mm256_slli_epi32(v_cost, OPTIMUM_OFFSET_SHIFT),
						  v_len);
			v_best = _mm256_min_epu32(v_cost, v_best);
		}
	}
	v_best = _mm256_add_epi32(v_best, _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
	best128 = _mm_min_epu32(_mm256_castsi256_si128(v_best),
				_mm256_extracti128_si256(v_best, 1));
	best128 = _mm_min_epu32(best128,
				_mm_shuffle_epi32(best128, _MM_SHUFFLE(1, 0, 3, 2)));
	best128 = _mm_min_epu32(best128,
				_mm_shuffle_epi32(best128, _MM_SHUFFLE(2, 3, 0, 1)));
	best = _mm_cvtsi128_si32(best128);
	*best_len = best & OPTIMUM_LEN_MASK;
	(*cost_to_end) += base_cost + (best >> OPTIMUM_OFFSET_SHIFT);
}

/*
 * For each position 'i' from 'block_length - 1' to 0, compute cost_to_end[i]
 * and optimum_items[i] by considering the literal and matches at that position.
 *
 * This matches the algorithm of deflate_find_min_cost_path_default(), but this
 * uses AVX2 instructions to consider multiple match lengths more efficiently.
 * The output is identical to the default version.  For more detailed comments
 * on the algorithm, see deflate_find_min_cost_path_default().
 */
static void _target_attribute("avx2")
deflate_find_min_cost_path_avx2(struct libdeflate_compressor *c,
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
				u32 match_len = match->length;
				u32 cost_to_end = deflate_get_offset_cost(c, offset);

				if (len == match_len)
					cost_to_end += c->p.n.costs.length[len] +
						       c->p.n.cost_to_end[i + len];
				else
					deflate_consider_match_avx2(c, i, len,
								    match_len,
								    &len,
								    &cost_to_end);
				if (cost_to_end < best_cost_to_end) {
					best_cost_to_end = cost_to_end;
					best_item = len | (offset << OPTIMUM_OFFSET_SHIFT);
				}
				len = match_len + 1;
			} while (++match != cache_ptr);
			cache_ptr -= num_matches;
		}
		c->p.n.cost_to_end[i] = best_cost_to_end;
		c->p.n.optimum_items[i] = best_item;
	} while (i != 0);
}

#define deflate_find_min_cost_path_arch deflate_find_min_cost_path_arch
static void
deflate_find_min_cost_path_arch(struct libdeflate_compressor *c,
				const u32 block_length,
				const struct lz_match *cache_ptr)
{
	if (HAVE_AVX2(get_x86_cpu_features()))
		deflate_find_min_cost_path_avx2(c, block_length, cache_ptr);
	else
		deflate_find_min_cost_path_default(c, block_length, cache_ptr);
}
#endif /* defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER) */

#endif /* LIB_X86_FIND_MIN_COST_PATH_H */
