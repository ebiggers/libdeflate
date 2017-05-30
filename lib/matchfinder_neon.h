/*
 * matchfinder_neon.h - matchfinding routines optimized for ARM NEON (Advanced
 * SIMD) instructions
 */

#include <arm_neon.h>

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
