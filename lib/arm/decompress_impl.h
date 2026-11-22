/*
 * arm/decompress_impl.h - ARM64 NEON decompression implementation
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

#ifndef LIB_ARM_DECOMPRESS_IMPL_H
#define LIB_ARM_DECOMPRESS_IMPL_H

#include "cpu_features.h"

#if defined(ARCH_ARM64) && HAVE_NEON_INTRIN && \
	CPU_IS_LITTLE_ENDIAN() /* untested on big endian */
#  define deflate_decompress_neon	deflate_decompress_neon
#  define SUFFIX			_neon
#  if HAVE_NEON_NATIVE
#    define ATTRIBUTES
#  elif defined(__clang__)
#    define ATTRIBUTES			_target_attribute("simd")
#  else
#    define ATTRIBUTES			_target_attribute("+simd")
#  endif

static forceinline ATTRIBUTES void
copy_match_neon(u8 *dst, const u8 *src, u32 offset, const u8 *out_next)
{
	/* Overrun up to 47 bytes is allowed; see FASTLOOP_MAX_MATCH_OVERRUN. */
	STATIC_ASSERT(FASTLOOP_MAX_MATCH_OVERRUN >= 47);

	/*
	 * offset >= 16: copy 16 bytes at a time.  Since the source and
	 * destination of each 16-byte copy don't overlap, this works even when
	 * the match overlaps itself (offset < length).  Unconditionally copying
	 * 48 bytes covers most matches without a loop.
	 */
	if (likely(offset >= 16)) {
		vst1q_u8(dst + 0, vld1q_u8(src + 0));
		vst1q_u8(dst + 16, vld1q_u8(src + 16));
		vst1q_u8(dst + 32, vld1q_u8(src + 32));
		dst += 48;
		src += 48;
		if (unlikely(dst < out_next)) {
			NO_VECTORIZE_LOOP
			do {
				vst1q_u8(dst + 0, vld1q_u8(src + 0));
				vst1q_u8(dst + 16, vld1q_u8(src + 16));
				dst += 32;
				src += 32;
			} while (dst < out_next);
		}
	} else {
		/*
		 * 1 <= offset <= 15: use tbl (vqtbl1q_u8) to repeat the
		 * 'offset'-byte pattern across a 16-byte vector, then store it
		 * at stride 'step = (16 / offset) * offset' (in 9..16) with no
		 * reloads.
		 *
		 * Loading 16 bytes from 'src' is safe even when offset < 16:
		 * fastloop output slack prevents overreading 'out_end', and
		 * lanes offset..15 (which overlap dst[0..15-offset] and may be
		 * uninitialized) are discarded by tbl since all indices in
		 * repeat_table[offset] are in 0..offset-1.  The 2 initial
		 * stores write up to step + 16 <= 32 bytes (overrun <= 29 for
		 * length >= 3), and any loop stores start at dst < out_next
		 * (overrun <= 15), well within FASTLOOP_MAX_MATCH_OVERRUN.
		 *
		 * Row 0 is unused as a shuffle mask (since offset >= 1), so we
		 * store the stride table '(16 / offset) * offset' there to keep
		 * the entire table 256 bytes instead of 272 bytes.
		 */
		static const u8 _aligned_attribute(16) repeat_table[16][16] = {
			/* [0]: strides repeat_table[0][offset] for offset 1..15 */
			{ 0, 16, 16, 15, 16, 15, 12, 14, 16, 9, 10, 11, 12, 13, 14, 15 },
			/* [1..15]: shuffle masks for offset 1..15 */
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
			{ 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 },
			{ 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 },
			{ 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3 },
			{ 0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0 },
			{ 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3 },
			{ 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 2, 3, 4, 5, 6 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 1, 2, 3, 4 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0, 1, 2 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0 },
		};
		uint8x16_t mask;
		uint8x16_t v;
		size_t step;

		ASSERT(offset >= 1 && offset <= 15);
#if __has_feature(memory_sanitizer)
		/*
		 * Older versions of LLVM's MemorySanitizer don't model
		 * vqtbl1q_u8() lane selection accurately.  Initialize
		 * dst[0..15] (covering src[offset..15] without touching
		 * src[0..offset-1]) before loading from src.
		 */
		vst1q_u8(dst, vdupq_n_u8(0));
#endif
		mask = vld1q_u8(repeat_table[offset]);
		v = vqtbl1q_u8(vld1q_u8(src), mask);
		step = repeat_table[0][offset];

		vst1q_u8(dst, v);
		dst += step;
		vst1q_u8(dst, v);
		dst += step;
		if (unlikely(dst < out_next)) {
			NO_VECTORIZE_LOOP
			do {
				vst1q_u8(dst, v);
				dst += step;
			} while (dst < out_next);
		}
	}
}
#  define COPY_MATCH	copy_match_neon
#  include "../decompress_template.h"

#  if HAVE_NEON_NATIVE
#    define DEFAULT_IMPL	deflate_decompress_neon
#  else
static inline decompress_func_t
arch_select_decompress_func(void)
{
	if (HAVE_NEON(get_arm_cpu_features()))
		return deflate_decompress_neon;
	return NULL;
}
#    define arch_select_decompress_func	arch_select_decompress_func
#  endif
#endif /* defined(ARCH_ARM64) && HAVE_NEON_INTRIN && CPU_IS_LITTLE_ENDIAN() */

#endif /* LIB_ARM_DECOMPRESS_IMPL_H */
