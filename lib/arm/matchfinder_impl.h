/*
 * arm/matchfinder_impl.h - ARM implementations of matchfinder functions
 *
 * Copyright 2016 Eric Biggers
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

#ifndef LIB_ARM_MATCHFINDER_IMPL_H
#define LIB_ARM_MATCHFINDER_IMPL_H

#include "cpu_features.h"

#undef DISPATCH_NEON
#if !defined(matchfinder_init_default) &&	\
	(defined(__ARM_NEON) || (ARM_CPU_FEATURES_ENABLED &&	\
				 COMPILER_SUPPORTS_NEON_TARGET_INTRINSICS))
#  ifdef __ARM_NEON
#    define ATTRIBUTES
#    define matchfinder_init_default	matchfinder_init_neon
#    define matchfinder_rebase_default	matchfinder_rebase_neon
#  else
#    ifdef __arm__
#      define ATTRIBUTES	__attribute__((target("fpu=neon")))
#    else
#      define ATTRIBUTES	__attribute__((target("+simd")))
#    endif
#    define DISPATCH		1
#    define DISPATCH_NEON	1
#  endif
#  include <arm_neon.h>
static void ATTRIBUTES
matchfinder_init_neon(mf_pos_t *data, size_t size)
{
	int16x8_t *p = (int16x8_t *)data;
	int16x8_t v = (int16x8_t) {
		MATCHFINDER_INITVAL, MATCHFINDER_INITVAL, MATCHFINDER_INITVAL,
		MATCHFINDER_INITVAL, MATCHFINDER_INITVAL, MATCHFINDER_INITVAL,
		MATCHFINDER_INITVAL, MATCHFINDER_INITVAL,
	};

	STATIC_ASSERT(MATCHFINDER_MEM_ALIGNMENT % sizeof(*p) == 0);
	STATIC_ASSERT(MATCHFINDER_SIZE_ALIGNMENT % (4 * sizeof(*p)) == 0);
	STATIC_ASSERT(sizeof(mf_pos_t) == 2);

	do {
		p[0] = v;
		p[1] = v;
		p[2] = v;
		p[3] = v;
		p += 4;
		size -= 4 * sizeof(*p);
	} while (size != 0);
}

static void ATTRIBUTES
matchfinder_rebase_neon(mf_pos_t *data, size_t size)
{
	int16x8_t *p = (int16x8_t *)data;
	int16x8_t v = (int16x8_t) {
		(u16)-MATCHFINDER_WINDOW_SIZE, (u16)-MATCHFINDER_WINDOW_SIZE,
		(u16)-MATCHFINDER_WINDOW_SIZE, (u16)-MATCHFINDER_WINDOW_SIZE,
		(u16)-MATCHFINDER_WINDOW_SIZE, (u16)-MATCHFINDER_WINDOW_SIZE,
		(u16)-MATCHFINDER_WINDOW_SIZE, (u16)-MATCHFINDER_WINDOW_SIZE,
	};

	STATIC_ASSERT(MATCHFINDER_MEM_ALIGNMENT % sizeof(*p) == 0);
	STATIC_ASSERT(MATCHFINDER_SIZE_ALIGNMENT % (4 * sizeof(*p)) == 0);
	STATIC_ASSERT(sizeof(mf_pos_t) == 2);

	do {
		p[0] = vqaddq_s16(p[0], v);
		p[1] = vqaddq_s16(p[1], v);
		p[2] = vqaddq_s16(p[2], v);
		p[3] = vqaddq_s16(p[3], v);
		p += 4;
		size -= 4 * sizeof(*p);
	} while (size != 0);
}
#undef ATTRIBUTES
#endif /* NEON implementation */

#ifdef DISPATCH
static inline mf_init_func_t
arch_select_matchfinder_init_func(void)
{
	u32 features = get_cpu_features();

#ifdef DISPATCH_NEON
	if (features & ARM_CPU_FEATURE_NEON)
		return matchfinder_init_neon;
#endif
	return NULL;
}

static inline mf_rebase_func_t
arch_select_matchfinder_rebase_func(void)
{
	u32 features = get_cpu_features();

#ifdef DISPATCH_NEON
	if (features & ARM_CPU_FEATURE_NEON)
		return matchfinder_rebase_neon;
#endif
	return NULL;
}
#endif /* DISPATCH */

#endif /* LIB_ARM_MATCHFINDER_IMPL_H */
