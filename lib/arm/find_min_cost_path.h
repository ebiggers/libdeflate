/*
 * arm/find_min_cost_path.h - NEON implementation of deflate_min_packed_cost
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

/*
 * This code depends on vminvq_u32(), which is arm64 only.
 * It should be endian-independent, but it's been tested only on little-endian.
 */
#if defined(ARCH_ARM64) && HAVE_NEON_INTRIN && CPU_IS_LITTLE_ENDIAN()

#if HAVE_NEON_NATIVE
#  define ATTRIBUTES
#elif defined(__clang__)
#  define ATTRIBUTES	_target_attribute("simd")
#else
#  define ATTRIBUTES	_target_attribute("+simd")
#endif

/*
 * Return the minimum of '(length_costs[len] + cost_to_end[len]) mod 2^32' over
 * 'start_len <= len <= max_len', assuming 'start_len <= max_len'.
 *
 * This is used to find the best length to choose for a match that has multiple
 * lengths available.  For details, see find_min_cost_path_template.h.
 *
 * This implementation processes 4 lengths at a time using NEON, and it can read
 * up to 'length_costs[max_len + 3]' and 'cost_to_end[max_len + 3]'.
 */
static forceinline ATTRIBUTES u32
deflate_min_packed_cost_neon(const u32 *length_costs, const u32 *cost_to_end,
			     const size_t start_len, const size_t max_len)
{
	size_t len = start_len;
	/* Aligned to avoid a cacheline split */
	static const u32 _aligned_attribute(32) tail_masks[7] = {
		0, 0, 0, 0, ~0U, ~0U, ~0U,
	};
	uint32x4_t v_best, v_cost;

	STATIC_ASSERT(COST_SIMD_PADDING >= 3);

	/*
	 * Compute the packed (cost, len) values for the first 4 lengths.  If
	 * fewer than 4 lengths are being considered, this loads entries past
	 * max_len; that's safe because cost_to_end and length_costs have at
	 * least 3 padding entries at the end, and the extra lanes are masked
	 * out below.
	 */
	v_best = vaddq_u32(vld1q_u32(&length_costs[len]),
			   vld1q_u32(&cost_to_end[len]));
	if (likely(len + 3 >= max_len)) {
		/*
		 * <= 4 lengths: set the lanes past max_len to all 1s so that
		 * they won't be chosen as the minimum.
		 */
		v_best = vorrq_u32(v_best,
				   vld1q_u32(&tail_masks[len + 3 - max_len]));
	} else {
		/*
		 * > 4 lengths: process full vectors of 4, then use an
		 * overlapping vector ending at max_len for the last 1..4.
		 */
		len += 4;
		while (len + 3 < max_len) {
			v_cost = vaddq_u32(vld1q_u32(&length_costs[len]),
					   vld1q_u32(&cost_to_end[len]));
			v_best = vminq_u32(v_cost, v_best);
			len += 4;
		}
		len = max_len - 3;
		v_cost = vaddq_u32(vld1q_u32(&length_costs[len]),
				   vld1q_u32(&cost_to_end[len]));
		v_best = vminq_u32(v_cost, v_best);
	}
	/* Return the minimum of the 4 lanes of v_best. */
	return vminvq_u32(v_best);
}
#define deflate_min_packed_cost_simd	deflate_min_packed_cost_neon
#define have_min_packed_cost_simd()	HAVE_NEON(get_arm_cpu_features())

#endif /* defined(ARCH_ARM64) && HAVE_NEON_INTRIN && CPU_IS_LITTLE_ENDIAN() */

#endif /* LIB_ARM_FIND_MIN_COST_PATH_H */
