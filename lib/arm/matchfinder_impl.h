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

#ifdef __ARM_NEON
#  if MATCHFINDER_ALIGNMENT < 16
#    undef MATCHFINDER_ALIGNMENT
#    define MATCHFINDER_ALIGNMENT 16
#  endif
#  include <arm_neon.h>
static forceinline bool
matchfinder_init_neon(mf_pos_t *data, size_t size)
{
	int16x8_t v, *p;
	size_t n;

	if (size % (sizeof(int16x8_t) * 4) != 0)
		return false;

	STATIC_ASSERT(sizeof(mf_pos_t) == 2);
	v = (int16x8_t) {
		MATCHFINDER_INITVAL, MATCHFINDER_INITVAL, MATCHFINDER_INITVAL,
		MATCHFINDER_INITVAL, MATCHFINDER_INITVAL, MATCHFINDER_INITVAL,
		MATCHFINDER_INITVAL, MATCHFINDER_INITVAL,
	};
	p = (int16x8_t *)data;
	n = size / (sizeof(int16x8_t) * 4);
	do {
		p[0] = v;
		p[1] = v;
		p[2] = v;
		p[3] = v;
		p += 4;
	} while (--n);
	return true;
}
#undef arch_matchfinder_init
#define arch_matchfinder_init matchfinder_init_neon

static forceinline bool
matchfinder_rebase_neon(mf_pos_t *data, size_t size)
{
	int16x8_t v, *p;
	size_t n;

	if (size % (sizeof(int16x8_t) * 4) != 0)
		return false;

	STATIC_ASSERT(sizeof(mf_pos_t) == 2);
	v = (int16x8_t) {
		(u16)-MATCHFINDER_WINDOW_SIZE, (u16)-MATCHFINDER_WINDOW_SIZE,
		(u16)-MATCHFINDER_WINDOW_SIZE, (u16)-MATCHFINDER_WINDOW_SIZE,
		(u16)-MATCHFINDER_WINDOW_SIZE, (u16)-MATCHFINDER_WINDOW_SIZE,
		(u16)-MATCHFINDER_WINDOW_SIZE, (u16)-MATCHFINDER_WINDOW_SIZE,
	};
	p = (int16x8_t *)data;
	n = size / (sizeof(int16x8_t) * 4);
	do {
		p[0] = vqaddq_s16(p[0], v);
		p[1] = vqaddq_s16(p[1], v);
		p[2] = vqaddq_s16(p[2], v);
		p[3] = vqaddq_s16(p[3], v);
		p += 4;
	} while (--n);
	return true;
}
#undef arch_matchfinder_rebase
#define arch_matchfinder_rebase matchfinder_rebase_neon

#endif /* __ARM_NEON */
