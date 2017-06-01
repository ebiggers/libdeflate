/*
 * crc32_arm.h
 *
 * Copyright 2017 Jun He <jun.he@linaro.org>
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
 * CRC-32 folding with ARM Crypto extension-PMULL
 *
 * Based on paper from Intel:
 *	"Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction"
 *	December 2009
 * For more information see the original paper here:
 *	http://www.intel.com/content/dam/www/public/us/en/documents/white-papers/fast-crc-computation-generic-polynomials-pclmulqdq-paper.pdf
 */

#include <stdio.h>

#define __shift_p128_left(data, imm)    vreinterpretq_u64_u8(vextq_u8(vdupq_n_u8(0), vreinterpretq_u8_u64((data)), (imm)))
#define __shift_p128_right(data, imm)   vreinterpretq_u64_u8(vextq_u8(vreinterpretq_u8_u64((data)), vdupq_n_u8(0), (imm)))

/* pre-computed constants */
static const uint64x2_t foldConstants_p4 = {0x000000008f352d95ULL, 0x000000001d9513d7ULL};
static const uint64x2_t foldConstants_p1 = {0x00000000ae689191ULL, 0x00000000ccaa009eULL};
static const uint64x2_t foldConstants_p0 = {0x00000000b8bc6765ULL, 0x00000000ccaa009eULL};
static const uint64x2_t foldConstants_br = {0x00000001db710641ULL, 0x00000001f7011641ULL};
static const uint64x2_t mask32 = {0xFFFFFFFFULL, 0ULL};

static inline uint64x2_t fold_128b(uint64x2_t to, const uint64x2_t from, const uint64x2_t constant)
{
    uint64x2_t tmp_h = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(from, 1), (poly64_t)vgetq_lane_u64(constant, 1));
    uint64x2_t tmp_l = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(from, 0), (poly64_t)vgetq_lane_u64(constant, 0));
    return veorq_u64(tmp_l, veorq_u64(to, tmp_h));
}

static inline uint64x2_t fold_64b(uint64x2_t from, const uint64x2_t constant)
{
    uint64x2_t tmp = from;

    /* from[0]*constant[1], in reflected domain ,from[0] is the high 64bit */
    from = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(from, 0), (poly64_t)vgetq_lane_u64(constant, 1));
    from = veorq_u64(__shift_p128_right(tmp, 8), from);

    /* Fold 96 => 64 bits */
    tmp = __shift_p128_right(from, 4);
    from = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(vandq_u64(from, mask32), 0), (poly64_t)vgetq_lane_u64(constant, 0));
    return veorq_u64(tmp, from);
}

static inline uint64_t barrett_reduction(uint64x2_t data, const uint64x2_t p_q)
{
    uint64x2_t tmp;

    tmp = data;
    data = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(vandq_u64(data, mask32), 0), (poly64_t)vgetq_lane_u64(p_q, 1));
    data = (uint64x2_t)vmull_p64((poly64_t)vgetq_lane_u64(vandq_u64(data, mask32), 0), (poly64_t)vgetq_lane_u64(p_q, 0));

    return vgetq_lane_u32((uint32x4_t)veorq_u64(tmp, data), 1);
}

static u32 ATTRIBUTES
FUNCNAME_ALIGNED(u32 remainder, const unsigned char *p, size_t length)
{
    uint64x2_t *p_data = (uint64x2_t *)p;
    uint64_t remain_len = length, crc = remainder;

    uint64x2_t x0, x1, x2, x3;
    uint64x2_t y0, y1, y2, y3;

    /* expand crc to 128bit */
    y0 = vcombine_u64((uint64x1_t)crc, (uint64x1_t)0ULL);

    /* load first 64B */
    x0 = *p_data++; x1 = *p_data++;
    x2 = *p_data++; x3 = *p_data++;
    remain_len -= 64;

    x0 = x0 ^ y0; /* x0 ^ crc */

    /* 1024bit --> 512bit loop */
    while(remain_len >= 64) {
        /* load 64 bytes */
        y0 = *p_data++; y1 = *p_data++;
        y2 = *p_data++; y3 = *p_data++;

        /* fold 1024bit --> 512bit */
        x0 = fold_128b(y0, x0, foldConstants_p4);
        x1 = fold_128b(y1, x1, foldConstants_p4);
        x2 = fold_128b(y2, x2, foldConstants_p4);
        x3 = fold_128b(y3, x3, foldConstants_p4);

        remain_len -= 64;
    }

    /* folding 512bit --> 128bit */
    x1 = fold_128b(x1, x0, foldConstants_p1);
    x2 = fold_128b(x2, x1, foldConstants_p1);
    x0 = fold_128b(x3, x2, foldConstants_p1);

    /*
     * now we have folded the whole data buffer to 128bits
     * next target is to fold 128bits -> 64bits.
     * this is done in two steps:
     * 1. (128+32)bits -> 96bits (32bits(x^32) are appended)
     * 2. 96bits -> 64bits
     */
    x0 = fold_64b(x0, foldConstants_p0);

    /* calc crc using barrett reduction method */
    remainder = barrett_reduction(x0, foldConstants_br);

    return remainder;
}

/*
 *
 * Note: on unaligned ends of the buffer, we fall back to crc32_slice1() instead
 * of crc32_slice8() because only a few bytes need to be processed, so a smaller
 * table is preferable.
 */
static u32 ATTRIBUTES
FUNCNAME(u32 remainder, const u8 *buffer, size_t nbytes)
{
	if ((uintptr_t)buffer & 15) {
		size_t n = MIN(nbytes, -(uintptr_t)buffer & 15);
		remainder = crc32_slice1(remainder, buffer, n);
		buffer += n;
		nbytes -= n;
	}

	if (nbytes >= 64) {
		remainder = FUNCNAME_ALIGNED(remainder, buffer, nbytes&(~63UL));
		buffer += nbytes & (~63UL);
		nbytes &= 63UL;
	}
	return crc32_slice1(remainder, buffer, nbytes);
}

#undef FUNCNAME
#undef FUNCNAME_ALIGNED
#undef ATTRIBUTES
